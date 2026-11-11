/*
 * PixCarsVita - character viewer (first milestone of the port).
 *
 * Loads a character converted by tools/vitaconv.py from ux0:data/pixcars/<char>/
 * (model.vcm + *.vca listed in anims.txt), plays its animations with CPU skinning
 * and draws it with vitaGL's fixed-function pipeline.
 *
 * Controls:  left stick  orbit camera      D-pad up/down  zoom
 *            L / R       previous / next animation
 *            Cross       pause / resume    Circle        bind pose
 *            Square      toggle lighting   Start         quit
 *
 * No game data is embedded in the VPK: the user converts their own copy on PC.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "vcm.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define DATA_ROOT "ux0:data/pixcars/"
#define MAX_ANIMS 128

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

typedef struct {
    char name[64];
    VcaAnim anim;
    int loaded;
} AnimSlot;

static VcmModel g_model;
static AnimSlot g_anims[MAX_ANIMS];
static int g_n_anims, g_cur = -1;
static float *g_uv_tmp;      /* scratch UVs for eye layers */
static uint8_t *g_col_tmp;   /* scratch vertex colours (CPU lighting) */
static int g_driver_node = -1;

static void log_msg(const char *s)
{
    sceClibPrintf("[pixcars] %s\n", s);
}

/* ------------------------------------------------------------------ assets */
static GLuint upload_texture(const VcmTexture *t)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    uint32_t w = t->width, h = t->height;
    for (uint32_t l = 0; l < t->n_levels; l++) {
        const VcmLevel *lv = &t->levels[l];
        switch (t->format) {
        case VCM_TEX_DXT1:
            glCompressedTexImage2D(GL_TEXTURE_2D, l, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, w, h, 0, lv->size, lv->data);
            break;
        case VCM_TEX_DXT3:
            glCompressedTexImage2D(GL_TEXTURE_2D, l, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, w, h, 0, lv->size, lv->data);
            break;
        case VCM_TEX_DXT5:
            glCompressedTexImage2D(GL_TEXTURE_2D, l, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, w, h, 0, lv->size, lv->data);
            break;
        default:
            glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv->data);
            break;
        }
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    t->n_levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return id;
}

static int load_character(const char *code)
{
    char path[256];
    snprintf(path, sizeof path, DATA_ROOT "%s/model.vcm", code);
    int r = vcm_load_file(&g_model, path);
    if (r) {
        sceClibPrintf("[pixcars] cannot load %s (%d)\n", path, r);
        return r;
    }
    uint32_t max_verts = 0;
    for (uint32_t i = 0; i < g_model.n_batch; i++)
        if (g_model.batch[i].n_verts > max_verts)
            max_verts = g_model.batch[i].n_verts;
    g_uv_tmp = malloc(8u * max_verts);
    g_col_tmp = malloc(4u * max_verts);
    for (uint32_t i = 0; i < g_model.n_tex; i++)
        g_model.tex[i].gpu_handle = upload_texture(&g_model.tex[i]);
    for (uint32_t i = 0; i < g_model.n_nodes; i++) {
        size_t n = strlen(g_model.nodes[i].name);
        if (n >= 7 && strcasecmp(g_model.nodes[i].name + n - 7, "_Driver") == 0)
            g_driver_node = (int)i;
    }

    snprintf(path, sizeof path, DATA_ROOT "%s/anims.txt", code);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[64];
        while (g_n_anims < MAX_ANIMS && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0])
                continue;
            AnimSlot *s = &g_anims[g_n_anims];
            snprintf(s->name, sizeof s->name, "%s", line);
            snprintf(path, sizeof path, DATA_ROOT "%s/%s.vca", code, line);
            if (vca_load_file(&s->anim, path) == 0) {
                vca_bind(&s->anim, &g_model);
                s->loaded = 1;
                g_n_anims++;
            }
        }
        fclose(f);
    }
    sceClibPrintf("[pixcars] %s: %u nodes, %u batches, %d animations\n", code, g_model.n_nodes,
                  g_model.n_batch, g_n_anims);
    if (g_n_anims)
        g_cur = 0;
    return 0;
}

/* ------------------------------------------------------------------ drawing */
static void light_batch(const VcmBatch *b, int lighting)
{
    /* light from the camera-ish side; model is drawn with Z mirrored (D3D -> GL) */
    const float L[3] = { 0.35f, 0.80f, 0.48f };
    for (uint32_t v = 0; v < b->n_verts; v++) {
        float s = 1.0f;
        if (lighting) {
            const float *n = b->skinned_nrm + v * 3;
            float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            float d = len > 0 ? (n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / len : 0;
            s = 0.45f + 0.65f * (d > 0 ? d : 0);
            if (s > 1.0f)
                s = 1.0f;
        }
        uint8_t c = (uint8_t)(s * 255.0f);
        g_col_tmp[v * 4 + 0] = c;
        g_col_tmp[v * 4 + 1] = c;
        g_col_tmp[v * 4 + 2] = c;
        g_col_tmp[v * 4 + 3] = 255;
    }
}

static void draw_model(int lighting)
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    for (uint32_t bi = 0; bi < g_model.n_batch; bi++) {
        const VcmBatch *b = &g_model.batch[bi];
        const VcmMaterial *mat = &g_model.mat[b->material];
        light_batch(b, lighting);
        glVertexPointer(3, GL_FLOAT, 0, b->skinned_pos);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, g_col_tmp);
        uint32_t layers = mat->n_layers ? mat->n_layers : 1;
        for (uint32_t li = 0; li < layers; li++) {
            const VcmLayer *l = &mat->layer[li];
            if (mat->n_layers && l->tex >= 0) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, g_model.tex[l->tex].gpu_handle);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                (l->flags & VCM_LAYER_CLAMP_U) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                (l->flags & VCM_LAYER_CLAMP_V) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            } else {
                glDisable(GL_TEXTURE_2D);
            }
            if (l->flags & VCM_LAYER_ALPHATEST) {
                glEnable(GL_ALPHA_TEST);
                glAlphaFunc(GL_GREATER, l->alpha_ref / 255.0f);
            } else {
                glDisable(GL_ALPHA_TEST);
            }
            if (l->eye_node >= 0) {
                /* CarsEye: shift UVs so the iris sits on the (animated) eye node */
                float off[2];
                vcm_eye_offset(&g_model, b->material, l, off);
                for (uint32_t v = 0; v < b->n_verts; v++) {
                    g_uv_tmp[v * 2 + 0] = b->uv[v * 2 + 0] + off[0];
                    g_uv_tmp[v * 2 + 1] = b->uv[v * 2 + 1] + off[1];
                }
                glTexCoordPointer(2, GL_FLOAT, 0, g_uv_tmp);
                if (li > 0)
                    glDepthFunc(GL_LEQUAL); /* second eye pass draws over the first */
            } else {
                glTexCoordPointer(2, GL_FLOAT, 0, b->uv);
            }
            glDrawElements(GL_TRIANGLE_STRIP, (GLsizei)b->n_idx, GL_UNSIGNED_SHORT, b->strip);
            glDepthFunc(GL_LESS);
        }
    }
    glDisable(GL_ALPHA_TEST);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void set_camera(float yaw, float pitch, float dist, const float target[3])
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float znear = 0.5f, zfar = 500.0f;
    float top = znear * tanf(30.0f * 3.14159265f / 180.0f);
    float right = top * (float)SCREEN_W / (float)SCREEN_H;
    glFrustum(-right, right, -top, top, znear, zfar);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -dist);
    glRotatef(pitch, 1, 0, 0);
    glRotatef(yaw, 0, 1, 0);
    glScalef(1, 1, -1); /* D3D left-handed -> GL right-handed */
    glTranslatef(-target[0], -target[1], -target[2]);
}

/* ------------------------------------------------------------------ main */
int main(void)
{
    vglInit(0x800000);
    vglWaitVblankStart(GL_TRUE);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    if (load_character("mcq") != 0) {
        /* nothing to show: red screen so the user knows the data is missing */
        for (int i = 0; i < 300; i++) {
            glClearColor(0.5f, 0.05f, 0.05f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            vglSwapBuffers(GL_FALSE);
        }
        sceKernelExitProcess(0);
        return 0;
    }

    /* frame the bind-pose model */
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (uint32_t i = 0; i < g_model.n_batch; i++)
        for (uint32_t v = 0; v < g_model.batch[i].n_verts; v++)
            for (int c = 0; c < 3; c++) {
                float p = g_model.batch[i].pos[v * 3 + c];
                mn[c] = p < mn[c] ? p : mn[c];
                mx[c] = p > mx[c] ? p : mx[c];
            }
    float target[3] = { (mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2 };
    float radius = sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                         (mx[2] - mn[2]) * (mx[2] - mn[2])) / 2;
    float yaw = 35.0f, pitch = 15.0f, dist = radius * 2.6f;
    float t = 0.0f;
    int paused = 0, lighting = 1;
    uint32_t prev_buttons = 0;
    SceUInt64 last = sceKernelGetProcessTimeWide();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE); /* most Cars materials are CULLMODE=NONE */

    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~prev_buttons;
        prev_buttons = pad.buttons;
        if (pad.buttons & SCE_CTRL_START)
            break;

        SceUInt64 now = sceKernelGetProcessTimeWide();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;

        int lx = (int)pad.lx - 128, ly = (int)pad.ly - 128;
        if (abs(lx) > 20) yaw += lx * dt * 1.2f;
        if (abs(ly) > 20) pitch += ly * dt * 0.8f;
        if (pitch > 80) pitch = 80;
        if (pitch < -80) pitch = -80;
        if (pad.buttons & SCE_CTRL_UP) dist -= radius * dt * 1.5f;
        if (pad.buttons & SCE_CTRL_DOWN) dist += radius * dt * 1.5f;
        if (dist < radius * 0.8f) dist = radius * 0.8f;
        if (pressed & SCE_CTRL_CROSS) paused = !paused;
        if (pressed & SCE_CTRL_SQUARE) lighting = !lighting;
        if (pressed & SCE_CTRL_CIRCLE) g_cur = -1;
        if (g_n_anims && (pressed & SCE_CTRL_RTRIGGER)) { g_cur = (g_cur + 1) % g_n_anims; t = 0; }
        if (g_n_anims && (pressed & SCE_CTRL_LTRIGGER)) { g_cur = (g_cur + g_n_anims - 1) % g_n_anims; t = 0; }
        if (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
            if (g_cur >= 0) log_msg(g_anims[g_cur].name);
        if (!paused)
            t += dt;

        const VcaAnim *anim = g_cur >= 0 ? &g_anims[g_cur].anim : NULL;
        vcm_pose(&g_model, anim, t);
        vcm_skin(&g_model);

        /* keep the car in frame during cutscenes: cancel root motion of <Char>_Driver */
        float follow[3] = { target[0], target[1], target[2] };
        if (g_driver_node >= 0) {
            const float *W = g_model.pose_world + g_driver_node * 16;
            const float *B = g_model.nodes[g_driver_node].local;
            follow[0] += W[12] - B[12];
            follow[2] += W[14] - B[14];
        }

        glViewport(0, 0, SCREEN_W, SCREEN_H);
        glClearColor(0.13f, 0.14f, 0.17f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        set_camera(yaw, pitch, dist, follow);
        draw_model(lighting);
        vglSwapBuffers(GL_FALSE);
    }

    for (int i = 0; i < g_n_anims; i++)
        vca_free(&g_anims[i].anim);
    vcm_free(&g_model);
    free(g_uv_tmp);
    free(g_col_tmp);
    sceKernelExitProcess(0);
    return 0;
}
