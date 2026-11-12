/*
 * PixCarsVita - collision grid and rigid-body car. See car.h.
 */
#include "car.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
#define MAX_TRI_CELLS 4096  /* bigger triangles go to one extra "big" cell every query scans */
#define BIG_CELL(w) ((uint32_t)((w)->nx * (w)->nz))

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
    w->cell_start = calloc(ncell + 2, 4);
    if (!w->cell_start)
        return -2;
    /* two passes: count per cell, then fill */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n_tri; i++) {
            int r[4];
            tri_range(w, w->tri + i * 9, r);
            int big = (r[1] - r[0] + 1) * (r[3] - r[2] + 1) > MAX_TRI_CELLS;
            if (big)
                r[1] = r[0], r[3] = r[2];
            for (int z = r[2]; z <= r[3]; z++)
                for (int x = r[0]; x <= r[1]; x++) {
                    uint32_t c = big ? ncell : (uint32_t)(z * w->nx + x);
                    if (pass == 0)
                        w->cell_start[c + 1]++;
                    else
                        w->items[w->cell_start[c]++] = i;
                }
        }
        if (pass == 0) {
            for (uint32_t c = 0; c <= ncell; c++)
                w->cell_start[c + 1] += w->cell_start[c];
            w->items = malloc(4u * (w->cell_start[ncell + 1] + 1));
            if (!w->items)
                return -2;
        } else {
            /* the fill advanced each start to the next cell's start: shift back */
            memmove(w->cell_start + 1, w->cell_start, 4u * (ncell + 1));
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
    for (int z = r[2]; z <= r[3] + 1; z++) /* last row: the big cell, once */
        for (int x = r[0]; x <= (z > r[3] ? r[0] : r[1]); x++) {
            uint32_t c = z > r[3] ? BIG_CELL(w) : (uint32_t)(z * w->nx + x);
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
    for (int z = r[2]; z <= r[3] + 1; z++) /* last row: the big cell, once */
        for (int x = r[0]; x <= (z > r[3] ? r[0] : r[1]); x++) {
            uint32_t c = z > r[3] ? BIG_CELL(w) : (uint32_t)(z * w->nx + x);
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

/* ------------------------------------------------------------------ car parameters */
#define MPH_TO_FTS 1.46666f  /* the PS2 loader's mph -> ft/s factor */
#define DEG 0.0174533f

/* McQueen's mcq.phy values (file units), used for keys a .phy does not set */
static const CarParams MCQ_DEFAULTS = {
    .weight = 3000, .gravity_scale = 1.2f, .inertia_scale = { 0.5f, 1, 1 }, .cofg = { 0, -0.2f, 0.7f },
    .restitution = 0.2f, .body_friction = 0.2f, .yaw_rate_limit = 10, .normal_load_scale = 1,
    .handbrake_scale = 0.43f,
    .tire = {
        { 0.7f, 8, 1.4f, 1.25f, 0.3f, 0.3f, 35, { 1, 1.2f, 6.2f, 6.2f }, { 1.5f, 2, 10.2f, 1 }, 2000, 10, 8 },
        { 0.7f, 8, 1.4f, 1.25f, 0.3f, 0.3f, 35, { 1, 1.2f, 6.2f, 6.2f }, { 1.5f, 2, 10.2f, 1 }, 2000, 10, 8 },
        { 0.9f, 8, 1.4f, 1.45f, 0.3f, 0.3f, 0, { 1, 1.2f, 7.4f, 6 }, { 1.5f, 2, 13.6f, 1 }, 2000, 10, 8 },
        { 0.9f, 8, 1.4f, 1.45f, 0.3f, 0.3f, 0, { 1, 1.2f, 7.4f, 6 }, { 1.5f, 2, 13.6f, 1 }, 2000, 10, 8 },
    },
    .drivetrain = 1, .n_rpm = 8, .n_gears = 5,
    .rpm = { 0, 1000, 2000, 3000, 4000, 5000, 6000, 7000 },
    .hp = { 0, 115, 225, 325, 435, 495, 540, 495 },
    .gear = { 6, 3.2f, 2.4f, 1.7f, 1.7f }, .final_ratio = 1.95f, .loss = 18, .min_rpm = 100,
    .engine_braking = 10, .engine_inertia = 0.2f, .idle_throttle = 0.01f,
    .max_k = 2.5f, .pump_torque_scale = 0.1f, .max_pump_torque = 1000,
    .low_rpm = 1000, .low_up = 5500, .low_down = 2000, .high_rpm = 1500, .high_up = 6700, .high_down = 4500,
    .shift_time = 1, .shift_up_time = 0.3f, .shift_up_throttle = 0.5f, .shift_down_time = 0.25f, .shift_down_throttle = 0.35f,
    .reverse_time = 0.1f,
    .nox_level = 1, .nox_base = 33, .nox_burn = 20, .nox_hp = 3000, .nox_refresh = 5, .nox_refresh_time = 1,
    .burnout_time = 1, .burnout_charge = 1.2f, .burnout_thrust = 5000,
    .aero_area = 35, .aero_drag = 0.3f, .aero_min_speed = 120, .nox_streamlining = 0.3f,
    .airborne_damping = 3000, .air_moment = { -2500, -1000, 0 }, .unroll_gain = 3, .unroll_torque = 1500,
    .ps_yaw = 3, .ps_hs_yaw = 1.65f, .ps_min_speed = 23, .ps_max_speed = 200, .ps_threshold = 0.95f,
    .ps_front = 0.9f, .ps_rear = 0.74f, .ps_torque = 10.8f,
    .ps_ls_front = 1, .ps_ls_rear = 0.3f, .ps_ls_rear_steer = 25, .ps_min_angle = 8, .ps_max_angle = 10,
    .ps_min_boost = 0, .ps_max_boost = 100,
    .ps_exit_time = 0.45f, .ps_front_exit = 1.2f, .ps_rear_exit = 1.4f, .ps_exit_yaw = 0.9f,
    .ps_steer_scale = 1.0f, .ps_rear_steer_scale = -0.2f,
    .land_rear = 0.1f, .trans_rear = 0.66f, .land_front = 0.55f, .trans_front = 0.2f,
    .land_exit_angle = 0.55f, .land_exit_yaw = 1,
    .preload_time = 0.08f, .max_preload = 42000, .preload_charge = 10, .max_preload_time = 0,
    .ws_angle = 50, .ws_roll_gain = -6, .ws_roll_rate_gain = 20000, .ws_steer_scale = 2, .ws_torque_scale = 2,
    .steer_power = 2, .x_rate = 25, .gas_rate = 4, .brake_rate = 20, .reverse_steer = 0.4f,
    .xdpad_on = 3, .xdpad_off = 2,
    .steer_speed = { 0, 80, 100 }, .steer_scale = { 0.6f, 0.23f, 0.28f },
    .dpad_speed = { 0, 100, 150 }, .dpad_scale = { 0.75f, 0.3f, 0.1f },
    .cam_offset = { 0, 6, 0 }, .cam_distance = 16,
};

/* value of key in [section] (case-insensitive), or NULL */
static const char *ini_find(const char *text, const char *section, const char *key)
{
    size_t sl = strlen(section), kl = strlen(key);
    int in = 0;
    for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        while (*l == ' ' || *l == '\t')
            l++;
        if (*l == '[') {
            const char *e = strchr(l, ']');
            in = e && (size_t)(e - l - 1) == sl && strncasecmp(l + 1, section, sl) == 0;
        } else if (in && strncasecmp(l, key, kl) == 0) {
            const char *q = l + kl;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '=')
                return q + 1;
        }
    }
    return NULL;
}

static void ini_f(const char *t, const char *sec, const char *key, float *out, int n)
{
    const char *v = ini_find(t, sec, key);
    for (int i = 0; v && i < n; i++) {
        char *end;
        float f = strtof(v, &end);
        if (end == v)
            return;
        out[i] = f;
        v = end;
        while (*v == 'f' || *v == ' ' || *v == ',')
            v++;
    }
}

static void ini_curve(const char *t, const char *sec, const char *pre, TireCurve *c)
{
    char k[64];
    snprintf(k, sizeof k, "%sRoundOff", pre); ini_f(t, sec, k, &c->round_off, 1);
    snprintf(k, sizeof k, "%sDropOff", pre); ini_f(t, sec, k, &c->drop_off, 1);
    snprintf(k, sizeof k, "%sScale", pre); ini_f(t, sec, k, &c->scale, 1);
    snprintf(k, sizeof k, "%sSpeedScale", pre); ini_f(t, sec, k, &c->speed_scale, 1);
}

void car_params_parse(CarParams *p, const char *t)
{
    *p = MCQ_DEFAULTS;
    if (t) {
        const char *M = "Mass Properties";
        ini_f(t, M, "Weight", &p->weight, 1);
        ini_f(t, M, "GravityScale", &p->gravity_scale, 1);
        ini_f(t, M, "InertiaScale", p->inertia_scale, 3);
        ini_f(t, M, "CofG", p->cofg, 3);
        ini_f(t, M, "Restitution", &p->restitution, 1);
        ini_f(t, M, "BodyFriction", &p->body_friction, 1);
        ini_f(t, M, "YawRateLimit", &p->yaw_rate_limit, 1);
        ini_f(t, M, "NormalLoadScale", &p->normal_load_scale, 1);
        const char *PS = "Powerslide";
        ini_f(t, PS, "HandBrakeFrictionScale", &p->handbrake_scale, 1);
        ini_f(t, PS, "PowerslideYawRateLimit", &p->ps_yaw, 1);
        ini_f(t, PS, "PowerslideHighSpeedYawRateLimit", &p->ps_hs_yaw, 1);
        ini_f(t, PS, "PowerslideMinSpeed", &p->ps_min_speed, 1);
        ini_f(t, PS, "PowerslideMaxSpeed", &p->ps_max_speed, 1);
        ini_f(t, PS, "PowerslideThreshold", &p->ps_threshold, 1);
        ini_f(t, PS, "PowerslideLowSpeedFrontFrictionScale", &p->ps_ls_front, 1);
        ini_f(t, PS, "PowerslideLowSpeedRearFrictionScale", &p->ps_ls_rear, 1);
        ini_f(t, PS, "PowerslideLowSpeedRearSteerAngle", &p->ps_ls_rear_steer, 1);
        ini_f(t, PS, "PowerslideFrontFrictionScale", &p->ps_front, 1);
        ini_f(t, PS, "PowerslideRearFrictionScale", &p->ps_rear, 1);
        ini_f(t, PS, "PowerslideMinAngle", &p->ps_min_angle, 1);
        ini_f(t, PS, "PowerslideMaxAngle", &p->ps_max_angle, 1);
        ini_f(t, PS, "PowerslideTorqueScale", &p->ps_torque, 1);
        ini_f(t, PS, "PowerslideExitTime", &p->ps_exit_time, 1);
        ini_f(t, PS, "PowerslideFrontExitFrictionScale", &p->ps_front_exit, 1);
        ini_f(t, PS, "PowerslideRearExitFrictionScale", &p->ps_rear_exit, 1);
        ini_f(t, PS, "PowerslideExitYawRateLimit", &p->ps_exit_yaw, 1);
        ini_f(t, PS, "PowerslideMinBoost", &p->ps_min_boost, 1);
        ini_f(t, PS, "PowerslideMaxBoost", &p->ps_max_boost, 1);
        ini_f(t, PS, "PowerslideSteeringScale", &p->ps_steer_scale, 1);
        ini_f(t, PS, "PowerslideRearSteeringScale", &p->ps_rear_steer_scale, 1);
        const char *LC = "LandingControl";
        ini_f(t, LC, "LandingRearFricitonScale", &p->land_rear, 1); /* sic, as the game spells it */
        ini_f(t, LC, "TransitionRearFrictionScale", &p->trans_rear, 1);
        ini_f(t, LC, "LandingFrontFrictionScale", &p->land_front, 1);
        ini_f(t, LC, "TransitionFrontFrictionScale", &p->trans_front, 1);
        ini_f(t, LC, "LandingExitAngle", &p->land_exit_angle, 1);
        ini_f(t, LC, "LandingExitYaw", &p->land_exit_yaw, 1);
        static const char *TN[4] = { "FrontLeftTire", "FrontRightTire", "RearLeftTire", "RearRightTire" };
        for (int i = 0; i < 4; i++) {
            TireParams *tp = &p->tire[i];
            char s[64];
            ini_f(t, TN[i], "MaxCompression", &tp->max_comp, 1);
            ini_f(t, TN[i], "MaxCompressionRate", &tp->max_comp_rate, 1);
            ini_f(t, TN[i], "MaxDecompressionRate", &tp->max_decomp_rate, 1);
            ini_f(t, TN[i], "TireRadius", &tp->radius, 1);
            ini_f(t, TN[i], "Spring", &tp->spring, 1);
            ini_f(t, TN[i], "TireInertia", &tp->inertia, 1);
            ini_f(t, TN[i], "BounceDampingRatio", &tp->bounce_damp, 1);
            ini_f(t, TN[i], "ReboundDampingRatio", &tp->rebound_damp, 1);
            ini_f(t, TN[i], "MaxSteerAngle", &tp->max_steer, 1);
            snprintf(s, sizeof s, "%s Surface 1", TN[i]);
            ini_curve(t, s, "SideForce", &tp->side);
            ini_curve(t, s, "DriveForce", &tp->drive);
            ini_f(t, s, "MaxBrakingTorque", &tp->max_brake_torque, 1);
        }
        const char *E = "Engine";
        float f = (float)p->drivetrain;
        ini_f(t, E, "DriveTrain", &f, 1);
        p->drivetrain = (int)f;
        f = (float)p->n_rpm;
        ini_f(t, E, "NumberOfRPMPoints", &f, 1);
        if (f >= 2 && f <= 16) {
            p->n_rpm = (int)f;
            for (int i = 0; i < p->n_rpm; i++) {
                char k[24];
                snprintf(k, sizeof k, "RPM%d", i + 1); ini_f(t, E, k, &p->rpm[i], 1);
                snprintf(k, sizeof k, "HP%d", i + 1); ini_f(t, E, k, &p->hp[i], 1);
            }
        }
        f = (float)p->n_gears;
        ini_f(t, E, "NumberOfGears", &f, 1);
        if (f >= 1 && f <= 8) {
            p->n_gears = (int)f;
            for (int i = 0; i < p->n_gears; i++) {
                char k[24];
                snprintf(k, sizeof k, "GearRatio%d", i + 1);
                ini_f(t, E, k, &p->gear[i], 1);
            }
        }
        ini_f(t, E, "FinalGearRatio", &p->final_ratio, 1);
        ini_f(t, E, "TransmissionLoss", &p->loss, 1);
        ini_f(t, E, "MinRPM", &p->min_rpm, 1);
        ini_f(t, E, "EngineBraking", &p->engine_braking, 1);
        ini_f(t, E, "EngineInertia", &p->engine_inertia, 1);
        ini_f(t, E, "IdleThrottle", &p->idle_throttle, 1);
        ini_f(t, E, "MaxK", &p->max_k, 1);
        ini_f(t, E, "PumpTorqueScale", &p->pump_torque_scale, 1);
        ini_f(t, E, "MaxPumpTorque", &p->max_pump_torque, 1);
        ini_f(t, E, "LowLoadRPM", &p->low_rpm, 1);
        ini_f(t, E, "LowLoadUpShiftRPM", &p->low_up, 1);
        ini_f(t, E, "LowLoadDownShiftRPM", &p->low_down, 1);
        ini_f(t, E, "HighLoadRPM", &p->high_rpm, 1);
        ini_f(t, E, "HighLoadUpShiftRPM", &p->high_up, 1);
        ini_f(t, E, "HighLoadDownShiftRPM", &p->high_down, 1);
        ini_f(t, E, "ShiftTime", &p->shift_time, 1);
        ini_f(t, E, "ReverseTime", &p->reverse_time, 1);
        ini_f(t, E, "ShiftUpDuration", &p->shift_up_time, 1);
        ini_f(t, E, "ShiftUpThrottle", &p->shift_up_throttle, 1);
        ini_f(t, E, "ShiftDownDuration", &p->shift_down_time, 1);
        ini_f(t, E, "ShiftDownThrottle", &p->shift_down_throttle, 1);
        ini_f(t, E, "NOXLevel", &p->nox_level, 1);
        ini_f(t, E, "NOXBaseVolume", &p->nox_base, 1);
        ini_f(t, E, "NOXBurnRate", &p->nox_burn, 1);
        ini_f(t, E, "NOXHPBoost", &p->nox_hp, 1);
        ini_f(t, E, "NOXRefreshRate", &p->nox_refresh, 1);
        ini_f(t, E, "NOXRefreshTime", &p->nox_refresh_time, 1);
        ini_f(t, E, "BurnoutTime", &p->burnout_time, 1);
        ini_f(t, E, "BurnoutChargeTime", &p->burnout_charge, 1);
        ini_f(t, E, "BurnoutThrust", &p->burnout_thrust, 1);
        float cur = 0;
        ini_f(t, "Tune Ups", "CurrentTuneUp", &cur, 1);
        if (cur >= 1) {
            char s[32];
            snprintf(s, sizeof s, "Tune Up %d", (int)cur);
            ini_f(t, s, "AeroSurfaceArea", &p->aero_area, 1);
            ini_f(t, s, "AeroDrag", &p->aero_drag, 1);
            ini_f(t, s, "AeroMinSpeed", &p->aero_min_speed, 1);
            ini_f(t, s, "NOXStreamlining", &p->nox_streamlining, 1);
        }
        const char *A = "Aerodynamics";
        ini_f(t, A, "AirborneDamping", &p->airborne_damping, 1);
        ini_f(t, A, "AirborneControlMoment", p->air_moment, 3);
        ini_f(t, A, "UnrollGain", &p->unroll_gain, 1);
        ini_f(t, A, "UnrollTorque", &p->unroll_torque, 1);
        ini_f(t, A, "UnyawGain", &p->unyaw_gain, 1);
        ini_f(t, A, "UnyawTorque", &p->unyaw_torque, 1);
        ini_f(t, "Preload", "PreloadTime", &p->preload_time, 1);
        ini_f(t, "Preload", "MaxPreload", &p->max_preload, 1);
        ini_f(t, "Preload", "MaxPreloadTime", &p->max_preload_time, 1);
        ini_f(t, "Preload", "PreloadChargeRate", &p->preload_charge, 1);
        const char *W = "Wheelstand";
        ini_f(t, W, "WheelStandAngle", &p->ws_angle, 1);
        ini_f(t, W, "WheelStandRollGain", &p->ws_roll_gain, 1);
        ini_f(t, W, "WheelStandRollRateGain", &p->ws_roll_rate_gain, 1);
        ini_f(t, W, "WheelStandSteerScale", &p->ws_steer_scale, 1);
        ini_f(t, W, "WheelStandTorqueScale", &p->ws_torque_scale, 1);
        const char *J = "Joystick";
        ini_f(t, J, "SteeringInputPower", &p->steer_power, 1);
        ini_f(t, J, "XAxisRateLimit", &p->x_rate, 1);
        ini_f(t, J, "XDPadRateLimitOn", &p->xdpad_on, 1);
        ini_f(t, J, "XDPadRateLimitOff", &p->xdpad_off, 1);
        ini_f(t, J, "GasRateLimit", &p->gas_rate, 1);
        ini_f(t, J, "BrakeRateLimit", &p->brake_rate, 1);
        ini_f(t, J, "ReverseSteerScale", &p->reverse_steer, 1);
        for (int i = 0; i < 3; i++) {
            char k[24];
            snprintf(k, sizeof k, "SteerSpeed%d", i + 1); ini_f(t, J, k, &p->steer_speed[i], 1);
            snprintf(k, sizeof k, "SteerScale%d", i + 1); ini_f(t, J, k, &p->steer_scale[i], 1);
            snprintf(k, sizeof k, "DPadSteerSpeed%d", i + 1); ini_f(t, J, k, &p->dpad_speed[i], 1);
            snprintf(k, sizeof k, "DPadSteerScale%d", i + 1); ini_f(t, J, k, &p->dpad_scale[i], 1);
        }
        ini_f(t, "Camera Properties", "CameraOffset", p->cam_offset, 3);
        ini_f(t, "Camera Properties", "CameraFollowingDistance", &p->cam_distance, 1);
    }
    /* unit conversions the PS2 loader applies after reading (0x37d770.., 0x37ec68, 0x37efcc) */
    p->ps_min_speed *= MPH_TO_FTS;
    p->ps_max_speed *= MPH_TO_FTS;
    p->aero_min_speed *= MPH_TO_FTS;
    p->ps_ls_rear_steer *= DEG;
    p->ps_min_angle *= DEG;
    p->ps_max_angle *= DEG;
    p->ws_angle *= DEG;
}

/* ------------------------------------------------------------------ car */
#define G_FT 32.174f         /* ft/s^2; weights are in pounds, forces in lbf, mass in slugs */
#define MPH 0.681818f        /* mph per ft/s */
#define RPM_PER_RADS 9.5492966f
#define BIND_WHEEL_Y 0.96f   /* wheel centre height in McQueen's bind pose (wheel*_root nodes) */

static const float WX[4] = { -2.43f, 2.43f, -2.69f, 2.69f };
static const float WZ[4] = { 4.55f, 4.55f, -3.9f, -3.9f };
static const float BOX[3] = { 7.14f, 5.36f, 15.17f }; /* model extents: inertia of a solid box */
/* body spheres, relative to the centre of mass (which the .phy puts only ~0.25 ft above the
   ground): kept clear of the road so only the bodywork touches walls */
static const float PROBE[5][3] = { { 0, 1.6f, 4.2f }, { 0, 1.6f, -4.4f }, { 0, 1.8f, 0 },
                                   { -2.0f, 1.6f, 0 }, { 2.0f, 1.6f, 0 } };
#define PROBE_R 1.3f

/* The game's tyre curve (PS2 ELF 0x216028), tabulated in 64 steps; lookups lerp between
   neighbouring entries like the game does (0x232a00). */
static float curve(const TireCurve *c, float x)
{
    float a = fabsf(c->round_off * c->speed_scale * x);
    return c->scale * c->speed_scale * x / (1.0f + powf(a, c->drop_off));
}

/* 0x232a00's lookup: the entry index comes from |x| (times idx_scale, capped at 0.99 for the side
   table), but the lerp uses the signed x, clamped to the two entries. So a negative x does not
   interpolate: it stays on entry i. */
static float table(const TireCurve *c, float x, float xmax, float idx_scale)
{
    float u = fabsf(x) / xmax * idx_scale;
    if (idx_scale < 1.0f)
        u = fminf(u, 0.99f);
    int i = (int)(u * 64.0f);
    float a = curve(c, xmax * i / 64.0f);
    if (i >= 63)
        return a;
    float x0 = xmax * i / 64.0f, x1 = xmax * (i + 1) / 64.0f, b = curve(c, x1);
    float v = a + (x - x0) / (x1 - x0) * (b - a);
    return fmaxf(fminf(a, b), fminf(fmaxf(a, b), v));
}

static float mass(const CarParams *p) { return p->weight / G_FT; }
/* PS2 0x2305f0: k = 32.2 * Spring * mass (= Spring * Weight, lbf/ft, per wheel);
   damping = 2 * ratio * sqrt(k * mass) with the whole car's mass */
static float spring_k(const CarParams *p, int i) { return G_FT * p->tire[i].spring * mass(p); }
/* wheel centre (= strut attach, the bind-pose wheel node) relative to the centre of mass */
static void attach(const CarParams *p, int i, float o[3])
{
    o[0] = WX[i] - p->cofg[0];
    o[1] = BIND_WHEEL_Y - p->cofg[1];
    o[2] = WZ[i] - p->cofg[2];
}

static void to_world(const Car *c, const float l[3], float o[3])
{
    for (int k = 0; k < 3; k++)
        o[k] = c->pos[k] + l[0] * c->rot[k] + l[1] * c->rot[3 + k] + l[2] * c->rot[6 + k];
}

/* local (right, up, fwd) vector to world */
static void dir_world(const Car *c, const float l[3], float o[3])
{
    for (int k = 0; k < 3; k++)
        o[k] = l[0] * c->rot[k] + l[1] * c->rot[3 + k] + l[2] * c->rot[6 + k];
}

void car_spawn(Car *c, const CarParams *p, const ColWorld *w, const float pos[3], float yaw)
{
    /* pos may alias c->pos (reset in place): read it before clearing the car; start the ray just
       above so a reset under a bridge lands on the road, not on top */
    float o[3] = { pos[0], pos[1] + 10.0f, pos[2] }, d[3] = { 0, -1, 0 }, t, n[3];
    memset(c, 0, sizeof *c);
    c->p = p;
    c->gear = 1;
    c->pos[0] = o[0];
    c->pos[2] = o[2];
    c->pos[1] = col_raycast(w, o, d, 4000.0f, &t, n) ? o[1] - t + 3.0f : o[1] - 10.0f;
    float s = sinf(yaw), co = cosf(yaw);
    float rot[9] = { co, 0, -s, 0, 1, 0, s, 0, co };
    memcpy(c->rot, rot, sizeof rot);
    for (int i = 0; i < 4; i++)
        c->lat_scale[i] = 1.0f;
    c->yaw_limit = p->yaw_rate_limit;
    c->boost = c->torque_scale = 1.0f;
    c->rev_t = p->reverse_time;
    c->jump_since = 1.0f;
    c->ps_armed = 1;
    c->turn_dir = 1.0f;
    c->nox = p->nox_base * p->nox_level; /* engine 0x2253a0: starts full */
}

float car_speed(const Car *c) { return dot3(c->vel, c->rot + 6); }

static float interp(const float *xs, const float *ys, int n, float x)
{
    if (x <= xs[0])
        return ys[0];
    for (int i = 1; i < n; i++)
        if (x <= xs[i])
            return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[n - 1];
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float sgnf(float v) { return v < 0 ? -1.0f : 1.0f; }
/* the game's two-point lerp, clamped to the end values (inlined all over 0x37fc90) */
static float lerp2(float x, float x0, float y0, float x1, float y1)
{
    if (x0 == x1)
        return y0;
    return clampf(y0 + (x - x0) / (x1 - x0) * (y1 - y0), fminf(y0, y1), fmaxf(y0, y1));
}

static int driven_wheel(const Car *c, int i)
{
    if (c->front_drive || c->backwards) /* powerslide / backwards: the front pair (0x225070) */
        return i < 2;
    const CarParams *p = c->p;
    return p->drivetrain == 2 || (p->drivetrain == 1 ? i >= 2 : i < 2);
}

/* Engine and torque converter as the PS2 game runs them (0x225810; setters 0x2250a0-0x2253a0).
   Returns the hub torque summed over the driven wheels (lb-ft); updates rpm, NOX and the gearbox. */
static float drivetrain(Car *c, float dt)
{
    const CarParams *p = c->p;
    int reverse = c->gear < 0;
    float dir = reverse ? -1.0f : 1.0f;
    if (c->backwards)
        dir = -dir; /* engine +132 = -1: driving backwards */
    float ratio = p->gear[reverse ? 0 : c->gear - 1] * p->final_ratio;
    float load = c->gas; /* in reverse the controls feed the brake into the throttle */
    if (c->rpm < p->min_rpm)
        c->rpm = p->min_rpm;

    /* gearbox side: driven wheels' average spin through the current ratio (rad/s -> rpm);
       a charging burnout (+1244) keeps the clutch open */
    float w = 0;
    int nd = 0;
    for (int i = 0; i < 4; i++)
        if (driven_wheel(c, i))
            w += c->omega[i], nd++;
    w = (nd ? w / nd : 0) * dir;
    /* 0x225810: the clutch opens during a gear change (ShiftUp/DownDuration) and while the
       brake is on, except in top gear; then the engine turns free and drives nothing */
    int clutch = !((c->shift_t != 0 || c->brake > 0) && !(c->gear == p->n_gears && !reverse));
    float trans = c->burn_hold ? 0 : RPM_PER_RADS * ratio * w;
    float slip = trans < 0 ? c->rpm : c->rpm - trans;
    if (!clutch) {
        trans = c->rpm;
        slip = 0;
    }
    /* pump torque k * slip^2 (k = PumpTorqueScale / 1000), limited to MaxPumpTorque */
    float k = p->pump_torque_scale * 0.001f;
    float pump = (slip < 0 ? -k : k) * slip * slip;
    pump = fmaxf(-p->max_pump_torque, fminf(p->max_pump_torque, pump));
    /* torque multiplication: maxK at stall, 1 when the turbine catches the engine */
    float mult = (c->rpm > trans && c->rpm > 0 && trans > 0)
                 ? p->max_k + (1.0f - p->max_k) * trans / c->rpm : p->max_k;
    float out = pump * mult;
    if (ratio > 0 && w > 0)
        out *= c->boost; /* engine +104: the powerslide torque scale */
    if (c->burn_hold || !clutch)
        out = 0;

    /* automatic gearbox (PS2 0x225540): load = converter slip between Low/HighLoadRPM; shift
       points blend Low->High; no shifting while braking or within ShiftTime of the last shift;
       during ShiftUp/DownDuration the throttle is replaced by ShiftUp/DownThrottle */
    float lt = (slip - p->low_rpm) / (p->high_rpm - p->low_rpm);
    lt = fmaxf(0.0f, fminf(1.0f, lt));
    float up = p->low_up + (p->high_up - p->low_up) * lt;
    float down = p->low_down + (p->high_down - p->low_down) * lt;
    int touching = 0; /* 0x225540: no shifting with both driven wheels in the air */
    for (int i = 0; i < 4; i++)
        touching |= driven_wheel(c, i) && c->contact[i];
    if (!touching || reverse)
        ;
    else if (c->shift_cool > 0)
        c->shift_cool -= dt;
    else if (c->brake <= 0) {
        if (c->rpm > up && c->gear < p->n_gears) {
            c->gear++;
            c->shift_t = p->shift_up_time;
            c->shift_cool = p->shift_time;
        } else if (c->rpm < down && c->gear > 1) {
            c->gear--;
            c->shift_t = -p->shift_down_time;
            c->shift_cool = p->shift_time;
        }
    }
    float shift_throttle = -1;
    if (c->shift_t > 0) {
        shift_throttle = p->shift_up_throttle;
        c->shift_t = fmaxf(0, c->shift_t - dt);
    } else if (c->shift_t < 0) {
        shift_throttle = p->shift_down_throttle;
        c->shift_t = fminf(0, c->shift_t + dt);
    }
    if (shift_throttle >= 0)
        load = shift_throttle;

    /* engine torque: gross curve (5252 HP / rpm + friction) times throttle (plus IdleThrottle),
       minus friction EngineBraking * 1e-6 * rpm^2 */
    float fr = p->engine_braking * 1e-6f;
    float rpm = c->rpm;
    float gross = interp(p->rpm, p->hp, p->n_rpm, rpm) * 5252.0f / fmaxf(rpm, 1.0f) + fr * rpm * rpm;
    float te = gross * fminf(load + p->idle_throttle, 1.0f) - fr * rpm * rpm;

    /* NOX (0x225810 +124..+168): the Boost action burns NOXBurnRate per second and adds
       5252 * NOXHPBoost / rpm; an empty tank waits NOXRefreshTime, then refills at NOXRefreshRate */
    float cap = p->nox_base * p->nox_level;
    c->nox_on = c->nox_on && c->nox > 0;
    if (!c->nox_on && c->nox < cap && c->nox_delay < 0)
        c->nox += p->nox_refresh * dt;
    c->nox = fminf(c->nox, cap);
    if (c->nox_on && rpm > 0) {
        te += 5252.0f * p->nox_hp / rpm;
        c->nox -= p->nox_burn * dt;
        if (c->nox <= 0) {
            c->nox_on = 0;
            c->nox_delay = p->nox_refresh_time;
        }
    }
    c->nox_delay -= dt;

    /* engine speed, implicit in the pump torque's slope */
    float inv_i = 1.0f / p->engine_inertia;
    float denom = 1.0f + dt * k * fabsf(slip) * inv_i;
    c->rpm += RPM_PER_RADS * dt * (te - pump) * inv_i / denom;
    /* rev limit: the last RPM point, 9000 while NOX burns */
    c->rpm = fmaxf(p->min_rpm, fminf(c->nox_on ? 9000.0f : p->rpm[p->n_rpm - 1], c->rpm));

    /* to each driven wheel: ratio, transmission loss, half the axle, at most 5000 lb-ft, then
       the WheelStandTorqueScale (+232; engine +0xe8 is applied after the limit) */
    float wheel = fmaxf(-5000.0f, fminf(5000.0f, out * ratio * (1.0f - p->loss / 100.0f) * 0.5f));
    return 2.0f * wheel * c->torque_scale * dir;
}

/* body inertia (x pitch, y yaw, z roll) */
static void inertia(const CarParams *p, float I[3])
{
    float m = mass(p);
    I[0] = m * (BOX[1] * BOX[1] + BOX[2] * BOX[2]) / 12 * p->inertia_scale[0];
    I[1] = m * (BOX[0] * BOX[0] + BOX[2] * BOX[2]) / 12 * p->inertia_scale[1];
    I[2] = m * (BOX[0] * BOX[0] + BOX[1] * BOX[1]) / 12 * p->inertia_scale[2];
}

/* Forces and torques of the game's driving actions, re-evaluated every substep because they
   follow the body's attitude and spin. F, T: world space accumulators. */
static void action_forces(Car *c, float F[3], float T[3])
{
    const CarParams *p = c->p;
    const float *r = c->rot, *u = c->rot + 3, *f = c->rot + 6;
    float m = mass(p);
    float wl[3] = { dot3(c->angvel, r), dot3(c->angvel, u), dot3(c->angvel, f) }; /* local spin */
    float roll = asinf(clampf(r[1], -1, 1)); /* about fwd; the game's +808 is its negative */
    float pitch = asinf(clampf(f[1], -1, 1)); /* nose up; the game's +800 */
    int g_all = c->grounded == 4, g_any = c->grounded > 0;

    /* 0x3817c0: powerslide push, PowerslideMin..MaxBoost by the slip angle (Min..MaxAngle),
       times mass / 10 along the car's nose */
    if (c->ps && c->slip_angle > 0.12f) {
        float b = lerp2(c->slip_angle, p->ps_min_angle, p->ps_min_boost, p->ps_max_angle, p->ps_max_boost);
        madd(F, f, fminf(b, p->ps_max_boost) * m * 0.1f);
    }
    /* aero drag above AeroMinSpeed: rho/2 * |v|^2 * area * drag, less while boosting */
    if (fabsf(c->fwd) > p->aero_min_speed) {
        float d = 0.00118845f * c->spd * c->spd * p->aero_area * p->aero_drag;
        if (c->nox_on)
            d *= p->nox_streamlining;
        madd(F, f, c->fwd < 0 ? d : -d);
    }
    /* not on all four wheels: AirborneDamping against the spin */
    if (!g_all)
        madd(T, c->angvel, -p->airborne_damping);
    /* in the air, 0.5 s after a jump: stick pitch (AirborneControlMoment.x * y) and yaw
       (8000 * x), unroll, damping, and nose levelling (0.5 s.. by the code) */
    if (!g_any && c->jump_since > 0.5f) {
        float tl[3];
        tl[0] = p->air_moment[0] * c->y;
        tl[1] = (c->backwards ? -8000.0f : 8000.0f) * c->x; /* driving backwards flips it */
        tl[2] = p->unroll_torque * (-p->unroll_gain * roll - wl[2]);
        for (int k = 0; k < 3; k++)
            tl[k] -= p->airborne_damping * wl[k];
        if (fabsf(c->x) < 0.1f)
            tl[1] += p->unyaw_torque * (p->unyaw_gain * c->slip_angle - wl[1]);
        int level = fabsf(c->y) < 0.1f || fabsf(pitch) > 1.309f ||
                    (c->vel[1] < 0 && fabsf(pitch) > 0.0872665f);
        if (level)
            tl[0] += lerp2(fabsf(pitch), 0, 1500, 1.309f, 3000) * (7.0f * pitch - wl[0]);
        float tw[3];
        dir_world(c, tl, tw);
        madd(T, tw, 1);
    }
    /* 0x381ce0: slope hold. On the ground, off the gas, under 10 ft/s: cancel the part of
       gravity along the ground that would slide the car sideways; with the brake held, all of
       it. Plane from the front-left tyre's contact (or the body's up axis in the air). */
    if (g_any && c->gas == 0 && fabsf(c->fwd) < 10.0f) {
        const float *n = c->contact[0] ? c->gnormal[0] : u;
        float g[3] = { 0, -p->weight * p->gravity_scale, 0 }, gt[3], fp[3];
        memcpy(gt, g, sizeof g);
        madd(gt, n, -dot3(g, n));
        memcpy(fp, f, sizeof fp);
        madd(fp, n, -dot3(fp, n));
        if (norm3(fp) == 0)
            fp[0] = 1, fp[1] = fp[2] = 0;
        float gf[3] = { 0, 0, 0 };
        madd(gf, fp, dot3(fp, gt));
        madd(F, gt, -1);
        if (c->brake_in == 0)
            madd(F, gf, 1); /* without the brake only the sideways part is held */
    }
    /* 0x381fc0: the jump, MaxPreload straight up for PreloadTime */
    if (c->jump_on)
        F[1] += p->max_preload;
    /* 0x382b40: wheelstand, a roll PD toward +-WheelStandAngle; on the way out half of it, the
       other way, plus the same torque pressing the car down */
    if (c->tilt) {
        float target = c->tilt_side > 0 ? -p->ws_angle : p->ws_angle;
        if (c->tilt_exit)
            target = -target;
        /* game: rate_gain * (roll_gain * (target - r808) - rate), r808 = -roll */
        float tq = p->ws_roll_rate_gain * (p->ws_roll_gain * (target + roll) - wl[2]);
        if (c->tilt_exit) {
            tq *= 0.5f;
            F[1] -= fabsf(tq / WX[1]);
        }
        madd(T, f, tq);
    }
    /* 0x383030: a charged burnout pushes the car forward while the gas is held */
    if (c->burn_t > 0 && !c->burn_charge && c->gas_in >= 1.0f && g_all)
        madd(F, f, p->burnout_thrust);
}

static void substep(Car *c, const ColWorld *w, float hub_torque, float h)
{
    const CarParams *p = c->p;
    float *r = c->rot, *u = c->rot + 3, *f = c->rot + 6;
    float m = mass(p);
    float F[3] = { 0, -p->weight * p->gravity_scale, 0 }, T[3] = { 0, 0, 0 };
    int grounded = 0;
    int driven_n = 0;
    for (int i = 0; i < 4; i++)
        driven_n += driven_wheel(c, i);
    for (int i = 0; i < 4; i++) {
        const TireParams *tp = &p->tire[i];
        /* tyre +0x374 picks the front or rear surface tables (curves, brake torque); driving
           backwards (0x386790) the game swaps them, so the leading pair uses the front ones */
        const TireParams *ts = &p->tire[c->backwards ? i ^ 2 : i];
        int rear = i >= 2;
        int driven = driven_wheel(c, i);
        float R = tp->radius;

        /* wheel spin (PS2 0x231e30 / 0x2326d0): drive + brake torque against the last
           longitudinal force, over TireInertia; at most 293.3 rad/s. The brake level (+1248)
           goes to all four wheels; the stopped hold locks them. */
        float brake = c->hold ? 1.0f : c->brake;
        if (brake > 0 && c->slip[i] > 0.95f) {
            c->omega[i] = 0; /* 0x2326d0: braking past 0.95 slip locks the wheel */
        } else {
            float spin_sign = fmaxf(-1.0f, fminf(1.0f, 0.5f * c->omega[i]));
            float torque = (driven ? hub_torque / driven_n : 0) - brake * ts->max_brake_torque * spin_sign;
            c->omega[i] += (torque - c->fx_prev[i] * R) / tp->inertia * h;
            c->omega[i] = fmaxf(-293.33f, fminf(293.33f, c->omega[i]));
        }
        c->spin[i] += c->omega[i] * h;
        if (rear && c->burn_charge)
            c->spin[i] += 10.0f * h; /* burnout charge: the rear wheels spin on the spot */

        /* suspension (PS2 0x2317b0): ray from the bind-pose wheel centre down one tyre
           radius; compression 0 at the bind pose */
        float la[3], pm[3], d[3] = { -u[0], -u[1], -u[2] }, t, n[3];
        attach(p, i, la);
        to_world(c, la, pm);
        if (!col_raycast(w, pm, d, R, &t, n)) {
            /* 0x2317b0 end: the stored compression drops at most MaxDecompressionRate */
            c->comp[i] = fmaxf(0.0f, c->comp[i] - tp->max_decomp_rate * h);
            c->fx_prev[i] = 0;
            c->contact[i] = 0;
            continue;
        }
        grounded++;
        c->contact[i] = 1;
        memcpy(c->gnormal[i], n, sizeof n);
        float comp = R - t;
        float cp[3] = { pm[0] + d[0] * t, pm[1] + d[1] * t, pm[2] + d[2] * t }, rel[3], vc[3];
        sub3(cp, c->pos, rel);
        cross3(c->angvel, rel, vc);
        madd(vc, c->vel, 1);
        if (comp > tp->max_comp) {
            /* bottomed out: push the body out and stop it moving into the ground */
            madd(c->pos, n, fminf(comp - tp->max_comp, tp->max_comp));
            float vn = dot3(vc, n); /* the contact point's closing speed, taken off the body */
            if (vn < 0) {
                madd(c->vel, n, -vn);
                madd(vc, n, -vn);
            }
            comp = tp->max_comp;
        }
        float vraw = dot3(vc, n);
        if (-vraw > c->strut_hit)
            c->strut_hit = -vraw; /* closing speed, for the suspension sounds */
        float vs = fmaxf(-tp->max_comp_rate, fminf(tp->max_comp_rate, vraw));
        float k = spring_k(p, i);
        float damp = 2.0f * (vs > 0 ? tp->bounce_damp : tp->rebound_damp) * sqrtf(k * m);
        float fn = fmaxf(0.0f, k * comp - damp * vs);
        /* the force uses the real compression; the stored one (wheel position) may only drop
           by MaxDecompressionRate per second */
        c->comp[i] = fminf(tp->max_comp, comp < c->comp[i] ? c->comp[i] - fminf(c->comp[i] - comp, tp->max_decomp_rate * h) : comp);
        float fv[3] = { n[0] * fn, n[1] * fn, n[2] * fn }, tq[3];
        madd(F, fv, 1);
        cross3(rel, fv, tq);
        madd(T, tq, 1);

        /* tyre frame on the ground plane (PS2 0x232350) */
        float s = i < 2 ? c->steer : c->rear_steer, cs = cosf(s), sn = sinf(s);
        float wf[3], ws[3];
        for (int k2 = 0; k2 < 3; k2++) {
            wf[k2] = f[k2] * cs + r[k2] * sn;
            ws[k2] = r[k2] * cs - f[k2] * sn;
        }
        madd(wf, n, -dot3(wf, n));
        madd(ws, n, -dot3(ws, n));
        norm3(wf);
        norm3(ws);
        float vz = dot3(vc, wf), vx = dot3(vc, ws), avz = fabsf(vz);

        /* slips (PS2 0x2326d0) */
        float alpha;
        if (avz > 20.0f) {
            alpha = acosf(fminf(1.0f, avz / sqrtf(vx * vx + vz * vz))) * (vx < 0 ? -1.0f : 1.0f);
            c->slip[i] = (vz - c->omega[i] * R) / avz;
        } else {
            alpha = 0.05f * vx;
            c->slip[i] += h * 3.0f * (vz - avz * c->slip[i] - c->omega[i] * R);
        }
        alpha = fmaxf(-1.5707963f, fminf(1.5707963f, alpha));
        if (rear)
            c->side_slip = fmaxf(c->side_slip, fabsf(alpha));
        c->slip[i] = fmaxf(-0.98f, fminf(0.98f, c->slip[i]));
        if (driven)
            c->drive_slip = fmaxf(c->drive_slip, -c->slip[i]);

        /* forces (PS2 0x232a00): independent curves, no friction circle; the lateral load
           blends a quarter of the weight with the strut force by NormalLoadScale */
        float fx = -(c->slip[i] > 0 ? 1.0f : -1.0f) * table(&ts->drive, c->slip[i], 1.0f, 1.0f) * fn;
        float load = p->weight * 0.25f + (fn - p->weight * 0.25f) * p->normal_load_scale;
        float fy = -(alpha > 0 ? 1.0f : -1.0f) * table(&ts->side, alpha, 1.5707963f, 0.99f) * load;
        fy *= c->lat_scale[i]; /* tyre +892: powerslide / exit / handbrake scales */
        c->fx_prev[i] = fx;
        float ft[3] = { 0, 0, 0 };
        madd(ft, wf, fx);
        madd(ft, ws, fy);
        madd(F, ft, 1);
        cross3(rel, ft, tq);
        madd(T, tq, 1);
    }
    c->grounded = grounded;
    action_forces(c, F, T);

    madd(c->vel, F, h / m);
    madd(c->pos, c->vel, h);
    /* angular: torque to body frame, divide by inertia (x pitch, y yaw, z roll), back to world */
    float I[3];
    inertia(p, I);
    madd(c->angvel, r, dot3(T, r) / I[0] * h);
    madd(c->angvel, u, dot3(T, u) / I[1] * h);
    madd(c->angvel, f, dot3(T, f) / I[2] * h);
    float yr = dot3(c->angvel, u);
    if (fabsf(yr) > c->yaw_limit)
        madd(c->angvel, u, (yr > 0 ? c->yaw_limit : -c->yaw_limit) - yr);
    for (int k = 0; k < 3; k++) {
        float dv[3];
        cross3(c->angvel, c->rot + k * 3, dv);
        madd(c->rot + k * 3, dv, h);
    }
    norm3(f);
    cross3(u, f, r);
    norm3(r);
    cross3(f, r, u);

    /* body against walls/terrain: push out, bounce with the .phy restitution and friction */
    for (int k = 0; k < 5; k++) {
        float pp[3], n[3] = { 0, 1, 0 };
        to_world(c, PROBE[k], pp);
        float depth = sphere_push(w, pp, PROBE_R, n);
        if (depth <= 0)
            continue;
        madd(c->pos, n, depth);
        float vn = dot3(c->vel, n);
        if (vn < 0) {
            c->impact = fmaxf(c->impact, -vn);
            madd(c->vel, n, -vn * (1.0f + p->restitution));
            float vt[3] = { c->vel[0], c->vel[1], c->vel[2] };
            madd(vt, n, -dot3(vt, n));
            madd(c->vel, vt, -fminf(1.0f, p->body_friction * -vn / (sqrtf(dot3(vt, vt)) + 1e-3f)));
            c->angvel[0] *= 0.8f; c->angvel[1] *= 0.8f; c->angvel[2] *= 0.8f;
        }
    }
}

static float rate(float cur, float target, float per_s, float dt)
{
    float d = target - cur, m = per_s * dt;
    return cur + (d > m ? m : (d < -m ? -m : d));
}

/* 0x37f6d0: actions -> stick / pedal values; d-pad steering takes over until the stick moves */
static void read_input(Car *c, const CarInput *in)
{
    c->x_prev = c->x;
    c->x = in->steer;
    c->y = in->steer_y;
    c->gas_in = in->throttle > 0 ? 1.0f : 0.0f; /* digital Gas (DigitalThrottleSpeed 0) */
    c->brake_in = in->brake > 0 ? 1.0f : 0.0f;
    c->ebrake_in = in->ebrake ? 1.0f : 0.0f;
    /* Boost: engine +124; with NOX left it also floors the gas */
    c->nox_on = in->boost && c->nox > 0;
    if (c->nox_on)
        c->gas_in = 1.0f;
    if (in->dpad_x) {
        c->x = (float)in->dpad_x;
        c->digital = 1;
    } else if (c->x != 0) {
        c->digital = 0;
    }
    if (in->dpad_y)
        c->y = (float)in->dpad_y;
}

/* 0x37fc90: steering filters and speed tables, brake ramp, stopped hold, reverse, gas ramp,
   burnout charge */
static void controls(Car *c, float dt)
{
    const CarParams *p = c->p;
    float sp_mph = fabsf(c->fwd) * MPH, scale;
    if (c->digital) {
        float r = (c->x != 0 ? p->xdpad_on : p->xdpad_off) * dt;
        c->xf += clampf(c->x - c->xf, -r, r);
        c->yf += clampf(c->y - c->yf, -r, r);
        scale = interp(p->dpad_speed, p->dpad_scale, 3, sp_mph);
    } else {
        c->x = sgnf(c->x) * powf(fabsf(c->x), p->steer_power);
        c->xf = c->x;
        float r = p->x_rate * dt;
        c->yf += clampf(c->y - c->yf, -r, r);
        scale = interp(p->steer_speed, p->steer_scale, 3, sp_mph);
    }
    if (c->tilt)
        scale *= p->ws_steer_scale;
    c->steer_cmd = scale * c->xf;
    if (c->backwards) { /* the rear wheels lead: they steer, the other way, 1.2 times */
        c->rear_cmd = -1.2f * c->steer_cmd;
        c->steer_cmd = 0;
    }

    /* stopped hold: the EBrake, or the Brake before reverse is armed, at <= 3 ft/s */
    int braking = c->ebrake_in != 0 || (c->brake_in != 0 && !c->rev_ready);
    if (braking && c->spd <= 3.0f) {
        /* shortcut: the game also sets engine +240 here; read as "back to first gear" (the
           gearbox does not shift while braking), not traced into 0x225810 */
        c->hold = 1;
        if (c->gear > 0)
            c->gear = 1;
    }
    /* brake ramp at BrakeRateLimit (+1252, level +1248 = half of it) */
    if (braking)
        c->brake_acc += clampf(1.0f - c->brake, -p->brake_rate * dt, p->brake_rate * dt);
    else
        c->brake_acc = 0;
    c->brake = 0.5f * c->brake_acc;
    c->burn_arm = c->brake_in != 0 && c->gas_in != 0;
    /* reverse: once armed, the brake held for ReverseTime selects reverse and becomes the gas */
    if ((c->brake_in != 0 || c->gas_in == 0) && c->rev_ready && !c->backwards) {
        if (c->rev_t <= 0) {
            c->gear = -1;
            c->gas_in = c->brake_in;
        } else {
            c->rev_t -= dt;
        }
    } else if (!c->backwards) {
        c->rev_t = p->reverse_time;
        if (c->gear < 0)
            c->gear = 1;
    }
    if (c->hold && ((c->gas_in > 0 && c->ebrake_in == 0) ||
                    (c->brake_in != 0 && c->rev_ready && c->ebrake_in == 0)))
        c->hold = 0;
    /* gas: GasRateLimit only on the way to full throttle */
    if (c->gas_in != 1.0f) {
        c->gas = c->gas_in;
        c->burn_hold = 0;
        c->burn_charge = 0;
        return;
    }
    c->gas = rate(c->gas, c->gas_in, p->gas_rate, dt);
    /* burnout: gas and brake together, standing still, charges for BurnoutChargeTime */
    if (c->burn_arm && fabsf(c->fwd) < 3.0f && fabsf(c->lat) < 2.0f && c->ebrake_in == 0) {
        c->burn_hold = 1;
        if (!c->burn_charge)
            c->burn_t = p->burnout_charge;
        c->burn_charge = 1;
    } else {
        c->burn_hold = 0;
        if (c->burn_charge && c->burn_t < 0) {
            c->burn_t = p->burnout_time;
            c->burn_charge = 0;
        }
    }
}

/* 0x388650: stop with the brake, let go, press again: reverse */
static void reverse_arming(Car *c)
{
    if (c->burn_charge)
        return;
    int any = c->brake_in != 0 || c->ebrake_in != 0;
    if (any && c->hold)
        c->rev_arm = 1;
    if (c->rev_arm && !any && c->hold) {
        c->rev_arm = 0;
        c->rev_ready = 1;
    }
    if (c->gas_in != 0 && c->brake_in == 0)
        c->rev_ready = c->rev_arm = 0;
}

/* 0x381fc0: pull the stick back past 0.8 (charging at PreloadChargeRate), then push it forward
   past 0.8 within 0.2 s: MaxPreload up for PreloadTime. Needs > 30 ft/s, wheels down, no brake. */
static void jump(Car *c, float dt)
{
    const CarParams *p = c->p;
    if (c->jump_on) {
        c->jump_t -= dt;
        if (c->jump_t <= 0) {
            c->jump_since = 0;
            c->jump_on = 0;
        }
    } else if (c->grounded > 0 && fabsf(c->fwd) > 30.0f && c->brake_in == 0 && !c->tilt) {
        c->jumped = 0;
        if (!c->jump_charge) {
            if (c->y > 0.8f) {
                c->jump_charge = 1;
                c->jump_t = 0;
                c->jump_win = 0.2f;
            }
        } else if (c->y > 0.8f) {
            c->jump_t += p->preload_charge * dt;
        } else if (c->y < -0.8f) {
            c->jump_charge = 0;
            if (c->jump_t >= 0) { /* MinPreloadTime: the game zeroes it every frame */
                c->jump_t = p->preload_time;
                c->jump_on = c->jumped = 1;
            }
        } else {
            c->jump_win -= dt;
            if (c->jump_t <= p->max_preload_time || c->jump_win < 0) {
                c->jump_charge = c->jump_on = 0;
                c->jump_t = 0;
            }
        }
    }
    c->jump_since += dt;
}

static void set_lat(Car *c, float front, float rear)
{
    c->lat_scale[0] = c->lat_scale[1] = front;
    c->lat_scale[2] = c->lat_scale[3] = rear;
}

/* 0x382320: powerslide. Held with the stick past PowerslideThreshold between Min/MaxSpeed:
   front-wheel drive, steering scales, slide frictions, torque boost; released (or braking, too
   slow): exit frictions for PowerslideExitTime. Under MinSpeed with gas and full lock the car
   spins on the spot (low-speed slide, no button). */
static void powerslide(Car *c, float dt)
{
    const CarParams *p = c->p;
    if (c->backwards) {
        c->boost = 1.0f;
        return;
    }
    if (c->ps_low && !c->tilt) {
        if (c->fwd >= 0) {
            c->lat_scale[0] = c->lat_scale[1] = p->ps_ls_front;
            c->rear_cmd = -c->x;
            c->lat_scale[2] = c->lat_scale[3] = c->gas_in != 0 ? p->ps_ls_rear : 1.0f;
            c->yaw_limit = p->ps_yaw;
        }
        if (c->gas_in == 0 || c->spd > p->ps_min_speed || c->x_prev * c->x <= 0)
            c->ps_low = c->ps = 0;
        return;
    }
    if (c->ps && !c->tilt && c->rot[4] > 0) {
        c->front_drive = 1;
        c->steer_cmd *= p->ps_steer_scale;
        c->rear_cmd = c->steer_cmd * p->ps_rear_steer_scale;
        set_lat(c, p->ps_front, p->ps_rear);
        c->yaw_limit = p->ps_hs_yaw;
        if (c->fwd > 0 && c->slip_angle > 0.12f)
            c->boost = 1.0f + fmaxf(0.0f, p->ps_torque + 0.5f * (c->ps_entry_speed - c->spd));
        else
            c->boost = 1.0f;
        if (c->brake_in != 0 || c->spd <= p->ps_min_speed || c->ebrake_in != 0 || !c->ps_button) {
            c->ps_exit_t = p->ps_exit_time;
            c->yaw_limit = p->ps_exit_yaw;
            c->ps = 0;
            if (!c->ps_button)
                c->ps_armed = 1;
        }
        return;
    }
    if (!c->ps_button)
        c->ps_armed = 1;
    if (fabsf(c->x) > p->ps_threshold && c->spd < p->ps_max_speed && c->spd > p->ps_min_speed &&
        c->ps_button && c->ps_armed) {
        c->ps = 1;
        c->ps_armed = c->ps_low = 0;
        c->ps_entry_x = c->x;
        c->ps_entry_speed = c->spd;
    } else if (fabsf(c->x) > p->ps_threshold && c->gas_in != 0 && c->spd < p->ps_min_speed &&
               c->x_prev * c->x >= 0) {
        c->ps_low = 1;
        c->ps = 0;
    }
    c->rear_cmd = 0;
    if (c->ps_exit_t <= 0) {
        set_lat(c, 1, 1);
        c->yaw_limit = p->yaw_rate_limit;
        c->boost = 1.0f;
        c->front_drive = 0;
    } else {
        c->ps_exit_t -= dt;
        c->yaw_limit = p->ps_exit_yaw;
        set_lat(c, p->ps_front_exit, p->ps_rear_exit);
    }
}

/* 0x382a00: EBrake. Started moving forward (or kept), front grip 1, rear HandBrakeFrictionScale
   until the car has turned 270 degrees. */
static void handbrake(Car *c, float dt)
{
    if (c->ebrake_in != 0) {
        if ((c->fwd > 0 && !c->hold) || c->hb_on) {
            c->lat_scale[0] = c->lat_scale[1] = 1.0f;
            if (fabsf(c->hb_yaw) < 4.71239f)
                c->lat_scale[2] = c->lat_scale[3] = c->p->handbrake_scale;
            c->hb_on = 1;
            c->hb_yaw += dt * dot3(c->angvel, c->rot + 3);
        }
    } else {
        c->hb_on = 0;
        c->hb_yaw = 0;
    }
}

/* 0x382b40: Tilt. On four wheels above 30 ft/s, no brake, stick past 0.3: up on two wheels on
   the stick's side (torque in action_forces), the engine at WheelStandTorqueScale. Letting go,
   slowing down or leaving the ground starts the way down (1 s at most). */
static void tilt(Car *c, int button, float dt)
{
    const CarParams *p = c->p;
    c->tilt_start = 0;
    if (c->tilt) {
        if (c->fwd < 30.0f || !button || c->grounded == 0) {
            if (!c->tilt_exit) {
                c->tilt_exit = 1;
                c->tilt_t = 1.0f;
            }
        }
        if (c->tilt_exit && (c->grounded == 4 || c->rot[4] >= 0.99f || c->tilt_t < 0)) {
            c->tilt = c->tilt_exit = 0;
            madd(c->angvel, c->rot + 6, -dot3(c->angvel, c->rot + 6)); /* roll rate zeroed */
            c->torque_scale = 1.0f;
        }
    } else if (c->grounded == 4 && c->fwd > 30.0f && c->brake_in == 0 && button && fabsf(c->x) > 0.3f) {
        c->tilt = c->tilt_start = 1;
        c->tilt_exit = 0;
        c->torque_scale = p->ws_torque_scale;
        c->tilt_side = c->x < 0 ? 1.0f : -1.0f;
    }
    c->tilt_t -= dt;
}

/* 0x386060: landing control. Set when all four wheels leave the ground (and kept only while the
   car flies roughly nose first); touching down, Landing*FrictionScale until all four are down,
   then Transition*FrictionScale until the car runs along its nose (|cos| > LandingExitAngle)
   with a yaw rate under LandingExitYaw. */
static void landing(Car *c)
{
    const CarParams *p = c->p;
    float cosv = c->spd > 1e-3f ? c->fwd / c->spd : 1.0f;
    if (c->grounded == 0)
        c->landing = 1;
    if (cosv < 0.707f)
        c->landing = 0;
    if (c->spd == 0)
        c->landing = 0;
    if (!c->landing)
        return;
    int all = c->grounded == 4;
    if (c->fwd > 0)
        set_lat(c, all ? p->trans_front : p->land_front, all ? p->trans_rear : p->land_rear);
    else
        set_lat(c, all ? p->trans_rear : p->land_rear, all ? p->trans_front : p->land_front);
    if (all && fabsf(cosv) > p->land_exit_angle && fabsf(dot3(c->angvel, c->rot + 3)) < p->land_exit_yaw)
        c->landing = 0;
}

/* 0x386790: the Backwards ability. Over 73.33 ft/s with the stick centred: EBrake, let go,
   EBrake again within 0.5 s (each step) and the car spins round keeping its velocity; moving
   tail first it then drives backwards (front-wheel drive, engine reversed, rear steering).
   Under 73.33 ft/s without brakes it spins back by itself. */
static void backwards_turn(Car *c, float dt)
{
    c->turn_dir = c->turn_dir < 0 ? 1.0f : -1.0f; /* +1180 flips every frame */
    c->turned = 0;
    if (c->backwards && c->spd < 73.333f && c->ebrake_in == 0 && c->brake_in == 0) {
        c->backwards = 0;
        c->turn_auto = 1;
    }
    int centred = c->x == 0;
    if (c->turn_state == 0 && c->ebrake_in != 0 && centred && c->spd > 73.333f) {
        c->turn_state = 1;
        c->turn_t = 0;
    } else if (c->turn_state == 1 && c->turn_t < 0.5f) {
        if (c->ebrake_in == 0 && centred) {
            c->turn_state = 2;
            c->turn_t = 0;
        }
    } else if ((c->turn_state == 2 && c->turn_t < 0.5f) || c->turn_auto) {
        if ((c->ebrake_in != 0 && centred && c->grounded > 0) || c->turn_auto) {
            c->turn_state = 3;
            c->turn_yaw = 0;
            memcpy(c->turn_vel, c->vel, sizeof c->turn_vel);
            c->turn_rpm = c->rpm;
            c->turn_sign = c->turn_dir; /* +1140 */
            c->turn_auto = 0;
            c->turned = 1; /* +1010 b7: the character's cool line */
        }
    } else if (c->turn_state == 3) {
        if (fabsf(c->turn_yaw) >= 1.5707963f) {
            c->turn_state = 0;
            madd(c->angvel, c->rot + 3, -dot3(c->angvel, c->rot + 3));
            if (c->grounded == 4)
                set_lat(c, 1, 1);
            c->backwards = car_speed(c) < 0;
            c->yaw_limit = c->p->yaw_rate_limit;
            c->rpm = c->turn_rpm;
            for (int i = 0; i < 4; i++)
                c->omega[i] = -c->omega[i];
        } else {
            /* the spin: velocity held, no grip, gas on. shortcut: the game sets a 6 rad/s spin
               through the rigid body (vtable +148) and stops after pi/2 of its own count; we
               spin at 12 rad/s so the same 0.26 s turns the car round */
            memcpy(c->vel, c->turn_vel, sizeof c->vel);
            madd(c->angvel, c->rot + 3, 12.0f * c->turn_sign - dot3(c->angvel, c->rot + 3));
            c->gas = 1.0f;
            c->brake_in = 0;
            set_lat(c, 0, 0);
            c->yaw_limit = 1e9f; /* the spin is set, not driven: past YawRateLimit */
            c->turn_yaw += 6.0f * dt;
        }
    } else {
        c->turn_state = 0;
        c->turn_t = 0;
    }
    c->turn_t += dt;
}

void car_step(Car *c, const ColWorld *w, const CarInput *in, float dt)
{
    const CarParams *p = c->p;
    if (dt > 0.1f)
        dt = 0.1f;
    /* the body's speeds as the game measures them */
    c->fwd = car_speed(c);
    c->lat = dot3(c->vel, c->rot);
    c->spd = sqrtf(dot3(c->vel, c->vel));
    c->slip_angle = c->spd > 1.0f ? atan2f(fabsf(c->lat), fabsf(c->fwd)) : 0.0f;

    read_input(c, in);
    c->ps_button = in->powerslide;
    controls(c, dt);
    reverse_arming(c);
    jump(c, dt);
    powerslide(c, dt);
    handbrake(c, dt);
    tilt(c, in->tilt, dt);
    landing(c);
    backwards_turn(c, dt);
    /* 0x383030: burnout timers (charge, then thrust while the gas is held) */
    if (c->burn_t > 0 && !c->burn_charge)
        c->burn_t -= dt;
    else if (c->burn_charge)
        c->burn_t -= dt;

    /* wheel angles: front by MaxSteerAngle, rear by PowerslideLowSpeedRearSteerAngle (the PS2
       loader gives the rear tyres that max angle); ReverseSteerScale backing up */
    float front = c->steer_cmd * p->tire[0].max_steer * DEG;
    if (c->fwd < -1.0f)
        front *= p->reverse_steer;
    c->steer = front;
    c->rear_steer = c->rear_cmd * p->ps_ls_rear_steer;

    float hub = drivetrain(c, dt);
    c->drive_slip = c->side_slip = c->strut_hit = c->impact = 0;
    int n = (int)ceilf(dt * 240.0f);
    if (n < 1) n = 1;
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++)
        substep(c, w, hub, dt / n);
}

void car_matrix(const Car *c, float out[16])
{
    const float *cg = c->p->cofg;
    for (int k = 0; k < 3; k++) {
        out[k] = c->rot[k];
        out[4 + k] = c->rot[3 + k];
        out[8 + k] = c->rot[6 + k];
        out[12 + k] = c->pos[k] - cg[0] * c->rot[k] - cg[1] * c->rot[3 + k] - cg[2] * c->rot[6 + k];
    }
    out[3] = out[7] = out[11] = 0;
    out[15] = 1;
}

float car_wheel_offset(const Car *c, int i)
{
    /* compression is measured from the bind pose: the wheel rises by it */
    return c->comp[i] > 0 ? fminf(c->comp[i], c->p->tire[i].max_comp) : 0;
}
