/* Host-side check of the portable core: load a VCM/VCA pair, pose at time t,
 * skin, and print every skinned vertex so it can be compared with tools/pose.py.
 * Build: gcc -O2 -std=c99 -Ivita/src vita/src/vcm.c vita/tests/host_test.c -lm -o host_test
 * Usage: host_test model.vcm [anim.vca time]
 */
#include <stdio.h>
#include <stdlib.h>
#include "vcm.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s model.vcm [anim.vca time]\n", argv[0]);
        return 1;
    }
    VcmModel m;
    int r = vcm_load_file(&m, argv[1]);
    if (r) {
        fprintf(stderr, "vcm_load_file failed: %d\n", r);
        return 1;
    }
    VcaAnim a;
    const VcaAnim *ap = NULL;
    float t = 0;
    if (argc >= 4) {
        r = vca_load_file(&a, argv[2]);
        if (r) {
            fprintf(stderr, "vca_load_file failed: %d\n", r);
            return 1;
        }
        vca_bind(&a, &m);
        ap = &a;
        t = (float)atof(argv[3]);
    }
    fprintf(stderr, "nodes=%u tex=%u mat=%u batches=%u", m.n_nodes, m.n_tex, m.n_mat, m.n_batch);
    if (ap) {
        int bound = 0;
        for (uint32_t i = 0; i < a.n_tracks; i++)
            bound += a.tracks[i].node >= 0;
        fprintf(stderr, " anim frames=%u tracks=%u bound=%d", a.n_frames, a.n_tracks, bound);
    }
    fprintf(stderr, "\n");
    vcm_pose(&m, ap, t);
    vcm_skin(&m);
    for (uint32_t b = 0; b < m.n_batch; b++)
        for (uint32_t v = 0; v < m.batch[b].n_verts; v++) {
            const float *p = m.batch[b].skinned_pos + v * 3;
            printf("%u %u %.5f %.5f %.5f\n", b, v, p[0], p[1], p[2]);
        }
    for (uint32_t i = 0; i < m.n_mat; i++)
        for (uint32_t l = 0; l < m.mat[i].n_layers; l++)
            if (m.mat[i].layer[l].eye_node >= 0) {
                float uv[2];
                vcm_eye_offset(&m, i, &m.mat[i].layer[l], uv);
                fprintf(stderr, "eye mat %u layer %u node %d offset %.4f %.4f\n", i, l,
                        m.mat[i].layer[l].eye_node, uv[0], uv[1]);
            }
    if (ap)
        vca_free(&a);
    vcm_free(&m);
    return 0;
}
