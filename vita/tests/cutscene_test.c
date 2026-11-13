/* Host test for the cutscene timeline:
 *   gcc -O2 -std=c99 -Ivita/src vita/src/cutscene.c vita/tests/cutscene_test.c -lm -o cutscene_test
 *   ./cutscene_test extracted/vita/act/cs_intro/
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cutscene.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    Cutscene c;
    int r = cutscene_load(&c, argc > 1 ? argv[1] : "extracted/vita/act/cs_intro/");
    CHECK(r == 0, "load %d", r);
    printf("duration %.2f scene %s next %s %s voice %s: %d toys, %d anims, %d cams, %d captions, %d tracks\n",
           c.duration, c.scene, c.next, c.next_start, c.voice, c.n_toys, c.n_anims, c.n_cams, c.n_caps, c.n_tracks);
    CHECK(c.n_toys == 2 && c.n_cams == 9 && c.n_caps == 11 && c.n_tracks == 9, "counts");
    CHECK(!strcmp(c.next, "EX_RS"), "next activity");

    float eye[3], look[3], up[3], fov;
    cutscene_camera(&c, 0, eye, look, up, &fov);
    printf("t=0 eye %.1f %.1f %.1f look %.2f %.2f %.2f fov %.1f\n", eye[0], eye[1], eye[2], look[0], look[1], look[2], fov);
    CHECK(fabsf(eye[0] - 9820.83f) < 0.1f && fabsf(fov - 50.63f) < 0.1f, "first camera");
    cutscene_camera(&c, 5.0f, eye, look, up, &fov);
    printf("t=5 eye %.1f %.1f %.1f fov %.1f\n", eye[0], eye[1], eye[2], fov);
    CHECK(fabsf(eye[0] - 9876.7f) < 2.0f, "second shot (CS_Intro_S2 from 4.4 s)");

    float lt;
    int mcq = -1;
    for (int i = 0; i < c.n_toys; i++)
        if (!strcmp(c.toy[i].code, "mcq"))
            mcq = i;
    int a = cutscene_toy_anim(&c, mcq, 5.0f, &lt);
    CHECK(a >= 0 && !strcmp(c.anim[a].vca, "mcq_beft_intro_2") && fabsf(lt - 0.6f) < 1e-3f, "mcq motion at 5 s");

    float al;
    const char *cap = cutscene_caption(&c, 2.0f, &al);
    printf("caption at 2 s (alpha %.2f): %s\n", al, cap ? cap : "(none)");
    CHECK(cap && strstr(cap, "Copa"), "first caption");
    CHECK(!cutscene_caption(&c, 0.1f, &al), "no caption before 0.3 s");
    CHECK(cutscene_black(&c, 0) > 0.99f && cutscene_black(&c, 2.0f) < 0.01f, "fade in");
    CHECK(cutscene_black(&c, 41.0f) > 0.99f, "fade out");
    cutscene_free(&c);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
