/* Host test for vita/src/race.c: load race.txt, put every racer on its grid slot on the real
 * scene, drive them all with race_ai and check every car finishes the laps in sane times.
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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) {
        puts("usage: race_test race.txt scene.vcs extracted_dir");
        return 1;
    }
    int trace = argc > 4 ? atoi(argv[4]) : -1; /* racer to print every 0.5 s */
    assert(race_load(&R, argv[1]) == 0);
    printf("race: %d laps, %d racers, %d boundary nodes, %d line nodes, lap %.0f ft\n", R.laps, R.nr, R.nb, R.np,
           R.lap_len);
    assert(R.laps > 0 && R.nr > 1 && R.nb > 2 && R.np > 2 && R.lap_len > 0);

    VcsScene sc;
    ColWorld w;
    assert(vcs_load_file(&sc, argv[2]) == 0);
    assert(col_build(&w, &sc) == 0);

    static CarParams P[RACE_MAX_RACERS];
    Car c[RACE_MAX_RACERS];
    RaceState s[RACE_MAX_RACERS];
    memset(s, 0, sizeof s);
    for (int k = 0; k < R.nr; k++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s/%s.phy", argv[3], R.racer[k].code, R.racer[k].code);
        char *t = slurp(path);
        car_params_parse(&P[k], t); /* NULL falls back to McQueen */
        free(t);
        float pos[3], fwd[3];
        race_grid(&R, R.racer[k].order, 0, pos, fwd);
        car_spawn(&c[k], &P[k], &w, pos, atan2f(fwd[0], fwd[2]));
        s[k].seed = 1234u + (unsigned)k;
        race_track(&R, &s[k], c[k].pos);
        printf("  %s slot %d at %.0f %.0f %.0f progress %.0f%s\n", R.racer[k].code, R.racer[k].order, c[k].pos[0],
               c[k].pos[1], c[k].pos[2], s[k].progress, t ? "" : " (default .phy)");
    }

    const float dt = 1.0f / 60.0f;
    float last_lap[RACE_MAX_RACERS] = { 0 };
    int done = 0;
    for (int f = 0; f < 60 * 60 * 6 && done < R.nr; f++) {
        float now = f * dt;
        for (int k = 0; k < R.nr; k++) {
            if (s[k].finished)
                continue;
            int rank = 1;
            for (int j = 0; j < R.nr; j++)
                rank += j != k && race_distance(&R, &s[j]) > race_distance(&R, &s[k]);
            CarInput in;
            race_ai(&R, &s[k], k, rank, &c[k], dt, &in);
            if (now < R.racer[k].delay)
                in.throttle = 0, in.brake = 1;
            car_step(&c[k], &w, &in, dt);
            if (k == trace && f % 30 == 0)
                printf("    %s t=%.1f pos %.0f %.1f %.0f v %.1f thr %.1f brk %.0f st %.2f gear %d gnd %d node %d\n",
                       R.racer[k].code, now, c[k].pos[0], c[k].pos[1], c[k].pos[2], car_speed(&c[k]), in.throttle,
                       in.brake, in.steer, c[k].gear, c[k].grounded, s[k].path_hint);
            for (int q = 0; q < 3; q++)
                assert(isfinite(c[k].pos[q]));
            int before = s[k].lap;
            race_track(&R, &s[k], c[k].pos);
            /* lap counts laps started: crossing the line at the go makes 1 */
            if (s[k].lap > before) {
                if (s[k].lap > 1)
                    printf("  t=%6.2f %s lap %d done in %.2fs\n", now, R.racer[k].code, s[k].lap - 1, now - last_lap[k]);
                last_lap[k] = now;
            }
            if (s[k].lap > R.laps) {
                s[k].finished = 1;
                s[k].finish_time = now;
                done++;
            }
        }
        if (f % (60 * 10) == 0)
            for (int k = 0; k < R.nr; k++)
                if (!s[k].finished && f > 0)
                    printf("  t=%3.0f %s lap %d progress %.0f speed %.0f mph\n", now, R.racer[k].code, s[k].lap,
                           s[k].progress, car_speed(&c[k]) * 0.681818f);
    }

    for (int k = 0; k < R.nr; k++) {
        printf("%s: %s %.2fs (%.2fs/lap)\n", R.racer[k].code, s[k].finished ? "finished" : "DNF", s[k].finish_time,
               s[k].finish_time / R.laps);
        assert(s[k].finished);
        /* the .rpt lap times for this race are 55-63 s; allow slack for the shortcut AI */
        float per_lap = s[k].finish_time / R.laps;
        assert(per_lap > 45 && per_lap < 80);
    }
    col_free(&w);
    vcs_free(&sc);
    puts("race: all racers finished");
    return 0;
}
