/*
 * PixCarsVita - scene lighting. See light.h.
 */
#include "light.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG 0.0174533f

int lights_parse(SceneLights *l, const char *t)
{
    memset(l, 0, sizeof *l);
    const char *line = t;
    while (line && *line) {
        float v[15];
        if (sscanf(line, "ambient %f %f %f", &v[0], &v[1], &v[2]) == 3) {
            memcpy(l->ambient, v, 12);
        } else if (l->n_dir < LIGHT_MAX_DIR &&
                   sscanf(line, "dir %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
            memcpy(l->dir[l->n_dir], v, 12);
            memcpy(l->dir_col[l->n_dir], v + 3, 12);
            l->n_dir++;
        } else if (l->n_spot < LIGHT_MAX_SPOT &&
                   sscanf(line, "spot %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3],
                          &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14]) == 15) {
            SpotLight *s = &l->spot[l->n_spot++];
            memcpy(s->pos, v, 12);
            memcpy(s->dir, v + 3, 12);
            s->inner = v[6] * DEG;
            s->outer = v[7] * DEG;
            memcpy(s->att, v + 8, 12);
            s->range = v[11];
            memcpy(s->col, v + 12, 12);
        } else if (sscanf(line, "fog %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
            l->has_fog = 1;
            memcpy(l->fog_col, v, 12);
            l->fog_min = v[3];
            l->fog_max = v[4];
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return 0;
}

int lights_load(SceneLights *l, const char *path)
{
    memset(l, 0, sizeof *l);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (!s) {
        fclose(f);
        return -2;
    }
    s[fread(s, 1, (size_t)n, f)] = 0;
    fclose(f);
    int r = lights_parse(l, s);
    free(s);
    return r;
}

/* D3D-style spot: cone falloff between the half inner and half outer angles, attenuation
   1 / (a0 + a1 d + a2 d^2). shortcut: the engine's own pick of lights per object (Cars.exe is
   encrypted, the PS2 does it in VU1) is not decoded; the strongest spots fill the free slots. */
static float spot_strength(const SpotLight *s, const float pos[3], float L[3])
{
    float p[3] = { pos[0] - s->pos[0], pos[1] - s->pos[1], pos[2] - s->pos[2] };
    float d = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (d < 1e-3f || (s->range > 0 && d > s->range))
        return 0;
    for (int k = 0; k < 3; k++)
        L[k] = -p[k] / d;
    float c = -(L[0] * s->dir[0] + L[1] * s->dir[1] + L[2] * s->dir[2]);
    float ci = cosf(s->inner * 0.5f), co = cosf(s->outer * 0.5f);
    float cone = c >= ci ? 1.0f : (c <= co ? 0.0f : (c - co) / (ci - co));
    float den = s->att[0] + s->att[1] * d + s->att[2] * d * d;
    return den > 1e-6f ? cone / den : cone;
}

void lights_for(const SceneLights *l, const float pos[3], ObjectLights *o)
{
    memcpy(o->ambient, l->ambient, 12);
    o->n = 0;
    for (int i = 0; i < l->n_dir && o->n < 3; i++, o->n++) {
        memcpy(o->dir[o->n], l->dir[i], 12);
        memcpy(o->col[o->n], l->dir_col[i], 12);
    }
    float best[3] = { 0, 0, 0 };
    int pick[3] = { -1, -1, -1 };
    float pl[3][3];
    int free_slots = 3 - o->n;
    for (int i = 0; i < l->n_spot; i++) {
        float L[3], w = spot_strength(&l->spot[i], pos, L);
        const float *c = l->spot[i].col;
        float mag = w * (fabsf(c[0]) + fabsf(c[1]) + fabsf(c[2]));
        for (int k = 0; k < free_slots; k++)
            if (mag > best[k]) {
                for (int j = free_slots - 1; j > k; j--) {
                    best[j] = best[j - 1];
                    pick[j] = pick[j - 1];
                    memcpy(pl[j], pl[j - 1], 12);
                }
                best[k] = mag;
                pick[k] = i;
                memcpy(pl[k], L, 12);
                break;
            }
    }
    for (int k = 0; k < free_slots; k++) {
        if (pick[k] < 0)
            break;
        float L[3], w = spot_strength(&l->spot[pick[k]], pos, L);
        memcpy(o->dir[o->n], pl[k], 12);
        for (int c = 0; c < 3; c++)
            o->col[o->n][c] = l->spot[pick[k]].col[c] * w;
        o->n++;
    }
}

void lights_shade(const ObjectLights *o, const float n[3], float rgb[3])
{
    for (int c = 0; c < 3; c++)
        rgb[c] = o->ambient[c];
    for (int i = 0; i < o->n; i++) {
        float d = n[0] * o->dir[i][0] + n[1] * o->dir[i][1] + n[2] * o->dir[i][2];
        if (d > 0)
            for (int c = 0; c < 3; c++)
                rgb[c] += d * o->col[i][c];
    }
    for (int c = 0; c < 3; c++)
        rgb[c] = rgb[c] < 0 ? 0 : (rgb[c] > 1 ? 1 : rgb[c]);
}
