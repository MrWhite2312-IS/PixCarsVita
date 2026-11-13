/*
 * PixCarsVita - races (see race.h).
 */
#include "race.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPH_FTS 1.46666f

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int read_ints(const char **s, int *out, int max)
{
    int n = 0, used;
    if (sscanf(*s, "%d%n", &n, &used) != 1)
        return 0;
    *s += used;
    for (int i = 0; i < n; i++) {
        int v;
        if (sscanf(*s, "%d%n", &v, &used) != 1)
            return i;
        *s += used;
        if (i < max)
            out[i] = v;
    }
    return n < max ? n : max;
}

static int read_node(const char *s, RaceNode *n, int path)
{
    int used;
    if (sscanf(s, "%f %f %f %f %f %f %f %f %f%n", &n->p[0], &n->p[1], &n->p[2], &n->l[0], &n->l[1], &n->l[2],
               &n->r[0], &n->r[1], &n->r[2], &used) != 9)
        return -1;
    s += used;
    if (path) {
        if (sscanf(s, "%f %f %d%n", &n->speed_limit, &n->pace, &n->boost, &used) != 3)
            return -1;
        s += used;
        n->n_next = read_ints(&s, n->next, 4);
    } else {
        if (sscanf(s, "%f%n", &n->dist, &used) != 1)
            return -1;
        s += used;
        n->n_next = read_ints(&s, n->next, 4);
        n->n_prev = read_ints(&s, n->prev, 4);
    }
    return 0;
}

int race_load(Race *r, const char *path)
{
    memset(r, 0, sizeof *r);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[512], w[16];
    while (fgets(line, sizeof line, f)) {
        int i, used;
        if (sscanf(line, "%15s%n", w, &used) != 1)
            continue;
        const char *s = line + used;
        if (!strcmp(w, "race")) {
            sscanf(s, "%d %d %d", &r->laps, &r->n_racers, &r->boost);
        } else if (!strcmp(w, "grid")) {
            sscanf(s, "%d %f %f %d %d", &r->per_row, &r->slot_w, &r->row_sep, &r->start_node, &r->finish_node);
        } else if ((!strcmp(w, "bnode") || !strcmp(w, "path")) && sscanf(s, "%d%n", &i, &used) == 1) {
            int is_path = w[0] == 'p';
            RaceNode *n = is_path ? r->p : r->b;
            int *cnt = is_path ? &r->np : &r->nb;
            if (i < 0 || i >= RACE_MAX_NODES || read_node(s + used, &n[i], is_path))
                continue;
            if (i + 1 > *cnt)
                *cnt = i + 1;
        } else if (!strcmp(w, "racer") && r->nr < RACE_MAX_RACERS) {
            RaceRacer *x = &r->racer[r->nr];
            int slot;
            if (sscanf(s, "%d %15s %d %f %f %f %f %f %f", &slot, x->code, &x->order, &x->delay, &x->pace_pct,
                       &x->min_pct, &x->max_pct, &x->max_offset, &x->offset_time) == 9) {
                /* StartLineOrder is 1-based: RR_1_1's orders 1..5 fill its one row of five
                   (VehiclesPerRow 5) in the order CS_1_1C ends on, MCQ FLE SAL HUD RAM */
                x->order -= 1;
                r->nr++;
            }
        } else if (!strcmp(w, "pace") && r->n_pace < RACE_MAX_RACERS) {
            int k;
            if (sscanf(s, "%d %f %f", &k, &r->pace_min[r->n_pace], &r->pace_max[r->n_pace]) == 3)
                r->n_pace++;
        } else if (!strcmp(w, "next")) {
            sscanf(s, "%63s", r->next);
        }
    }
    fclose(f);
    if (r->nb < 2 || r->np < 2)
        return -2;
    /* lap length: the largest DistanceToFinish plus the step from that node to the line */
    int far = 0;
    for (int i = 0; i < r->nb; i++)
        if (r->b[i].dist > r->b[far].dist)
            far = i;
    const RaceNode *a = &r->b[far], *z = &r->b[r->start_node];
    float d[3] = { a->p[0] - z->p[0], a->p[1] - z->p[1], a->p[2] - z->p[2] };
    r->lap_len = a->dist + sqrtf(dot3(d, d));
    return 0;
}

void race_grid(const Race *r, int slot, float offset, float pos[3], float fwd[3])
{
    int per = r->per_row > 0 ? r->per_row : 1;
    int col = slot % per;
    float back = r->row_sep * (float)(slot / per) + offset;
    /* walk back from the start line node through its previous nodes */
    int node = r->start_node, prev = node;
    float dir[3] = { 1, 0, 0 }, seg = 0;
    for (int guard = 0; guard < r->nb; guard++) {
        prev = r->b[node].n_prev > 0 ? r->b[node].prev[0] : node;
        for (int k = 0; k < 3; k++)
            dir[k] = r->b[node].p[k] - r->b[prev].p[k];
        seg = sqrtf(dot3(dir, dir));
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
}

/* closest point on boundary segment i -> next: returns squared distance, t along it */
static float seg_dist(const RaceNode *nodes, int i, const float pos[3], float *t)
{
    const RaceNode *a = &nodes[i], *b = &nodes[a->n_next > 0 ? a->next[0] : i];
    float ab[3] = { b->p[0] - a->p[0], 0, b->p[2] - a->p[2] }, ap[3] = { pos[0] - a->p[0], 0, pos[2] - a->p[2] };
    float l2 = dot3(ab, ab);
    float u = l2 > 0 ? clampf(dot3(ap, ab) / l2, 0, 1) : 0;
    float d[3] = { ap[0] - ab[0] * u, 0, ap[2] - ab[2] * u };
    *t = u;
    return dot3(d, d);
}

void race_track(const Race *r, RaceState *s, const float pos[3])
{
    /* search near the last segment first, the whole boundary if that is far off */
    int best = -1;
    float bd = 1e30f, bt = 0, t;
    for (int k = -3; k <= 6; k++) {
        int i = ((s->hint + k) % r->nb + r->nb) % r->nb;
        float d = seg_dist(r->b, i, pos, &t);
        if (d < bd)
            bd = d, best = i, bt = t;
    }
    if (bd > 300.0f * 300.0f)
        for (int i = 0; i < r->nb; i++) {
            float d = seg_dist(r->b, i, pos, &t);
            if (d < bd)
                bd = d, best = i, bt = t;
        }
    s->hint = best;
    const RaceNode *a = &r->b[best];
    int ni = a->n_next > 0 ? a->next[0] : best;
    float da = a->dist, db = r->b[ni].dist;
    if (ni == r->start_node)
        db = 0;
    if (best == r->start_node)
        da = r->lap_len;
    float p = r->lap_len - (da + (db - da) * bt);
    /* crossing the line forwards starts a lap; backwards takes it away */
    if (s->progress > r->lap_len * 0.75f && p < r->lap_len * 0.25f)
        s->lap++;
    else if (s->progress < r->lap_len * 0.25f && p > r->lap_len * 0.75f && s->lap > 0)
        s->lap--;
    s->progress = p;
}

float race_distance(const Race *r, const RaceState *s)
{
    return (float)s->lap * r->lap_len + s->progress;
}

static float frand(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (float)((*seed >> 8) & 0xffff) / 65535.0f;
}

void race_ai(const Race *r, RaceState *s, int k, int pos_rank, const Car *c, float dt, CarInput *in)
{
    memset(in, 0, sizeof *in);
    const RaceRacer *x = &r->racer[k];
    const float *f = c->rot + 6;
    /* leader line segment the car is on (searched around the last one; segments, not nodes,
       since nodes can be 1800 ft apart) */
    int best = s->path_hint;
    float bd = 1e30f, bt;
    for (int j = -2; j <= 8; j++) {
        int i = ((s->path_hint + j) % r->np + r->np) % r->np;
        float dd = seg_dist(r->p, i, c->pos, &bt);
        if (dd < bd)
            bd = dd, best = i;
    }
    if (bd > 300.0f * 300.0f)
        for (int i = 0; i < r->np; i++) {
            float dd = seg_dist(r->p, i, c->pos, &bt);
            if (dd < bd)
                bd = dd, best = i;
        }
    s->path_hint = best;
    /* random offset from the line (EnableRandomOffset: +-MaxOffsetDist, new every
       TimeToSwitchOffset seconds) */
    s->offset_t -= dt;
    if (s->offset_t <= 0 && x->offset_time > 0) {
        s->offset = (frand(&s->seed) * 2 - 1) * x->max_offset;
        s->offset_t = x->offset_time;
    }
    /* target: walk along the line from the nearest node by a speed-dependent look-ahead */
    float spd = fabsf(car_speed(c));
    float look = 40.0f + spd * 0.6f;
    int i = best;
    float tgt[3];
    memcpy(tgt, r->p[i].p, sizeof tgt);
    float pace = r->p[i].pace;
    {
        /* measure the look-ahead from the car's point on segment best -> next, not from the node:
           line nodes can be 300 ft apart, so the node alone puts the target behind the car */
        int n = r->p[i].n_next > 0 ? r->p[i].next[0] : i;
        float d[3] = { r->p[n].p[0] - r->p[i].p[0], 0, r->p[n].p[2] - r->p[i].p[2] };
        float a[3] = { c->pos[0] - r->p[i].p[0], 0, c->pos[2] - r->p[i].p[2] };
        float l2 = dot3(d, d);
        if (l2 > 0)
            look += sqrtf(l2) * clampf(dot3(a, d) / l2, 0, 1);
    }
    for (int guard = 0; guard < r->np && look > 0; guard++) {
        int n = r->p[i].n_next > 0 ? r->p[i].next[0] : i;
        float d[3] = { r->p[n].p[0] - r->p[i].p[0], r->p[n].p[1] - r->p[i].p[1], r->p[n].p[2] - r->p[i].p[2] };
        float l = sqrtf(dot3(d, d));
        if (l >= look || n == i) {
            for (int q = 0; q < 3; q++)
                tgt[q] = r->p[i].p[q] + d[q] * (l > 0 ? look / l : 0);
            break;
        }
        look -= l;
        i = n;
        pace = fminf(pace, r->p[i].pace);
    }
    /* sideways offset across the line */
    float dir[3] = { tgt[0] - c->pos[0], 0, tgt[2] - c->pos[2] };
    float dl = sqrtf(dot3(dir, dir));
    if (dl > 1e-3f) {
        tgt[0] += dir[2] / dl * s->offset;
        tgt[2] -= dir[0] / dl * s->offset;
    }
    float to[3] = { tgt[0] - c->pos[0], 0, tgt[2] - c->pos[2] };
    /* steering: the angle to the target, right positive (car right = rot row 0) */
    const float *rt = c->rot;
    float ang = atan2f(dot3(to, rt), dot3(to, f));
    in->steer = clampf(ang * 2.5f, -1, 1);
    /* speed: the line's PaceSpeed scaled so a lap takes the .rpt time for this place (the
       leader line's own lap is the PaceSpeed lap) */
    int slot = pos_rank - 1;
    if (slot < 0)
        slot = 0;
    if (slot >= r->n_pace)
        slot = r->n_pace - 1;
    float scale = 1.0f;
    if (r->n_pace > 0) {
        float want = 0.5f * (r->pace_min[slot] + r->pace_max[slot]);
        /* shortcut: 42 s is the leader line's lap (Path0Segment TimesToFinish) for RR_1_1 */
        scale = 42.0f / want;
    }
    float target = pace * MPH_FTS * scale * (1.0f + x->pace_pct / 100.0f);
    if (fabsf(ang) > 0.5f)
        target *= 0.7f;
    if (spd < target - 3.0f)
        in->throttle = 1;
    else if (spd > target + 8.0f)
        in->brake = 1;
}
