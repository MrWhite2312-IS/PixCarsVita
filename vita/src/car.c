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
/* McQueen's mcq.phy values, used for keys a .phy does not set */
static const CarParams MCQ_DEFAULTS = {
    .weight = 3000, .gravity_scale = 1.2f, .inertia_scale = { 0.5f, 1, 1 }, .cofg = { 0, -0.2f, 0.7f },
    .restitution = 0.2f, .body_friction = 0.2f, .yaw_rate_limit = 10, .normal_load_scale = 1,
    .handbrake_scale = 0.43f,
    .tire = {
        { 0.7f, 8, 1.4f, 1.25f, 0.3f, 0.3f, 35, { 1, 1.2f, 6.2f, 6.2f }, { 1.5f, 2, 10.2f, 1 }, 2000, 10 },
        { 0.7f, 8, 1.4f, 1.25f, 0.3f, 0.3f, 35, { 1, 1.2f, 6.2f, 6.2f }, { 1.5f, 2, 10.2f, 1 }, 2000, 10 },
        { 0.9f, 8, 1.4f, 1.45f, 0.3f, 0.3f, 0, { 1, 1.2f, 7.4f, 6 }, { 1.5f, 2, 13.6f, 1 }, 2000, 10 },
        { 0.9f, 8, 1.4f, 1.45f, 0.3f, 0.3f, 0, { 1, 1.2f, 7.4f, 6 }, { 1.5f, 2, 13.6f, 1 }, 2000, 10 },
    },
    .drivetrain = 1, .n_rpm = 8, .n_gears = 5,
    .rpm = { 0, 1000, 2000, 3000, 4000, 5000, 6000, 7000 },
    .hp = { 0, 115, 225, 325, 435, 495, 540, 495 },
    .gear = { 6, 3.2f, 2.4f, 1.7f, 1.7f }, .final_ratio = 1.95f, .loss = 18, .min_rpm = 100,
    .engine_braking = 10, .engine_inertia = 0.2f, .idle_throttle = 0.01f,
    .max_k = 2.5f, .pump_torque_scale = 0.1f, .max_pump_torque = 1000,
    .low_rpm = 1000, .low_up = 5500, .low_down = 2000, .high_rpm = 1500, .high_up = 6700, .high_down = 4500,
    .shift_up_time = 0.3f, .shift_up_throttle = 0.5f, .shift_down_time = 0.25f, .shift_down_throttle = 0.35f,
    .aero_area = 35, .aero_drag = 0.3f,
    .steer_power = 2, .x_rate = 25, .gas_rate = 4, .brake_rate = 20, .reverse_steer = 0.4f,
    .steer_speed = { 0, 80, 100 }, .steer_scale = { 0.6f, 0.23f, 0.28f },
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
    if (!t)
        return;
    const char *M = "Mass Properties";
    ini_f(t, M, "Weight", &p->weight, 1);
    ini_f(t, M, "GravityScale", &p->gravity_scale, 1);
    ini_f(t, M, "InertiaScale", p->inertia_scale, 3);
    ini_f(t, M, "CofG", p->cofg, 3);
    ini_f(t, M, "Restitution", &p->restitution, 1);
    ini_f(t, M, "BodyFriction", &p->body_friction, 1);
    ini_f(t, M, "YawRateLimit", &p->yaw_rate_limit, 1);
    ini_f(t, M, "NormalLoadScale", &p->normal_load_scale, 1);
    ini_f(t, "Powerslide", "HandBrakeFrictionScale", &p->handbrake_scale, 1);
    static const char *TN[4] = { "FrontLeftTire", "FrontRightTire", "RearLeftTire", "RearRightTire" };
    for (int i = 0; i < 4; i++) {
        TireParams *tp = &p->tire[i];
        char s[64];
        ini_f(t, TN[i], "MaxCompression", &tp->max_comp, 1);
        ini_f(t, TN[i], "MaxCompressionRate", &tp->max_comp_rate, 1);
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
    ini_f(t, E, "ShiftUpDuration", &p->shift_up_time, 1);
    ini_f(t, E, "ShiftUpThrottle", &p->shift_up_throttle, 1);
    ini_f(t, E, "ShiftDownDuration", &p->shift_down_time, 1);
    ini_f(t, E, "ShiftDownThrottle", &p->shift_down_throttle, 1);
    float cur = 0;
    ini_f(t, "Tune Ups", "CurrentTuneUp", &cur, 1);
    if (cur >= 1) {
        char s[32];
        snprintf(s, sizeof s, "Tune Up %d", (int)cur);
        ini_f(t, s, "AeroSurfaceArea", &p->aero_area, 1);
        ini_f(t, s, "AeroDrag", &p->aero_drag, 1);
    }
    const char *J = "Joystick";
    ini_f(t, J, "SteeringInputPower", &p->steer_power, 1);
    ini_f(t, J, "XAxisRateLimit", &p->x_rate, 1);
    ini_f(t, J, "GasRateLimit", &p->gas_rate, 1);
    ini_f(t, J, "BrakeRateLimit", &p->brake_rate, 1);
    ini_f(t, J, "ReverseSteerScale", &p->reverse_steer, 1);
    for (int i = 0; i < 3; i++) {
        char k[24];
        snprintf(k, sizeof k, "SteerSpeed%d", i + 1); ini_f(t, J, k, &p->steer_speed[i], 1);
        snprintf(k, sizeof k, "SteerScale%d", i + 1); ini_f(t, J, k, &p->steer_scale[i], 1);
    }
    ini_f(t, "Camera Properties", "CameraOffset", p->cam_offset, 3);
    ini_f(t, "Camera Properties", "CameraFollowingDistance", &p->cam_distance, 1);
}

/* ------------------------------------------------------------------ car */
#define G_FT 32.174f         /* ft/s^2; weights are in pounds, forces in lbf, mass in slugs */
#define RHO_AIR 0.00238f     /* slug/ft^3, sea level */
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

static float table(const TireCurve *c, float x, float xmax)
{
    float f = fabsf(x) / xmax * 64.0f;
    int i = (int)f;
    if (i >= 63)
        return curve(c, xmax * 63 / 64.0f);
    float a = curve(c, xmax * i / 64.0f), b = curve(c, xmax * (i + 1) / 64.0f);
    return a + (b - a) * (f - i);
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

/* Engine and torque converter as the PS2 game runs them (0x225810; setters 0x2250a0-0x2253a0).
   Returns the hub torque summed over the driven wheels (lb-ft); updates rpm and the gearbox. */
static float drivetrain(Car *c, float dt)
{
    const CarParams *p = c->p;
    int reverse = c->gear < 0;
    float dir = reverse ? -1.0f : 1.0f;
    float ratio = p->gear[reverse ? 0 : c->gear - 1] * p->final_ratio;
    float load = reverse ? c->brake : c->gas;
    int high = load > 0.5f;
    if (c->rpm < p->min_rpm)
        c->rpm = p->min_rpm;

    /* gearbox side: driven wheels' average spin through the current ratio (rad/s -> rpm) */
    float w = 0.5f * (c->omega[2] + c->omega[3]) * dir;
    float trans = RPM_PER_RADS * ratio * w;
    float slip = trans < 0 ? c->rpm : c->rpm - trans;
    /* pump torque k * slip^2 (k = PumpTorqueScale / 1000), limited to MaxPumpTorque */
    float k = p->pump_torque_scale * 0.001f;
    float pump = (slip < 0 ? -k : k) * slip * slip;
    pump = fmaxf(-p->max_pump_torque, fminf(p->max_pump_torque, pump));
    /* torque multiplication: maxK at stall, 1 when the turbine catches the engine */
    float mult = (c->rpm > trans && c->rpm > 0 && trans > 0)
                 ? p->max_k + (1.0f - p->max_k) * trans / c->rpm : p->max_k;
    float out = pump * mult;

    /* automatic shifts at the load-dependent RPMs (shortcut: the PS2 shift scheduler is not
       decoded; this uses the .phy thresholds on engine rpm with the shift durations) */
    if (!reverse && c->shift_t <= 0) {
        float up = high ? p->high_up : p->low_up, down = high ? p->high_down : p->low_down;
        /* turbine rpm the next lower gear would give: only drop if it stays under the up point */
        float lower = c->gear > 1 ? trans * p->gear[c->gear - 2] / p->gear[c->gear - 1] : 0;
        if (c->rpm > up && c->gear < p->n_gears) {
            c->gear++;
            c->shift_t = p->shift_up_time;
        } else if (c->rpm < down && c->gear > 1 && lower < up * 0.9f) {
            c->gear--;
            c->shift_t = -p->shift_down_time;
        }
    }
    float shift_scale = 1;
    if (c->shift_t > 0) {
        shift_scale = p->shift_up_throttle;
        c->shift_t = fmaxf(0, c->shift_t - dt);
    } else if (c->shift_t < 0) {
        shift_scale = p->shift_down_throttle;
        c->shift_t = fminf(0, c->shift_t + dt);
    }

    /* engine torque: gross curve (5252 HP / rpm + friction) times throttle (plus IdleThrottle),
       minus friction EngineBraking * 1e-6 * rpm^2 */
    float fr = p->engine_braking * 1e-6f;
    float rpm = c->rpm;
    float gross = interp(p->rpm, p->hp, p->n_rpm, rpm) * 5252.0f / fmaxf(rpm, 1.0f) + fr * rpm * rpm;
    float te = gross * fminf(load * shift_scale + p->idle_throttle, 1.0f) - fr * rpm * rpm;
    /* engine speed, implicit in the pump torque's slope */
    float inv_i = 1.0f / p->engine_inertia;
    float denom = 1.0f + dt * k * fabsf(slip) * inv_i;
    c->rpm += RPM_PER_RADS * dt * (te - pump) * inv_i / denom;
    c->rpm = fmaxf(p->min_rpm, fminf(p->rpm[p->n_rpm - 1], c->rpm));

    /* to each driven wheel: ratio, transmission loss, half the axle, at most 5000 lb-ft */
    float wheel = fmaxf(-5000.0f, fminf(5000.0f, out * ratio * (1.0f - p->loss / 100.0f) * 0.5f));
    return 2.0f * wheel * dir;
}

static void substep(Car *c, const ColWorld *w, float hub_torque, int handbrake, float h)
{
    const CarParams *p = c->p;
    float *r = c->rot, *u = c->rot + 3, *f = c->rot + 6;
    float m = mass(p);
    float F[3] = { 0, -p->weight * p->gravity_scale, 0 }, T[3] = { 0, 0, 0 };
    int grounded = 0;
    int driven_n = p->drivetrain == 2 ? 4 : 2;
    for (int i = 0; i < 4; i++) {
        const TireParams *tp = &p->tire[i];
        int rear = i >= 2;
        int driven = p->drivetrain == 2 || (p->drivetrain == 1 ? rear : !rear);
        float R = tp->radius;

        /* wheel spin (PS2 0x231e30 / 0x2326d0): drive + brake torque against the last
           longitudinal force, over TireInertia; at most 293.3 rad/s */
        float brake = (c->gear > 0 ? c->brake : 0);
        if (rear && handbrake)
            brake = 1;
        float spin_sign = fmaxf(-1.0f, fminf(1.0f, 0.5f * c->omega[i]));
        float torque = (driven ? hub_torque / driven_n : 0) - brake * tp->max_brake_torque * spin_sign;
        c->omega[i] += (torque - c->fx_prev[i] * R) / tp->inertia * h;
        c->omega[i] = fmaxf(-293.33f, fminf(293.33f, c->omega[i]));
        c->spin[i] += c->omega[i] * h;

        /* suspension (PS2 0x2317b0): ray from the bind-pose wheel centre down one tyre
           radius; compression 0 at the bind pose */
        float la[3], pm[3], d[3] = { -u[0], -u[1], -u[2] }, t, n[3];
        attach(p, i, la);
        to_world(c, la, pm);
        if (!col_raycast(w, pm, d, R, &t, n)) {
            c->comp[i] = 0;
            c->fx_prev[i] = 0;
            continue;
        }
        grounded++;
        float comp = R - t;
        float cp[3] = { pm[0] + d[0] * t, pm[1] + d[1] * t, pm[2] + d[2] * t }, rel[3], vc[3];
        sub3(cp, c->pos, rel);
        cross3(c->angvel, rel, vc);
        madd(vc, c->vel, 1);
        if (comp > tp->max_comp) {
            /* bottomed out: push the body out and stop it moving into the ground */
            madd(c->pos, n, fminf(comp - tp->max_comp, tp->max_comp));
            float vn = dot3(c->vel, n);
            if (vn < 0)
                madd(c->vel, n, -vn);
            comp = tp->max_comp;
        }
        float vs = fmaxf(-tp->max_comp_rate, fminf(tp->max_comp_rate, dot3(vc, n)));
        float k = spring_k(p, i);
        float damp = 2.0f * (vs > 0 ? tp->bounce_damp : tp->rebound_damp) * sqrtf(k * m);
        float fn = fmaxf(0.0f, k * comp - damp * vs);
        c->comp[i] = comp;
        float fv[3] = { n[0] * fn, n[1] * fn, n[2] * fn }, tq[3];
        madd(F, fv, 1);
        cross3(rel, fv, tq);
        madd(T, tq, 1);

        /* tyre frame on the ground plane (PS2 0x232350) */
        float s = i < 2 ? c->steer : 0, cs = cosf(s), sn = sinf(s);
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
        c->slip[i] = fmaxf(-0.98f, fminf(0.98f, c->slip[i]));

        /* forces (PS2 0x232a00): independent curves, no friction circle; the lateral load
           blends a quarter of the weight with the strut force by NormalLoadScale */
        float fx = -(c->slip[i] > 0 ? 1.0f : -1.0f) * table(&tp->drive, c->slip[i], 1.0f) * fn;
        float load = p->weight * 0.25f + (fn - p->weight * 0.25f) * p->normal_load_scale;
        float fy = -(alpha > 0 ? 1.0f : -1.0f) * table(&tp->side, alpha * 0.99f, 1.5707963f) * load;
        if (rear && handbrake)
            fy *= p->handbrake_scale; /* shortcut: assumed to scale the rear lateral grip (the
                                          vehicle code that sets the per-tyre scales is not decoded) */
        c->fx_prev[i] = fx;
        float ft[3] = { 0, 0, 0 };
        madd(ft, wf, fx);
        madd(ft, ws, fy);
        madd(F, ft, 1);
        cross3(rel, ft, tq);
        madd(T, tq, 1);
    }
    c->grounded = grounded;
    float sp = sqrtf(dot3(c->vel, c->vel));
    madd(F, c->vel, -0.5f * RHO_AIR * p->aero_drag * p->aero_area * sp);

    madd(c->vel, F, h / m);
    madd(c->pos, c->vel, h);
    /* angular: torque to body frame, divide by inertia (x pitch, y yaw, z roll), back to world */
    float I[3] = { m * (BOX[1] * BOX[1] + BOX[2] * BOX[2]) / 12 * p->inertia_scale[0],
                   m * (BOX[0] * BOX[0] + BOX[2] * BOX[2]) / 12 * p->inertia_scale[1],
                   m * (BOX[0] * BOX[0] + BOX[1] * BOX[1]) / 12 * p->inertia_scale[2] };
    madd(c->angvel, r, dot3(T, r) / I[0] * h);
    madd(c->angvel, u, dot3(T, u) / I[1] * h);
    madd(c->angvel, f, dot3(T, f) / I[2] * h);
    float yr = dot3(c->angvel, u);
    if (fabsf(yr) > p->yaw_rate_limit)
        madd(c->angvel, u, (yr > 0 ? p->yaw_rate_limit : -p->yaw_rate_limit) - yr);
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

void car_step(Car *c, const ColWorld *w, const CarInput *in, float dt)
{
    const CarParams *p = c->p;
    if (dt > 0.1f)
        dt = 0.1f;
    float speed = car_speed(c);
    /* reverse: brake held while (nearly) stopped engages reverse; gas engages drive */
    if (c->gear > 0 && in->brake > 0 && speed < 1.0f && in->throttle <= 0)
        c->gear = -1;
    else if (c->gear < 0 && (in->throttle > 0 || (in->brake <= 0 && speed > -1.0f)))
        c->gear = 1;
    c->gas = rate(c->gas, in->throttle, p->gas_rate, dt);
    c->brake = rate(c->brake, in->brake, p->brake_rate, dt);
    /* steering: input curve, rate limit, speed table (mph), max wheel angle */
    float x = in->steer;
    float shaped = (x < 0 ? -1.0f : 1.0f) * powf(fabsf(x), p->steer_power);
    c->steer_in = rate(c->steer_in, shaped, p->x_rate, dt);
    float scale = interp(p->steer_speed, p->steer_scale, 3, fabsf(speed) * MPH);
    if (speed < -1.0f)
        scale *= p->reverse_steer;
    c->steer = c->steer_in * scale * p->tire[0].max_steer * 3.14159265f / 180.0f;
    float hub = drivetrain(c, dt);
    int n = (int)ceilf(dt * 240.0f);
    if (n < 1) n = 1;
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++)
        substep(c, w, hub, in->handbrake, dt / n);
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
