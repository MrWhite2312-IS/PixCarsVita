/*
 * PixCarsVita - collision grid and rigid-body car. See car.h.
 */
#include "car.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ vectors */
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float o[3])
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static void madd(float o[3], const float a[3], float s) { o[0] += a[0] * s; o[1] += a[1] * s; o[2] += a[2] * s; }
static void sub3(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float norm3(float v[3])
{
    float l = sqrtf(dot3(v, v));
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    return l;
}

/* ------------------------------------------------------------------ collision grid */
#define MAX_CELLS (1u << 20)
#define MAX_TRI_CELLS 16384 /* shortcut: bigger triangles (sky dome) are left out of the grid;
                               raise if some huge ground triangle turns out to be missing */

static void cell_range(const ColWorld *w, float xa, float xb, float za, float zb, int r[4])
{
    r[0] = (int)floorf((xa - w->x0) / w->cell);
    r[1] = (int)floorf((xb - w->x0) / w->cell);
    r[2] = (int)floorf((za - w->z0) / w->cell);
    r[3] = (int)floorf((zb - w->z0) / w->cell);
    if (r[0] < 0) r[0] = 0;
    if (r[2] < 0) r[2] = 0;
    if (r[1] >= w->nx) r[1] = w->nx - 1;
    if (r[3] >= w->nz) r[3] = w->nz - 1;
}

static void tri_range(const ColWorld *w, const float *t, int r[4])
{
    float xa = t[0], xb = t[0], za = t[2], zb = t[2];
    for (int k = 1; k < 3; k++) {
        xa = fminf(xa, t[k * 3]); xb = fmaxf(xb, t[k * 3]);
        za = fminf(za, t[k * 3 + 2]); zb = fmaxf(zb, t[k * 3 + 2]);
    }
    cell_range(w, xa, xb, za, zb, r);
}

int col_build_tris(ColWorld *w, const float *tris, uint32_t n_tri)
{
    memset(w, 0, sizeof *w);
    if (!n_tri)
        return -1;
    w->tri = malloc(36u * n_tri);
    if (!w->tri)
        return -2;
    memcpy(w->tri, tris, 36u * n_tri);
    w->n_tri = n_tri;
    float xa = 1e30f, xb = -1e30f, za = 1e30f, zb = -1e30f;
    for (uint32_t i = 0; i < n_tri * 3; i++) {
        xa = fminf(xa, tris[i * 3]); xb = fmaxf(xb, tris[i * 3]);
        za = fminf(za, tris[i * 3 + 2]); zb = fmaxf(zb, tris[i * 3 + 2]);
    }
    w->x0 = xa;
    w->z0 = za;
    w->cell = 24.0f;
    while (((xb - xa) / w->cell + 1) * ((zb - za) / w->cell + 1) > MAX_CELLS)
        w->cell *= 1.5f;
    w->nx = (int)((xb - xa) / w->cell) + 1;
    w->nz = (int)((zb - za) / w->cell) + 1;
    uint32_t ncell = (uint32_t)(w->nx * w->nz);
    w->cell_start = calloc(ncell + 1, 4);
    if (!w->cell_start)
        return -2;
    /* two passes: count per cell, then fill */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n_tri; i++) {
            int r[4];
            tri_range(w, w->tri + i * 9, r);
            if ((r[1] - r[0] + 1) * (r[3] - r[2] + 1) > MAX_TRI_CELLS)
                continue;
            for (int z = r[2]; z <= r[3]; z++)
                for (int x = r[0]; x <= r[1]; x++) {
                    uint32_t c = (uint32_t)(z * w->nx + x);
                    if (pass == 0)
                        w->cell_start[c + 1]++;
                    else
                        w->items[w->cell_start[c]++] = i;
                }
        }
        if (pass == 0) {
            for (uint32_t c = 0; c < ncell; c++)
                w->cell_start[c + 1] += w->cell_start[c];
            w->items = malloc(4u * (w->cell_start[ncell] + 1));
            if (!w->items)
                return -2;
        } else {
            /* the fill advanced each start to the next cell's start: shift back */
            memmove(w->cell_start + 1, w->cell_start, 4u * ncell);
            w->cell_start[0] = 0;
        }
    }
    return 0;
}

int col_build(ColWorld *w, const VcsScene *s)
{
    uint32_t cap = 0, n = 0;
    for (uint32_t i = 0; i < s->n_inst; i++) {
        const VcsMesh *m = &s->mesh[s->inst[i].mesh];
        for (uint32_t b = 0; b < m->n_batch; b++)
            cap += m->batch[b].n_idx;
    }
    float *t = malloc(36u * (cap ? cap : 1));
    if (!t)
        return -2;
    for (uint32_t i = 0; i < s->n_inst; i++) {
        const VcsInstance *in = &s->inst[i];
        const float *M = in->world;
        const VcsMesh *m = &s->mesh[in->mesh];
        for (uint32_t b = 0; b < m->n_batch; b++) {
            const VcsBatch *bt = &m->batch[b];
            const VcmMaterial *mat = &s->mat[bt->material];
            if (mat->n_layers && (mat->layer[0].flags & (VCM_LAYER_ALPHATEST | VCM_LAYER_BLEND)))
                continue;
            for (uint32_t k = 2; k < bt->n_idx; k++) {
                uint16_t id[3] = { bt->strip[k - 2], bt->strip[k - 1], bt->strip[k] };
                if (id[0] == id[1] || id[1] == id[2] || id[0] == id[2])
                    continue;
                float *o = t + n * 9;
                for (int v = 0; v < 3; v++) {
                    const float *p = bt->pos + id[v] * 3;
                    for (int c = 0; c < 3; c++)
                        o[v * 3 + c] = p[0] * M[c] + p[1] * M[4 + c] + p[2] * M[8 + c] + M[12 + c];
                }
                float e1[3], e2[3], nn[3];
                sub3(o + 3, o, e1);
                sub3(o + 6, o, e2);
                cross3(e1, e2, nn);
                if (dot3(nn, nn) > 1e-8f)
                    n++;
            }
        }
    }
    int r = col_build_tris(w, t, n);
    free(t);
    return r;
}

void col_free(ColWorld *w)
{
    free(w->tri);
    free(w->cell_start);
    free(w->items);
    memset(w, 0, sizeof *w);
}

/* Moller-Trumbore, both faces */
static int ray_tri(const float o[3], const float d[3], const float *v, float *t)
{
    float e1[3], e2[3], p[3], q[3], s[3];
    sub3(v + 3, v, e1);
    sub3(v + 6, v, e2);
    cross3(d, e2, p);
    float det = dot3(e1, p);
    if (fabsf(det) < 1e-9f)
        return 0;
    float inv = 1.0f / det;
    sub3(o, v, s);
    float u = dot3(s, p) * inv;
    if (u < 0 || u > 1)
        return 0;
    cross3(s, e1, q);
    float vv = dot3(d, q) * inv;
    if (vv < 0 || u + vv > 1)
        return 0;
    *t = dot3(e2, q) * inv;
    return 1;
}

int col_raycast(const ColWorld *w, const float o[3], const float d[3], float maxt, float *tout, float n[3])
{
    if (!w->n_tri)
        return 0;
    float ex = o[0] + d[0] * maxt, ez = o[2] + d[2] * maxt;
    int r[4];
    cell_range(w, fminf(o[0], ex), fmaxf(o[0], ex), fminf(o[2], ez), fmaxf(o[2], ez), r);
    float best = maxt;
    int hit = -1;
    for (int z = r[2]; z <= r[3]; z++)
        for (int x = r[0]; x <= r[1]; x++) {
            uint32_t c = (uint32_t)(z * w->nx + x);
            for (uint32_t k = w->cell_start[c]; k < w->cell_start[c + 1]; k++) {
                float t;
                uint32_t i = w->items[k];
                if (ray_tri(o, d, w->tri + i * 9, &t) && t >= 0 && t <= best) {
                    best = t;
                    hit = (int)i;
                }
            }
        }
    if (hit < 0)
        return 0;
    const float *v = w->tri + hit * 9;
    float e1[3], e2[3];
    sub3(v + 3, v, e1);
    sub3(v + 6, v, e2);
    cross3(e1, e2, n);
    norm3(n);
    if (dot3(n, d) > 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
    *tout = best;
    return 1;
}

/* closest point on triangle (Ericson, Real-Time Collision Detection 5.1.5) */
static void closest_on_tri(const float p[3], const float *v, float out[3])
{
    const float *a = v, *b = v + 3, *c = v + 6;
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    sub3(b, a, ab); sub3(c, a, ac); sub3(p, a, ap);
    float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(out, a, 12); return; }
    sub3(p, b, bp);
    float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(out, b, 12); return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float t = d1 / (d1 - d3);
        memcpy(out, a, 12); madd(out, ab, t); return;
    }
    sub3(p, c, cp);
    float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(out, c, 12); return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float t = d2 / (d2 - d6);
        memcpy(out, a, 12); madd(out, ac, t); return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float bc[3], t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        sub3(c, b, bc);
        memcpy(out, b, 12); madd(out, bc, t); return;
    }
    float den = 1.0f / (va + vb + vc), sv = vb * den, sw = vc * den;
    memcpy(out, a, 12); madd(out, ab, sv); madd(out, ac, sw);
}

/* Deepest push-out of a sphere from the scene: returns depth (0 = free) and the normal. */
static float sphere_push(const ColWorld *w, const float p[3], float rad, float n[3])
{
    int r[4];
    cell_range(w, p[0] - rad, p[0] + rad, p[2] - rad, p[2] + rad, r);
    float best = 0;
    for (int z = r[2]; z <= r[3]; z++)
        for (int x = r[0]; x <= r[1]; x++) {
            uint32_t c = (uint32_t)(z * w->nx + x);
            for (uint32_t k = w->cell_start[c]; k < w->cell_start[c + 1]; k++) {
                float q[3], d[3];
                closest_on_tri(p, w->tri + w->items[k] * 9, q);
                sub3(p, q, d);
                float l2 = dot3(d, d);
                if (l2 >= rad * rad || l2 < 1e-10f)
                    continue;
                float l = sqrtf(l2), depth = rad - l;
                if (depth > best) {
                    best = depth;
                    n[0] = d[0] / l; n[1] = d[1] / l; n[2] = d[2] / l;
                }
            }
        }
    return best;
}

/* ------------------------------------------------------------------ car */
#define GRAVITY 48.0f        /* ft/s^2, a bit above 32 for an arcade feel */
#define COM_Y 1.2f           /* centre of mass above the model origin */
#define WHEEL_R 1.48f
#define REST 0.9f            /* suspension travel from mount to wheel centre, unloaded */
#define SAG 0.3f             /* static compression */
#define MOUNT_Y (0.96f - COM_Y + REST - SAG) /* wheel centre sits at bind height at rest */
#define K_SPRING (GRAVITY / (4.0f * SAG))
#define C_DAMP (0.45f * 2.0f * sqrtf(K_SPRING * 0.25f)) /* 0.45 of critical per wheel */
#define ENGINE 34.0f         /* ft/s^2 at full throttle */
#define BRAKE 60.0f
#define DRAG 0.0018f         /* top speed ~ sqrt(ENGINE/DRAG) ~ 137 ft/s (93 mph) */
#define MU 1.35f
#define GRIP 0.3f            /* share of slip cancelled per substep */
#define MAX_STEER 0.55f

static const float WX[4] = { -2.43f, 2.43f, -2.69f, 2.69f };
static const float WZ[4] = { 4.55f, 4.55f, -3.9f, -3.9f };
static const float INERTIA[3] = { 42.0f, 23.0f, 14.0f }; /* roll/yaw/pitch axes padded for stability */
static const float PROBE[5][3] = { { 0, 0.5f, 4.6f }, { 0, 0.5f, -4.6f }, { 0, 0.5f, 0 },
                                   { -1.9f, 0.5f, 0 }, { 1.9f, 0.5f, 0 } };
#define PROBE_R 1.6f

static void to_world(const Car *c, const float l[3], float o[3])
{
    for (int k = 0; k < 3; k++)
        o[k] = c->pos[k] + l[0] * c->rot[k] + l[1] * c->rot[3 + k] + l[2] * c->rot[6 + k];
}

void car_spawn(Car *c, const ColWorld *w, const float pos[3], float yaw)
{
    /* pos may alias c->pos (reset in place): read it before clearing the car; start the ray just
       above so a reset under a bridge lands on the road, not on top */
    float o[3] = { pos[0], pos[1] + 10.0f, pos[2] }, d[3] = { 0, -1, 0 }, t, n[3];
    memset(c, 0, sizeof *c);
    c->pos[0] = o[0];
    c->pos[2] = o[2];
    c->pos[1] = col_raycast(w, o, d, 4000.0f, &t, n) ? o[1] - t + 2.5f : o[1] - 10.0f;
    float s = sinf(yaw), co = cosf(yaw);
    float rot[9] = { co, 0, -s, 0, 1, 0, s, 0, co };
    memcpy(c->rot, rot, sizeof rot);
}

float car_speed(const Car *c) { return dot3(c->vel, c->rot + 6); }

static void substep(Car *c, const ColWorld *w, const CarInput *in, float h)
{
    float *r = c->rot, *u = c->rot + 3, *f = c->rot + 6;
    float F[3] = { 0, -GRAVITY, 0 }, T[3] = { 0, 0, 0 };
    float fwd_speed = dot3(c->vel, f);
    int grounded = 0;
    for (int i = 0; i < 4; i++) {
        float lm[3] = { WX[i], MOUNT_Y, WZ[i] }, p[3], d[3] = { -u[0], -u[1], -u[2] }, t, n[3];
        to_world(c, lm, p);
        float len = REST + WHEEL_R;
        if (!col_raycast(w, p, d, len, &t, n)) {
            c->comp[i] = 0;
            continue;
        }
        grounded++;
        float comp = len - t, vcomp = (comp - c->comp[i]) / h;
        c->comp[i] = comp;
        float fs = K_SPRING * comp + C_DAMP * vcomp;
        if (fs < 0)
            fs = 0;
        float cp[3] = { p[0] + d[0] * t, p[1] + d[1] * t, p[2] + d[2] * t }, arm[3], tq[3], fv[3];
        /* suspension along the ground normal (along body up it would push a pitched car), at the mount */
        sub3(p, c->pos, arm);
        fv[0] = n[0] * fs; fv[1] = n[1] * fs; fv[2] = n[2] * fs;
        madd(F, fv, 1);
        cross3(arm, fv, tq);
        madd(T, tq, 1);

        /* tyre frame on the ground plane */
        float s = i < 2 ? c->steer : 0, cs = cosf(s), sn = sinf(s);
        float wf[3], ws[3];
        for (int k = 0; k < 3; k++) {
            wf[k] = f[k] * cs + r[k] * sn;
            ws[k] = r[k] * cs - f[k] * sn;
        }
        madd(wf, n, -dot3(wf, n));
        madd(ws, n, -dot3(ws, n));
        norm3(wf);
        norm3(ws);
        float rel[3], vc[3];
        sub3(cp, c->pos, rel);
        cross3(c->angvel, rel, vc);
        madd(vc, c->vel, 1);
        float vf = dot3(vc, wf), vs = dot3(vc, ws), share = 0.25f / h;
        int rear = i >= 2;
        float flat = -vs * share * GRIP * (rear && in->handbrake ? 0.35f : 1.0f);
        float flong = 0;
        if (in->throttle > 0)
            flong += in->throttle * ENGINE * 0.25f;
        if (in->brake > 0) {
            if (fwd_speed > 3.0f)
                flong -= fminf(fabsf(vf) * share, in->brake * BRAKE * 0.25f) * (vf > 0 ? 1 : -1);
            else
                flong -= in->brake * ENGINE * 0.5f * 0.25f; /* reverse */
        }
        if (rear && in->handbrake)
            flong -= fminf(fabsf(vf) * share, BRAKE * 0.25f) * (vf > 0 ? 1 : -1);
        flong -= vf * 0.25f * 0.25f; /* rolling resistance */
        if (in->throttle <= 0 && in->brake <= 0 && fabsf(vf) < 2.0f)
            flong -= vf * share * GRIP; /* parked: static friction holds the car */
        float maxf = MU * fs, mag = sqrtf(flat * flat + flong * flong);
        if (mag > maxf && mag > 0) {
            flat *= maxf / mag;
            flong *= maxf / mag;
        }
        float ft[3] = { 0, 0, 0 };
        madd(ft, wf, flong);
        madd(ft, ws, flat);
        madd(F, ft, 1);
        /* apply tyre forces closer to the centre of mass height: less body roll */
        madd(cp, u, -0.7f * dot3(rel, u));
        sub3(cp, c->pos, arm);
        cross3(arm, ft, tq);
        madd(T, tq, 1);
        c->spin[i] += vf / WHEEL_R * h;
    }
    c->grounded = grounded;
    float sp = sqrtf(dot3(c->vel, c->vel));
    madd(F, c->vel, -DRAG * sp);

    madd(c->vel, F, h);
    madd(c->pos, c->vel, h);
    /* angular: torque to body frame, divide by inertia, back to world */
    float ab[3] = { dot3(T, r) / INERTIA[0], dot3(T, u) / INERTIA[1], dot3(T, f) / INERTIA[2] };
    madd(c->angvel, r, ab[0] * h);
    madd(c->angvel, u, ab[1] * h);
    madd(c->angvel, f, ab[2] * h);
    float damp = 1.0f - (grounded ? 1.5f : 0.3f) * h;
    c->angvel[0] *= damp; c->angvel[1] *= damp; c->angvel[2] *= damp;
    for (int k = 0; k < 3; k++) {
        float dv[3];
        cross3(c->angvel, c->rot + k * 3, dv);
        madd(c->rot + k * 3, dv, h);
    }
    norm3(f);
    cross3(u, f, r);
    norm3(r);
    cross3(f, r, u);

    /* body against walls/terrain: push out, kill the inward velocity */
    for (int k = 0; k < 5; k++) {
        float p[3], n[3] = { 0, 1, 0 };
        to_world(c, PROBE[k], p);
        float depth = sphere_push(w, p, PROBE_R, n);
        if (depth <= 0)
            continue;
        madd(c->pos, n, depth);
        float vn = dot3(c->vel, n);
        if (vn < 0) {
            madd(c->vel, n, -vn * 1.2f);
            madd(c->vel, c->vel, -0.02f); /* scrape */
            c->angvel[0] *= 0.8f; c->angvel[1] *= 0.8f; c->angvel[2] *= 0.8f;
        }
    }
}

void car_step(Car *c, const ColWorld *w, const CarInput *in, float dt)
{
    if (dt > 0.1f)
        dt = 0.1f;
    float speed = fabsf(car_speed(c));
    float target = in->steer * MAX_STEER / (1.0f + speed / 70.0f);
    float rate = 2.5f * dt;
    c->steer += fmaxf(-rate, fminf(rate, target - c->steer));
    int n = (int)ceilf(dt * 240.0f);
    if (n < 1) n = 1;
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++)
        substep(c, w, in, dt / n);
}

void car_matrix(const Car *c, float out[16])
{
    for (int k = 0; k < 3; k++) {
        out[k] = c->rot[k];
        out[4 + k] = c->rot[3 + k];
        out[8 + k] = c->rot[6 + k];
        out[12 + k] = c->pos[k] - COM_Y * c->rot[3 + k];
    }
    out[3] = out[7] = out[11] = 0;
    out[15] = 1;
}

float car_wheel_offset(const Car *c, int i)
{
    /* wheel centre below the mount: REST - comp (unloaded: full REST); bind is at SAG */
    float travel = c->comp[i] > 0 ? c->comp[i] : 0;
    return travel - SAG;
}
