/* Host test for vita/src/race.c: load race.txt, put every racer on its grid slot on the real
 * scene, run the game's countdown and track manager, drive every car with the AI (the player's
 * slot too) and check every car finishes the laps.
 * Build: gcc -O2 -std=c99 -Ivita/src vita/src/race.c vita/src/car.c vita/src/scene.c vita/src/vcm.c vita/tests/race_test.c -lm -o race_test
 * Run:   ./race_test extracted/vita/act/rr_1_1/race.txt extracted/vita/scene/rs.vcs extracted/vita [trace_racer] */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "race.h"
#include "scene.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    s[fread(s, 1, (size_t)n, f)] = 0;
    fclose(f);
    return s;
}

static Race R;
static RaceTrack T;
static RaceAI A[RACE_MAX_RACERS];

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) {
        puts("usage: race_test race.txt scene.vcs extracted_dir [trace_racer]");
        return 1;
    }
    int trace = argc > 4 ? atoi(argv[4]) : -1;
    assert(race_load(&R, argv[1], "Normal") == 0);
    printf("race: %d laps, %d racers, %d boundary nodes (lap %.0f ft), %d line nodes, %d segments\n", R.laps, R.nr,
           R.nb, R.b_lap, R.np, R.ns);
    assert(R.laps == 3 && R.nr == 5 && R.np > 2 && R.ns > 0);
    for (int k = 0; k < R.nr; k++)
        printf("  %s slot %d delay %.2f pace %.3f/%.3f laps %s %.2f-%.2f\n", R.racer[k].code, R.racer[k].order,
               R.racer[k].start_delay, R.racer[k].min_pace, R.racer[k].max_pace, R.racer[k].has_lap ? "yes" : "no",
               R.racer[k].lap_min, R.racer[k].lap_max);
    /* the .rpt lap times belong to racer slots (0x3301d0): Vehicle1 Min1/Max1 */
    assert(!R.racer[0].has_lap && R.racer[1].has_lap && R.racer[1].lap_min == 55.12f);

    VcsScene sc;
    ColWorld w;
    assert(vcs_load_file(&sc, argv[2]) == 0);
    assert(col_build(&w, &sc) == 0);

    static CarParams P[RACE_MAX_RACERS];
    static Car c[RACE_MAX_RACERS];
    const Car *cars[RACE_MAX_RACERS];
    for (int k = 0; k < R.nr; k++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s/%s.phy", argv[3], R.racer[k].code, R.racer[k].code);
        char *t = slurp(path);
        car_params_parse(&P[k], t);
        free(t);
        float pos[3], fwd[3];
        race_grid(&R, R.racer[k].order, 0, pos, fwd, NULL);
        car_spawn(&c[k], &P[k], &w, pos, atan2f(fwd[0], fwd[2]));
        c[k].locked = 1;
        cars[k] = &c[k];
    }
    race_track_init(&T, &R, cars);
    for (int k = 0; k < R.nr; k++) {
        race_ai_init(&A[k], &R, k, &T.rs[k], k == 0 ? 12345u : 0);
        assert(T.rs[k].flags & 4); /* standing start: behind the line */
    }
    /* one row of five: every grid node is the same */
    for (int k = 1; k < R.nr; k++)
        assert(T.rs[k].n90 == T.rs[0].n90);

    const float dt = 1.0f / 60.0f;
    float go_t = -1;
    int lap_seen[RACE_MAX_RACERS] = { 0 };
    float lap_start[RACE_MAX_RACERS] = { 0 };
    for (int f = 0; f < 60 * 60 * 8; f++) {
        float now = f * dt;
        if (race_track_update(&T, &R, cars, dt)) {
            go_t = now;
            for (int k = 0; k < R.nr; k++) {
                printf("  go at t=%.2f: %s %.3f ft/s\n", now, R.racer[k].code, car_speed(&c[k]));
                assert(fabsf(car_speed(&c[k])) < 0.1f); /* held through the countdown */
                c[k].locked = 0;
            }
        }
        int done = 1;
        for (int k = 0; k < R.nr; k++) {
            CarInput in;
            race_ai(&A[k], &R, k, &T, &c[k], dt, &in);
            car_step(&c[k], &w, &in, dt);
            for (int q = 0; q < 3; q++)
                assert(isfinite(c[k].pos[q]));
            const RaceState *s = &T.rs[k];
            if (k == trace && f % 30 == 0)
                printf("    %s t=%.1f pos %.0f %.0f v %.1f thr %.0f brk %.0f st %.2f boost %d | line %d->%d seg %d off %.2f "
                       "tgt %.1f want %.1f need %.1f | bnd %d lap %d dist %.0f place %d flags %x off %d wrong %d\n",
                       R.racer[k].code, now, c[k].pos[0], c[k].pos[2], car_speed(&c[k]), in.throttle, in.brake,
                       in.steer, in.boost, A[k].behind, A[k].ahead, A[k].seg, A[k].off, A[k].target, A[k].want_time,
                       A[k].need_time, s->n90, s->lap, s->dist, s->place, s->flags, s->off, s->wrong);
            if (s->lap != lap_seen[k]) {
                printf("  t=%6.2f %s lap %d in %.2f s (place %d)\n", now, R.racer[k].code, s->lap,
                       now - lap_start[k], s->place);
                lap_seen[k] = s->lap;
                lap_start[k] = now;
            }
            done &= (s->flags & 1) != 0;
        }
        if (done)
            break;
    }
    assert(go_t > 0);
    int fails = 0;
    for (int k = 0; k < R.nr; k++) {
        const RaceState *s = &T.rs[k];
        printf("%s: %s, place %d, %d laps, race clock %.2f s, best lap %.2f\n", R.racer[k].code,
               (s->flags & 1) ? "finished" : "DNF", s->place, s->lap, s->clock, s->best);
        fails += !(s->flags & 1);
    }
    col_free(&w);
    vcs_free(&sc);
    assert(fails == 0);
    puts("race: all racers finished");
    return 0;
}
