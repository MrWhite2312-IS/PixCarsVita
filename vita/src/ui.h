/*
 * PixCarsVita - the game's front-end UI: fonts, strings and layers exported by tools/vitaui.py
 * (ui.vcs textures + ui.txt), drawn in the game's 640x480 UI space mapped onto the Vita screen.
 */
#ifndef PIXCARS_UI_H
#define PIXCARS_UI_H

#include <stdint.h>

#include "vcm.h"

#define UI_MAX_GLYPHS 256
#define UI_MAX_FONTS 4
#define UI_MAX_ITEMS 64
#define UI_MAX_STRS 64
#define UI_MAX_SIGNS 8

typedef struct {
    float u0, v0, u1, v1;
    int16_t w, h, xoff, yoff;
    uint8_t present;
} UiGlyph;

typedef struct {
    char name[48];
    int tex, line_h;
    UiGlyph g[UI_MAX_GLYPHS];
} UiFont;

typedef struct {
    char name[24];             /* component name, spaces as '_' */
    int layer, kind;           /* kind 0 icon, 1 text */
    int tex, font;
    char id[48];               /* text: string ID */
    float x, y, w, h, u0, u1, v0, v1, scale;
    uint8_t rgba[4];
    int hj, vj;
    float max_w;               /* text: word-wrap width (0 = one line) */
    int max_lines;
} UiItem;

typedef struct {
    char id[48];
    char desc[48];             /* description string ID, "" if none */
    int tex;
    uint8_t rgba[4];           /* tint (BackgroundColor), scaled by the slot's brightness */
} UiSign;

typedef struct {
    char name[16];
    float pos[3], fwd[3];
    float fov; /* horizontal degrees on a 4:3 screen, 0 if unknown */
} UiCamera;

typedef struct {
    unsigned gl_tex[32];
    int tex_w[32], tex_h[32];
    char tex_names[32][48];
    int n_tex;
    UiFont font[UI_MAX_FONTS];
    int n_font;
    UiItem item[UI_MAX_ITEMS];
    int n_item;
    char layers[8][32];
    int n_layers;
    struct { char id[48]; char text[128]; } str[UI_MAX_STRS];
    int n_str;
    /* the main menu's CarsUISignMenu */
    float sign_x, sign_y, label_x, label_y;
    int sign_font;
    char sign_label[48];
    uint8_t sign_rgba[4], sign_sel_rgba[4];
    float sign_anim;           /* AnimationDuration: one sign step, seconds */
    /* FrontEndMenuBackground [General]: duration, max/final circle scale, circle ratio,
       header speed, TitleX1..3 */
    float bg[8];
    UiSign sign[UI_MAX_SIGNS];
    int n_sign;
    UiCamera cam[4];
    int n_cam;
    float boost_len[3];        /* BoostMeterOverlay.lyr BoostLevelLengths, by NOX level */
} Ui;

/* Load <dir>ui.vcs + <dir>ui.txt; upload(tex) turns a scene texture into a GL name. */
int ui_load(Ui *ui, const char *dir, unsigned (*upload)(const VcmTexture *t));
/* Text for a string ID (set with ui_set_str for the game's STR_DYN_* placeholders), "" if none. */
const char *ui_str(const Ui *ui, const char *id);
void ui_set_str(Ui *ui, const char *id, const char *text);
const UiCamera *ui_camera(const Ui *ui, const char *name);
int ui_tex(const Ui *ui, const char *name);

/* 2D drawing: call ui_begin once per frame before the draw calls (sets an ortho view). */
void ui_begin(void);
void ui_draw_layer(const Ui *ui, const char *layer);
/* The menu background with its intro animation as the PS2 game plays it (0x27b210):
   p 0..1 over bg[0] seconds (reverse it for the outro); header_t: seconds since the intro
   ended, scrolls the checkered header strip. */
void ui_draw_background(const Ui *ui, float p, float header_t);
/* The sign menu (PS2 0x2490d0 / 0x249560): five signs, the selected one centred; dir +1/-1
   slides them one step towards the next/previous item, t 0..1 over sign_anim seconds.
   time drives the label's pulse. */
void ui_draw_signs(const Ui *ui, int selected, int dir, float t, float time);
/* The in-game boost meter: bar length from BoostLevelLengths (by NOX level), fill 0..1. */
void ui_draw_boost_meter(const Ui *ui, float length, float fill);
void ui_text(const Ui *ui, int font, const char *s, float x, float y, float scale, const uint8_t rgba[4], int hj, int vj);
/* SpeedometerOverlay (PS2 0x3b01b0): speed in ft/s shown in mph (x0.681818) or the game's km/h
   (x1.08), rounded, as three digit slots with leading blanks; reverse shows the R message
   instead (0x3b0110). */
void ui_draw_speedometer(Ui *ui, float speed, int kph, int reverse);
/* ExploreMap compass (PS2 0x3ac950): heading-up ring, the player's marker in the middle and the
   north arrow on a circle around it. heading = atan2(z, x) of the view direction in game space. */
void ui_draw_compass(const Ui *ui, float heading);

#endif
