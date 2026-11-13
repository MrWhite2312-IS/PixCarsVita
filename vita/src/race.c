/*
 * PixCarsVita - races, ported from the PS2 game (see race.h). Each function names the PS2
 * function it follows. Not ported yet (left out, not approximated): the off-track / wrong-way
 * respawn (0x22c420 / 0x22c6f0), the AI's car avoidance (0x4062a0), rubber banding needs the
 * player's racer (0x389b10, ported), the AI powerslide / jump / drive-backwards actions
 * (0x404b50 / 0x404860: only the boost is ported).
 */
#include "race.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPH_FTS 1.46666f /* the game's mph -> ft/s (RacePath 0x38c990, AI parse 0x403e20) */
#define PI_F 3.1415927f

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(const float a[3], const float b[3], float o[3])
{
    o[0] = a[0] - b[0];
    o[1] = a[1] - b[1];
    o[2] = a[2] - b[2];
}
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
static float minf_(float a, float b) { return b < a ? b : a; }
static float maxf_(float a, float b) { return b >= a ? b : a; }

/* ------------------------------------------------------------------ rand */
/* the game's rand (0x47fb18): newlib's 64-bit LCG, top 31 bits. shortcut: the game seeds it from
   the console clock (0x47fb00 with the RTC); the port seeds from race_ai_init's seed. */
static unsigned long long g_rand = 1;
static int game_rand(void)
{
    g_rand = g_rand * 6364136223846793005ULL + 1;
    return (int)((g_rand >> 32) & 0x7fffffff);
}
static float rand01(void) { return (float)game_rand() * 4.656613e-10f; }

/* ------------------------------------------------------------------ loading */
static int read_list(const char **s, int *out, int max)
{
    int n = 0, used;
    if (sscanf(*s, "%d%n", &n, &used) != 1)
        return 0;
    *s += used;
    for (int i = 0; i < n; i++) {
        int v;
        if (sscanf(*s, "%d%n", &v, &used) != 1)
            return i < max ? i : max;
        *s += used;
        if (i < max)
            out[i] = v;
    }
    return n < max ? n : max;
}

static int read_vecs(const char **s, float *v, int count)
{
    for (int i = 0; i < count; i++) {
        int used;
        if (sscanf(*s, "%f%n", &v[i], &used) != 1)
            return -1;
        *s += used;
    }
    return 0;
}

/* the game's boolean keys (FUN_001638d0): a number, or T/TRUE/F/FALSE */
static int key_bool(const char *v)
{
    if (*v == 'T' || *v == 't')
        return 1;
    if (*v == 'F' || *v == 'f')
        return 0;
    return atoi(v) != 0;
}

/* a racer's [VehicleN] key, as the AI parsers (0x388b50, 0x403e20) read it */
static void racer_key(RaceRacer *x, const char *k, const char *v, int *has_min_pace)
{
    float f = (float)atof(v);
    if (!strcmp(k, "StartLineOrder"))
        x->order = atoi(v) - 1; /* 1-based: RR_1_1's 1..5 fill its one row of 5 as CS_1_1C ends */
    else if (!strcmp(k, "StartDelay"))
        x->start_delay = f;
    else if (!strcmp(k, "UseALOD"))
        x->use_alod = key_bool(v);
    else if (!strcmp(k, "MaxPacePercentModifier"))
        x->max_pace = f / 100.0f;
    else if (!strcmp(k, "MinPacePercentModifier"))
        x->min_pace = f / 100.0f, *has_min_pace = 1;
    else if (!strcmp(k, "MaxOffsetDist"))
        x->max_offset = f;
    else if (!strcmp(k, "TimeToSwitchOffset"))
        x->offset_time = f;
    else if (!strcmp(k, "PathID"))
        x->path_id = atoi(v);
    else if (!strcmp(k, "DontSkipNodes"))
        x->dont_skip = key_bool(v);
    else if (!strcmp(k, "MaxTimeOffPath"))
        x->max_time_off = f;
    else if (!strcmp(k, "MinimumLookAheadDist"))
        x->min_look = f;
    else if (!strcmp(k, "LookAheadFactor"))
        x->look_factor = f;
    else if (!strcmp(k, "MaxPathCheckDist"))
        x->max_check = f;
    else if (!strcmp(k, "Min180ReverseSpeed"))
        x->min180 = f;
    else if (!strcmp(k, "SteeringMultiplier"))
        x->steer_mult = f;
    else if (!strcmp(k, "SteeringBoost"))
        x->steer_boost = f;
    else if (!strcmp(k, "PowerslideLookAheadDist"))
        x->ps_look = f;
    else if (!strcmp(k, "MinPowerSlideSpeed"))
        x->min_ps_speed = f * MPH_FTS;
    else if (!strcmp(k, "EnablePowerSlide"))
        x->flags = key_bool(v) ? x->flags | 1 : x->flags & ~1u;
    else if (!strcmp(k, "EnableJump"))
        x->flags = key_bool(v) ? x->flags | 2 : x->flags & ~2u;
    else if (!strcmp(k, "EnableBoost"))
        x->flags = key_bool(v) ? x->flags | 4 : x->flags & ~4u;
    else if (!strcmp(k, "EnableDriveBackwards"))
        x->flags = key_bool(v) ? x->flags | 8 : x->flags & ~8u;
    else if (!strcmp(k, "EnableRandomOffset"))
        x->flags = key_bool(v) ? x->flags | 0x10 : x->flags & ~0x10u;
}

/* defaults: AI ctor 0x3889d0 and the parsers' default arguments (0x388b50, 0x403e20) */
static void racer_defaults(RaceRacer *x)
{
    memset(x, 0, sizeof *x);
    x->order = -1;
    x->max_pace = x->min_pace = 0.1f;
    x->max_offset = 5;
    x->offset_time = 10;
    x->min_look = 60;
    x->max_check = 600;
    x->min180 = 65;
    x->steer_mult = 1.2f;
    x->steer_boost = 10;
    x->ps_look = 125;
    x->min_ps_speed = 95 * MPH_FTS;
}

int race_load(Race *r, const char *path, const char *difficulty)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    memset(r, 0, sizeof *r);
    r->laps = 1;              /* TotalLaps default (0x429970) */
    r->first_lap_pace = 4.0f; /* FirstLapPaceModifier default (0x32ef20) */
    r->valid_skip = 350.0f;   /* ValidSkipDistance: race ctor (0x429828) */
    r->per_row = 3;
    r->slot_w = 10.5f;
    r->row_sep = 100;
    int has_min[RACE_MAX_RACERS] = { 0 };
    char line[1024], w[32];
    while (fgets(line, sizeof line, f)) {
        int used;
        if (sscanf(line, "%31s%n", w, &used) != 1)
            continue;
        const char *s = line + used;
        if (!strcmp(w, "act")) {
            char k[64], v[256] = "";
            if (sscanf(s, "%63s %255[^\r\n]", k, v) < 1)
                continue;
            if (!strcmp(k, "TotalLaps"))
                r->laps = atoi(v);
            else if (!strcmp(k, "NumRacers"))
                r->nr = atoi(v);
            else if (!strcmp(k, "HasPaceLap"))
                r->has_pace_lap = key_bool(v);
            else if (!strcmp(k, "DisableNOX"))
                r->disable_nox = key_bool(v);
            else if (!strcmp(k, "FirstLapPaceModifier"))
                r->first_lap_pace = (float)atof(v);
            else if (!strcmp(k, "ValidSkipDistance"))
                r->valid_skip = (float)atof(v);
        } else if (!strcmp(w, "grid")) {
            sscanf(s, "%d %f %f %d %d", &r->per_row, &r->slot_w, &r->row_sep, &r->start_node, &r->finish_node);
        } else if (!strcmp(w, "bnode")) {
            int i, zone, noreset, progress;
            if (sscanf(s, "%d%n", &i, &used) != 1 || i < 0 || i >= RACE_MAX_NODES)
                continue;
            s += used;
            RaceBNode *n = &r->b[i];
            if (read_vecs(&s, n->p, 3) || read_vecs(&s, n->l, 3) || read_vecs(&s, n->r, 3))
                continue;
            if (sscanf(s, "%f %d %d %d %d%n", &n->dist, &zone, &n->branch, &noreset, &progress, &used) != 5)
                continue;
            s += used;
            n->zone = zone - 1;
            n->flags = noreset ? 1 : 0;
            if (progress && r->n_progress < 10)
                r->progress[r->n_progress++] = i;
            n->n_next = read_list(&s, n->next, 4);
            n->n_prev = read_list(&s, n->prev, 4);
            if (i + 1 > r->nb)
                r->nb = i + 1;
        } else if (!strcmp(w, "path")) {
            int p, nn, ns, lin, start, tc;
            if (sscanf(s, "%d %d %d %d %d %d", &p, &nn, &ns, &lin, &start, &tc) == 6 && p == 0) {
                r->p_linear = lin;
                r->times_computed = tc;
            }
        } else if (!strcmp(w, "seg")) {
            int p, i, sec, has_t;
            if (sscanf(s, "%d %d%n", &p, &i, &used) != 2 || p != 0 || i < 0 || i >= RACE_MAX_SEGS)
                continue;
            s += used;
            RacePSeg *g = &r->s[i];
            if (sscanf(s, "%d %d %d %d %f %d%n", &g->start, &g->finish, &sec, &has_t, &g->time, &g->n_branch,
                       &used) != 6)
                continue;
            s += used;
            if (has_t)
                r->times_computed = 1; /* SegmentTime read: path +0x5c (0x38c990) */
            g->section = (sec - 1 >= 0 && sec - 1 < 32) ? 1u << (sec - 1) : 0;
            if (g->n_branch > 4)
                g->n_branch = 4;
            for (int b = 0; b < g->n_branch; b++) {
                int nt;
                if (sscanf(s, "%d %d%n", &g->branch[b], &nt, &used) != 2)
                    break;
                s += used;
                g->n_times[b] = nt < 4 ? nt : 4;
                for (int t = 0; t < nt; t++) {
                    float v;
                    if (sscanf(s, "%f%n", &v, &used) != 1)
                        break;
                    s += used;
                    if (t < 4)
                        g->times[b][t] = v;
                }
            }
            if (i + 1 > r->ns)
                r->ns = i + 1;
        } else if (!strcmp(w, "pnode")) {
            int p, i;
            if (sscanf(s, "%d %d%n", &p, &i, &used) != 2 || p != 0 || i < 0 || i >= RACE_MAX_NODES)
                continue;
            s += used;
            RacePNode *n = &r->p[i];
            if (read_vecs(&s, n->p, 3) || read_vecs(&s, n->l, 3) || read_vecs(&s, n->r, 3))
                continue;
            if (sscanf(s, "%f %f %u %f %f %d%n", &n->speed_limit, &n->pace, &n->flags, &n->dist, &n->rst,
                       &n->seg_id, &used) != 6)
                continue;
            s += used;
            n->speed_limit *= MPH_FTS;
            n->pace *= MPH_FTS;
            n->n_next = read_list(&s, n->next, 4);
            float d[3];
            sub3(n->l, n->p, d);
            n->wl = len3(d); /* 0x38d5f0 */
            sub3(n->r, n->p, d);
            n->wr = len3(d);
            if (i + 1 > r->np)
                r->np = i + 1;
        } else if (!strcmp(w, "racer")) {
            int slot;
            char code[16];
            if (sscanf(s, "%d %15s", &slot, code) == 2 && slot >= 0 && slot < RACE_MAX_RACERS) {
                racer_defaults(&r->racer[slot]);
                snprintf(r->racer[slot].code, sizeof r->racer[slot].code, "%s", code);
            }
        } else if (!strcmp(w, "rkey")) {
            int slot;
            char k[64], v[256] = "";
            if (sscanf(s, "%d %63s %255[^\r\n]", &slot, k, v) >= 2 && slot >= 0 && slot < RACE_MAX_RACERS)
                racer_key(&r->racer[slot], k, v, &has_min[slot]);
        } else if (!strcmp(w, "rpt")) {
            char sec[32];
            int i;
            float mn, mx;
            if (sscanf(s, "%31s %d %f %f", sec, &i, &mn, &mx) == 4 && !strcmp(sec, difficulty) && i >= 0 &&
                i < RACE_MAX_RACERS) {
                r->racer[i].has_lap = 1; /* 0x3301d0: Max%d/Min%d for racer slot %d */
                r->racer[i].lap_min = mn;
                r->racer[i].lap_max = mx;
            }
        } else if (!strcmp(w, "next")) {
            sscanf(s, "%63s", r->next);
        }
    }
    fclose(f);
    for (int k = 0; k < RACE_MAX_RACERS; k++)
        if (!has_min[k])
            r->racer[k].min_pace = r->racer[k].max_pace; /* 0x388b50: Min defaults to Max */
    if (r->nr > RACE_MAX_RACERS)
        r->nr = RACE_MAX_RACERS;
    if (r->nb < 2 || r->np < 2 || r->nr < 1)
        return -2;
    /* boundary lap (0x38b290 +0x68) and path lap (0x38c990 +0x54): node 0 to its next, plus
       that node's DistanceToFinish */
    float d[3];
    int b1 = r->b[0].n_next > 0 ? r->b[0].next[0] : 0;
    sub3(r->b[b1].p, r->b[0].p, d);
    r->b_lap = len3(d) + r->b[b1].dist;
    int p1 = r->p[0].n_next > 0 ? r->p[0].next[0] : 0;
    sub3(r->p[0].p, r->p[p1].p, d);
    r->p_lap = len3(d) + r->p[p1].dist;
    return 0;
}

/* ------------------------------------------------------------------ boundary */
static int b_next(const Race *r, int n, int i) { return i < r->b[n].n_next ? r->b[n].next[i] : n; } /* 0x38b9c0 */
static int b_prev(const Race *r, int n, int i) { return i < r->b[n].n_prev ? r->b[n].prev[i] : n; } /* 0x38ba20 */

/* 0x38ba80: length of the boundary step a -> b */
static float b_seglen(const Race *r, int a, int b)
{
    if (r->b[a].n_next < 2 || r->b[a].branch == r->b[b].branch)
        return (a == 0 ? r->b_lap : r->b[a].dist) - r->b[b].dist;
    float d[3];
    sub3(r->b[a].p, r->b[b].p, d);
    return len3(d);
}

/* one edge of 0x38bec0's crossing test (XZ, ray along X) */
static int cross_edge(float x, float z, const float *a, const float *b)
{
    float e = b[0] - a[0], t = e * (x - a[0]);
    return 0.0f <= t && t < e * e && (t / (e * e)) * (b[2] - a[2]) + a[2] + 0.0f < z;
}

/* 0x38bec0: is the point inside left(a) -> left(b) -> pos(b) -> right(b) -> right(a) -> pos(a) */
static int b_inside(const Race *r, const float pt[3], int a, int b)
{
    const RaceBNode *A = &r->b[a], *B = &r->b[b];
    const float *v[6] = { A->l, B->l, B->p, B->r, A->r, A->p };
    int in = 0;
    for (int i = 0; i < 6; i++)
        in ^= cross_edge(pt[0], pt[2], v[i], v[(i + 1) % 6]);
    return in;
}

/* the clamped point-to-segment distance the game uses everywhere (0x22b480, 0x405c30, ...):
   from p to the segment b -> a */
static float seg_dist(const float p[3], const float a[3], const float b[3])
{
    float ab[3], pb[3];
    sub3(a, b, ab);
    sub3(p, b, pb);
    float t = dot3(pb, ab), pp = dot3(pb, pb);
    if (t <= 0.0f)
        return sqrtf(pp);
    float aa = dot3(ab, ab);
    if (aa <= t)
        return sqrtf(maxf_(0, aa + ((pp + 0.0f) - t * 2.0f)));
    float u = t / aa;
    return sqrtf(maxf_(0, aa * u * u + ((pp + 0.0f) - u * 2.0f * t)));
}

/* 0x22d820 / 0x22dd80: search forwards (backwards) from node n for the boundary step holding
   pos, within the distance budget p2; FLT_MAX if none */
static float b_search_back(const Race *r, float p1, float p2, const float pos[3], int rec, int n, int *oa, int *ob);
static float b_search(const Race *r, float p1, float p2, const float pos[3], int rec, int n, int *oa, int *ob)
{
    for (int i = 0; i < r->b[n].n_next; i++) {
        int b = b_next(r, n, i);
        if (b_inside(r, pos, n, b)) {
            *oa = n, *ob = b;
            return p2 - seg_dist(pos, r->b[n].l, r->b[n].r);
        }
    }
    for (int i = 0; i < r->b[n].n_next; i++) {
        int b = b_next(r, n, i);
        float rem = p2 - b_seglen(r, n, b);
        if (rem > 0) {
            float v = b_search(r, p1, rem, pos, rec, b, oa, ob);
            if (v != FLT_MAX)
                return v;
        }
    }
    if (rec) {
        for (int i = 1; i < r->b[n].n_prev; i++) {
            int a = b_prev(r, n, i);
            if (b_inside(r, pos, n, a)) {
                *oa = n, *ob = a;
                return (p1 - p2) - seg_dist(pos, r->b[n].l, r->b[n].r);
            }
        }
        for (int i = 1; i < r->b[n].n_prev; i++) {
            int a = b_prev(r, n, i);
            float rem = p1 + ((p1 - b_seglen(r, a, n)) - p2);
            if (rem > 0) {
                float v = b_search_back(r, p1, rem, pos, 0, a, oa, ob);
                if (v != FLT_MAX)
                    return v;
            }
        }
    }
    return FLT_MAX;
}

static float b_search_back(const Race *r, float p1, float p2, const float pos[3], int rec, int n, int *oa, int *ob)
{
    for (int i = 0; i < r->b[n].n_prev; i++) {
        int a = b_prev(r, n, i);
        if (b_inside(r, pos, a, n)) {
            *oa = a, *ob = n;
            return p2 - seg_dist(pos, r->b[n].l, r->b[n].r);
        }
    }
    for (int i = 0; i < r->b[n].n_prev; i++) {
        int a = b_prev(r, n, i);
        float rem = p2 - b_seglen(r, a, n);
        if (rem > 0) {
            float v = b_search_back(r, p1, rem, pos, rec, a, oa, ob);
            if (v != FLT_MAX)
                return v;
        }
    }
    if (rec) {
        for (int i = 1; i < r->b[n].n_next; i++) {
            int b = b_next(r, n, i);
            if (b_inside(r, pos, b, n)) {
                *oa = b, *ob = n;
                return (p1 - p2) - seg_dist(pos, r->b[n].l, r->b[n].r);
            }
        }
        for (int i = 1; i < r->b[n].n_next; i++) {
            int b = b_next(r, n, i);
            float rem = p1 + ((p1 - b_seglen(r, n, b)) - p2);
            if (rem > 0) {
                float v = b_search(r, p1, rem, pos, 0, b, oa, ob);
                if (v != FLT_MAX)
                    return v;
            }
        }
    }
    return FLT_MAX;
}

void race_grid(const Race *r, int slot, float offset, float pos[3], float fwd[3], int *node_out)
{
    /* 0x38c340: walk back from the start line node through its previous nodes */
    int per = r->per_row > 0 ? r->per_row : 1;
    int col = slot % per;
    float back = r->row_sep * (float)(slot / per) + offset;
    int node = r->start_node, prev = node;
    float dir[3] = { 1, 0, 0 }, seg = 0;
    for (int guard = 0; guard < r->nb; guard++) {
        prev = b_prev(r, node, 0);
        sub3(r->b[node].p, r->b[prev].p, dir);
        seg = len3(dir);
        if (seg >= back || prev == node)
            break;
        back -= seg;
        node = prev;
    }
    for (int k = 0; k < 3; k++)
        dir[k] = seg > 0 ? dir[k] / seg : (k == 0);
    /* side = up x dir; slot 0 centred, then alternating sides one slot further each pair */
    float side[3] = { dir[2], 0, -dir[0] };
    float lat = 0;
    if (col) {
        int h = col >> 1;
        lat = (col % 2 == 0) ? (float)h * r->slot_w : -(float)(h + 1) * r->slot_w;
    }
    for (int k = 0; k < 3; k++) {
        pos[k] = r->b[node].p[k] + side[k] * lat - dir[k] * back;
        fwd[k] = dir[k];
    }
    if (node_out)
        *node_out = node;
}

/* ------------------------------------------------------------------ track manager */
void race_track_init(RaceTrack *t, const Race *r, const Car *cars[])
{
    memset(t, 0, sizeof *t);
    /* 0x22a650: lap length from boundary nodes 0 and 1, the ValidSkipDistance */
    float d[3];
    sub3(r->b[1].p, r->b[0].p, d);
    t->lap_len = r->b[1].dist + len3(d);
    t->max_jump = r->valid_skip;
    /* 0x22aae0: the countdown */
    t->state = 2;
    t->count_t = 3.0f;
    for (int k = 0; k < r->nr; k++) {
        RaceState *s = &t->rs[k];
        float pos[3], fwd[3];
        int node = r->start_node;
        race_grid(r, r->racer[k].order, 0, pos, fwd, &node);
        /* 0x3315c0: the grid node ahead (+0x90) and its previous (+0x94); a standing start puts
           every racer behind the line (+0x48 bit 4) */
        s->n90 = node;
        s->n94 = b_prev(r, node, 0);
        s->place = k + 1; /* 0x22a9f0 */
        if (!r->has_pace_lap)
            s->flags |= 4;
        memcpy(s->pos, cars[k]->pos, sizeof s->pos);
    }
}

/* 0x22d550: find the racer's boundary step (not the HasPaceLap boundary-polygon mode) */
static int track_find(const RaceTrack *t, const Race *r, RaceState *s, const float pos[3])
{
    float d;
    if (!s->off)
        d = seg_dist(pos, r->b[s->n90].l, r->b[s->n90].r);
    else
        d = s->dist_prev - r->b[s->node38].dist;
    float len = b_seglen(r, s->n94, s->n90);
    float budget = (len - d) + t->max_jump + 450.0f;
    int oa = s->n94, ob = s->n90;
    float f = b_search(r, budget, budget, pos, 1, s->n94, &oa, &ob);
    if (f <= 0 || f == FLT_MAX) {
        float g = (t->max_jump + 450.0f) - (len - d);
        if (g > 0)
            f = b_search_back(r, g, g, pos, 1, s->n94, &oa, &ob);
        if (f <= 0 || f == FLT_MAX)
            return 0;
    }
    if (s->n90 != ob) {
        s->n90 = ob;
        s->n94 = oa;
    }
    return 1;
}

/* 0x38bb90 / 0x38bc00: does node n's prev list hold the start (finish) line node */
static int prev_has(const Race *r, int n, int line)
{
    for (int i = 0; i < r->b[n].n_prev; i++)
        if (r->b[n].prev[i] == line)
            return 1;
    return 0;
}

/* 0x22bc80 (the non-boundary-polygon branch) */
static void track_racer(RaceTrack *t, const Race *r, RaceState *s, const Car *c, float dt)
{
    int old = s->n90;
    s->last_lap = r->laps <= s->lap + 1;
    int found = track_find(t, r, s, c->pos), on = 1;
    if (!found) {
        /* off the boundary: 0x22a130(dt, rs, 1). shortcut: the game puts the car back on the
           track here (0x22c6f0 / 0x22c420, after 3 s); the respawn is not ported yet */
        on = 0;
        s->off_new = 0;
        if (!s->off)
            s->off = 1;
        s->flags |= 0x80;
        s->off_t += dt;
    } else {
        /* 0x22a130(0, rs, 0) */
        s->off_new = s->off != 0;
        s->off = 0;
        s->flags &= ~0x80u;
        s->off_t = 0;
        if (s->off_new) {
            float j = s->dist - s->dist_prev;
            if (t->lap_len / 2.0f < fabsf(j)) {
                int pos_ = j > 0;
                j = t->lap_len - fabsf(j);
                if (pos_)
                    j = -j;
            }
            /* shortcut: a jump past ValidSkipDistance respawns the car (0x22c420), not ported */
            (void)j;
        }
    }
    if (on) {
        /* wrong way: the travel direction (heading, or the velocity above 10 ft/s if it points
           backwards) against the boundary's direction at +0x94 (0x38b880) */
        float hd[3] = { c->rot[6], 0, c->rot[8] }, v[3] = { c->vel[0], c->vel[1], c->vel[2] };
        if (fabsf(car_speed(c)) > 10.0f && dot3(hd, v) < 0) {
            float l = len3(v);
            for (int k = 0; k < 3; k++)
                hd[k] = l > 0 ? v[k] / l : 0;
        }
        int a = r->b[s->n94].n_next < 2 ? s->n94 : b_prev(r, s->n94, 0);
        float sd[3];
        sub3(r->b[b_next(r, a, 0)].p, r->b[a].p, sd);
        float sl = len3(sd);
        for (int k = 0; k < 3; k++)
            sd[k] = sl > 0 ? sd[k] / sl : (k == 0);
        if (dot3(sd, hd) >= -0.1f) {
            s->wrong = 0;
            s->wrong_t = 0;
        } else {
            /* shortcut: over 3 s the game respawns the car (0x22c420), not ported */
            s->wrong = 1;
            s->wrong_t += dt;
        }
    }
    int now = s->n90;
    if (old != now) {
        if (s->prog < r->n_progress && old == r->progress[s->prog])
            s->prog++;
        int crossed = !(s->flags & 2) &&
                      ((!r->linear && old == r->start_node && prev_has(r, now, r->start_node)) ||
                       (r->linear && old == r->finish_node && prev_has(r, now, r->finish_node)));
        if (!crossed) {
            if (!r->linear && now == r->start_node && prev_has(r, old, r->start_node))
                s->flags |= 4; /* back over the line: behind it again */
            else if (r->linear && old == r->start_node && prev_has(r, now, r->start_node))
                s->flags &= ~4u;
        } else {
            if (s->flags & 4) {
                s->flags &= ~4u; /* the first crossing from behind the line counts no lap */
            } else if (!(s->flags & 1) && s->prog == r->n_progress) {
                if (!(s->flags & 8)) {
                    s->lap++;
                    s->flags |= 2;
                    if (!s->off)
                        s->flags &= ~0x40u;
                }
                s->flags |= 2;
                s->prog = 0;
            }
            s->flags = (s->flags & ~0x80u) | (s->off ? 0x80u : 0);
        }
    }
    if (!s->wrong && !s->off)
        s->node38 = now;
    memcpy(s->pos, c->pos, sizeof s->pos);
}

int race_track_update(RaceTrack *t, const Race *r, const Car *cars[], float dt)
{
    /* 0x22a510 */
    if (t->state == 2) {
        if (t->count_t > 0) {
            t->count_t -= dt;
            return 0;
        }
        t->state = 1; /* 0x22ac10: the go */
        for (int k = 0; k < r->nr; k++)
            t->rs[k].flags |= 0x10;
        return 1;
    }
    if (t->state != 1)
        return 0;
    t->clock += dt;
    /* 0x22ad70 */
    for (int k = 0; k < r->nr; k++) {
        RaceState *s = &t->rs[k];
        s->flags &= ~2u;
        track_racer(t, r, s, cars[k], dt);
        if (s->flags & 2) {
            float lt = (float)(int)(s->lap_time * 100.0f) / 100.0f;
            if (s->best != 0) {
                lt = minf_(s->best, lt);
                lt = lt >= 0 ? lt : 0;
            }
            s->best = lt;
            s->lap_time = 0;
        }
        s->lap_time += dt;
        s->clock = t->clock;
    }
    /* 0x22b600: distance to the finish and places */
    for (int k = 0; k < r->nr; k++) {
        RaceState *s = &t->rs[k];
        if ((s->flags & 1) || s->off)
            continue;
        float d = r->b[s->n90].dist + seg_dist(s->pos, r->b[s->n90].l, r->b[s->n90].r); /* 0x22b480 */
        if (s->flags & 4)
            d += t->lap_len;
        s->dist = d;
        if (!s->off)
            s->dist_prev = s->dist;
    }
    for (int k = 0; k < r->nr; k++) {
        RaceState *s = &t->rs[k];
        if (s->flags & 1)
            continue;
        int behind = 0;
        for (int j = 0; j < r->nr; j++) {
            const RaceState *o = &t->rs[j];
            if (o->flags & 1)
                continue;
            if (o->lap < s->lap || (o->flags & 8) || (s->lap == o->lap && s->dist < o->dist))
                behind++;
        }
        int place = r->nr - behind;
        s->flags = (s->flags & ~0x20u) | (s->place != place ? 0x20u : 0);
        s->place = place;
    }
    /* 0x22ad70: finishers, in order */
    for (int k = 0; k < r->nr; k++) {
        RaceState *s = &t->rs[k];
        if (!(s->flags & 1) && (s->flags & 2) && r->laps <= s->lap) {
            s->place = ++t->n_finished;
            s->flags |= 1;
        }
    }
    /* 0x22ba80: gaps to the leader */
    int lead = 0;
    for (int k = 0; k < r->nr; k++)
        if (t->rs[k].place == 1) {
            t->rs[k].gap = 0;
            lead = k;
            break;
        }
    const RaceState *L = &t->rs[lead];
    float ld = t->lap_len * (float)L->lap + (t->lap_len - L->dist) + 0.0f;
    float sp = L->clock > 0 ? ld / L->clock : 0;
    for (int k = 0; k < r->nr; k++)
        if (k != lead) {
            const RaceState *s = &t->rs[k];
            t->rs[k].gap = (ld - (t->lap_len * (float)s->lap + (t->lap_len - s->dist) + 0.0f)) / (sp < 1.0f ? 1.0f : sp);
        }
    return 0;
}

/* ------------------------------------------------------------------ AI */
static const RacePNode *pn(const Race *r, int i) { return &r->p[i]; }
/* 0x405fe0: the next node, the chosen branch (+0x360) where the line splits */
static int path_next(const RaceAI *a, const Race *r, int n)
{
    if (r->p[n].n_next > 1 && a->branch < r->p[n].n_next)
        return r->p[n].next[a->branch];
    return r->p[n].n_next > 0 ? r->p[n].next[0] : n;
}

/* 0x38dea0: the point at lateral fraction f (-1 left edge .. 1 right edge) of node n */
static void off_point(const Race *r, float f, int n, float out[3])
{
    const RacePNode *p = pn(r, n);
    for (int k = 0; k < 3; k++)
        out[k] = f <= 0.0f ? p->p[k] + (p->l[k] - p->p[k]) * fabsf(f) : p->p[k] + (p->r[k] - p->p[k]) * f;
}

/* 0x38e150: is pos inside the quad of path nodes a -> b (left / right edges, XZ crossing test) */
static int p_inside(const Race *r, const float pos[3], int a, int b)
{
    const RacePNode *A = pn(r, a), *B = pn(r, b);
    const float *v[4] = { A->l, B->l, B->r, A->r };
    int in = 0;
    for (int i = 0; i < 4; i++)
        in ^= cross_edge(pos[0], pos[2], v[i], v[(i + 1) % 4]);
    return in;
}

/* 0x4046c0: did the car (from its last position +0x320 to pos) cross node n's left -> right
   edge (0x13fdd0 2D segment intersection, both parameters in 0..1) */
static int ai_crossed(const RaceAI *a, const Race *r, int n, const float pos[3])
{
    float p1 = a->pos[0], p2 = a->pos[2];
    if (p1 == 0.0f && p2 == 0.0f)
        return 0;
    const RacePNode *N = pn(r, n);
    float d1 = pos[0] - p1, d2 = pos[2] - p2, q1 = N->l[0], q2 = N->l[2];
    float e1 = N->r[0] - N->l[0], e2 = N->r[2] - N->l[2];
    float den = d2 * e1 - e2 * d1;
    if (den == 0.0f)
        return 0;
    float s = (e1 * (q2 - p2) + e2 * (p1 - q1) + 0.0f) / den, t;
    if (e1 == 0.0f)
        t = ((d2 * s + p2 + 0.0f) - q2) / e2;
    else
        t = ((d1 * s + p1 + 0.0f) - q1) / e1;
    return !(s < 0.0f || 1.0f < s || t < 0.0f || 1.0f < t);
}

/* the node pair found: 0x405580 tests the flags of the old +0x350, 0x4057a0 those of b */
static void ai_found(RaceAI *a, const Race *r, int n, int b, int flag_node, const float pos[3])
{
    if (r->p[b].n_next > 1 || r->p[flag_node].flags != 0)
        a->cross = ai_crossed(a, r, a->ahead, pos);
    if (!r->p_linear || r->p[b].speed_limit != 0) {
        a->behind = n;
        a->ahead = b;
    } else {
        a->behind = b;
        a->ahead = b;
    }
}

/* 0x405580: recursive search from node n for the path segment holding pos. (Its loop over the
   next nodes keeps testing the node 0x405fe0 picked, so one node is tested.) */
static int ai_find(RaceAI *a, const Race *r, const RaceRacer *x, float range, const float pos[3], int n)
{
    int b = path_next(a, r, n);
    if (p_inside(r, pos, n, b)) {
        if (b != a->ahead)
            ai_found(a, r, n, b, a->ahead, pos);
        return 1;
    }
    if (range > 0.0f) {
        float d[3];
        sub3(r->p[n].p, r->p[b].p, d);
        float rem = range - len3(d);
        if (ai_find(a, r, x, minf_(rem, range), pos, b))
            return 1;
    }
    return 0;
}

/* 0x4057a0: search every node from +0x354 round the line for the segment holding pos, else the
   nearest segment (the car is then lost: +0x344) */
static void ai_find_any(RaceAI *a, const Race *r, const float pos[3])
{
    float best = 0;
    int have = 0, bn = a->behind, ba = -1, n = a->behind, count = 0;
    a->cross = 0;
    do {
        for (int i = 0; i < r->p[n].n_next; i++) {
            int b = r->p[n].next[i];
            float dd2;
            /* 0x38e320: pos between node n's and b's left -> right lines (normals up x edge) */
            {
                float e[3], q[3], up[3] = { 0, 1, 0 }, nn[3];
                const RacePNode *N = pn(r, n), *B = pn(r, b);
                int ok;
                sub3(N->l, N->r, e);
                nn[0] = up[1] * e[2] - up[2] * e[1];
                nn[1] = up[2] * e[0] - up[0] * e[2];
                nn[2] = up[0] * e[1] - up[1] * e[0];
                sub3(N->l, pos, q);
                ok = !(0.0f < dot3(q, nn));
                if (ok) {
                    sub3(B->l, B->r, e);
                    nn[0] = up[1] * e[2] - up[2] * e[1];
                    nn[1] = up[2] * e[0] - up[0] * e[2];
                    nn[2] = up[0] * e[1] - up[1] * e[0];
                    sub3(B->l, pos, q);
                    ok = 0.0f <= dot3(q, nn);
                }
                if (ok && p_inside(r, pos, n, b)) {
                    ai_found(a, r, n, b, b, pos);
                    return;
                }
                if (ok) {
                    for (int j = 0; j < r->p[n].n_next; j++) {
                        int m = r->p[n].next[j];
                        float seg[3], pm[3];
                        sub3(N->p, r->p[m].p, seg);
                        sub3(pos, r->p[m].p, pm);
                        float t = dot3(pm, seg);
                        dd2 = dot3(pm, pm);
                        if (0.0f < t) {
                            float ss = dot3(seg, seg);
                            if (ss <= t) {
                                dd2 = ss + ((dd2 + 0.0f) - t * 2.0f);
                            } else {
                                float u = t / ss;
                                dd2 = ss * u * u + ((dd2 + 0.0f) - u * 2.0f * t);
                            }
                        }
                        if (dd2 < best || ba == -1)
                            ba = m, best = dd2, bn = n;
                        count++;
                    }
                }
            }
        }
        n++;
        if (r->np <= n)
            n = 0;
    } while (n != a->behind);
    (void)have;
    a->lost = 1;
    if (a->behind != bn) {
        if (r->p[bn].flags != 0)
            a->cross = ai_crossed(a, r, bn, pos);
        a->behind = bn;
    }
    if (count) {
        if (a->seg == r->p[bn].seg_id && r->p[bn].n_next > 1 && a->branch < r->p[bn].n_next)
            a->ahead = r->p[bn].next[a->branch];
        else
            a->ahead = r->p[bn].n_next > 0 ? r->p[bn].next[0] : bn;
    }
}

/* 0x405480 */
static void ai_track(RaceAI *a, const Race *r, const RaceRacer *x, const float pos[3], float dt)
{
    float range = minf_(x->max_check, maxf_(a->speed * 4.0f, 100.0f));
    a->lost = 0;
    a->cross = 0;
    if (!ai_find(a, r, x, range, pos, a->behind)) {
        if (!x->dont_skip) {
            if (x->max_time_off <= a->off_path_t)
                ai_find_any(a, r, pos);
            else
                a->off_path_t += dt;
        } else {
            a->lost = 1;
        }
    } else {
        a->off_path_t = 0;
    }
    int sid = r->p[a->ahead].seg_id;
    if (sid != a->seg) {
        a->seg_prev = a->seg;
        a->seg = sid;
    }
}

/* 0x405c30: the point L ft ahead along the (offset) line, from the car's segment */
static void ai_look_point(RaceAI *a, const Race *r, const RaceRacer *x, float L, const float pos[3], int powersliding,
                          float out[3])
{
    if (a->lost && x->dont_skip) { /* +0x344 && DontSkipNodes: the point cached at +0x330 */
        memcpy(out, a->cache, sizeof a->cache);
        return;
    }
    const RacePNode *B = pn(r, a->behind);
    float lr[3], pr[3];
    sub3(B->l, B->r, lr);
    sub3(pos, B->r, pr);
    float t = dot3(pr, lr), pp = dot3(pr, pr);
    if (t <= 0.0f) {
        L += sqrtf(pp);
    } else {
        float ll = dot3(lr, lr);
        if (ll <= t) {
            L += sqrtf(maxf_(0, ll + ((pp + 0.0f) - t * 2.0f)));
        } else {
            float u = t / ll;
            L += sqrtf(maxf_(0, ll * u * u + ((pp + 0.0f) - u * 2.0f * t)));
        }
    }
    int cur = a->behind, nx = cur;
    float acc = 0, seg = 0;
    for (int guard = 0; guard < r->np + 1; guard++) {
        int c0 = cur;
        nx = path_next(a, r, c0);
        if (c0 == 0) {
            seg = r->p_lap - r->p[nx].dist;
        } else if (!r->p_linear || c0 != nx) {
            float d[3];
            sub3(r->p[c0].p, r->p[nx].p, d);
            seg = len3(d);
        } else {
            acc = 0;
            seg = L;
        }
        acc += seg;
        cur = nx;
        if (acc >= L) {
            cur = c0;
            break;
        }
    }
    float rem = L - (acc - seg);
    float A[3], Bp[3];
    if (powersliding || (r->p[cur].flags & 2)) {
        memcpy(A, r->p[nx].p, sizeof A);
        memcpy(Bp, r->p[cur].p, sizeof Bp);
    } else {
        off_point(r, a->off, nx, A);
        off_point(r, a->off, cur, Bp);
    }
    for (int k = 0; k < 3; k++)
        out[k] = Bp[k] + (A[k] - Bp[k]) * (seg == 0.0f ? rem : rem / seg);
    if (!a->lost)
        memcpy(a->cache, out, sizeof a->cache);
}

/* 0x389e90: the time this racer needs to the finish of the lap along its segment */
static void ai_need_time(RaceAI *a, const Race *r, const RaceState *s, const float pos[3])
{
    if (a->seg < 0 || a->seg >= r->ns)
        return;
    const RacePSeg *g = &r->s[a->seg], *gp = (a->seg_prev >= 0 && a->seg_prev < r->ns) ? &r->s[a->seg_prev] : g;
    const RacePNode *A = pn(r, a->ahead), *B = pn(r, a->behind);
    a->want_time = a->lap_target * (a->rubber + 1.0f);
    float ab[3];
    sub3(B->p, A->p, ab);
    float f = seg_dist(pos, A->l, A->r) / len3(ab);
    if (a->ahead == g->start) {
        if (a->ahead == gp->finish) {
            f = f * B->rst;
            g = gp;
        } else {
            f = f * (g->time - A->rst) + A->rst;
        }
    } else {
        f = f * (B->rst - A->rst) + A->rst;
    }
    float best = -1.0f, pick = FLT_MAX;
    for (int i = 0; i < g->n_branch; i++) {
        int bs = g->branch[i];
        if (bs < 0 || bs >= r->ns)
            continue;
        if (r->p[r->s[bs].start].flags & 0x10) /* NoAI on the branch's StartNode */
            continue;
        for (int t = 0; t < g->n_times[i]; t++) {
            float tt = g->times[i][t];
            float d = fabsf(a->want_time - (s->lap_time + tt));
            if (best == -1.0f || d < best) {
                best = d;
                pick = tt;
                if (a->cross && r->p[a->ahead].n_next > 1)
                    a->branch = i;
            }
        }
    }
    if (pick == FLT_MAX && g->n_branch > 0 && g->n_times[0] > 0) {
        pick = g->times[0][0];
        if (a->cross && r->p[a->ahead].n_next > 1)
            a->branch = 0;
    }
    a->need_time = f + pick;
}

/* 0x389b10: rubber band against the player (racer 0) */
static void ai_rubber(RaceAI *a, const Race *r, int k, const RaceTrack *t)
{
    const RaceRacer *x = &r->racer[k];
    const RaceState *pl = &t->rs[0], *me = &t->rs[k];
    if (!x->use_alod) {
        a->rubber = 0;
        return;
    }
    if (pl->clock > 0.0f) {
        float L = t->lap_len;
        float pd = L * (float)pl->lap + (L - pl->dist) + 0.0f;
        float pspeed = pd / pl->clock;
        float md = L * (float)me->lap + (L - me->dist) + 0.0f;
        float v = -(((pd - md) / (pspeed < 1.0f ? 1.0f : pspeed)) / a->lap_target) * 2.0f;
        v = v < x->max_pace ? v : x->max_pace;
        v = v >= -x->min_pace ? v : -x->min_pace;
        float cur = a->rubber;
        if ((0.0f < cur && cur < v) || (v < 0.02f && v < cur) || (0.02f < v && cur < v))
            a->rubber = v;
    }
    if (pl->last_lap && pl->dist > 1500.0f && a->rubber < 0.0f)
        a->rubber = 0;
}

/* 0x3896a0: the target speed */
static void ai_target_speed(RaceAI *a, const Race *r, int k, const RaceTrack *t, const float pos[3])
{
    const RaceRacer *x = &r->racer[k];
    const RaceState *s = &t->rs[k];
    a->bits &= ~8u;
    float spd = r->p[a->ahead].speed_limit;
    unsigned fl = r->p[a->ahead].flags;
    if ((fl & 0x80) && (fl & 1))
        a->bits |= 8;
    /* mode 3 (+0x3e4) is a racer with .rpt lap times (0x3315c0 -> 0x389550) */
    if (!(s->flags & 1) && x->has_lap && r->times_computed && !(s->flags & 8) && !(s->flags & 4)) {
        ai_rubber(a, r, k, t);
        if (!(fl & 0x80)) {
            if (s->flags & 2)
                a->lap_target = rand01() * (x->lap_max - x->lap_min) + x->lap_min + 0.0f;
            if (!(fl & 0x10)) {
                ai_need_time(a, r, s, pos);
                float rem = a->want_time - s->lap_time;
                if (rem <= 0.0f || a->need_time <= 0.05f) {
                    a->bits |= 8;
                } else {
                    float pace = r->p[a->ahead].pace;
                    float q = (rem - a->need_time) / rem;
                    spd = minf_(pace, (1.0f - q) * pace);
                    if (q < -0.05f)
                        a->bits |= 8;
                }
            }
        }
    }
    float v = maxf_(spd, 0.0f);
    a->target = minf_(v, a->speed + 15.0f);
}

/* the acos table of 0x137ea0: 30001 floats, (x + 1) * 14999.5 indexes it, linear between */
static float g_acos[30002];
static int g_acos_ready;
static float acos_t(float x)
{
    if (!g_acos_ready) {
        for (int i = 0; i < 30000; i++) {
            float v = (float)i * 6.666889e-05f + -1.0f;
            if (v > 1.0f)
                v = 1.0f;
            g_acos[i] = (float)acos((double)v);
        }
        g_acos[30000] = g_acos[29999];
        g_acos[30001] = g_acos[30000];
        g_acos_ready = 1;
    }
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    float u = (x + 1.0f) * 14999.5f;
    int i = (int)u;
    float f = u - (float)i;
    return (1.0f - f) * g_acos[i] + f * g_acos[i + 1];
}

void race_ai_init(RaceAI *a, const Race *r, int k, const RaceState *s, unsigned seed)
{
    memset(a, 0, sizeof *a);
    const RaceRacer *x = &r->racer[k];
    if (seed)
        g_rand = seed;
    /* the grid nodes on the AI line: the line node segment around the grid spot is found by the
       first ai_track; start from the line's start node */
    a->behind = 0;
    a->ahead = r->p[0].n_next > 0 ? r->p[0].next[0] : 0;
    a->seg = a->seg_prev = r->p[a->ahead].seg_id;
    a->delay = x->start_delay;
    a->off_timer = x->offset_time; /* 0x3889d0 / 0x388b50: +0x440 = TimeToSwitchOffset */
    /* 0x3301d0: the lap time for this slot, plus FirstLapPaceModifier without a pace lap */
    if (x->has_lap) {
        a->lap_target = rand01() * (x->lap_max - x->lap_min) + x->lap_min + 0.0f;
        if (!r->has_pace_lap)
            a->lap_target += r->first_lap_pace;
    }
    (void)s;
}

void race_ai(RaceAI *a, const Race *r, int k, const RaceTrack *t, const Car *c, float dt, CarInput *in)
{
    const RaceRacer *x = &r->racer[k];
    const RaceState *s = &t->rs[k];
    memset(in, 0, sizeof *in);
    float pos[3] = { c->pos[0], c->pos[1], c->pos[2] };
    /* 0x388d90 */
    a->off_prev = a->off;
    a->behind_prev = a->behind;
    ai_track(a, r, x, pos, dt);
    /* 0x389110 (with 0x404b50: the boost request on a Boost node, EnableBoost) */
    a->req_set = 0;
    for (int i = 0; i < 5; i++)
        if (a->req_bits[i]) {
            a->req_set |= a->req_bits[i];
            a->req |= a->req_bits[i];
            a->req_time[i] -= dt;
            if (a->req_time[i] < 0)
                a->req_bits[i] = 0;
        }
    if ((x->flags & 4) && (r->p[a->behind].flags & 1))
        a->req |= 4;
    if (x->flags & 0x10) {
        a->req |= 0x10;
        a->off_timer -= dt;
        if (a->off_timer < 0.0f) {
            int m = (int)(x->max_offset * 100.0f);
            a->off_ft = (float)((int)((float)(m * 2 + 1) * rand01()) - m) / 100.0f;
            a->off_timer = x->offset_time;
        }
    }
    /* 0x389080 (with 0x404860: the boost) */
    int boost = 0;
    a->mode = 0;
    if (a->req & 4) {
        if ((!(r->p[a->ahead].flags & 1) && !(a->req_set & 4)) || a->lost || !(a->bits & 8)) {
            a->req &= ~4u;
        } else {
            boost = 1;
            a->mode = 2;
        }
    }
    if (a->req & 0x10)
        a->off = a->off_ft / (a->off_ft < 0.0f ? r->p[a->ahead].wl : r->p[a->ahead].wr);
    a->speed = car_speed(c);
    if (s->flags & 0x10) {
        /* StartDelay (+0x3dc) counts from the go; the controls stay as they are */
        a->delay -= dt;
        if (a->delay >= 0.0f)
            return;
    }
    /* 0x389560: 0x3896a0 target speed, 0x4060e0 direction (0x4062a0 avoidance: not ported) */
    if (!(a->req & 0x10))
        a->off = 0;
    ai_target_speed(a, r, k, t, pos);
    {
        /* 0x4060e0 */
        float kk = dt / 4.0f;
        if (kk > 1.0f)
            kk = 1.0f;
        if (a->off != a->off_prev)
            a->off = (1.0f - kk) * a->off_prev + kk * a->off;
        /* 0x38d810: from pos to the offset line between +0x354 and +0x350 */
        float A[3], B[3];
        off_point(r, a->off, a->behind, A);
        off_point(r, a->off, a->ahead, B);
        float d = seg_dist(pos, A, B);
        float look = x->look_factor * d + x->min_look;
        float m = maxf_(a->speed, x->min_look);
        look = minf_(look, m);
        float P[3];
        ai_look_point(a, r, x, look, pos, 0, P);
        sub3(P, pos, a->dir);
        float l = len3(a->dir);
        for (int i = 0; i < 3; i++)
            a->dir[i] = l > 0 ? a->dir[i] / l : 0;
        memcpy(a->pos, pos, sizeof a->pos);
    }
    /* 0x4044e0: throttle and brake */
    float gas = 0, brake = 0;
    if (a->mode == 3) {
        brake = 1;
        a->target = a->speed - 10.0f;
    } else if (a->mode == 2) {
        gas = 1;
        a->target = a->speed + 10.0f;
    } else if (a->mode == 1) {
        a->target = a->speed;
    } else {
        float d = a->target - a->speed;
        if (d < -10.0f || a->target <= 0.0f || (d < 0.0f && (a->bits & 1))) {
            brake = 1;
            a->bits |= 1;
        } else if (d > 0.0f) {
            gas = 1;
            a->bits &= ~1u;
        } else {
            a->bits &= ~1u;
        }
    }
    /* 0x404d90: steering, with a wheel on the ground (+0x3f7 bit 6) */
    float x_ = 0;
    if (c->grounded > 0 && (a->target != 0.0f || a->speed * 0.68182f >= 0.5f)) {
        float tx = a->dir[0], tz = a->dir[2];
        float tl = 1.0f / sqrtf(tz * tz + tx * tx);
        float at = acos_t(tx * tl);
        if (tz < 0.0f)
            at = -at;
        float fx = c->rot[6], fz = c->rot[8];
        float fl = 1.0f / sqrtf(fz * fz + fx * fx);
        float af = acos_t(fx * fl);
        if (fz < 0.0f)
            af = -af;
        if (c->rot[4] < 0.0f) { /* upside down */
            at = -at;
            af = -af;
        }
        if (af > 0.0f && at < 0.0f)
            at += 6.2831855f;
        float dd = af - at;
        if (dd < -PI_F)
            dd += 6.2831855f;
        float sm = x->steer_mult * dd;
        sm = sm > 1.0f ? 1.0f : (sm < -1.0f ? -1.0f : sm);
        x_ = x->steer_boost * (1.5707964f - acos_t(sm));
        if (fabsf(x_) > 1.0f)
            x_ = x_ / fabsf(x_);
    }
    in->steer = x_;
    in->throttle = gas;
    in->brake = brake;
    in->boost = boost && !r->disable_nox;
}
