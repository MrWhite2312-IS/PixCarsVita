/*
 * PixCarsVita - static scene loader (portable C99). Reads VCS1 files written by
 * tools/vitascene.py: shared textures/materials (same layout as VCM), meshes with
 * baked vertex colours, and placed instances with bounding spheres.
 */
#ifndef PIXCARS_SCENE_H
#define PIXCARS_SCENE_H

#include "vcm.h"

typedef struct {
    uint32_t material, n_verts, n_idx;
    const float *pos;          /* n_verts*3 */
    const float *uv;           /* n_verts*2 */
    const uint8_t *rgba;       /* n_verts*4, baked lighting */
    const uint16_t *strip;
    uint32_t vbo, ibo;         /* GPU buffers, filled by the renderer (0 = none) */
} VcsBatch;

typedef struct {
    float center[3], radius;   /* local bounding sphere */
    uint32_t n_batch;
    VcsBatch *batch;
} VcsMesh;

typedef struct {
    uint32_t mesh;
    float world[16];           /* row-major, v' = v * M (pass straight to glMultMatrixf) */
    float center[3], radius;   /* world bounding sphere */
} VcsInstance;

typedef struct {
    uint8_t *file;
    uint32_t n_tex, n_mat, n_mesh, n_inst;
    float start[3], start_dir[3];
    VcmTexture *tex;
    VcmMaterial *mat;
    VcsMesh *mesh;
    VcsInstance *inst;
} VcsScene;

int vcs_load_mem(VcsScene *s, uint8_t *buf, size_t size);
int vcs_load_file(VcsScene *s, const char *path);
void vcs_free(VcsScene *s);

#endif
