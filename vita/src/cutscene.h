/*
 * PixCarsVita - in-engine cutscenes (portable C99): the timeline tools/vitacutscene.py exports
 * from an activity's .as action script (Type = CutScene, e.g. CS_Intro): animated cars, animated
 * cameras, captions and fades. Drawing is left to the caller.
 */
#ifndef PIXCARS_CUTSCENE_H
#define PIXCARS_CUTSCENE_H

#define CS_MAX_TOYS 8
#define CS_MAX_ANIMS 64
#define CS_MAX_CAMS 32
#define CS_MAX_CAPS 48
#define CS_MAX_TRACKS 32

typedef struct {
    char name[32], code[16];           /* toy name, character code (model folder) */
    float pos[3], fwd[3], up[3];       /* placement from the .toy */
} CsToy;

typedef struct { float t; int toy; char vca[48]; } CsAnim;
typedef struct { float t; int track; } CsCam;
typedef struct { float t0, t1; char text[256]; } CsCaption;

typedef struct {
    char name[32];
    float fps;
    int n;
    float *rows;                       /* n * 10: pos, look, up, horizontal FOV (4:3) */
} CsTrack;

typedef struct {
    float duration;
    char scene[16], next[32], next_start[32], voice[64];
    CsToy toy[CS_MAX_TOYS];
    int n_toys;
    CsAnim anim[CS_MAX_ANIMS];
    int n_anims;
    CsCam cam[CS_MAX_CAMS];
    int n_cams;
    CsCaption cap[CS_MAX_CAPS];
    int n_caps;
    CsTrack track[CS_MAX_TRACKS];
    int n_tracks;
    float fade_in_t, fade_in_d;        /* VideoFX fadein (from black) */
    float fade_out_t, fade_out_d, fade_out_hold; /* fadeout (to black), then hold */
} Cutscene;

/* Load dir/cutscene.txt and its cam_<track>.txt files (dir ends with '/'). 0 on success. */
int cutscene_load(Cutscene *c, const char *dir);
void cutscene_free(Cutscene *c);
/* The active camera at time t: eye, look and up directions, horizontal FOV. 0 if none. */
int cutscene_camera(const Cutscene *c, float t, float eye[3], float look[3], float up[3], float *fov);
/* The motion a toy plays at time t (index into c->anim, -1 if none) and the time into it. */
int cutscene_toy_anim(const Cutscene *c, int toy, float t, float *local_t);
/* The caption shown at time t (NULL if none) and its opacity (Subtitles TransitionTime fade). */
const char *cutscene_caption(const Cutscene *c, float t, float *alpha);
/* Black overlay opacity at time t from the fades (0 clear .. 1 black). */
float cutscene_black(const Cutscene *c, float t);

#endif
