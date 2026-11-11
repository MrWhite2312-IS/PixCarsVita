/*
 * PixCarsVita - runtime model/animation core (portable C99, no platform deps).
 * Loads the VCM/VCA files produced by tools/vitaconv.py, poses the skeleton
 * and skins vertices on the CPU.
 */
#ifndef PIXCARS_VCM_H
#define PIXCARS_VCM_H

#include <stdint.h>
#include <stddef.h>

#define VCM_NAME_LEN 32

enum { VCM_TEX_RGBA8 = 0, VCM_TEX_DXT1 = 1, VCM_TEX_DXT3 = 2, VCM_TEX_DXT5 = 3 };
enum { VCM_LAYER_ALPHATEST = 1, VCM_LAYER_CLAMP_U = 2, VCM_LAYER_CLAMP_V = 4 };

typedef struct {
    char name[VCM_NAME_LEN];
    int32_t parent;
    float local[16];          /* row-major, v' = v * M, translation in row 3 */
    float inv_bind_world[16];
} VcmNode;

typedef struct {
    uint32_t size;
    const uint8_t *data;
} VcmLevel;

typedef struct {
    uint32_t format, width, height, n_levels;
    VcmLevel *levels;
    uint32_t gpu_handle;      /* filled by the renderer */
} VcmTexture;

typedef struct {
    int32_t tex;
    uint32_t flags, alpha_ref;
    int32_t eye_node;         /* CarsEye layer: node whose position centres the iris */
    float affine[6];          /* bind-space (x,y) -> (u,v) fit; fallback only */
    float center[2];
} VcmLayer;

typedef struct {
    uint32_t n_layers;
    VcmLayer layer[2];
} VcmMaterial;

typedef struct {
    uint32_t material, n_verts, n_idx;
    const float *pos, *nrm, *uv;
    const uint8_t *bone, *weight;
    const uint16_t *strip;
    float *skinned_pos;       /* n_verts*3, written by vcm_skin */
    float *skinned_nrm;       /* n_verts*3 */
} VcmBatch;

typedef struct {
    uint8_t *file;            /* owns all the data the pointers above refer to */
    uint32_t n_nodes, n_tex, n_mat, n_batch;
    VcmNode *nodes;
    VcmTexture *tex;
    VcmMaterial *mat;
    VcmBatch *batch;
    float *pose_world;        /* n_nodes * 16, current posed world matrices */
    float *skin;              /* n_nodes * 16, inv_bind_world * pose_world */
} VcmModel;

typedef struct {
    char name[VCM_NAME_LEN];
    uint32_t flags;           /* 1 rotation, 2 translation */
    const float *rot;         /* n_frames * 4 (x,y,z,w) */
    const float *trans;       /* n_frames * 3 */
    int32_t node;             /* bound node index in the model, -1 if absent */
} VcaTrack;

typedef struct {
    uint8_t *file;
    uint32_t n_frames, n_tracks;
    float fps;
    VcaTrack *tracks;
} VcaAnim;

/* Parse an in-memory file (takes ownership of buf, freed by vcm_free). 0 on success. */
int vcm_load_mem(VcmModel *m, uint8_t *buf, size_t size);
int vca_load_mem(VcaAnim *a, uint8_t *buf, size_t size);
int vcm_load_file(VcmModel *m, const char *path);
int vca_load_file(VcaAnim *a, const char *path);
void vcm_free(VcmModel *m);
void vca_free(VcaAnim *a);

/* Match animation tracks to model nodes by name (case-insensitive). */
void vca_bind(VcaAnim *a, const VcmModel *m);
float vca_duration(const VcaAnim *a);

/* Compute pose_world/skin for time t (seconds, wrapped). anim may be NULL (bind pose). */
void vcm_pose(VcmModel *m, const VcaAnim *a, float t);
/* Skin every batch into skinned_pos/skinned_nrm using the current pose. */
void vcm_skin(VcmModel *m);
/* UV offset that centres a CarsEye layer's iris on its eye node for the current pose.
 * mat_index is the material the layer belongs to (its batches form the eye surface). */
void vcm_eye_offset(const VcmModel *m, uint32_t mat_index, const VcmLayer *l, float out_uv[2]);

void vcm_mat_mul(const float *a, const float *b, float *out);

#endif
