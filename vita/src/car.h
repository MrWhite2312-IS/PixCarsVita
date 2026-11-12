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
    float max_decomp_rate;     /* MaxDecompressionRate: how fast the wheel may drop (ft/s) */
} TireParams;

typedef struct {
    float weight, gravity_scale, inertia_scale[3], cofg[3];
    float restitution, body_friction, yaw_rate_limit, normal_load_scale, handbrake_scale;
    TireParams tire[4];        /* FL, FR, RL, RR */
    int drivetrain, n_rpm, n_gears;
    float rpm[16], hp[16], gear[8], final_ratio, loss, min_rpm, engine_braking;
    float engine_inertia, idle_throttle, max_k, pump_torque_scale, max_pump_torque;
    float low_rpm, low_up, low_down, high_rpm, high_up, high_down;
    float shift_time, shift_up_time, shift_up_throttle, shift_down_time, shift_down_throttle;
    float reverse_time;
    /* NOX (boost): capacity = NOXBaseVolume * NOXLevel, used at NOXBurnRate per second for
       NOXHPBoost extra horsepower; refills at NOXRefreshRate after NOXRefreshTime */
    float nox_level, nox_base, nox_burn, nox_hp, nox_refresh, nox_refresh_time;
    float burnout_time, burnout_charge, burnout_thrust;
    /* current tune up: drag only above AeroMinSpeed (ft/s), scaled by NOXStreamlining on boost */
    float aero_area, aero_drag, aero_min_speed, nox_streamlining;
    float airborne_damping, air_moment[3], unroll_gain, unroll_torque, unyaw_gain, unyaw_torque;
    /* [Powerslide] (speeds in ft/s, angles in radians, as the PS2 loader converts them) */
    float ps_yaw, ps_hs_yaw, ps_min_speed, ps_max_speed, ps_threshold, ps_front, ps_rear, ps_torque;
    float ps_ls_front, ps_ls_rear, ps_ls_rear_steer, ps_min_angle, ps_max_angle, ps_min_boost, ps_max_boost;
    float ps_exit_time, ps_front_exit, ps_rear_exit, ps_exit_yaw, ps_steer_scale, ps_rear_steer_scale;
    /* [LandingControl] */
    float land_rear, trans_rear, land_front, trans_front, land_exit_angle, land_exit_yaw;
    /* [Preload] (jump) and [Wheelstand] (tilt, angle in radians) */
    float preload_time, max_preload, preload_charge, max_preload_time;
    float ws_angle, ws_roll_gain, ws_roll_rate_gain, ws_steer_scale, ws_torque_scale;
    /* [Joystick] (steer speeds in mph) */
    float steer_power, x_rate, gas_rate, brake_rate, reverse_steer, xdpad_on, xdpad_off;
    float steer_speed[3], steer_scale[3], dpad_speed[3], dpad_scale[3];
    float cam_offset[3], cam_distance;
} CarParams;

/* Fill from the text of a .phy (INI). Keys missing from the text keep McQueen's values. */
void car_params_parse(CarParams *p, const char *text);

/* One frame of the game's driving actions (controller_presets_ps2.txt). */
typedef struct {
    float throttle;            /* Gas, 0..1 */
    float brake;               /* Brake, 0..1 (stop, then press again to reverse) */
    float steer;               /* stick x, -1 left .. 1 right */
    float steer_y;             /* stick y, -1 up .. 1 down (pull back then push up: Jump) */
    int dpad_x, dpad_y;        /* digital steer / jump: -1, 0, 1 (up = -1) */
    int ebrake;                /* EBrake */
    int powerslide;            /* Powerslide (held) */
    int tilt;                  /* Tilt (two-wheel driving, held) */
    int boost;                 /* Boost (held) */
} CarInput;

typedef struct {
    const CarParams *p;
    float pos[3];              /* centre of mass */
    float vel[3], angvel[3];   /* world space */
    float rot[9];              /* rows: right, up, fwd */
    float steer, rear_steer;   /* wheel angles (rad) */
    float comp[4];             /* suspension compression per wheel (FL, FR, RL, RR) */
    float spin[4];             /* wheel roll angle (rad) */
    float omega[4];            /* wheel spin rate (rad/s) */
    float slip[4];             /* longitudinal slip ratio (relaxed at low speed, as in the game) */
    float fx_prev[4];          /* last longitudinal tyre force (lbf), loads the wheel spin */
    int gear;                  /* 1..n, -1 reverse */
    float rpm, shift_t, shift_cool;
    int grounded;              /* wheels on the ground */
    int contact[4];            /* per wheel: touching the ground (tyre +0x1aa bit 2) */
    float gnormal[4][3];       /* per wheel: ground normal at the contact (tyre +0x120) */
    /* controls, in the PS2 vehicle's terms (offsets of CarsVehiclePhysics) */
    float x, x_prev, y, xf, yf;/* stick +1628/1632/1636, filtered +1640/1644 */
    int digital;               /* steering from the d-pad (+1015 bit 3) */
    float gas_in, brake_in, ebrake_in; /* +1216, +1212, +1220 */
    float steer_cmd, rear_cmd; /* +1256 / +1260: fractions of the front / rear max steer angle */
    float gas, brake;          /* engine throttle +1240, brake level +1248 (ramp +1252) */
    float brake_acc;
    int hold, rev_arm, rev_ready; /* stopped hold (+1009 b7), reverse arming (+1014 b3, b2) */
    float rev_t;               /* +1100 */
    int burn_arm, burn_hold, burn_charge; float burn_t; /* burnout +1236 +1244 +1013 b2 +1284 */
    int jump_on, jump_charge, jumped; float jump_t, jump_since, jump_win; /* +1012 b0 b1, +1464.. */
    int tilt, tilt_exit, tilt_start; float tilt_t, tilt_side; /* +1014 b4 b6, +1492 +1496 */
    int ps, ps_low, ps_armed, ps_button; /* powerslide (+1013 b4), low-speed slide (b5), released (b7) */
    float ps_entry_x, ps_entry_speed, ps_exit_t, hb_yaw;
    int hb_on, front_drive;
    int landing;               /* landing control (+1009 b2): set with all wheels in the air */
    /* Backwards (0x386790): EBrake tap, release, press within 0.5 s at > 50 mph, steering
       centred: a 180 at speed, then driving backwards until under 50 mph (+1013 b0) */
    int backwards, turn_state, turn_auto, turned; float turn_t, turn_yaw, turn_dir, turn_vel[3];
    float turn_rpm, turn_sign;
    float lat_scale[4], yaw_limit, boost, torque_scale;
    float nox, nox_delay; int nox_on;  /* engine +140, +164, +128 */
    /* measured each step: forward / lateral speed (+760 / +752), |v| (+1840), slip angle */
    float fwd, lat, spd, slip_angle;
    /* per car_step, for sounds: max driven-wheel spin slip, max rear slip angle (rad),
       max strut closing speed and wall impact speed (ft/s) */
    float drive_slip, side_slip, strut_hit, impact;
} Car;

void car_spawn(Car *c, const CarParams *p, const ColWorld *w, const float pos[3], float yaw);
void car_step(Car *c, const ColWorld *w, const CarInput *in, float dt);
/* Model (bind pose) to world, row-major v' = v * M. */
void car_matrix(const Car *c, float out[16]);
/* Vertical offset of a wheel from its bind-pose position (suspension travel). */
float car_wheel_offset(const Car *c, int wheel);
float car_speed(const Car *c);

#endif
