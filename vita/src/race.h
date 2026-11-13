/*
 * PixCarsVita - races (portable C99), ported from the PS2 game (SLUS_211.51):
 *   - the race activity data (tools/vitarace.py race.txt from the .aty/.trk/.rpt), parsed with
 *     the game's defaults: the race boundary (RaceBoundary.cpp 0x38b290), the AI leader line
 *     (RacePath.cpp 0x38c990), the racers' [VehicleN] keys (AI parse 0x388b50 / 0x403e20) and
 *     the AI lap times (0x3301d0);
 *   - the track manager's lap tracking (0x22a510 / 0x22ad70 / 0x22bc80 / 0x22b600);
 *   - the AI driver (vtable 0x4da460; update 0x388d90).
 * Units as the game: feet, seconds, ft/s (the files' mph x 1.46666).
 */
#ifndef PIXCARS_RACE_H
#define PIXCARS_RACE_H

#include "car.h"

#define RACE_MAX_NODES 256
#define RACE_MAX_RACERS 8
#define RACE_MAX_SEGS 16

/* race boundary node (0x60 bytes in the game) */
typedef struct {
    float p[3], l[3], r[3];            /* +0x10 Position, +0x20 LeftEdge, +0x30 RightEdge */
    float dist;                        /* +0x50 DistanceToFinish */
    unsigned flags;                    /* +0: 1 = NoReset */
    int zone, branch;                  /* +0x54 ZoneID - 1, +0x58 BranchID */
    int n_next, next[4], n_prev, prev[4];
} RaceBNode;

/* AI leader line node (0x60 bytes) */
typedef struct {
    float p[3], l[3], r[3];
    float wl, wr;                      /* +0x3c / +0x40: |LeftEdge - Position|, |RightEdge - Position| */
    float speed_limit, pace;           /* +0x44 SpeedLimit, +0x48 PaceSpeed (ft/s) */
    unsigned flags;                    /* +0x30: Boost 1, PowerSlide 2, Jump 8, NoAI 0x10, Corner 0x20,
                                          DriveBackward 0x40, ForceTargetSpeed 0x80 */
    float dist, rst;                   /* +0x4c DistanceToFinish, +0x50 RemainingSegmentTime */
    int seg_id;                        /* +0x54 SegmentID */
    int n_next, next[4];
} RacePNode;

/* AI leader line segment (0x20 bytes) */
typedef struct {
    int start, finish;                 /* +0xc StartNode, +8 FinishNode */
    unsigned section;                  /* +0x10: 1 << (SectionID - 1) */
    float time;                        /* +0x14 SegmentTime */
    int n_branch, branch[4];           /* +0 BranchCount, +4 Branch%d */
    int n_times[4];                    /* +0x1c NumberOfLapTimes%d */
    float times[4][4];                 /* +0x18 TimesToFinish%d%d */
} RacePSeg;

/* a racer's [VehicleN] keys with the AI's defaults */
typedef struct {
    char code[16];
    int order;                         /* grid slot: StartLineOrder - 1 */
    float start_delay;                 /* +0x3d8 StartDelay */
    int use_alod;                      /* +0x3e0 UseALOD */
    float max_pace, min_pace;          /* +0x41c / +0x420: Max/MinPacePercentModifier / 100 */
    unsigned flags;                    /* +0x378: EnablePowerSlide 1, EnableJump 2, EnableBoost 4,
                                          EnableDriveBackwards 8, EnableRandomOffset 0x10 */
    float max_offset, offset_time;     /* +0x438 MaxOffsetDist, +0x43c TimeToSwitchOffset */
    int path_id, dont_skip;            /* +0x2b0 PathID, +0x2b4 DontSkipNodes */
    float max_time_off;                /* +0x2cc MaxTimeOffPath */
    float min_look, look_factor;       /* +0x2bc MinimumLookAheadDist, +0x2b8 LookAheadFactor */
    float max_check;                   /* +0x2c0 MaxPathCheckDist */
    float min180;                      /* +0x3ac Min180ReverseSpeed */
    float steer_mult, steer_boost;     /* +0x2c4 SteeringMultiplier, +0x2c8 SteeringBoost */
    float ps_look, min_ps_speed;       /* +0x3b0 PowerslideLookAheadDist, +0x3b4 MinPowerSlideSpeed */
    int has_lap;                       /* +0x424: the .rpt has Min/Max for this slot */
    float lap_min, lap_max;            /* +0x42c / +0x430 */
} RaceRacer;

typedef struct {
    int laps, nr;                      /* TotalLaps, NumRacers */
    int has_pace_lap, disable_nox;     /* race +0xe20, +0xe18 */
    float first_lap_pace;              /* race +0xeb4 FirstLapPaceModifier */
    float valid_skip;                  /* race +0xc38 ValidSkipDistance: the manager's +0x50 */
    /* boundary */
    int nb, start_node, finish_node, linear, per_row;
    float slot_w, row_sep, b_lap;      /* b_lap: +0x68 */
    RaceBNode b[RACE_MAX_NODES];
    int progress[10], n_progress;      /* ProgressNode list (+0x28, +0x6c) */
    /* AI leader line (Path0) */
    int np, ns, p_linear, times_computed;
    float p_lap;                       /* path +0x54 */
    RacePNode p[RACE_MAX_NODES];
    RacePSeg s[RACE_MAX_SEGS];
    RaceRacer racer[RACE_MAX_RACERS];  /* slot 0 is the player */
    char next[64];
} Race;

/* A racer's state in the track manager (the game's racer state, +0x80 of a racer entry) */
typedef struct {
    float pos[3];                      /* +0x10 */
    float lap_time, best;              /* +0x20, +0x24 */
    float clock;                       /* +0x28: the race clock (manager +0x30) */
    float gap;                         /* +0x2c */
    float dist, dist_prev;             /* +0x30 distance to the finish, +0x34 */
    int node38;                        /* +0x38 */
    int lap;                           /* +0x3c laps done */
    int place;                         /* +0x40 */
    unsigned flags;                    /* +0x48: 1 finished, 2 crossed the line this frame, 4 behind
                                          the start line, 8 ?, 0x10 StartDelay pending, 0x20 place
                                          changed, 0x40, 0x80 off track */
    int last_lap;                      /* +0x49 */
    int off, off_new;                  /* +0x54 off track, +0x58 came back this frame */
    float off_t;                       /* +0x5c */
    int wrong;                         /* +0x70 wrong way */
    float wrong_t;                     /* +0x74 */
    /* the racer entry's boundary nodes: +0x90 ahead, +0x94 behind; +0x98 progress node index */
    int n90, n94, prog;
} RaceState;

/* The AI driver's state (offsets of the game's AI racer object) */
typedef struct {
    int ahead, behind, behind_prev;    /* +0x350, +0x354, +0x34c */
    int seg, seg_prev, branch;         /* +0x358, +0x35c, +0x360 */
    float off, off_prev;               /* +0x36c lateral fraction, +0x370 */
    float off_ft, off_timer;           /* +0x434, +0x440 */
    unsigned req, req_set;             /* +0x374, +0x380 */
    unsigned req_bits[5]; float req_time[5]; /* +0x384.., +0x398.. */
    int mode;                          /* +0x2f4 */
    float speed, target;               /* +0x364, +0x368 */
    float dir[3], pos[3], cache[3];    /* +0x300, +0x320, +0x330 */
    int lost, cross;                   /* +0x344, +0x340 */
    float off_path_t;                  /* +0x2d0 */
    unsigned bits;                     /* +0x3cc: 1 braking, 2, 4, 8 at full speed */
    float lap_target;                  /* +0x428 */
    float rubber;                      /* +0x418 */
    float want_time, need_time;        /* +0x410, +0x414 */
    float delay;                       /* +0x3dc */
    unsigned seed;
} RaceAI;

int race_load(Race *r, const char *path, const char *difficulty);
/* Start grid slot (0x38c340): position and facing for a slot, offset ft behind; *node gets the
   boundary node ahead of the slot. */
void race_grid(const Race *r, int slot, float offset, float pos[3], float fwd[3], int *node);

/* Track manager (0x22a650 setup, 0x22a510 update). */
typedef struct {
    int state;                         /* +0x8c: 2 counting down, 1 racing */
    float count_t, clock;              /* +0x34, +0x30 */
    float lap_len, max_jump;           /* +0x38, +0x50 */
    int n_finished;                    /* +0x74 */
    RaceState rs[RACE_MAX_RACERS];
} RaceTrack;

void race_track_init(RaceTrack *t, const Race *r, const Car *cars[]);
/* One frame: the countdown and, racing, lap tracking. Returns 1 on the frame of the go. */
int race_track_update(RaceTrack *t, const Race *r, const Car *cars[], float dt);

/* AI driver for racer slot k: one frame (0x388d90) writing the car's controls into *in. */
void race_ai_init(RaceAI *a, const Race *r, int k, const RaceState *s, unsigned seed);
void race_ai(RaceAI *a, const Race *r, int k, const RaceTrack *t, const Car *c, float dt, CarInput *in);

#endif
