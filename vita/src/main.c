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
 * Scene mode (Select switches; ux0:data/pixcars/scene/<name>.vcs listed in scenes.txt),
 * driving McQueen (default), the game's preset 1 (Gas A, Brake X, EBrake Y, Powerslide RT):
 *            Cross       accelerate        Square / L    brake, then reverse
 *            left stick  steer             Triangle      handbrake
 *            R (hold)    powerslide        Circle        free camera
 *            D-pad down  reset the car     D-pad right   next music track
 * free camera:
 *            left stick  move / strafe     right stick   look
 *            D-pad up/dn rise / descend    R (hold)      fast
 *            L           next scene        Circle        back to driving
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

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "vcm.h"
#include "scene.h"
#include "car.h"
#include "audio.h"
#include "movie.h"

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
static Audio g_audio;
static volatile int g_audio_run, g_audio_next;

/* Mixer thread: one BGM port at 44.1 kHz stereo, fed 1024 frames at a time. */
static int audio_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    static int16_t buf[2 * 1024];
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        return 0;
    while (g_audio_run) {
        if (g_audio_next) {
            g_audio_next = 0;
            audio_next_track(&g_audio);
        }
        audio_mix(&g_audio, buf, 1024);
        sceAudioOutOutput(port, buf);
    }
    sceAudioOutReleasePort(port);
    return 0;
}

static ColWorld g_col;
static Car g_car;
static CarParams g_params;     /* from ux0:data/pixcars/<char>/<char>.phy (McQueen's if absent) */
static int g_idle[8], g_n_idle; /* <char>_beft_idle* animations, played when parked */
/* lean pose strips (PS2 slots 7-12): base, back (accelerating), forward (braking), slow/fast */
static const char *LEAN_NAMES[6] = { "_bef_lean_slow", "_bef_lean_fast", "_bef_lean_bck_slow",
                                     "_bef_lean_bck_fast", "_bef_lean_fwd_slow", "_bef_lean_fwd_fast" };
static int g_lean[6] = { -1, -1, -1, -1, -1, -1 };
/* eye strips (PS2 slots 4-6), blended with the same signals (0x2b1b00) */
static const char *EYE_NAMES[3] = { "_e_eyesdown", "_e_eyesmid", "_e_eyesup" };
static int g_eyes[3] = { -1, -1, -1 };

/* The game poses node groups separately (0x35e140): eyes/lids/brows vs the body. */
static int eye_group(const char *n)
{
    return strstr(n, "Eye_") || strstr(n, "Lid") || strstr(n, "Brow");
}

static void keep_group(VcaAnim *a, int eyes)
{
    for (uint32_t i = 0; i < a->n_tracks; i++)
        if (eye_group(a->tracks[i].name) != eyes)
            a->tracks[i].node = -1;
}
/* wheel nodes, in car.c order FL, FR, RL, RR: <Char>_wheel*_root (steer, suspension) and
   their child <Char>_wheel* (roll) */
static const char *WHEEL_NAMES[4] = { "wheelfrontleft", "wheelfrontright", "wheelrearleft", "wheelrearright" };
static int g_wroot[4] = { -1, -1, -1, -1 }, g_wheel[4] = { -1, -1, -1, -1 };

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
        for (int w = 0; w < 4; w++) {
            size_t k = strlen(WHEEL_NAMES[w]);
            if (n >= k + 5 && strcasecmp(g_model.nodes[i].name + n - k - 5, WHEEL_NAMES[w]) == 0
                && strcasecmp(g_model.nodes[i].name + n - 5, "_root") == 0)
                g_wroot[w] = (int)i;
            else if (n >= k && strcasecmp(g_model.nodes[i].name + n - k, WHEEL_NAMES[w]) == 0)
                g_wheel[w] = (int)i;
        }
    }

    /* the game's physics file for this character */
    snprintf(path, sizeof path, DATA_ROOT "%s/%s.phy", code, code);
    char *phy = NULL;
    FILE *pf = fopen(path, "rb");
    if (pf) {
        fseek(pf, 0, SEEK_END);
        long n = ftell(pf);
        fseek(pf, 0, SEEK_SET);
        phy = malloc((size_t)n + 1);
        if (phy)
            phy[fread(phy, 1, (size_t)n, pf)] = 0;
        fclose(pf);
    }
    car_params_parse(&g_params, phy);
    plog("physics: %s", phy ? path : "built-in McQueen values");
    free(phy);

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
    for (int i = 0; i < g_n_anims; i++) {
        for (int k = 0; k < 6; k++) {
            size_t n = strlen(g_anims[i].name), l = strlen(LEAN_NAMES[k]);
            if (n >= l && strcasecmp(g_anims[i].name + n - l, LEAN_NAMES[k]) == 0) {
                g_lean[k] = i;
                keep_group(&g_anims[i].anim, 0);
            }
        }
        for (int k = 0; k < 3; k++) {
            size_t n = strlen(g_anims[i].name), l = strlen(EYE_NAMES[k]);
            if (n >= l && strcasecmp(g_anims[i].name + n - l, EYE_NAMES[k]) == 0) {
                g_eyes[k] = i;
                keep_group(&g_anims[i].anim, 1);
            }
        }
        if (strstr(g_anims[i].name, "_beft_idle") && g_n_idle < 8)
            g_idle[g_n_idle++] = i;
    }
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
    col_free(&g_col);
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
    r = col_build(&g_col, &g_scene);
    plog("collision: %d, %u triangles, grid %dx%d", r, g_col.n_tri, g_col.nx, g_col.nz);
    car_spawn(&g_car, &g_params, &g_col, g_scene.start, atan2f(g_scene.start_dir[0], g_scene.start_dir[2]));
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

/* Bind pose with the wheels steered, rolled and following the suspension. */
/* Body lean as the PS2 game does it (0x2b1860 / 0x2b1be0 / 0x37fc90): two signals in 0..1,
   each smoothed twice by a 0.1 s filter. lat = (1 + shaped steering) / 2 picks the time in the
   5-pose strips (left .. right); fa = (1 + throttle-brake axis) / 2 blends forward <- base ->
   back; the slow and fast strips cross-fade between 44 and 146.67 ft/s (30 and 100 mph). */
typedef struct { float lat, fa, lat_t, fa_t, axis; } Lean;

static float lean_filter(float v, float target, float dt)
{
    return v + (target - v) * fminf(dt, 0.1f) / 0.1f;
}

static int lean_pose(Lean *ln, const CarInput *in, float speed, float dt,
                     const VcaAnim **a, float *w, float *t)
{
    for (int k = 0; k < 6; k++)
        if (g_lean[k] < 0)
            return 0;
    float x = in->steer, p = g_params.steer_power;
    float shaped = (x < 0 ? -1.0f : 1.0f) * powf(fabsf(x), p);
    float d = (in->throttle - in->brake) - ln->axis, r = g_params.x_rate * dt;
    ln->axis += d > r ? r : (d < -r ? -r : d);
    ln->lat_t = lean_filter(ln->lat_t, (1.0f + shaped) * 0.5f, dt);
    ln->fa_t = lean_filter(ln->fa_t, (1.0f + ln->axis) * 0.5f, dt);
    ln->lat = lean_filter(ln->lat, ln->lat_t, dt);
    ln->fa = lean_filter(ln->fa, ln->fa_t, dt);
    /* per speed set: pair (first weighted by f, second by 1 - f) */
    float f;
    int first[2], second[2]; /* [slow, fast] lean slot indices into g_lean */
    if (ln->fa < 0.5f) {
        f = 2.0f * ln->fa;
        first[0] = 0; first[1] = 1; second[0] = 4; second[1] = 5;
    } else {
        f = 2.0f * (ln->fa - 0.5f);
        first[0] = 2; first[1] = 3; second[0] = 0; second[1] = 1;
    }
    float sp = fabsf(speed), fast = fminf(1.0f, fmaxf(0.0f, (sp - 44.0f) / (146.67f - 44.0f)));
    int n = 0;
    for (int set = 0; set < 2; set++) {
        float sw = set ? fast : 1.0f - fast;
        if (sw <= 0)
            continue;
        const int ids[2] = { g_lean[first[set]], g_lean[second[set]] };
        const float ws[2] = { f, 1.0f - f };
        for (int j = 0; j < 2; j++) {
            a[n] = &g_anims[ids[j]].anim;
            w[n] = ws[j] * sw;
            /* time = lat * duration, kept just inside the strip so it never wraps */
            t[n] = fminf(ln->lat, 0.999f) * vca_duration(a[n]);
            n++;
        }
    }
    if (g_eyes[0] >= 0 && g_eyes[1] >= 0 && g_eyes[2] >= 0) {
        /* eyes: down <- mid -> up by fore/aft, the strip time by the lateral signal */
        int e0 = ln->fa < 0.5f ? g_eyes[1] : g_eyes[2], e1 = ln->fa < 0.5f ? g_eyes[0] : g_eyes[1];
        const int ids[2] = { e0, e1 };
        const float ws[2] = { f, 1.0f - f };
        for (int j = 0; j < 2; j++) {
            a[n] = &g_anims[ids[j]].anim;
            w[n] = ws[j];
            t[n] = fminf(ln->lat, 0.999f) * vca_duration(a[n]);
            n++;
        }
    }
    return n;
}

static void pose_car(const Car *c, const VcaAnim *const *anims, const float *weights,
                     const float *times, int n_anims)
{
    float saved[8][16];
    for (int w = 0; w < 4; w++) {
        if (g_wroot[w] >= 0) {
            float *L = g_model.nodes[g_wroot[w]].local;
            memcpy(saved[w], L, 64);
            float s = w < 2 ? c->steer : 0, cs = cosf(s), sn = sinf(s);
            const float Ry[16] = { cs, 0, -sn, 0, 0, 1, 0, 0, sn, 0, cs, 0, 0, 0, 0, 1 };
            vcm_mat_mul(Ry, saved[w], L);
            L[13] += car_wheel_offset(c, w);
        }
        if (g_wheel[w] >= 0) {
            float *L = g_model.nodes[g_wheel[w]].local;
            memcpy(saved[4 + w], L, 64);
            float cs = cosf(c->spin[w]), sn = sinf(c->spin[w]);
            const float Rx[16] = { 1, 0, 0, 0, 0, cs, sn, 0, 0, -sn, cs, 0, 0, 0, 0, 1 };
            vcm_mat_mul(Rx, saved[4 + w], L);
        }
    }
    vcm_pose_blend(&g_model, anims, weights, times, n_anims);
    vcm_skin(&g_model);
    for (int w = 0; w < 4; w++) {
        if (g_wroot[w] >= 0)
            memcpy(g_model.nodes[g_wroot[w]].local, saved[w], 64);
        if (g_wheel[w] >= 0)
            memcpy(g_model.nodes[g_wheel[w]].local, saved[4 + w], 64);
    }
}

/* Driving sounds from the fx.aud parameters (exported to sfx/sfx.txt). */
typedef struct { int burn, slide; float burn_lv, slide_lv, sus_cool, imp_cool; int brake_armed; } DriveSfx;

static float sfx_loop(int voice, int id, int on, float slip, float *level, float dt)
{
    const Sfx *x = &g_audio.sfx[id];
    float t = (slip - x->slip_min) / (x->slip_max - x->slip_min);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float target = on ? (x->vol_min + (x->vol_max - x->vol_min) * t) / 100.0f : 0.0f;
    float step = dt / (x->fade > 0 ? x->fade : 0.1f); /* FadeDuration */
    *level += fmaxf(-step, fminf(step, target - *level));
    if (voice >= 0) {
        g_audio.voice[voice].vol = *level;
        g_audio.voice[voice].pitch = 1.0f + x->freq_min + (x->freq_max - x->freq_min) * t;
    }
    return *level;
}

static void drive_sounds(DriveSfx *d, const Car *c, const CarInput *in, float dt)
{
    int burn = audio_sfx(&g_audio, "burnout", 0), slide = audio_sfx(&g_audio, "powerslide", 0);
    if (burn >= 0 && d->burn < 0)
        d->burn = audio_play(&g_audio, burn, 0, 1, 1);
    if (slide >= 0 && d->slide < 0)
        d->slide = audio_play(&g_audio, slide, 0, 1, 1);
    if (burn >= 0)
        sfx_loop(d->burn, burn, in->throttle > 0 && c->grounded && c->drive_slip > 0.05f, c->drive_slip, &d->burn_lv, dt);
    if (slide >= 0)
        sfx_loop(d->slide, slide, c->ps == 1 && c->grounded, c->side_slip, &d->slide_lv, dt);
    float speed = fabsf(car_speed(c));
    if (in->brake > 0 && speed > 30.0f && c->gear > 0 && d->brake_armed) {
        audio_play(&g_audio, audio_sfx(&g_audio, "brake", 0), 1, 1, 0);
        d->brake_armed = 0;
    }
    if (in->brake <= 0)
        d->brake_armed = 1;
    /* suspension: the highest velocity tier reached, attenuation by Min/MaxLevelVelocity */
    d->sus_cool -= dt;
    int sus = -1;
    for (int k = 0; audio_sfx(&g_audio, "suspension", k) >= 0; k++) {
        int id = audio_sfx(&g_audio, "suspension", k);
        if (c->strut_hit >= g_audio.sfx[id].slip_min)
            sus = id;
    }
    if (sus >= 0 && d->sus_cool <= 0) {
        const Sfx *x = &g_audio.sfx[sus];
        float t = (c->strut_hit - x->fade) / (x->slip_max - x->fade);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        audio_play(&g_audio, sus, (x->vol_min + (x->vol_max - x->vol_min) * t) / 100.0f, 1, 0);
        d->sus_cool = 0.25f;
    }
    /* wall impacts */
    d->imp_cool -= dt;
    if (c->impact > 5.0f && d->imp_cool <= 0) {
        audio_play(&g_audio, audio_sfx(&g_audio, "impact", rand() % 3), fminf(1.0f, c->impact / 40.0f), 1, 0);
        d->imp_cool = 0.3f;
    }
}

static void stop_drive_sounds(DriveSfx *d)
{
    if (d->burn >= 0)
        g_audio.voice[d->burn].vol = 0;
    if (d->slide >= 0)
        g_audio.voice[d->slide].vol = 0;
    d->burn_lv = d->slide_lv = 0;
}

/* Play one of the game's videos full-screen; Cross or Start skips it. */
static void play_movie(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, DATA_ROOT "movies/%s.mp4", name);
    int r = movie_start(path);
    plog("movie %s: %s (%d)", path, r == 0 ? "playing" : (r == -1 ? "file not found" : "player init failed"), r);
    if (r)
        return;
    uint32_t prev = ~0u;
    while (movie_draw()) {
        vglSwapBuffers(GL_FALSE);
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & ~prev) & (SCE_CTRL_CROSS | SCE_CTRL_START))
            break;
        prev = pad.buttons;
    }
    movie_stop();
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
    plog("boot logos");
    /* the game's VideoLogos sequence (PS2 ELF table: BVGLogo, THQLogo, Rainbow, PixLogo) */
    static const char *LOGOS[] = { "bvglogo", "thqlogo", "rainbow", "pixlogo" };
    for (int i = 0; i < 4; i++)
        play_movie(LOGOS[i]);
    int have_char = load_character("mcq") == 0;
    plog("character: %s", have_char ? "ok" : "missing");
    if (audio_init(&g_audio, DATA_ROOT "audio/") > 0) {
        g_audio.engine_vol = 0; /* engine only while driving */
        g_audio_run = 1;
        SceUID th = sceKernelCreateThread("pixcars_audio", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
        if (th >= 0)
            sceKernelStartThread(th, 0, NULL);
    }
    plog("audio: %d tracks, %d engine loops", g_audio.n_tracks, g_audio.n_loops);
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
    int paused = 0, lighting = 1, driving = have_char;
    float chase[3] = { 0, 0, 0 };
    int chase_init = 0, idle_anim = -1;
    Lean lean = { 0.5f, 0.5f, 0.5f, 0.5f, 0.0f };
    DriveSfx dsfx = { -1, -1, 0, 0, 0, 0, 1 };
    float parked_t = 0, idle_t = 0;
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
            chase_init = 0;
            plog("mode -> %s", mode ? "scene" : "character");
        }

        if (mode == 1 && have_char && (pressed & SCE_CTRL_CIRCLE)) {
            driving = !driving;
            chase_init = 0;
        }
        if (mode == 1 && driving && g_scene_loaded) {
            int lx = (int)pad.lx - 128;
            CarInput in = { 0 };
            in.throttle = (pad.buttons & SCE_CTRL_CROSS) ? 1.0f : 0.0f;
            in.brake = (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_SQUARE)) ? 1.0f : 0.0f;
            in.steer = abs(lx) > 16 ? lx / 128.0f : 0.0f;
            in.handbrake = (pad.buttons & SCE_CTRL_TRIANGLE) != 0;
            in.powerslide = (pad.buttons & SCE_CTRL_RTRIGGER) != 0;
            if (pressed & SCE_CTRL_DOWN) {
                float yaw_now = atan2f(g_car.rot[6], g_car.rot[8]);
                car_spawn(&g_car, &g_params, &g_col, g_car.pos, yaw_now);
            }
            car_step(&g_car, &g_col, &in, dt);
            g_audio.rpm = audio_speed_to_rpm(&g_audio, fabsf(car_speed(&g_car)) * 0.681818f);
            g_audio.engine_vol = 0.8f;
            if (pressed & SCE_CTRL_RIGHT)
                g_audio_next = 1;
            drive_sounds(&dsfx, &g_car, &in, dt);

            /* parked with no input: play one of the game's idle animations, then wait again */
            int active = in.throttle > 0 || in.brake > 0 || in.steer != 0 || fabsf(car_speed(&g_car)) > 0.5f;
            parked_t = active ? 0 : parked_t + dt;
            if (active)
                idle_anim = -1;
            if (idle_anim < 0 && g_n_idle && parked_t > 5.0f) { /* shortcut: 5 s trigger is a guess */
                idle_anim = g_idle[rand() % g_n_idle];
                idle_t = 0;
            }
            const VcaAnim *ia = NULL;
            if (idle_anim >= 0) {
                idle_t += dt;
                if (idle_t >= vca_duration(&g_anims[idle_anim].anim)) {
                    idle_anim = -1;
                    parked_t = 0;
                } else {
                    ia = &g_anims[idle_anim].anim;
                }
            }

            /* chase camera from the .phy: CameraOffset above the car, CameraFollowingDistance behind */
            float M[16];
            car_matrix(&g_car, M);
            const float *co = g_params.cam_offset;
            float fx = g_car.rot[6], fz = g_car.rot[8], fl = sqrtf(fx * fx + fz * fz);
            if (fl < 1e-3f) { fx = 0; fz = 1; fl = 1; }
            float tgt[3] = { M[12] + co[0], M[13] + co[1], M[14] + co[2] };
            float want[3] = { tgt[0] - fx / fl * g_params.cam_distance, tgt[1], tgt[2] - fz / fl * g_params.cam_distance };
            float k = chase_init ? fminf(1.0f, 6.0f * dt) : 1.0f;
            for (int c = 0; c < 3; c++)
                chase[c] += (want[c] - chase[c]) * k;
            chase_init = 1;
            float look[3] = { tgt[0] - chase[0], tgt[1] - chase[1], tgt[2] - chase[2] };
            float ll = sqrtf(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
            memcpy(cam, chase, sizeof cam);
            cyaw = atan2f(look[0], look[2]);
            cpitch = asinf(look[1] / (ll > 1e-3f ? ll : 1.0f));
            const VcaAnim *pa[6];
            float pw[6], pt[6];
            int pn;
            if (ia) {
                pa[0] = ia; pw[0] = 1; pt[0] = idle_t; pn = 1;
            } else {
                pn = lean_pose(&lean, &in, car_speed(&g_car), dt, pa, pw, pt);
            }
            pose_car(&g_car, pa, pw, pt, pn);
        }

        if (mode != 1 || !driving) {
            g_audio.engine_vol = 0;
            stop_drive_sounds(&dsfx);
        }
        if (mode == 1) {
            int lx = (int)pad.lx - 128, ly = (int)pad.ly - 128;
            int rx = (int)pad.rx - 128, ry = (int)pad.ry - 128;
            if (driving)
                lx = ly = rx = ry = 0, pad.buttons &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN), pressed = 0;
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
            if (g_scene_loaded && have_char) {
                float M[16];
                car_matrix(&g_car, M);
                glPushMatrix();
                glMultMatrixf(M);
                draw_model(lighting);
                glPopMatrix();
            }
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

    g_audio_run = 0;
    sceKernelDelayThread(100000); /* let the mixer finish its last buffer */
    for (int i = 0; i < g_n_anims; i++)
        vca_free(&g_anims[i].anim);
    vcm_free(&g_model);
    free_scene();
    free(g_uv_tmp);
    free(g_col_tmp);
    sceKernelExitProcess(0);
    return 0;
}
