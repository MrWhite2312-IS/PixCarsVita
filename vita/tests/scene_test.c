/* Host check of the scene loader: prints counts and the world bounding box.
 * Build: gcc -O2 -std=c99 -Ivita/src vita/src/scene.c vita/tests/scene_test.c -o scene_test */
#include <stdio.h>
#include "scene.h"

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    VcsScene s;
    int r = vcs_load_file(&s, argv[1]);
    if (r) {
        printf("load failed %d\n", r);
        return 1;
    }
    unsigned long tris = 0, verts = 0;
    for (uint32_t m = 0; m < s.n_mesh; m++)
        for (uint32_t b = 0; b < s.mesh[m].n_batch; b++) {
            verts += s.mesh[m].batch[b].n_verts;
            tris += s.mesh[m].batch[b].n_idx;
        }
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < s.n_inst; i++)
        for (int c = 0; c < 3; c++) {
            float v = s.inst[i].center[c];
            mn[c] = v < mn[c] ? v : mn[c];
            mx[c] = v > mx[c] ? v : mx[c];
        }
    printf("tex=%u mat=%u mesh=%u inst=%u unique_verts=%lu strip_indices=%lu\n", s.n_tex, s.n_mat,
           s.n_mesh, s.n_inst, verts, tris);
    printf("start=(%.1f %.1f %.1f) bbox=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n", s.start[0], s.start[1],
           s.start[2], mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    vcs_free(&s);
    return 0;
}
