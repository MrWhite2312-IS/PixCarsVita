/* Host test for vita/src/car.c: .phy parsing, then settle, accelerate (gears), steer, brake,
 * reverse and hit a wall on synthetic geometry, then (optionally) drive on a real scene.
 * Build: gcc -O2 -std=c99 -Ivita/src vita/src/car.c vita/src/scene.c vita/src/vcm.c vita/tests/car_test.c -lm -o car_test
 * Run:   ./car_test [mcq.phy [scene.vcs]] */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "car.h"

static CarParams P;

static void run(Car *c, const ColWorld *w, CarInput in, float secs)
{
    for (int i = 0; i < (int)(secs * 60); i++)
        car_step(c, w, &in, 1.0f / 60.0f);
}

static void check_sane(const Car *c)
{
    for (int k = 0; k < 3; k++)
        assert(isfinite(c->pos[k]) && isfinite(c->vel[k]));
}

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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    car_params_parse(&P, NULL);
    if (argc > 1) {
        /* the real file must reproduce the built-in McQueen values */
        CarParams q;
        char *t = slurp(argv[1]);
        assert(t);
        car_params_parse(&q, t);
        assert(q.weight == 3000 && q.cofg[2] == 0.7f && q.n_gears == 5 && q.gear[0] == 6);
        assert(q.hp[6] == 540 && q.tire[2].drive.scale == 13.6f && q.tire[0].side.scale == 6.2f);
        assert(q.aero_area == 35 && q.steer_scale[1] == 0.23f && q.cam_distance == 16);
        assert(memcmp(&q, &P, sizeof q) == 0);
        free(t);
        puts("phy: parsed values match");
    }

    /* ground plane y=0 (4000 ft square) and a wall across x at z = 1600 */
    float tris[] = {
        -2000, 0, -2000,  2000, 0, -2000,  2000, 0, 2000,
        -2000, 0, -2000,  2000, 0, 2000,  -2000, 0, 2000,
        -2000, 0, 1600,  2000, 0, 1600,  2000, 40, 1600,
        -2000, 0, 1600,  2000, 40, 1600,  -2000, 40, 1600,
    };
    ColWorld w;
    assert(col_build_tris(&w, tris, 4) == 0);
    float o[3] = { 5, 10, 5 }, d[3] = { 0, -1, 0 }, t, n[3];
    assert(col_raycast(&w, o, d, 20, &t, n) && fabsf(t - 10) < 1e-4f && n[1] > 0.99f);

    Car c;
    float start[3] = { 0, 0, -1500 };
    CarInput idle = { 0 }, gas = { 1, 0, 0, 0 }, turn = { 0.6f, 0, 1, 0 }, stop = { 0, 1, 0, 0 };
    car_spawn(&c, &P, &w, start, 0);
    run(&c, &w, idle, 4);
    check_sane(&c);
    float v = sqrtf(c.vel[0] * c.vel[0] + c.vel[1] * c.vel[1] + c.vel[2] * c.vel[2]);
    float M[16];
    car_matrix(&c, M);
    printf("rest: model origin y=%.3f up=%.4f comp=%.3f/%.3f v=%.4f\n", M[13], c.rot[4], c.comp[0], c.comp[2], v);
    assert(c.grounded == 4 && c.rot[4] > 0.999f && v < 0.05f);
    /* tyres (radius 1.4, centre at bind 0.96) on the plane: origin ~0.44 up */
    assert(fabsf(M[13] - 0.44f) < 0.1f);

    float z0 = c.pos[2], t60 = -1, tt = 0;
    int max_gear = 1;
    while (tt < 12) {
        run(&c, &w, gas, 0.05f);
        tt += 0.05f;
        if (t60 < 0 && car_speed(&c) * 0.681818f >= 60)
            t60 = tt;
        if (c.gear > max_gear)
            max_gear = c.gear;
    }
    printf("gas 12s: 0-60 mph %.2fs, %.0f ft, %.1f mph, gear %d (max %d), rpm %.0f, up=%.3f\n", t60,
           c.pos[2] - z0, car_speed(&c) * 0.681818f, c.gear, max_gear, c.rpm, c.rot[4]);
    assert(t60 > 0 && t60 < 8 && max_gear >= 3 && c.rot[4] > 0.98f && fabsf(c.pos[0]) < 2);

    float yaw0 = atan2f(c.rot[6], c.rot[8]);
    run(&c, &w, turn, 2);
    float yaw1 = atan2f(c.rot[6], c.rot[8]);
    printf("turn right 2s: yaw %.2f -> %.2f rad, %.1f mph, up=%.3f\n", yaw0, yaw1, car_speed(&c) * 0.681818f, c.rot[4]);
    assert(yaw1 - yaw0 > 0.3f && c.rot[4] > 0.9f); /* steering right turns toward +x */

    float tstop = 0, v0 = car_speed(&c);
    while (car_speed(&c) > 1.0f && tstop < 10) {
        run(&c, &w, stop, 0.05f);
        tstop += 0.05f;
    }
    printf("brake from %.1f mph: stopped in %.2fs (%.2f g)\n", v0 * 0.681818f, tstop, v0 / tstop / 32.174f);
    assert(tstop < 6);
    run(&c, &w, stop, 3); /* keep holding: reverse */
    printf("reverse 3s: %.1f mph gear %d\n", car_speed(&c) * 0.681818f, c.gear);
    assert(car_speed(&c) < -10 && c.gear == -1);

    /* reset in place (pos aliases the car): stays where it is */
    float px = c.pos[0], pz = c.pos[2];
    car_spawn(&c, &P, &w, c.pos, 0);
    assert(c.pos[0] == px && c.pos[2] == pz && fabsf(c.pos[1] - 3.0f) < 1e-3f);

    /* straight into the wall at z = 1600 */
    car_spawn(&c, &P, &w, (float[3]){ 0, 0, 1200 }, 0);
    run(&c, &w, idle, 1);
    run(&c, &w, gas, 8);
    check_sane(&c);
    printf("wall: z=%.2f speed=%.2f\n", c.pos[2], car_speed(&c));
    assert(c.pos[2] < 1600 - 1.5f && c.pos[2] > 1550);
    col_free(&w);

    if (argc > 2) {
        VcsScene s;
        assert(vcs_load_file(&s, argv[2]) == 0);
        assert(col_build(&w, &s) == 0);
        printf("scene: %u solid triangles, grid %dx%d cell %.0f, %u entries\n", w.n_tri, w.nx, w.nz,
               w.cell, w.cell_start[w.nx * w.nz]);
        car_spawn(&c, &P, &w, s.start, atan2f(s.start_dir[0], s.start_dir[2]));
        run(&c, &w, idle, 3);
        printf("scene rest: (%.1f %.1f %.1f) grounded=%d up=%.3f\n", c.pos[0], c.pos[1], c.pos[2], c.grounded, c.rot[4]);
        assert(c.grounded >= 3);
        run(&c, &w, gas, 5);
        check_sane(&c);
        printf("scene drive 5s: (%.1f %.1f %.1f) %.1f mph up=%.3f grounded=%d\n", c.pos[0], c.pos[1], c.pos[2],
               car_speed(&c) * 0.681818f, c.rot[4], c.grounded);
        col_free(&w);
        vcs_free(&s);
    }
    puts("car_test ok");
    return 0;
}
