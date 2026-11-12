/* Host test for vita/src/light.c: a .lit's lights, the three per object, the diffuse shading.
 * Build: gcc -O2 -std=c99 -Ivita/src vita/src/light.c vita/tests/light_test.c -lm -o light_test
 * Run:   ./light_test [extracted/vita/scene/rs.lit] */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "light.h"

int main(int argc, char **argv)
{
    SceneLights l;
    /* a sun from above-right, half ambient, one darkening spot straight down at the origin */
    lights_parse(&l, "ambient 0.5 0.5 0.5\ndir 0 1 0 0.5 0.5 0.5\n"
                     "spot 0 100 0 0 -1 0 60 90 1 0 0 0 -0.4 -0.4 -0.4\nfog 0.1 0.2 0.3 100 1000\n");
    assert(l.n_dir == 1 && l.n_spot == 1 && l.has_fog && l.fog_max == 1000);
    ObjectLights o;
    float under[3] = { 0, 0, 0 }, away[3] = { 1000, 0, 0 }, up[3] = { 0, 1, 0 }, down[3] = { 0, -1, 0 }, rgb[3];
    lights_for(&l, under, &o);
    assert(o.n == 2);
    lights_shade(&o, up, rgb);
    printf("under the spot, roof %.2f; ", rgb[0]);
    assert(fabsf(rgb[0] - 0.6f) < 1e-3f); /* 0.5 + 0.5 - 0.4 */
    lights_shade(&o, down, rgb);
    assert(fabsf(rgb[0] - 0.5f) < 1e-3f); /* ambient only */
    lights_for(&l, away, &o);
    lights_shade(&o, up, rgb);
    printf("outside its cone, roof %.2f\n", rgb[0]);
    assert(o.n == 1 && rgb[0] == 1.0f); /* saturates */
    if (argc > 1) {
        assert(lights_load(&l, argv[1]) == 0);
        printf("%s: ambient %.2f %.2f %.2f, %d directional, %d spots, fog %.0f-%.0f\n", argv[1], l.ambient[0],
               l.ambient[1], l.ambient[2], l.n_dir, l.n_spot, l.fog_min, l.fog_max);
        assert(l.n_dir >= 1);
    }
    puts("light_test ok");
    return 0;
}
