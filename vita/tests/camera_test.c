/* Host test for camera.c with the game's rscamera.ini (extracted/vita/mcq/rscamera.ini).
   gcc -O2 -I../src camera_test.c ../src/camera.c -lm -o camera_test && ./camera_test <ini> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "camera.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "../../extracted/vita/mcq/rscamera.ini", "rb");
    if (!f) { printf("no rscamera.ini\n"); return 1; }
    static char ini[200000];
    ini[fread(ini, 1, sizeof ini - 1, f)] = 0;
    fclose(f);
    FollowCam c;
    int n = followcam_load(&c, ini, "mcq");
    CHECK(n == 2, "presets %d", n);
    const CamPreset *p = &c.p[0];
    printf("McQ1: ax %.1f ay %.1f dist %.1f fov %.0f off %.1f %.1f %.1f look %.2f %.2f %.2f pos %.2f %.2f %.2f\n",
           p->angle_x * 57.2958f, p->angle_y * 57.2958f, p->dist, p->fov, p->body_offset[0], p->body_offset[1],
           p->body_offset[2], p->look_f[0], p->look_f[1], p->look_f[2], p->pos_f[0], p->pos_f[1], p->pos_f[2]);
    CHECK(fabsf(p->angle_x * 57.2958f - 26) < 0.01f && fabsf(p->dist - 19) < 1e-4f && p->fov == 75, "McQ1 override");
    CHECK(fabsf(p->body_offset[1] - 2.5f) < 1e-4f, "Preset1 OffsetFromBodyPoint kept");
    CHECK(fabsf(c.p[1].dist - 32) < 1e-4f, "McQ2 distance");

    /* parked at the origin facing +Z */
    const float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, pos[3] = { 0, 0, 0 };
    float vel[3] = { 0, 0, 0 }, eye[3], look[3], fov;
    followcam_update(&c, pos, rot, vel, 1 / 60.0f, eye, look, &fov);
    float dx = eye[0], dy = eye[1] - 2.5f, dz = eye[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz), pitch = asinf(dy / d) * 57.2958f;
    printf("parked: eye %.2f %.2f %.2f look %.2f %.2f %.2f dist %.2f pitch %.1f\n", eye[0], eye[1], eye[2], look[0],
           look[1], look[2], d, pitch);
    CHECK(eye[2] < 0 && eye[1] > 2.5f && fabsf(eye[0]) < 1e-3f, "behind and above");
    CHECK(fabsf(d - 19) < 0.01f, "19 ft from the body point");
    CHECK(fabsf(pitch - 26 * 0.7f) < 0.1f, "low-speed AngleX 0.7 * 26");

    /* driving along +X at 100 ft/s for 3 s: the camera ends up behind (-X), at full AngleX */
    const float rx[9] = { 0, 0, -1, 0, 1, 0, 1, 0, 0 };
    float p2[3] = { 0, 0, 0 };
    vel[0] = 100;
    for (int i = 0; i < 180; i++) {
        p2[0] += vel[0] / 60.0f;
        followcam_update(&c, p2, rx, vel, 1 / 60.0f, eye, look, &fov);
    }
    dx = eye[0] - p2[0], dy = eye[1] - 2.5f, dz = eye[2];
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    printf("driving +X: eye-car %.2f %.2f %.2f dist %.2f pitch %.1f\n", dx, dy, dz, d, asinf(dy / d) * 57.2958f);
    CHECK(dx < -15 && fabsf(dz) < 0.5f, "behind along -X");

    /* first-person camera (fpcam.ini [1Player] values): eye 3.8 ft up the car's axis, looking
       along the nose; after a 90 degree turn the filtered view catches up within a second */
    const char *fp_ini = "[1Player]\nFOV=90.0\nOffsetFromBodyPoint=0.0,3.8,0.0\nLookFilter=0.04,0.05,0.05\n";
    FirstPersonCam fp;
    CHECK(fpcam_load(&fp, fp_ini) == 1, "fpcam [1Player]");
    const float rz[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    fpcam_update(&fp, p2, rz, 1 / 60.0f, eye, look, &fov);
    CHECK(fabsf(eye[1] - 3.8f) < 1e-4f && fabsf(eye[0] - p2[0]) < 1e-4f && fov == 90, "fp eye on the roof");
    CHECK(look[2] - eye[2] > 9.9f, "fp looks along the nose");
    for (int i = 0; i < 60; i++)
        fpcam_update(&fp, p2, rx, 1 / 60.0f, eye, look, &fov);
    printf("first person after a turn to +X: look dir %.2f %.2f %.2f\n", (look[0] - eye[0]) / 10,
           (look[1] - eye[1]) / 10, (look[2] - eye[2]) / 10);
    CHECK(look[0] - eye[0] > 9.9f, "fp view follows the nose");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
