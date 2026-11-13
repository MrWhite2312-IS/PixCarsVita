/*
 * PixCarsVita - races (portable C99): the track a race activity uses (tools/vitarace.py
 * race.txt from the .aty/.trk/.rpt): the race boundary (lap progress, start grid), the AI
 * leader line, the racers and the AI lap times; lap counting and positions; an AI driver that
 * turns the leader line into CarInput.
 */
#ifndef PIXCARS_RACE_H
#define PIXCARS_RACE_H

#include "car.h"

#define RACE_MAX_NODES 256
#define RACE_MAX_RACERS 8

typedef struct {
    float p[3], l[3], r[3];            /* centre, left and right edge */
    float dist;                        /* DistanceToFinish (boundary) */
    float speed_limit, pace;           /* leader line: SpeedLimit, PaceSpeed (mph) */
    int boost;
    int n_next, next[4], n_prev, prev[4];
} RaceNode;

typedef struct {
    char code[16];                     /* character (model folder, .phy) */
    int order;                         /* grid slot: StartLineOrder - 1 */
    float delay;                       /* StartDelay (s after the go) */
    float pace_pct, min_pct, max_pct;  /* PacePercentModifier, Min/Max (percent) */
    float max_offset, offset_time;     /* MaxOffsetDist (ft), TimeToSwitchOffset (s) */
} RaceRacer;

typedef struct {
    int laps, n_racers, boost;
    int per_row, start_node, finish_node;
    float slot_w, row_sep;
    RaceNode b[RACE_MAX_NODES];        /* race boundary */
    int nb;
    RaceNode p[RACE_MAX_NODES];        /* AI leader line */
    int np;
    RaceRacer racer[RACE_MAX_RACERS];  /* slot 0 is the player */
    int nr;
    float pace_min[RACE_MAX_RACERS], pace_max[RACE_MAX_RACERS]; /* AI lap times by position */
    int n_pace;
    float lap_len;                     /* one lap along the boundary (ft) */
    char next[64];                     /* activity after the race */
} Race;

/* Per-racer race state. */
typedef struct {
    int lap;                           /* laps started (crossing the line at the start makes 1) */
    float progress;                    /* ft into the lap */
    int hint;                          /* boundary segment last found */
    int finished;
    float finish_time;
    /* AI driver */
    int path_hint;
    float offset, offset_t;            /* current random offset from the line and its timer */
    unsigned seed;
} RaceState;

int race_load(Race *r, const char *path);
/* Start grid slot (0x38c340): position and facing for StartLineOrder slot, offset ft behind. */
void race_grid(const Race *r, int slot, float offset, float pos[3], float fwd[3]);
/* Update lap and progress from a position. */
void race_track(const Race *r, RaceState *s, const float pos[3]);
/* Race distance (laps and progress) used to rank racers. */
float race_distance(const Race *r, const RaceState *s);
/* AI driver for racer slot k at race position pos_rank (1 = leading). shortcut: a pure-pursuit
   follower of the leader line with a speed target from the .rpt lap times, not the game's AI
   (the PS2 AI driver is not decoded yet). */
void race_ai(const Race *r, RaceState *s, int k, int pos_rank, const Car *c, float dt, CarInput *in);

#endif
