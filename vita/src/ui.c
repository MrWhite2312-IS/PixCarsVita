/*
 * PixCarsVita - front-end UI. See ui.h.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitaGL.h>

#include "scene.h"

#define SCREEN_W 960.0f
#define SCREEN_H 544.0f
/* The game lays its UI out on a 640x480 screen. Positions stretch to the Vita's 16:9;
   text keeps its proportions (vertical factor on both axes).
   shortcut: icons stretch with the screen; revisit if a widescreen layout is wanted. */
#define SX (SCREEN_W / 640.0f)
#define SY (SCREEN_H / 480.0f)

static int find_font(const Ui *ui, const char *name)
{
    for (int i = 0; i < ui->n_font; i++)
        if (!strcmp(ui->font[i].name, name))
            return i;
    return -1;
}

static int find_layer(Ui *ui, const char *name)
{
    for (int i = 0; i < ui->n_layers; i++)
        if (!strcmp(ui->layers[i], name))
            return i;
    return -1;
}

int ui_tex(const Ui *ui, const char *name)
{
    for (int i = 0; i < ui->n_tex; i++)
        if (!strcmp(ui->tex_names[i], name))
            return i;
    return -1;
}

int ui_load(Ui *ui, const char *dir, unsigned (*upload)(const VcmTexture *t))
{
    char path[256];
    memset(ui, 0, sizeof *ui);
    VcsScene s;
    snprintf(path, sizeof path, "%sui.vcs", dir);
    int r = vcs_load_file(&s, path);
    if (r)
        return r;
    ui->n_tex = s.n_tex < 32 ? (int)s.n_tex : 32;
    for (int i = 0; i < ui->n_tex; i++) {
        ui->gl_tex[i] = upload(&s.tex[i]);
        ui->tex_w[i] = (int)s.tex[i].width;
        ui->tex_h[i] = (int)s.tex[i].height;
    }
    const float bg_defaults[8] = { 0.5f, 2.7f, 2.0f, 0.8f, 1.0f, 400, 20, 55 };
    memcpy(ui->bg, bg_defaults, sizeof bg_defaults);
    ui->sign_anim = 1.0f;
    vcs_free(&s);

    snprintf(path, sizeof path, "%sui.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[512], a[64], b[64];
    int layer = -1;
    UiFont *font = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        int i, n;
        if (line[0] == 'g' && line[1] == ' ' && font) {
            UiGlyph g;
            int code, w, h, xo, yo;
            if (sscanf(line + 2, "%d %f %f %f %f %d %d %d %d", &code, &g.u0, &g.v0, &g.u1, &g.v1, &w, &h, &xo, &yo) == 9 &&
                code >= 0 && code < UI_MAX_GLYPHS) {
                g.w = (int16_t)w, g.h = (int16_t)h, g.xoff = (int16_t)xo, g.yoff = (int16_t)yo, g.present = 1;
                font->g[code] = g;
            }
        } else if (sscanf(line, "tex %d %63s", &i, a) == 2 && i >= 0 && i < 32) {
            snprintf(ui->tex_names[i], sizeof ui->tex_names[i], "%s", a);
        } else if (sscanf(line, "font %63s %d %d", a, &i, &n) == 3 && ui->n_font < UI_MAX_FONTS) {
            font = &ui->font[ui->n_font++];
            snprintf(font->name, sizeof font->name, "%s", a);
            font->tex = i;
            font->line_h = n;
        } else if (sscanf(line, "str %63s %n", a, &n) == 1 && ui->n_str < UI_MAX_STRS) {
            ui_set_str(ui, a, line + n);
        } else if (sscanf(line, "layer %31s", a) == 1 && ui->n_layers < 8) {
            layer = ui->n_layers;
            snprintf(ui->layers[ui->n_layers++], 32, "%s", a);
        } else if (!strncmp(line, "icon ", 5) && ui->n_item < UI_MAX_ITEMS) {
            UiItem *it = &ui->item[ui->n_item];
            int c[4];
            if (sscanf(line + 5, "%23s %d %f %f %f %f %f %f %f %f %d %d %d %d %d %d", it->name, &it->tex, &it->x, &it->y, &it->w, &it->h,
                       &it->u0, &it->u1, &it->v0, &it->v1, &c[0], &c[1], &c[2], &c[3], &it->hj, &it->vj) == 16) {
                for (int k = 0; k < 4; k++)
                    it->rgba[k] = (uint8_t)c[k];
                it->layer = layer;
                it->kind = 0;
                ui->n_item++;
            }
        } else if (!strncmp(line, "text ", 5) && ui->n_item < UI_MAX_ITEMS) {
            UiItem *it = &ui->item[ui->n_item];
            int c[4];
            if (sscanf(line + 5, "%23s %63s %47s %f %f %f %d %d %d %d %d %d", it->name, a, it->id, &it->x, &it->y, &it->scale,
                       &c[0], &c[1], &c[2], &c[3], &it->hj, &it->vj) == 12) {
                for (int k = 0; k < 4; k++)
                    it->rgba[k] = (uint8_t)c[k];
                it->font = find_font(ui, a);
                it->layer = layer;
                it->kind = 1;
                ui->n_item++;
            }
        } else if (!strncmp(line, "signs ", 6)) {
            int c[8];
            if (sscanf(line + 6, "%f %f %63s %47s %f %f %d %d %d %d %d %d %d %d %f", &ui->sign_x, &ui->sign_y, a,
                       ui->sign_label, &ui->label_x, &ui->label_y, &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6],
                       &c[7], &ui->sign_anim) >= 14) {
                ui->sign_font = find_font(ui, a);
                for (int k = 0; k < 4; k++) {
                    ui->sign_rgba[k] = (uint8_t)c[k];
                    ui->sign_sel_rgba[k] = (uint8_t)c[4 + k];
                }
            }
        } else if (!strncmp(line, "bg ", 3)) {
            sscanf(line + 3, "%f %f %f %f %f %f %f %f", &ui->bg[0], &ui->bg[1], &ui->bg[2], &ui->bg[3], &ui->bg[4],
                   &ui->bg[5], &ui->bg[6], &ui->bg[7]);
        } else if (sscanf(line, "sign %47s %d", b, &i) == 2 && ui->n_sign < UI_MAX_SIGNS) {
            snprintf(ui->sign[ui->n_sign].id, sizeof ui->sign[0].id, "%s", b);
            ui->sign[ui->n_sign++].tex = i;
        } else if (!strncmp(line, "camera ", 7) && ui->n_cam < 4) {
            UiCamera *cm = &ui->cam[ui->n_cam];
            if (sscanf(line + 7, "%15s %f %f %f %f %f %f", cm->name, &cm->pos[0], &cm->pos[1], &cm->pos[2],
                       &cm->fwd[0], &cm->fwd[1], &cm->fwd[2]) == 7)
                ui->n_cam++;
        }
    }
    fclose(f);
    return ui->n_font ? 0 : -2;
}

const char *ui_str(const Ui *ui, const char *id)
{
    for (int i = 0; i < ui->n_str; i++)
        if (!strcmp(ui->str[i].id, id))
            return ui->str[i].text;
    return "";
}

void ui_set_str(Ui *ui, const char *id, const char *text)
{
    int i = 0;
    while (i < ui->n_str && strcmp(ui->str[i].id, id))
        i++;
    if (i == UI_MAX_STRS)
        return;
    if (i == ui->n_str)
        ui->n_str++;
    snprintf(ui->str[i].id, sizeof ui->str[i].id, "%s", id);
    snprintf(ui->str[i].text, sizeof ui->str[i].text, "%s", text);
}

const UiCamera *ui_camera(const Ui *ui, const char *name)
{
    for (int i = 0; i < ui->n_cam; i++)
        if (!strcmp(ui->cam[i].name, name))
            return &ui->cam[i];
    return NULL;
}

void ui_begin(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
}

/* one textured quad in screen pixels */
static void quad(unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 const uint8_t rgba[4])
{
    const float v[8] = { x, y, x + w, y, x, y + h, x + w, y + h };
    const float t[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glColor4ub(rgba[0], rgba[1], rgba[2], rgba[3]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static float text_width(const UiFont *fo, const char *s)
{
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (fo->g[*p].present)
            w += fo->g[*p].w + fo->g[*p].xoff;
    return w;
}

/* x, y in 640x480 UI space; glyph advance = width + xoff (the .cel's outline overlap) */
void ui_text(const Ui *ui, int font, const char *s, float x, float y, float scale, const uint8_t rgba[4], int hj, int vj)
{
    if (font < 0 || !*s)
        return;
    const UiFont *fo = &ui->font[font];
    float k = scale * SY;
    float px = x * SX - text_width(fo, s) * k * hj * 0.5f;
    float py = y * SY - fo->line_h * k * vj * 0.5f;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const UiGlyph *g = &fo->g[*p];
        if (!g->present)
            continue;
        quad(ui->gl_tex[fo->tex], px, py + g->yoff * k, g->w * k, g->h * k, g->u0, g->v0, g->u1, g->v1, rgba);
        px += (g->w + g->xoff) * k;
    }
}

/* one component; dx shifts it in UI space, size (if > 0) replaces an icon's size, alpha scales its alpha */
static void draw_item(const Ui *ui, const UiItem *it, float dx, float size, float alpha)
{
    uint8_t c[4] = { it->rgba[0], it->rgba[1], it->rgba[2], (uint8_t)(it->rgba[3] * alpha) };
    if (it->kind == 1) {
        ui_text(ui, it->font, ui_str(ui, it->id), it->x + dx, it->y, it->scale, c, it->hj, it->vj);
        return;
    }
    /* a sized icon (the red circle) scales by SY both ways so it stays round on 16:9, like the signs */
    float w = size > 0 ? size * SY : it->w * SX, h = (size > 0 ? size : it->h) * SY;
    float x = (it->x + dx) * SX - w * it->hj * 0.5f, y = it->y * SY - h * it->vj * 0.5f;
    quad(ui->gl_tex[it->tex], x, y, w, h, it->u0, it->v0, it->u1, it->v1, c);
}

void ui_draw_layer(const Ui *ui, const char *layer)
{
    int l = find_layer((Ui *)ui, layer);
    for (int i = 0; i < ui->n_item; i++)
        if (l >= 0 && ui->item[i].layer == l)
            draw_item(ui, &ui->item[i], 0, 0, 1);
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void ui_draw_background(const Ui *ui, float p, float header_t)
{
    int l = find_layer((Ui *)ui, "FrontEndMenuBackground");
    const float *g = ui->bg; /* duration, max circle, final circle, ratio, header speed, title x1..x3 */
    float a = clamp01(p / 0.5f), b = clamp01((p - 0.5f) / 0.5f);
    /* idle: the checkered strip steps left one pixel at a time, 32 px per HeaderAnimationSpeed */
    float hs = p >= 1 ? floorf(32.0f * fmodf(header_t, g[4]) / g[4]) : 0;
    /* red circle: grows to the max over CircleGrowingRatio of its half, then settles */
    float circle = b < g[3] ? b / g[3] * g[1] : g[1] - (b - g[3]) / (1 - g[3]) * (g[1] - g[2]);
    /* title: fades in while sliding TitleX1 -> TitleX2 (80%) -> TitleX3 */
    float tx = p <= 0.8f ? g[5] - p * (g[5] - g[6]) / 0.8f : g[6] + (p - 0.8f) * (g[7] - g[6]) / 0.2f;
    /* Pass 1: HeaderBackground first. The .lyr lists it after Header1..3, but header_bar_blue is
       opaque where the strips sit, so in list order it would hide the checks. */
    for (int i = 0; i < ui->n_item; i++) {
        const UiItem *it = &ui->item[i];
        if (l < 0 || it->layer != l)
            continue;
        if (!strcmp(it->name, "HeaderBackground"))
            draw_item(ui, it, 640.0f * (1 - a), 0, 1);
    }
    /* Pass 2: everything else in file order. */
    for (int i = 0; i < ui->n_item; i++) {
        const UiItem *it = &ui->item[i];
        if (l < 0 || it->layer != l)
            continue;
        const char *n = it->name;
        if (!strcmp(n, "HeaderBackground"))
            continue;                          /* already drawn */
        if (!strncmp(n, "Header", 6))          /* Header1..3: slide in from the right */
            draw_item(ui, it, 640.0f * (1 - a) - hs, 0, 1);
        else if (!strncmp(n, "Footer", 6))     /* FooterLeft/Middle/Right: from the left */
            draw_item(ui, it, -640.0f * (1 - a), 0, 1);
        else if (!strcmp(n, "Red_Circle"))
            draw_item(ui, it, 0, ui->tex_w[it->tex] * circle, 1);
        else if (!strcmp(n, "Title"))
            draw_item(ui, it, tx - it->x, 0, p);
        else                                   /* button texts and labels fade in */
            draw_item(ui, it, 0, 0, p);
    }
}

void ui_draw_signs(const Ui *ui, int selected, int dir, float t, float time)
{
    int n = ui->n_sign;
    if (n <= 0 || selected < 0 || selected >= n)
        return;
    /* slot tables at 0x48dab0 / 0x48dad0 / 0x48daf0: x offset, scale, brightness.
       shortcut: always five slots (the game shows 3 or 1 for menus under 5 or 3 items) */
    static const float SX5[5] = { -248, -144, 0, 144, 248 }, SC5[5] = { 0.5f, 0.75f, 1, 0.75f, 0.5f };
    t = clamp01(t);
    for (int k = 0; k < 5; k++) {
        int to = k - dir; /* sliding to the next item moves every sign one slot left */
        if (dir && (to < 0 || to > 4))
            continue;   /* the sign leaving the row disappears at once, as in the game */
        float x = SX5[k] + (dir ? (SX5[to] - SX5[k]) * t : 0);
        float sc = SC5[k] + (dir ? (SC5[to] - SC5[k]) * t : 0); /* brightness = scale */
        const UiSign *sg = &ui->sign[((selected + k - 2) % n + n) % n];
        uint8_t c[4] = { (uint8_t)(255 * sc), (uint8_t)(255 * sc), (uint8_t)(255 * sc), 255 };
        float sz = ui->tex_w[sg->tex] * sc;
        quad(ui->gl_tex[sg->tex], (ui->sign_x + x) * SX - sz * SY / 2, ui->sign_y * SY - sz * SY / 2, sz * SY,
             sz * SY, 0, 0, 1, 1, c);
    }
    /* label: PulseIfSelected (MaxPulseScale 1.2, PulseDuration .3) and HighlightArrows.
       shortcut: pulse is a 0.3 s up / 0.3 s down triangle, the PS2 curve is not decoded */
    static const uint8_t yellow[4] = { 170, 123, 13, 255 };
    float ph = fmodf(time, 0.6f) / 0.3f;
    float pulse = 1.0f + 0.2f * (ph < 1 ? ph : 2 - ph);
    const char *txt = ui_str(ui, ui->sign[selected].id);
    float lx = ui->sign_x + ui->label_x, ly = ui->sign_y + ui->label_y;
    ui_text(ui, ui->sign_font, txt, lx, ly, pulse, ui->sign_sel_rgba, 1, 1);
    int at = ui_tex(ui, "highlight_arrow");
    if (at >= 0 && ui->sign_font >= 0) {
        float half = text_width(&ui->font[ui->sign_font], txt) * SY / 2 + 8 * SX;
        float a = 32 * SY, cx = lx * SX, cy = ly * SY - a / 2;
        quad(ui->gl_tex[at], cx - half - a, cy, a, a, 0, 0, 1, 1, yellow);     /* angle 0 points left */
        quad(ui->gl_tex[at], cx + half, cy, a, a, 1, 0, 0, 1, yellow);         /* mirrored: right */
    }
}
