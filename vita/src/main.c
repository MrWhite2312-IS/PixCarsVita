/*
 * PixCarsVita - character viewer (first milestone of the port).
 *
 * Loads a character converted by tools/vitaconv.py from ux0:data/pixcars/<char>/
 * (model.vcm + *.vca listed in anims.txt), plays its animations with CPU skinning
 * and draws it with vitaGL's fixed-function pipeline.
 *
 * Character mode (default):
 *            left stick  orbit camera      D-pad up/down  zoom
 *            L / R       previous / next animation
 *            Cross       pause / resume    Circle        bind pose
 *            Square      toggle lighting
 * Scene mode (Select switches; ux0:data/pixcars/scene/<name>.vcs listed in scenes.txt):
 *            left stick  move / strafe     right stick   look
 *            D-pad up/dn rise / descend    R (hold)      fast
 *            L           next scene
 * Start      quit
 *
 * No game data is embedded in the VPK: the user converts their own copy on PC.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "vcm.h"
#include "scene.h"

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

/* Reserve the C heap before vitaGL takes the remaining memory. */
int _newlib_heap_size_user = 192 * 1024 * 1024;

#define MAX_SCENES 32
#define SCENE_FAR 7000.0f
static VcsScene g_scene;
static int g_scene_loaded;
static char g_scene_names[MAX_SCENES][64];
static int g_n_scenes, g_scene_cur;

static void log_msg(const char *s)
{
    sceClibPrintf("[pixcars] %s\n", s);
}

/* Progress log on the memory card, flushed line by line so a crash leaves a trace. */
static void plog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sceClibPrintf("[pixcars] %s\n", buf);
    FILE *f = fopen("ux0:data/pixcars/log.txt", "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ assets */
static GLuint upload_texture(const VcmTexture *t)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    /* shortcut: level 0 only; vitaGL crashes in gpu_alloc_compressed_texture when compressed
       mip levels are uploaded (two core dumps). Upgrade: build mips the way vitaGL expects. */
    const VcmLevel *lv = &t->levels[0];
    GLsizei w = (GLsizei)t->width, h = (GLsizei)t->height;
    switch (t->format) {
    case VCM_TEX_DXT1:
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, w, h, 0, lv->size, lv->data);
        break;
    case VCM_TEX_DXT3:
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, w, h, 0, lv->size, lv->data);
        break;
    case VCM_TEX_DXT5:
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, w, h, 0, lv->size, lv->data);
        break;
    default:
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv->data);
        break;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
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

/* ------------------------------------------------------------------ scenes */
static void free_scene(void)
{
    if (!g_scene_loaded)
        return;
    for (uint32_t i = 0; i < g_scene.n_tex; i++)
        glDeleteTextures(1, &g_scene.tex[i].gpu_handle);
    for (uint32_t m = 0; m < g_scene.n_mesh; m++)
        for (uint32_t b = 0; b < g_scene.mesh[m].n_batch; b++) {
            VcsBatch *bt = &g_scene.mesh[m].batch[b];
            if (bt->vbo)
                glDeleteBuffers(1, &bt->vbo);
            if (bt->ibo)
                glDeleteBuffers(1, &bt->ibo);
        }
    vcs_free(&g_scene);
    g_scene_loaded = 0;
}

static int load_scene(int index)
{
    char path[256];
    free_scene();
    snprintf(path, sizeof path, DATA_ROOT "scene/%s.vcs", g_scene_names[index]);
    plog("loading scene %s", path);
    int r = vcs_load_file(&g_scene, path);
    if (r) {
        plog("cannot load %s (%d)", path, r);
        return r;
    }
    plog("parsed: %u textures, %u meshes, %u instances", g_scene.n_tex, g_scene.n_mesh, g_scene.n_inst);
    for (uint32_t i = 0; i < g_scene.n_tex; i++) {
        const VcmTexture *t = &g_scene.tex[i];
        plog("tex %u: fmt=%u %ux%u levels=%u", i, t->format, t->width, t->height, t->n_levels);
        g_scene.tex[i].gpu_handle = upload_texture(t);
    }
    plog("textures uploaded");
    /* upload geometry once: pos | uv | rgba are contiguous in the file, strips separate */
    uint32_t bytes = 0;
    for (uint32_t m = 0; m < g_scene.n_mesh; m++)
        for (uint32_t b = 0; b < g_scene.mesh[m].n_batch; b++) {
            VcsBatch *bt = &g_scene.mesh[m].batch[b];
            GLuint ids[2];
            glGenBuffers(2, ids);
            bt->vbo = ids[0];
            bt->ibo = ids[1];
            glBindBuffer(GL_ARRAY_BUFFER, bt->vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizei)(24u * bt->n_verts), bt->pos, GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bt->ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizei)(2u * bt->n_idx), bt->strip, GL_STATIC_DRAW);
            bytes += 24u * bt->n_verts + 2u * bt->n_idx;
        }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    plog("geometry uploaded: %u KB", bytes / 1024);
    g_scene_loaded = 1;
    sceClibPrintf("[pixcars] scene %s: %u meshes, %u instances\n", g_scene_names[index],
                  g_scene.n_mesh, g_scene.n_inst);
    return 0;
}

static void list_scenes(void)
{
    FILE *f = fopen(DATA_ROOT "scene/scenes.txt", "r");
    if (!f)
        return;
    char line[64];
    while (g_n_scenes < MAX_SCENES && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            snprintf(g_scene_names[g_n_scenes++], 64, "%s", line);
    }
    fclose(f);
}

/* View for an eye in game (D3D, left-handed) space looking along yaw/pitch. */
static void scene_camera(const float eye[3], float yaw, float pitch, float fwd_out[3])
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float znear = 1.0f;
    float top = znear * tanf(32.0f * 3.14159265f / 180.0f);
    float right = top * (float)SCREEN_W / (float)SCREEN_H;
    glFrustum(-right, right, -top, top, znear, SCENE_FAR);

    float f[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };
    memcpy(fwd_out, f, sizeof f);
    /* to GL (right-handed) space: mirror Z, then a standard lookAt */
    float e[3] = { eye[0], eye[1], -eye[2] }, g[3] = { f[0], f[1], -f[2] };
    float sx = -g[2], sy = 0.0f, sz = g[0]; /* s = g x up, up = (0,1,0) */
    float sl = sqrtf(sx * sx + sz * sz);
    if (sl < 1e-6f)
        sl = 1e-6f;
    sx /= sl;
    sz /= sl;
    float ux = sy * g[2] - sz * g[1], uy = sz * g[0] - sx * g[2], uz = sx * g[1] - sy * g[0];
    float m[16] = {
        sx, ux, -g[0], 0,
        sy, uy, -g[1], 0,
        sz, uz, -g[2], 0,
        -(sx * e[0] + sy * e[1] + sz * e[2]), -(ux * e[0] + uy * e[1] + uz * e[2]),
        (g[0] * e[0] + g[1] * e[1] + g[2] * e[2]), 1
    };
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(m);
    glScalef(1, 1, -1);
}

static void draw_scene(const float eye[3], const float fwd[3])
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    /* pass 0: opaque, pass 1: blended (lamp glows etc.) over the finished depth buffer */
    for (int pass = 0; pass < 2; pass++)
    for (uint32_t i = 0; i < g_scene.n_inst; i++) {
        const VcsInstance *in = &g_scene.inst[i];
        float d[3] = { in->center[0] - eye[0], in->center[1] - eye[1], in->center[2] - eye[2] };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist - in->radius > SCENE_FAR)
            continue;
        if (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2] < -in->radius)
            continue; /* entirely behind the camera */
        const VcsMesh *me = &g_scene.mesh[in->mesh];
        glPushMatrix();
        glMultMatrixf(in->world);
        for (uint32_t b = 0; b < me->n_batch; b++) {
            const VcsBatch *bt = &me->batch[b];
            const VcmMaterial *mat = &g_scene.mat[bt->material];
            const VcmLayer *l = &mat->layer[0];
            int blended = mat->n_layers && (l->flags & VCM_LAYER_BLEND);
            if (blended != pass)
                continue;
            if (blended) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, (l->flags & VCM_LAYER_ADDITIVE) ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask((l->flags & VCM_LAYER_NOZWRITE) ? GL_FALSE : GL_TRUE);
            }
            if (mat->n_layers && l->tex >= 0) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, g_scene.tex[l->tex].gpu_handle);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                (l->flags & VCM_LAYER_CLAMP_U) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                (l->flags & VCM_LAYER_CLAMP_V) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            } else {
                glDisable(GL_TEXTURE_2D);
            }
            if (mat->n_layers && (l->flags & VCM_LAYER_ALPHATEST)) {
                glEnable(GL_ALPHA_TEST);
                glAlphaFunc(GL_GREATER, l->alpha_ref / 255.0f);
            } else {
                glDisable(GL_ALPHA_TEST);
            }
            glBindBuffer(GL_ARRAY_BUFFER, bt->vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bt->ibo);
            glVertexPointer(3, GL_FLOAT, 0, (const void *)0);
            glTexCoordPointer(2, GL_FLOAT, 0, (const void *)(uintptr_t)(12u * bt->n_verts));
            glColorPointer(4, GL_UNSIGNED_BYTE, 0, (const void *)(uintptr_t)(20u * bt->n_verts));
            glDrawElements(GL_TRIANGLE_STRIP, (GLsizei)bt->n_idx, GL_UNSIGNED_SHORT, (const void *)0);
        }
        glPopMatrix();
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
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

    {
        FILE *f = fopen("ux0:data/pixcars/log.txt", "w"); /* fresh log each run */
        if (f)
            fclose(f);
    }
    plog("start");
    int have_char = load_character("mcq") == 0;
    plog("character: %s", have_char ? "ok" : "missing");
    list_scenes();
    plog("scenes listed: %d", g_n_scenes);
    if (!have_char && g_n_scenes)
        load_scene(0); /* no character: go straight to the scene */
    int mode = have_char ? 0 : 1; /* 0 character, 1 scene */
    if (!have_char && !g_scene_loaded) {
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
    if (!have_char)
        radius = 10.0f;
    float yaw = 35.0f, pitch = 15.0f, dist = radius * 2.6f;
    /* free camera for scenes */
    float cam[3] = { 0, 0, 0 }, cyaw = 0, cpitch = 0, cfwd[3] = { 0, 0, 1 };
    if (g_scene_loaded) {
        memcpy(cam, g_scene.start, sizeof cam);
        cyaw = atan2f(g_scene.start_dir[0], g_scene.start_dir[2]);
        cpitch = asinf(g_scene.start_dir[1]);
    }
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

        if ((pressed & SCE_CTRL_SELECT) && g_n_scenes && have_char) {
            if (!g_scene_loaded && load_scene(g_scene_cur) == 0) {
                memcpy(cam, g_scene.start, sizeof cam);
                cyaw = atan2f(g_scene.start_dir[0], g_scene.start_dir[2]);
                cpitch = asinf(g_scene.start_dir[1]);
            }
            if (g_scene_loaded)
                mode = !mode;
            plog("mode -> %s", mode ? "scene" : "character");
        }

        if (mode == 1) {
            int lx = (int)pad.lx - 128, ly = (int)pad.ly - 128;
            int rx = (int)pad.rx - 128, ry = (int)pad.ry - 128;
            float speed = (pad.buttons & SCE_CTRL_RTRIGGER) ? 2400.0f : 500.0f;
            if (abs(rx) > 20)
                cyaw += rx * dt * 0.018f;
            if (abs(ry) > 20)
                cpitch -= ry * dt * 0.012f;
            if (cpitch > 1.4f)
                cpitch = 1.4f;
            if (cpitch < -1.4f)
                cpitch = -1.4f;
            float fx = sinf(cyaw), fz = cosf(cyaw);
            if (abs(ly) > 20) {
                cam[0] -= fx * ly / 128.0f * speed * dt;
                cam[2] -= fz * ly / 128.0f * speed * dt;
            }
            if (abs(lx) > 20) {
                cam[0] += fz * lx / 128.0f * speed * dt;
                cam[2] -= fx * lx / 128.0f * speed * dt;
            }
            if (pad.buttons & SCE_CTRL_UP)
                cam[1] += speed * 0.5f * dt;
            if (pad.buttons & SCE_CTRL_DOWN)
                cam[1] -= speed * 0.5f * dt;
            if (g_n_scenes > 1 && (pressed & SCE_CTRL_LTRIGGER)) {
                g_scene_cur = (g_scene_cur + 1) % g_n_scenes;
                if (load_scene(g_scene_cur) == 0) {
                    memcpy(cam, g_scene.start, sizeof cam);
                    cyaw = atan2f(g_scene.start_dir[0], g_scene.start_dir[2]);
                    cpitch = asinf(g_scene.start_dir[1]);
                }
            }

            glViewport(0, 0, SCREEN_W, SCREEN_H);
            const float sky[4] = { 0.59f, 0.67f, 0.78f, 1.0f };
            glClearColor(sky[0], sky[1], sky[2], 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_FOG);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, SCENE_FAR * 0.45f);
            glFogf(GL_FOG_END, SCENE_FAR);
            glFogfv(GL_FOG_COLOR, sky);
            scene_camera(cam, cyaw, cpitch, cfwd);
            static int logged_frames;
            if (logged_frames < 2)
                plog("scene frame %d: drawing", logged_frames);
            if (g_scene_loaded)
                draw_scene(cam, cfwd);
            glDisable(GL_FOG);
            vglSwapBuffers(GL_FALSE);
            if (logged_frames < 2)
                plog("scene frame %d: done", logged_frames++);
            continue;
        }

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
    free_scene();
    free(g_uv_tmp);
    free(g_col_tmp);
    sceKernelExitProcess(0);
    return 0;
}
