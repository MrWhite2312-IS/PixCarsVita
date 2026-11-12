/* Host test for vita/src/car.c: settle, drive, steer and hit a wall on synthetic
 * geometry, then (optionally) drive on a real exported scene.
 * Build: gcc -O2 -std=c99 -Ivita/src vita/src/car.c vita/src/scene.c vita/src/vcm.c vita/tests/car_test.c -lm -o car_test
 * Run:   ./car_test [scene.vcs] */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "car.h"

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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* ground plane y=0 (2000 ft square) and a wall across x at z = 600 */
    float tris[] = {
        -1000, 0, -1000,  1000, 0, -1000,  1000, 0, 1000,
        -1000, 0, -1000,  1000, 0, 1000,  -1000, 0, 1000,
        -1000, 0, 600,  1000, 0, 600,  1000, 40, 600,
        -1000, 0, 600,  1000, 40, 600,  -1000, 40, 600,
    };
    ColWorld w;
    assert(col_build_tris(&w, tris, 4) == 0);
    float o[3] = { 5, 10, 5 }, d[3] = { 0, -1, 0 }, t, n[3];
    assert(col_raycast(&w, o, d, 20, &t, n) && fabsf(t - 10) < 1e-4f && n[1] > 0.99f);

    Car c;
    float start[3] = { 0, 0, 0 };
    CarInput idle = { 0 }, gas = { 1, 0, 0, 0 }, turn = { 0.6f, 0, 1, 0 }, stop = { 0, 1, 0, 0 };
    car_spawn(&c, &w, start, 0);
    run(&c, &w, idle, 6);
    check_sane(&c);
    printf("rest: y=%.3f up=%.4f comp=%.3f v=%.4f\n", c.pos[1], c.rot[4], c.comp[0], sqrtf(c.vel[0] * c.vel[0] + c.vel[1] * c.vel[1] + c.vel[2] * c.vel[2]));
    assert(c.grounded == 4 && c.rot[4] > 0.999f);
    assert(fabsf(c.vel[0]) + fabsf(c.vel[1]) + fabsf(c.vel[2]) < 0.05f); /* parked, no creep */
    assert(fabsf(c.pos[1] - (1.2f + 0.52f)) < 0.15f); /* model ground (-0.52) on the plane */

    run(&c, &w, gas, 4);
    printf("gas 4s: z=%.1f speed=%.1f ft/s up=%.3f\n", c.pos[2], car_speed(&c), c.rot[4]);
    assert(c.pos[2] > 150 && car_speed(&c) > 60 && c.rot[4] > 0.98f && fabsf(c.pos[0]) < 1);

    float yaw0 = atan2f(c.rot[6], c.rot[8]);
    run(&c, &w, turn, 2);
    float yaw1 = atan2f(c.rot[6], c.rot[8]);
    printf("turn right 2s: yaw %.2f -> %.2f rad, up=%.3f\n", yaw0, yaw1, c.rot[4]);
    assert(yaw1 - yaw0 > 0.5f && c.rot[4] > 0.95f); /* steering right turns toward +x */

    float tstop = 0;
    while (car_speed(&c) > 3.0f && tstop < 6) {
        run(&c, &w, stop, 0.1f);
        tstop += 0.1f;
    }
    printf("brake: stopped in %.1fs\n", tstop);
    assert(tstop < 4);
    run(&c, &w, stop, 3); /* keep holding: reverse */
    printf("reverse 3s: speed=%.2f\n", car_speed(&c));
    assert(car_speed(&c) < -10);

    /* straight into the wall at z = 600 */
    car_spawn(&c, &w, (float[3]){ 0, 0, 400 }, 0);
    run(&c, &w, idle, 1);
    run(&c, &w, gas, 6);
    check_sane(&c);
    printf("wall: z=%.2f speed=%.2f\n", c.pos[2], car_speed(&c));
    assert(c.pos[2] < 600 - 1.5f && c.pos[2] > 550);
    col_free(&w);

    if (argc > 1) {
        VcsScene s;
        assert(vcs_load_file(&s, argv[1]) == 0);
        assert(col_build(&w, &s) == 0);
        printf("scene: %u solid triangles, grid %dx%d cell %.0f, %u entries\n", w.n_tri, w.nx, w.nz,
               w.cell, w.cell_start[w.nx * w.nz]);
        car_spawn(&c, &w, s.start, atan2f(s.start_dir[0], s.start_dir[2]));
        float y0 = c.pos[1];
        run(&c, &w, idle, 3);
        printf("scene rest: (%.1f %.1f %.1f) spawned at y=%.1f grounded=%d up=%.3f\n", c.pos[0], c.pos[1], c.pos[2], y0, c.grounded, c.rot[4]);
        assert(c.grounded >= 3);
        run(&c, &w, gas, 5);
        check_sane(&c);
        printf("scene drive 5s: (%.1f %.1f %.1f) speed=%.1f up=%.3f grounded=%d\n", c.pos[0], c.pos[1], c.pos[2], car_speed(&c), c.rot[4], c.grounded);
        col_free(&w);
        vcs_free(&s);
    }
    puts("car_test ok");
    return 0;
}
