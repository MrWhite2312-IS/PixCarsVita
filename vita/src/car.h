/*
 * PixCarsVita - drivable car (portable C99): a collision grid built from a VCS scene
 * and a rigid-body car driven by the game's own <char>.phy parameters (mass, centre of
 * gravity, per-tyre suspension and force curves, engine HP curve and gearbox, brakes,
 * aero drag, steering tables, camera), plus sphere probes against walls.
 *
 * Game space: D3D left-handed, y up, units = feet, pounds. Car frame rows: right, up, fwd (+Z).
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

/* Curve "Scale*SpeedScale*x / (1 + |RoundOff*SpeedScale*x|^DropOff)" (PS2 ELF @0x216028). */
typedef struct { float round_off, drop_off, scale, speed_scale; } TireCurve;

typedef struct {
    float max_comp, max_comp_rate, radius, spring, bounce_damp, rebound_damp, max_steer;
    TireCurve side, drive;     /* surface 1 */
    float max_brake_torque, inertia;
} TireParams;

typedef struct {
    float weight, gravity_scale, inertia_scale[3], cofg[3];
    float restitution, body_friction, yaw_rate_limit, normal_load_scale, handbrake_scale;
    TireParams tire[4];        /* FL, FR, RL, RR */
    int drivetrain, n_rpm, n_gears;
    float rpm[16], hp[16], gear[8], final_ratio, loss, min_rpm, engine_braking;
    float low_rpm, low_up, low_down, high_rpm, high_up, high_down;
    float shift_up_time, shift_up_throttle, shift_down_time, shift_down_throttle;
    float aero_area, aero_drag;
    float steer_power, x_rate, gas_rate, brake_rate, reverse_steer;
    float steer_speed[3], steer_scale[3];
    float cam_offset[3], cam_distance;
} CarParams;

/* Fill from the text of a .phy (INI). Keys missing from the text keep McQueen's values. */
void car_params_parse(CarParams *p, const char *text);

typedef struct {
    float throttle;            /* 0..1 */
    float brake;               /* 0..1, reverses once stopped */
    float steer;               /* -1 left .. 1 right (raw stick) */
    int handbrake;
} CarInput;

typedef struct {
    const CarParams *p;
    float pos[3];              /* centre of mass */
    float vel[3], angvel[3];   /* world space */
    float rot[9];              /* rows: right, up, fwd */
    float steer;               /* current front wheel angle (rad) */
    float steer_in, gas, brake;/* rate-limited inputs */
    float comp[4];             /* suspension compression per wheel (FL, FR, RL, RR) */
    float spin[4];             /* wheel roll angle (rad) */
    float omega[4];            /* wheel spin rate (rad/s) */
    float slip[4];             /* longitudinal slip ratio (relaxed at low speed, as in the game) */
    float fx_prev[4];          /* last longitudinal tyre force (lbf), loads the wheel spin */
    int gear;                  /* 1..n, -1 reverse */
    float rpm, shift_t;
    int grounded;
} Car;

void car_spawn(Car *c, const CarParams *p, const ColWorld *w, const float pos[3], float yaw);
void car_step(Car *c, const ColWorld *w, const CarInput *in, float dt);
/* Model (bind pose) to world, row-major v' = v * M. */
void car_matrix(const Car *c, float out[16]);
/* Vertical offset of a wheel from its bind-pose position (suspension travel). */
float car_wheel_offset(const Car *c, int wheel);
float car_speed(const Car *c);

#endif
