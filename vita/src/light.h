/*
 * PixCarsVita - the game's scene lighting (portable C99): the <scene>.lit written by
 * tools/vitascene.py from the .scn [LightN] / [Fog] sections, and the per-object lighting of
 * the game's diffuse vertex shaders (DXVAnimSkin4Diffuse.xvu, decoded from its NV2A
 * microcode): colour = ambient + sum over 3 lights of max(0, N.L) * colour, saturated.
 */
#ifndef PIXCARS_LIGHT_H
#define PIXCARS_LIGHT_H

#define LIGHT_MAX_DIR 4
#define LIGHT_MAX_SPOT 96

typedef struct {
    float pos[3], dir[3];      /* dir: where the spot points */
    float inner, outer;        /* full cone angles, radians */
    float att[3], range;       /* 1 / (a0 + a1 d + a2 d^2); range 0 = unlimited */
    float col[3];              /* may be negative: the game darkens with them */
} SpotLight;

typedef struct {
    float ambient[3];
    int n_dir;
    float dir[LIGHT_MAX_DIR][3];   /* toward the light, world space */
    float dir_col[LIGHT_MAX_DIR][3];
    int n_spot;
    SpotLight spot[LIGHT_MAX_SPOT];
    int has_fog;
    float fog_col[3], fog_min, fog_max;
} SceneLights;

/* The three lights the shader gets for an object at pos: the directional lights, then the
   spots that reach it most strongly (each as a directional light from the spot). */
typedef struct {
    float ambient[3];
    int n;
    float dir[3][3], col[3][3];
} ObjectLights;

int lights_parse(SceneLights *l, const char *text);
int lights_load(SceneLights *l, const char *path);
void lights_for(const SceneLights *l, const float pos[3], ObjectLights *o);
/* lit colour (0..1 per channel, saturated) of a unit normal n, both in the same space as o */
void lights_shade(const ObjectLights *o, const float n[3], float rgb[3]);

#endif
