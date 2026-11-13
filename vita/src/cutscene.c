/*
 * PixCarsVita - in-engine cutscene timeline (see cutscene.h).
 */
#include "cutscene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* the Subtitles layer fades its text in and out over TransitionTime (ui\subtitles.lyr) */
#define CAPTION_FADE 0.5f

static int find_track(Cutscene *c, const char *dir, const char *name)
{
    for (int i = 0; i < c->n_tracks; i++)
        if (!strcmp(c->track[i].name, name))
            return i;
    if (c->n_tracks >= CS_MAX_TRACKS)
        return -1;
    char path[256];
    snprintf(path, sizeof path, "%scam_%s.txt", dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    CsTrack *t = &c->track[c->n_tracks];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    if (fscanf(f, " fps %f n %d", &t->fps, &t->n) != 2 || t->n <= 0 || t->fps <= 0) {
        fclose(f);
        return -1;
    }
    t->rows = malloc(sizeof(float) * 10 * (size_t)t->n);
    if (!t->rows) {
        fclose(f);
        return -1;
    }
    for (int i = 0; i < t->n * 10; i++)
        if (fscanf(f, "%f", &t->rows[i]) != 1) {
            t->n = i / 10;
            break;
        }
    fclose(f);
    if (t->n <= 0) {
        free(t->rows);
        return -1;
    }
    return c->n_tracks++;
}

/* toy names are matched without case: the .toy says FLE_1_1A2 / Ram_1_1a where the .as actions
   name FLE_1_1a2 / RAM_1_1a */
static int find_toy(const Cutscene *c, const char *name)
{
    for (int i = 0; i < c->n_toys; i++)
        if (!strcasecmp(c->toy[i].name, name))
            return i;
    return -1;
}

int cutscene_load(Cutscene *c, const char *dir)
{
    memset(c, 0, sizeof *c);
    c->fade_out_t = 1e9f;
    char path[256], line[512];
    snprintf(path, sizeof path, "%scutscene.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char w[32] = "", a[64] = "", b[64] = "";
        float t, t1, d, h;
        int used = 0;
        if (sscanf(line, "%31s%n", w, &used) != 1)
            continue;
        const char *rest = line + used;
        if (!strcmp(w, "duration")) {
            sscanf(rest, "%f", &c->duration);
        } else if (!strcmp(w, "scene")) {
            sscanf(rest, "%15s", c->scene);
        } else if (!strcmp(w, "next")) {
            sscanf(rest, "%31s %31s", c->next, c->next_start);
        } else if (!strcmp(w, "voice")) {
            sscanf(rest, "%63s", c->voice);
        } else if (!strcmp(w, "toy") && c->n_toys < CS_MAX_TOYS) {
            CsToy *y = &c->toy[c->n_toys];
            if (sscanf(rest, "%31s %15s %f %f %f %f %f %f %f %f %f", y->name, y->code, &y->pos[0], &y->pos[1],
                       &y->pos[2], &y->fwd[0], &y->fwd[1], &y->fwd[2], &y->up[0], &y->up[1], &y->up[2]) == 11)
                c->n_toys++;
        } else if (!strcmp(w, "anim") && c->n_anims < CS_MAX_ANIMS) {
            if (sscanf(rest, "%f %63s %63s", &t, a, b) == 3) {
                CsAnim *x = &c->anim[c->n_anims];
                x->t = t;
                x->toy = find_toy(c, a);
                snprintf(x->vca, sizeof x->vca, "%s", b);
                if (x->toy >= 0)
                    c->n_anims++;
            }
        } else if (!strcmp(w, "cam") && c->n_cams < CS_MAX_CAMS) {
            if (sscanf(rest, "%f %63s", &t, a) == 2) {
                int k = find_track(c, dir, a);
                if (k >= 0) {
                    c->cam[c->n_cams].t = t;
                    c->cam[c->n_cams].track = k;
                    c->n_cams++;
                }
            }
        } else if (!strcmp(w, "caption") && c->n_caps < CS_MAX_CAPS) {
            int n2 = 0;
            if (sscanf(rest, "%f %f %n", &t, &t1, &n2) == 2) {
                CsCaption *x = &c->cap[c->n_caps++];
                x->t0 = t;
                x->t1 = t1;
                snprintf(x->text, sizeof x->text, "%s", rest + n2);
            }
        } else if (!strcmp(w, "fade")) {
            if (sscanf(rest, " in %f %f", &t, &d) == 2) {
                c->fade_in_t = t;
                c->fade_in_d = d;
            } else if (sscanf(rest, " out %f %f %f", &t, &d, &h) == 3) {
                c->fade_out_t = t;
                c->fade_out_d = d;
                c->fade_out_hold = h;
            }
        }
    }
    fclose(f);
    return c->n_cams > 0 ? 0 : -2;
}

void cutscene_free(Cutscene *c)
{
    for (int i = 0; i < c->n_tracks; i++)
        free(c->track[i].rows);
    c->n_tracks = 0;
}

int cutscene_camera(const Cutscene *c, float t, float eye[3], float look[3], float up[3], float *fov)
{
    int k = -1;
    for (int i = 0; i < c->n_cams; i++)
        if (c->cam[i].t <= t && (k < 0 || c->cam[i].t >= c->cam[k].t))
            k = i;
    if (k < 0)
        k = 0;
    const CsTrack *tr = &c->track[c->cam[k].track];
    float f = (t - c->cam[k].t) * tr->fps;
    if (f < 0)
        f = 0;
    int i0 = (int)f;
    if (i0 >= tr->n - 1) {
        i0 = tr->n - 1;
        f = (float)i0;
    }
    int i1 = i0 + 1 < tr->n ? i0 + 1 : i0;
    float u = f - (float)i0;
    const float *a = tr->rows + i0 * 10, *b = tr->rows + i1 * 10;
    float v[10];
    for (int j = 0; j < 10; j++)
        v[j] = a[j] + (b[j] - a[j]) * u;
    memcpy(eye, v, 3 * sizeof(float));
    memcpy(look, v + 3, 3 * sizeof(float));
    memcpy(up, v + 6, 3 * sizeof(float));
    *fov = v[9];
    return 1;
}

int cutscene_toy_anim(const Cutscene *c, int toy, float t, float *local_t)
{
    int k = -1;
    for (int i = 0; i < c->n_anims; i++)
        if (c->anim[i].toy == toy && c->anim[i].t <= t && (k < 0 || c->anim[i].t >= c->anim[k].t))
            k = i;
    if (k < 0) /* before its first motion: the first one's first frame (InitialFrame) */
        for (int i = 0; i < c->n_anims; i++)
            if (c->anim[i].toy == toy && (k < 0 || c->anim[i].t < c->anim[k].t))
                k = i;
    *local_t = k >= 0 && t > c->anim[k].t ? t - c->anim[k].t : 0;
    return k;
}

const char *cutscene_caption(const Cutscene *c, float t, float *alpha)
{
    const CsCaption *best = NULL;
    for (int i = 0; i < c->n_caps; i++)
        if (t >= c->cap[i].t0 && t < c->cap[i].t1 + CAPTION_FADE && (!best || c->cap[i].t0 > best->t0))
            best = &c->cap[i];
    if (!best)
        return NULL;
    float in = (t - best->t0) / CAPTION_FADE, out = (best->t1 + CAPTION_FADE - t) / CAPTION_FADE;
    float a = in < out ? in : out;
    *alpha = a < 0 ? 0 : (a > 1 ? 1 : a);
    return best->text;
}

float cutscene_black(const Cutscene *c, float t)
{
    float k = 0;
    if (c->fade_in_d > 0 && t < c->fade_in_t + c->fade_in_d)
        k = 1.0f - (t - c->fade_in_t) / c->fade_in_d;
    if (t >= c->fade_out_t) {
        float o = c->fade_out_d > 0 ? (t - c->fade_out_t) / c->fade_out_d : 1.0f;
        k = o > k ? o : k;
    }
    return k < 0 ? 0 : (k > 1 ? 1 : k);
}
