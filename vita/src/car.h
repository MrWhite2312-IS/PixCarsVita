/*
 * PixCarsVita - drivable car (portable C99): a collision grid built from a VCS scene
 * and a rigid-body car with four raycast wheels (spring/damper suspension, tyre grip
 * with a friction circle, drive/brake, drag) plus sphere probes against walls.
 *
 * Game space: D3D left-handed, y up, units = feet. Car frame rows: right, up, fwd (+Z).
 */
#ifndef PIXCARS_CAR_H
#define PIXCARS_CAR_H

#include <stdint.h>
#include "scene.h"

typedef struct {
    float *tri;                /* n_tri * 9, world space */
    uint32_t n_tri;
    float x0, z0, cell;        /* XZ grid over the triangles */
    int nx, nz;
    uint32_t *cell_start;      /* nx*nz + 1 offsets into items */
    uint32_t *items;           /* triangle indices */
} ColWorld;

/* Solid geometry of a scene (alpha-tested / blended layers such as shrubs and decals skipped). */
int col_build(ColWorld *w, const VcsScene *s);
/* Grid over a world-space triangle soup (copied). */
int col_build_tris(ColWorld *w, const float *tris, uint32_t n_tri);
void col_free(ColWorld *w);
/* Nearest hit along o + d*t, 0 <= t <= maxt (d unit). Returns 1 and t / unit normal on a hit. */
int col_raycast(const ColWorld *w, const float o[3], const float d[3], float maxt, float *t, float n[3]);

typedef struct {
    float throttle;            /* 0..1 */
    float brake;               /* 0..1, reverses once stopped */
    float steer;               /* -1 left .. 1 right */
    int handbrake;
} CarInput;

typedef struct {
    float pos[3];              /* centre of mass */
    float vel[3], angvel[3];   /* world space */
    float rot[9];              /* rows: right, up, fwd */
    float steer;               /* current front wheel angle (rad) */
    float comp[4];             /* suspension compression per wheel (FL, FR, RL, RR) */
    float spin[4];             /* wheel roll angle (rad) */
    int grounded;              /* wheels touching */
} Car;

void car_spawn(Car *c, const ColWorld *w, const float pos[3], float yaw);
void car_step(Car *c, const ColWorld *w, const CarInput *in, float dt);
/* Model (bind pose, ground at y = -0.52) to world, row-major v' = v * M. */
void car_matrix(const Car *c, float out[16]);
/* Vertical offset of a wheel from its bind-pose position (suspension travel). */
float car_wheel_offset(const Car *c, int wheel);
float car_speed(const Car *c);

#endif
