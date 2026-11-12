/* PixCarsVita - runtime model/animation core. See vcm.h and tools/vitaconv.py. */
#include "vcm.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *p, *end;
    int err;
} Cursor;

static const void *take(Cursor *c, size_t n)
{
    size_t padded = (n + 3) & ~(size_t)3;
    if (c->err || (size_t)(c->end - c->p) < padded) {
        c->err = 1;
        return NULL;
    }
    void *r = c->p;
    c->p += padded;
    return r;
}

static uint32_t take_u32(Cursor *c)
{
    const uint32_t *v = take(c, 4);
    return v ? *v : 0;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = buf ? (size_t)n : 0;
    return buf;
}

void vcm_mat_mul(const float *a, const float *b, float *out)
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof r);
}

/* ------------------------------------------------------------------ model */
int vcm_load_mem(VcmModel *m, uint8_t *buf, size_t size)
{
    memset(m, 0, sizeof *m);
    m->file = buf;
    Cursor c = { buf, buf + size, 0 };
    const char *magic = take(&c, 4);
    if (!magic || memcmp(magic, "VCM1", 4) != 0)
        return -1;
    m->n_nodes = take_u32(&c);
    m->n_tex = take_u32(&c);
    m->n_mat = take_u32(&c);
    m->n_batch = take_u32(&c);
    if (c.err || m->n_nodes > 255)
        return -2;
    m->nodes = (VcmNode *)take(&c, sizeof(VcmNode) * m->n_nodes);
    m->tex = calloc(m->n_tex ? m->n_tex : 1, sizeof(VcmTexture));
    for (uint32_t i = 0; i < m->n_tex && !c.err; i++) {
        VcmTexture *t = &m->tex[i];
        t->format = take_u32(&c);
        t->width = take_u32(&c);
        t->height = take_u32(&c);
        t->n_levels = take_u32(&c);
        if (t->n_levels > 16) {
            c.err = 1;
            break;
        }
        t->levels = calloc(t->n_levels ? t->n_levels : 1, sizeof(VcmLevel));
        for (uint32_t l = 0; l < t->n_levels && !c.err; l++) {
            t->levels[l].size = take_u32(&c);
            t->levels[l].data = take(&c, t->levels[l].size);
        }
    }
    m->mat = (VcmMaterial *)take(&c, sizeof(VcmMaterial) * m->n_mat);
    m->batch = calloc(m->n_batch ? m->n_batch : 1, sizeof(VcmBatch));
    for (uint32_t i = 0; i < m->n_batch && !c.err; i++) {
        VcmBatch *b = &m->batch[i];
        b->material = take_u32(&c);
        b->n_verts = take_u32(&c);
        b->n_idx = take_u32(&c);
        if (b->material >= m->n_mat) {
            c.err = 1;
            break;
        }
        b->pos = take(&c, 12u * b->n_verts);
        b->nrm = take(&c, 12u * b->n_verts);
        b->uv = take(&c, 8u * b->n_verts);
        b->bone = take(&c, 4u * b->n_verts);
        b->weight = take(&c, 4u * b->n_verts);
        b->strip = take(&c, 2u * b->n_idx);
        b->skinned_pos = malloc(12u * (b->n_verts ? b->n_verts : 1));
        b->skinned_nrm = malloc(12u * (b->n_verts ? b->n_verts : 1));
    }
    if (c.err)
        return -3;
    m->pose_world = malloc(64u * m->n_nodes);
    m->skin = malloc(64u * m->n_nodes);
    vcm_pose(m, NULL, 0.0f);
    return 0;
}

int vcm_load_file(VcmModel *m, const char *path)
{
    size_t n;
    uint8_t *buf = read_file(path, &n);
    if (!buf)
        return -10;
    int r = vcm_load_mem(m, buf, n);
    if (r)
        vcm_free(m);
    return r;
}

void vcm_free(VcmModel *m)
{
    if (m->tex)
        for (uint32_t i = 0; i < m->n_tex; i++)
            free(m->tex[i].levels);
    if (m->batch)
        for (uint32_t i = 0; i < m->n_batch; i++) {
            free(m->batch[i].skinned_pos);
            free(m->batch[i].skinned_nrm);
        }
    free(m->tex);
    free(m->batch);
    free(m->pose_world);
    free(m->skin);
    free(m->file);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ animation */
int vca_load_mem(VcaAnim *a, uint8_t *buf, size_t size)
{
    memset(a, 0, sizeof *a);
    a->file = buf;
    Cursor c = { buf, buf + size, 0 };
    const char *magic = take(&c, 4);
    if (!magic || memcmp(magic, "VCA1", 4) != 0)
        return -1;
    a->n_frames = take_u32(&c);
    const float *fps = take(&c, 4);
    a->fps = fps ? *fps : 30.0f;
    a->n_tracks = take_u32(&c);
    if (c.err || a->n_frames == 0 || a->n_tracks > 1024)
        return -2;
    a->tracks = calloc(a->n_tracks ? a->n_tracks : 1, sizeof(VcaTrack));
    for (uint32_t i = 0; i < a->n_tracks && !c.err; i++) {
        VcaTrack *t = &a->tracks[i];
        const char *nm = take(&c, VCM_NAME_LEN);
        if (nm)
            memcpy(t->name, nm, VCM_NAME_LEN);
        t->name[VCM_NAME_LEN - 1] = 0;
        t->flags = take_u32(&c);
        t->node = -1;
        if (t->flags & 1)
            t->rot = take(&c, 16u * a->n_frames);
        if (t->flags & 2)
            t->trans = take(&c, 12u * a->n_frames);
    }
    return c.err ? -3 : 0;
}

int vca_load_file(VcaAnim *a, const char *path)
{
    size_t n;
    uint8_t *buf = read_file(path, &n);
    if (!buf)
        return -10;
    int r = vca_load_mem(a, buf, n);
    if (r)
        vca_free(a);
    return r;
}

void vca_free(VcaAnim *a)
{
    free(a->tracks);
    free(a->file);
    memset(a, 0, sizeof *a);
}

static int name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

void vca_bind(VcaAnim *a, const VcmModel *m)
{
    for (uint32_t i = 0; i < a->n_tracks; i++) {
        a->tracks[i].node = -1;
        for (uint32_t n = 0; n < m->n_nodes; n++)
            if (name_eq(a->tracks[i].name, m->nodes[n].name)) {
                a->tracks[i].node = (int32_t)n;
                break;
            }
    }
}

float vca_duration(const VcaAnim *a)
{
    return a->n_frames > 1 ? (float)(a->n_frames - 1) / a->fps : 0.0f;
}

static void quat_to_rows(const float q_in[4], float *m)
{
    float x = q_in[0], y = q_in[1], z = q_in[2], w = q_in[3];
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n > 0) {
        x /= n; y /= n; z /= n; w /= n;
    }
    /* row-vector convention (same as tools/pose.py quat_to_mat) */
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y);
}

void vcm_pose(VcmModel *m, const VcaAnim *a, float t)
{
    float w = 1.0f;
    vcm_pose_blend(m, &a, &w, &t, a ? 1 : 0);
}

void vcm_pose_blend(VcmModel *m, const VcaAnim *const *anims, const float *weights,
                    const float *times, int n)
{
    float local[256][16];
    float qs[256][4], ps[256][3], wq[256], wp[256];
    for (uint32_t i = 0; i < m->n_nodes; i++) {
        memcpy(local[i], m->nodes[i].local, 64);
        wq[i] = wp[i] = 0;
    }
    for (int ai = 0; ai < n; ai++) {
        const VcaAnim *a = anims[ai];
        float wgt = weights[ai], t = times[ai];
        if (!a || !a->n_frames || wgt <= 0)
            continue;
        float dur = vca_duration(a);
        if (dur > 0) {
            t = fmodf(t, dur);
            if (t < 0)
                t += dur;
        } else {
            t = 0;
        }
        float fpos = t * a->fps;
        uint32_t f0 = (uint32_t)fpos;
        if (f0 >= a->n_frames - 1)
            f0 = a->n_frames - 1;
        uint32_t f1 = f0 + 1 < a->n_frames ? f0 + 1 : f0;
        float k = fpos - (float)f0;
        if (k < 0) k = 0;
        if (k > 1) k = 1;
        for (uint32_t i = 0; i < a->n_tracks; i++) {
            const VcaTrack *tr = &a->tracks[i];
            int nd = tr->node;
            if (nd < 0)
                continue;
            if (tr->rot) {
                const float *q0 = tr->rot + f0 * 4, *q1 = tr->rot + f1 * 4;
                float q[4];
                for (int c = 0; c < 4; c++)
                    q[c] = q0[c] + (q1[c] - q0[c]) * k; /* nlerp; hemisphere fixed offline */
                if (wq[nd] > 0 && q[0] * qs[nd][0] + q[1] * qs[nd][1] + q[2] * qs[nd][2] + q[3] * qs[nd][3] < 0)
                    for (int c = 0; c < 4; c++)
                        q[c] = -q[c];
                for (int c = 0; c < 4; c++)
                    qs[nd][c] = (wq[nd] > 0 ? qs[nd][c] : 0) + q[c] * wgt;
                wq[nd] += wgt;
            }
            if (tr->trans) {
                const float *p0 = tr->trans + f0 * 3, *p1 = tr->trans + f1 * 3;
                for (int c = 0; c < 3; c++)
                    ps[nd][c] = (wp[nd] > 0 ? ps[nd][c] : 0) + (p0[c] + (p1[c] - p0[c]) * k) * wgt;
                wp[nd] += wgt;
            }
        }
    }
    for (uint32_t i = 0; i < m->n_nodes; i++) {
        if (wq[i] > 0)
            quat_to_rows(qs[i], local[i]); /* absolute local rotation (normalised inside) */
        if (wp[i] > 0)
            for (int c = 0; c < 3; c++)
                local[i][12 + c] = ps[i][c] / wp[i];
    }
    /* nodes are stored parents-first by the exporter? not guaranteed: resolve by recursion */
    uint8_t done[256] = { 0 };
    for (uint32_t pass = 0; pass < m->n_nodes; pass++) {
        int progress = 0;
        for (uint32_t i = 0; i < m->n_nodes; i++) {
            if (done[i])
                continue;
            int32_t p = m->nodes[i].parent;
            if (p < 0 || p == (int32_t)i || p >= (int32_t)m->n_nodes) {
                memcpy(m->pose_world + i * 16, local[i], 64);
            } else if (done[p]) {
                vcm_mat_mul(local[i], m->pose_world + p * 16, m->pose_world + i * 16);
            } else {
                continue;
            }
            done[i] = 1;
            progress = 1;
        }
        if (!progress)
            break;
    }
    for (uint32_t i = 0; i < m->n_nodes; i++)
        vcm_mat_mul(m->nodes[i].inv_bind_world, m->pose_world + i * 16, m->skin + i * 16);
}

void vcm_skin(VcmModel *m)
{
    for (uint32_t bi = 0; bi < m->n_batch; bi++) {
        VcmBatch *b = &m->batch[bi];
        for (uint32_t v = 0; v < b->n_verts; v++) {
            const float *p = b->pos + v * 3, *n = b->nrm + v * 3;
            float op[3] = { 0, 0, 0 }, on[3] = { 0, 0, 0 };
            for (int j = 0; j < 4; j++) {
                uint8_t wq = b->weight[v * 4 + j];
                if (!wq)
                    continue;
                float w = wq * (1.0f / 255.0f);
                const float *M = m->skin + b->bone[v * 4 + j] * 16;
                op[0] += w * (p[0] * M[0] + p[1] * M[4] + p[2] * M[8] + M[12]);
                op[1] += w * (p[0] * M[1] + p[1] * M[5] + p[2] * M[9] + M[13]);
                op[2] += w * (p[0] * M[2] + p[1] * M[6] + p[2] * M[10] + M[14]);
                on[0] += w * (n[0] * M[0] + n[1] * M[4] + n[2] * M[8]);
                on[1] += w * (n[0] * M[1] + n[1] * M[5] + n[2] * M[9]);
                on[2] += w * (n[0] * M[2] + n[1] * M[6] + n[2] * M[10]);
            }
            memcpy(b->skinned_pos + v * 3, op, 12);
            memcpy(b->skinned_nrm + v * 3, on, 12);
        }
    }
}

/* UV of the batch surface under (x, y), projecting along Z in bind space. */
static int surface_uv(const VcmBatch *b, float x, float y, float *u, float *v)
{
    for (uint32_t i = 0; i + 2 < b->n_idx; i++) {
        uint16_t ia = b->strip[i], ib = b->strip[i + 1], ic = b->strip[i + 2];
        if (ia == ib || ib == ic || ia == ic)
            continue;
        const float *A = b->pos + ia * 3, *B = b->pos + ib * 3, *C = b->pos + ic * 3;
        float area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
        if (fabsf(area) < 1e-12f)
            continue;
        float w0 = ((B[0] - x) * (C[1] - y) - (B[1] - y) * (C[0] - x)) / area;
        float w1 = ((C[0] - x) * (A[1] - y) - (C[1] - y) * (A[0] - x)) / area;
        float w2 = 1.0f - w0 - w1;
        if (w0 >= -1e-6f && w1 >= -1e-6f && w2 >= -1e-6f) {
            *u = w0 * b->uv[ia * 2] + w1 * b->uv[ib * 2] + w2 * b->uv[ic * 2];
            *v = w0 * b->uv[ia * 2 + 1] + w1 * b->uv[ib * 2 + 1] + w2 * b->uv[ic * 2 + 1];
            return 1;
        }
    }
    return 0;
}

void vcm_eye_offset(const VcmModel *m, uint32_t mat_index, const VcmLayer *l, float out_uv[2])
{
    out_uv[0] = out_uv[1] = 0.0f;
    if (l->eye_node < 0 || l->eye_node >= (int32_t)m->n_nodes)
        return;
    const float *W = m->pose_world + l->eye_node * 16;
    int32_t par = m->nodes[l->eye_node].parent;
    float x = W[12], y = W[13], z = W[14];
    if (par >= 0 && par < (int32_t)m->n_nodes) {
        /* back into the bind space of the eye surface (skinned to the parent):
           p_bind = p_posed * inverse(skin[parent]); skin matrices are rigid */
        const float *S = m->skin + par * 16;
        float d[3] = { x - S[12], y - S[13], z - S[14] };
        x = d[0] * S[0] + d[1] * S[1] + d[2] * S[2];
        y = d[0] * S[4] + d[1] * S[5] + d[2] * S[6];
    }
    float u = 0, v = 0;
    int hit = 0;
    for (uint32_t i = 0; i < m->n_batch && !hit; i++)
        if (m->batch[i].material == mat_index)
            hit = surface_uv(&m->batch[i], x, y, &u, &v);
    if (!hit) { /* gaze outside the surface: linear extrapolation */
        u = l->affine[0] * x + l->affine[1] * y + l->affine[2];
        v = l->affine[3] * x + l->affine[4] * y + l->affine[5];
    }
    out_uv[0] = l->center[0] - u;
    out_uv[1] = l->center[1] - v;
}
