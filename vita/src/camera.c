/*
 * PixCarsVita - the game's follow camera. See camera.h.
 *
 * Decoded from the PS2 build (SLUS_211.51), CarsCameraFollow:
 *   preset loader     0x223bc0  (AngleX/AngleY deg -> rad at 0x2243c4; nothing else converted)
 *   update            0x21f4f0  heading, body point, offset direction, distance
 *   angles            0x21d400  (vtable +260) AngleY - swing, AngleX low-speed scaling
 *   look point        0x21d870  (vtable +264)
 *   eye point         0x21dcf0  (vtable +268)
 *   filtering         0x21e390  (vtable +280) per-axis filters, distance clamp
 */
#include "camera.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PI 3.14159265f

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
        while (*v == 'f' || *v == ' ' || *v == '\t' || *v == ',')
            v++;
    }
}

static void ini_b(const char *t, const char *sec, const char *key, int *out)
{
    const char *v = ini_find(t, sec, key);
    if (!v)
        return;
    while (*v == ' ' || *v == '\t')
        v++;
    *out = strncasecmp(v, "true", 4) == 0 || atoi(v) != 0;
}

static void read_section(CamPreset *p, const char *t, const char *s)
{
    float ax = -1000, ay = -1000;
    ini_f(t, s, "AngleX", &ax, 1);
    ini_f(t, s, "AngleY", &ay, 1);
    if (ax > -1000)
        p->angle_x = ax * PI / 180.0f;
    if (ay > -1000)
        p->angle_y = ay * PI / 180.0f;
    ini_f(t, s, "FollowingDistance", &p->dist, 1);
    ini_f(t, s, "MinFollowingDistance", &p->min_dist, 1);
    ini_f(t, s, "MaxFollowingDistance", &p->max_dist, 1);
    ini_f(t, s, "FOV", &p->fov, 1);
    ini_f(t, s, "MinFOV", &p->min_fov, 1);
    ini_f(t, s, "MaxFOV", &p->max_fov, 1);
    ini_f(t, s, "LookAheadDistance", &p->look_ahead, 1);
    ini_b(t, s, "CycleThruThisPreset", &p->cycle);
    ini_b(t, s, "SwingEnabled", &p->swing);
    ini_f(t, s, "CameraSwing", &p->cam_swing, 1);
    ini_f(t, s, "LookSwing", &p->look_swing, 1);
    ini_f(t, s, "LookFilters", p->look_f, 3);
    ini_f(t, s, "PosFilters", p->pos_f, 3);
    ini_f(t, s, "LowSpeedAngleXVelCutoff", &p->ls_cutoff, 1);
    ini_f(t, s, "LowSpeedAngleXScaleFactor", &p->ls_scale, 1);
    ini_f(t, s, "OffsetFromBodyPoint", p->body_offset, 3);
    ini_f(t, s, "BodyLookOffset", p->body_look, 3);
    ini_f(t, s, "MinAngleYLookAheadAngle", &p->min_ay_la, 1);
    ini_f(t, s, "MaxAngleYLookAheadAngle", &p->max_ay_la, 1);
    ini_f(t, s, "VelocityAdjustment", &p->velocity_adjust, 1);
}

int followcam_load(FollowCam *c, const char *ini, const char *code)
{
    memset(c, 0, sizeof *c);
    float n = 2;
    ini_f(ini, "NumberOfPresets", "NumberOfPresets", &n, 1);
    c->n = (int)n;
    if (c->n < 1)
        c->n = 1;
    if (c->n > CAM_MAX_PRESETS)
        c->n = CAM_MAX_PRESETS;
    for (int i = 0; i < c->n; i++) {
        CamPreset *p = &c->p[i];
        /* values for keys neither section sets (rscamera.ini's Preset1 sets them all) */
        p->angle_y = PI;
        p->angle_x = 28.0f * PI / 180.0f;
        p->dist = p->min_dist = p->max_dist = 22;
        p->fov = 75, p->min_fov = 40, p->max_fov = 110;
        p->cycle = 1;
        for (int k = 0; k < 3; k++)
            p->look_f[k] = p->pos_f[k] = 0.15f;
        p->min_ay_la = 85, p->max_ay_la = 120;
        p->ls_scale = 1;
        char sec[32];
        snprintf(sec, sizeof sec, "Preset%d", i + 1);
        read_section(p, ini, sec);
        snprintf(sec, sizeof sec, "%s%d", code, i + 1); /* [McQ1]: matched without case */
        read_section(p, ini, sec);
    }
    return c->n;
}

int followcam_next(FollowCam *c)
{
    for (int i = c->cur + 1; i < c->n; i++)
        if (c->p[i].cycle) {
            c->cur = i;
            return 1;
        }
    for (int i = 0; i < c->n; i++)
        if (c->p[i].cycle) {
            c->cur = i;
            break;
        }
    return 0;
}

void followcam_reset(FollowCam *c)
{
    c->init = 0;
}

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float wrap_pi(float a)
{
    while (a > PI)
        a -= 2 * PI;
    while (a < -PI)
        a += 2 * PI;
    return a;
}

/* rotation about +Y as the PS2's 0x13de10 does it: atan2(x, z) grows by a */
static void rot_y(const float v[3], float a, float out[3])
{
    float c = cosf(a), s = sinf(a), x = v[0], z = v[2];
    out[0] = x * c + z * s;
    out[1] = v[1];
    out[2] = -x * s + z * c;
}

/* the filter object (0x21e1f0 sets it, applied at 0x21f180): time constant tau seconds */
static float filt(float v, float target, float tau, float dt)
{
    if (tau <= 0)
        return target;
    return v + (target - v) * (fminf(dt, tau) / tau);
}

void followcam_update(FollowCam *c, const float car_pos[3], const float rot[9], const float vel[3], float dt,
                      float eye[3], float look[3], float *fov)
{
    const CamPreset *p = &c->p[c->cur];
    const float *up = rot + 3, *fwd = rot + 6;
    float speed = len3(vel);
    if (dt <= 0)
        dt = 1.0f / 60.0f;

    /* heading (0x21f5d4): the velocity above 20 ft/s while going forward, else the body's
       forward; first as a 3D direction (for the body point), then flattened */
    float h3[3];
    if (speed > 20.0f && dot3(fwd, vel) >= 0)
        memcpy(h3, vel, sizeof h3);
    else
        memcpy(h3, fwd, sizeof h3);
    float l = len3(h3);
    if (l < 1e-4f) {
        h3[0] = 0, h3[1] = 0, h3[2] = 1, l = 1;
    }
    for (int k = 0; k < 3; k++)
        h3[k] /= l;

    /* body point (0x21f8a0): OffsetFromBodyPoint along the heading, the car's up and their side */
    const float *o = p->body_offset;
    float side[3] = { up[1] * h3[2] - up[2] * h3[1], up[2] * h3[0] - up[0] * h3[2], up[0] * h3[1] - up[1] * h3[0] };
    float body[3];
    for (int k = 0; k < 3; k++)
        body[k] = car_pos[k] + side[k] * o[0] + up[k] * o[1] + h3[k] * o[2];

    float h[3] = { h3[0], 0, h3[2] };
    l = sqrtf(h[0] * h[0] + h[2] * h[2]);
    if (l < 1e-4f) {
        h[0] = fwd[0], h[2] = fwd[2];
        l = sqrtf(h[0] * h[0] + h[2] * h[2]);
        if (l < 1e-4f)
            h[0] = 0, h[2] = 1, l = 1;
    }
    h[0] /= l, h[2] /= l;
    /* upside down (car up.y < -0.1): keep the last heading (0x21ff44) */
    if (c->init && up[1] < -0.1f)
        memcpy(h, c->heading, sizeof h);
    memcpy(c->heading, h, sizeof h);

    /* yaw rate of the car (the target's +132), as d/dt of atan2(x, z) */
    float yaw = atan2f(fwd[0], fwd[2]);
    c->yaw_rate = c->init ? wrap_pi(yaw - c->yaw_prev) / dt : 0;
    c->yaw_prev = yaw;
    float fov_ratio = p->max_fov > 0 ? p->fov / p->max_fov : 1;
    float w = 0.21f * c->yaw_rate * fov_ratio; /* 0x21fb64: 0.21 * -(+132) * FOV / MaxFOV */

    /* angles (0x21d400): AngleY swung by the yaw rate, AngleX scaled down at low speed */
    float ay = wrap_pi(p->angle_y + (p->swing ? p->cam_swing : 0) *
                       clampf((fabsf(wrap_pi(p->angle_y)) - PI / 2) / (2.0944f - PI / 2), 0, 1) * w);
    float ax = clampf(p->angle_x, -1.4826f, 1.4826f);
    if (p->ls_cutoff != 0 && speed < p->ls_cutoff)
        ax *= p->ls_scale + speed / p->ls_cutoff * (1 - p->ls_scale);

    /* offset direction: the heading pitched up by AngleX, then turned by AngleY about +Y */
    float d0[3] = { h[0] * cosf(ax), sinf(ax), h[2] * cosf(ax) }, dir[3];
    rot_y(d0, ay, dir);

    /* distance (0x220570): FollowingDistance within Min/Max, plus VelocityAdjustment by speed */
    float dist = clampf(p->dist, p->min_dist, p->max_dist > p->min_dist ? p->max_dist : p->min_dist);
    dist += clampf(speed / 100.0f * p->velocity_adjust, fminf(0, p->velocity_adjust), fmaxf(0, p->velocity_adjust));

    /* the tracked point leads the body point by the velocity over min(dt, 0.2) (0x2207e4) */
    float tgt[3];
    for (int k = 0; k < 3; k++)
        tgt[k] = body[k] + vel[k] * fminf(0.2f, dt);
    float want_eye[3];
    for (int k = 0; k < 3; k++)
        want_eye[k] = tgt[k] + dir[k] * dist;

    /* look point (0x21d870). The look-ahead's AngleY factor compares the angle in radians with
       Min/MaxAngleYLookAheadAngle as read (85, 120): it is 0 for every shipped preset, as on the
       PS2. vmax: the target's +184; shortcut: the constructor's 293.33 ft/s (200 mph) */
    const float vmax = 293.33f;
    float la = (speed / vmax) * (1 + dist * fov_ratio / 90.0f) * (1 + dist * fov_ratio / 90.0f) *
               1.8f * (1 - dist / 600.0f);
    if (vel[1] < -9.0f && speed > 0)
        la *= 1 + vel[1] / speed / 1.8f;
    float la_range = p->max_ay_la - p->min_ay_la;
    float ay_t = la_range != 0 ? (fabsf(ay) - p->min_ay_la) / la_range : 0;
    la = fmaxf(0, la * p->look_ahead) * clampf(ay_t, 0, 1);
    float ld[3];
    rot_y(h, (p->swing ? p->look_swing : 0) * 0.7f * w, ld);
    float want_look[3];
    for (int k = 0; k < 3; k++)
        want_look[k] = tgt[k] + ld[k] * la + h[k] * p->body_look[2] + up[k] * p->body_look[1];
    want_look[1] += (1 - fov_ratio) * 0.25f * vel[1];

    if (!c->init) {
        memcpy(c->pos, want_eye, sizeof c->pos);
        memcpy(c->look, want_look, sizeof c->look);
        c->init = 1;
    }

    /* filters (0x21e390): LookFilters / PosFilters per world axis, scaled from 1 at rest to
       f8 above 20 mph; f8 again compares AngleY in radians with the degree values: 0.25 */
    float f8 = clampf(0.25f + ay_t * 0.75f, 0.25f, 1.0f);
    float sc = 1 + clampf(speed / 29.3333f, 0, 1) * (f8 - 1);
    for (int k = 0; k < 3; k++) {
        c->look[k] = filt(c->look[k], want_look[k], p->look_f[k] * sc, dt);
        c->pos[k] = filt(c->pos[k], want_eye[k], p->pos_f[k] * sc, dt);
    }
    /* then the eye is held between Min and MaxFollowingDistance of the body point (0x21f2a4) */
    float r[3] = { c->pos[0] - body[0], c->pos[1] - body[1], c->pos[2] - body[2] };
    float rl = len3(r);
    if (rl > 1e-4f) {
        float d = clampf(rl, p->min_dist, p->max_dist > p->min_dist ? p->max_dist : p->min_dist);
        for (int k = 0; k < 3; k++)
            c->pos[k] = body[k] + r[k] / rl * d;
    }
    /* shortcut: not ported yet: camera collision and sink/fade (0x221d00), boost FOV and shake,
       the reverse/look-back and airborne cases */
    memcpy(eye, c->pos, sizeof c->pos);
    memcpy(look, c->look, sizeof c->look);
    *fov = p->fov;
}

/* ------------------------------------------------------------------ first-person camera */
int fpcam_load(FirstPersonCam *c, const char *ini)
{
    memset(c, 0, sizeof *c);
    c->fov = 90; /* fpcam.ini [1Player] values as defaults */
    c->offset[1] = 3.8f;
    c->look_f[0] = 0.04f, c->look_f[1] = c->look_f[2] = 0.05f;
    if (!ini || !ini_find(ini, "1Player", "FOV"))
        return 0;
    ini_f(ini, "1Player", "FOV", &c->fov, 1);
    ini_f(ini, "1Player", "OffsetFromBodyPoint", c->offset, 3);
    ini_f(ini, "1Player", "LookFilter", c->look_f, 3);
    return 1;
}

void fpcam_reset(FirstPersonCam *c)
{
    c->init = 0;
}

void fpcam_update(FirstPersonCam *c, const float car_pos[3], const float rot[9], float dt,
                  float eye[3], float look[3], float *fov)
{
    const float *r = rot, *u = rot + 3, *f = rot + 6;
    for (int k = 0; k < 3; k++)
        eye[k] = car_pos[k] + r[k] * c->offset[0] + u[k] * c->offset[1] + f[k] * c->offset[2];
    /* the nose direction, filtered per axis (snapped on the first frame) */
    for (int k = 0; k < 3; k++)
        c->look[k] = c->init ? filt(c->look[k], f[k], c->look_f[k], dt) : f[k];
    c->init = 1;
    float l = len3(c->look);
    for (int k = 0; k < 3; k++)
        look[k] = eye[k] + (l > 1e-6f ? c->look[k] / l : f[k]) * 10.0f;
    *fov = c->fov;
}

