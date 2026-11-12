/*
 * PixCarsVita - the game's follow camera (CarsCameraFollow, PS2 update 0x21f4f0) with the
 * presets of data\rscamera.ini: [Preset<n>] overridden by the character's [<Code><n>].
 * All vectors in game (D3D, left-handed, y up) space, feet and seconds.
 */
#ifndef PIXCARS_CAMERA_H
#define PIXCARS_CAMERA_H

#define CAM_MAX_PRESETS 4

typedef struct {
    float angle_x, angle_y;            /* radians (the loader converts, 0x2243c4) */
    float dist, min_dist, max_dist;    /* FollowingDistance, Min, Max */
    float fov, min_fov, max_fov;       /* degrees, horizontal on a 4:3 screen */
    float look_ahead;
    int cycle, swing;                  /* CycleThruThisPreset, SwingEnabled */
    float cam_swing, look_swing;
    float look_f[3], pos_f[3];         /* LookFilters / PosFilters: time constants (s) per axis */
    float ls_cutoff, ls_scale;         /* LowSpeedAngleXVelCutoff, LowSpeedAngleXScaleFactor */
    float body_offset[3], body_look[3];/* OffsetFromBodyPoint, BodyLookOffset (x side, y up, z ahead) */
    float min_ay_la, max_ay_la;        /* Min/MaxAngleYLookAheadAngle, as read (degrees) */
    float velocity_adjust;
} CamPreset;

typedef struct {
    CamPreset p[CAM_MAX_PRESETS];
    int n, cur;
    int init;
    float pos[3], look[3];             /* filtered eye and look-at point */
    float heading[3];
    float yaw_prev, yaw_rate;
} FollowCam;

/* Parse rscamera.ini (its text) for character code (e.g. "mcq"); returns the preset count. */
int followcam_load(FollowCam *c, const char *ini, const char *code);
/* Next preset with CycleThruThisPreset = 1 (the Camera Preset button); returns 0 after the
   last one (wrapping to the first). */
int followcam_next(FollowCam *c);
void followcam_reset(FollowCam *c);
/* One frame: car origin, rotation rows (right, up, fwd) and world velocity. Writes the eye,
   the look-at point and the horizontal FOV (degrees, 4:3). */
void followcam_update(FollowCam *c, const float car_pos[3], const float rot[9], const float vel[3], float dt,
                      float eye[3], float look[3], float *fov);

/* The game's first-person camera (CarsCameraFirstPerson, PS2 update 0x21b900; datapcam.ini
   [1Player], loaded at 0x21b540): the eye fixed at OffsetFromBodyPoint in the car's frame, the
   view along the car's nose filtered per world axis by LookFilter. The game hides the car while
   it is active (0x21b050). */
typedef struct {
    float fov;                         /* horizontal degrees on 4:3 */
    float offset[3];                   /* OffsetFromBodyPoint (x side, y up, z ahead) */
    float look_f[3];                   /* LookFilter time constants (s) */
    float look[3];                     /* filtered view direction */
    int init;
} FirstPersonCam;

/* Parse fpcam.ini (its text); returns 1 if [1Player] was found. */
int fpcam_load(FirstPersonCam *c, const char *ini);
void fpcam_reset(FirstPersonCam *c);
void fpcam_update(FirstPersonCam *c, const float car_pos[3], const float rot[9], float dt,
                  float eye[3], float look[3], float *fov);

#endif
