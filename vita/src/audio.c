/*
 * PixCarsVita - audio. See audio.h.
 */
#include "audio.h"

#include <stdlib.h>
#include <string.h>

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

int16_t ima_step(ImaState *s, int n)
{
    int step = STEP[s->index], diff = step >> 3;
    if (n & 4) diff += step;
    if (n & 2) diff += step >> 1;
    if (n & 1) diff += step >> 2;
    int p = s->pred + ((n & 8) ? -diff : diff);
    s->pred = (int16_t)(p < -32768 ? -32768 : (p > 32767 ? 32767 : p));
    int ix = s->index + INDEX[n];
    s->index = (int8_t)(ix < 0 ? 0 : (ix > 88 ? 88 : ix));
    return s->pred;
}

/* Find the "data" chunk of a RIFF .wav; returns its size and leaves f at its start. */
static uint32_t wav_data(FILE *f, uint32_t *rate)
{
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8)
            return 0;
        uint32_t n = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4) && n >= 8) {
            uint8_t fm[8];
            if (fread(fm, 1, 8, f) != 8)
                return 0;
            *rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
            fseek(f, (long)(n - 8 + (n & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            return n;
        } else {
            fseek(f, (long)(n + (n & 1)), SEEK_CUR);
        }
    }
}

static void open_track(Audio *a)
{
    if (a->music)
        fclose(a->music);
    a->music = NULL;
    a->music_left = 0;
    for (int tries = 0; tries < a->n_tracks && !a->music; tries++) {
        char path[384];
        snprintf(path, sizeof path, "%smusic/%s.wav", a->dir, a->tracks[a->cur_track]);
        a->music = fopen(path, "rb");
        if (a->music) {
            a->music_left = wav_data(a->music, &a->music_rate);
            if (!a->music_left) {
                fclose(a->music);
                a->music = NULL;
            }
        }
        if (!a->music)
            a->cur_track = (a->cur_track + 1) % a->n_tracks;
    }
    memset(&a->ml, 0, sizeof a->ml);
    memset(&a->mr, 0, sizeof a->mr);
}

void audio_next_track(Audio *a)
{
    if (!a->n_tracks)
        return;
    a->cur_track = (a->cur_track + 1) % a->n_tracks;
    open_track(a);
}

/* Decode a whole mono IMA .wav (low nibble first) into 16-bit samples. */
static int load_ima_mono(const char *path, int16_t **pcm, uint32_t *n, uint32_t *rate)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    *rate = 0;
    uint32_t bytes = wav_data(f, rate);
    uint8_t *buf = bytes ? malloc(bytes) : NULL;
    if (!buf || fread(buf, 1, bytes, f) != bytes) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *pcm = malloc(4u * bytes);
    if (!*pcm) {
        free(buf);
        return 0;
    }
    ImaState s = { 0, 0 };
    for (uint32_t i = 0; i < bytes; i++) {
        (*pcm)[2 * i] = ima_step(&s, buf[i] & 15);
        (*pcm)[2 * i + 1] = ima_step(&s, buf[i] >> 4);
    }
    free(buf);
    *n = 2 * bytes;
    return 1;
}

static int load_loop(Audio *a, const char *line)
{
    if (a->n_loops >= AUDIO_MAX_LOOPS)
        return 0;
    EngineLoop *l = &a->loop[a->n_loops];
    char name[64];
    int used = 0;
    float base;
    if (sscanf(line, "loop %63s %f%n", name, &base, &used) != 2)
        return 0;
    memset(l, 0, sizeof *l);
    l->base_rate = base;
    const char *p = line + used;
    float r, v, rt;
    int n;
    while (l->n_keys < AUDIO_MAX_KEYS && sscanf(p, " %f:%f:%f%n", &r, &v, &rt, &n) == 3) {
        l->key_rpm[l->n_keys] = r;
        l->key_vol[l->n_keys] = v / 100.0f;
        l->key_rate[l->n_keys] = rt;
        l->n_keys++;
        p += n;
    }
    char path[384];
    snprintf(path, sizeof path, "%sengine/%s", a->dir, name);
    uint32_t rate;
    if (!load_ima_mono(path, &l->pcm, &l->n, &rate))
        return 0;
    a->n_loops++;
    return 1;
}

int audio_init(Audio *a, const char *root)
{
    memset(a, 0, sizeof *a);
    snprintf(a->dir, sizeof a->dir, "%s", root);
    a->music_vol = 0.55f;
    a->engine_vol = 0.8f;
    a->sfx_vol = 0.9f;
    int parts = 0;
    char path[384], line[1024];
    snprintf(path, sizeof path, "%smusic/playlist.txt", root);
    FILE *f = fopen(path, "r");
    if (f) {
        while (a->n_tracks < AUDIO_MAX_TRACKS && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0])
                snprintf(a->tracks[a->n_tracks++], 64, "%.63s", line);
        }
        fclose(f);
        if (a->n_tracks) {
            open_track(a);
            parts += a->music != NULL;
        }
    }
    snprintf(path, sizeof path, "%sengine/engine.txt", root);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            float g[4];
            if (!strncmp(line, "gear", 4) && a->n_gears < AUDIO_MAX_GEARS &&
                sscanf(line, "gear %f %f %f %f", &g[0], &g[1], &g[2], &g[3]) == 4)
                memcpy(a->gear[a->n_gears++], g, sizeof g);
            else if (!strncmp(line, "loop", 4))
                load_loop(a, line);
        }
        fclose(f);
        parts += a->n_loops > 0;
    }
    snprintf(path, sizeof path, "%ssfx/sfx.txt", root);
    f = fopen(path, "r");
    if (f) {
        while (a->n_sfx < AUDIO_MAX_SFX && fgets(line, sizeof line, f)) {
            Sfx *x = &a->sfx[a->n_sfx];
            char file[64];
            memset(x, 0, sizeof *x);
            x->vol_min = x->vol_max = 100;
            x->fade = 0.1f;
            int k = sscanf(line, "%31s %63s %f %f %f %f %f %f %f", x->name, file, &x->slip_min, &x->slip_max,
                           &x->freq_min, &x->freq_max, &x->vol_min, &x->vol_max, &x->fade);
            if (k < 2)
                continue;
            snprintf(path, sizeof path, "%ssfx/%s", root, file);
            if (load_ima_mono(path, &x->pcm, &x->n, &x->rate))
                a->n_sfx++;
        }
        fclose(f);
    }
    return parts;
}

int audio_sfx(const Audio *a, const char *name, int nth)
{
    for (int i = 0; i < a->n_sfx; i++)
        if (!strcmp(a->sfx[i].name, name) && nth-- == 0)
            return i;
    return -1;
}

int audio_play(Audio *a, int sfx, float vol, float pitch, int loop)
{
    if (sfx < 0)
        return -1;
    for (int v = 0; v < AUDIO_MAX_VOICES; v++) {
        Voice *o = &a->voice[v];
        if (o->active)
            continue;
        o->sfx = sfx;
        o->pos = 0;
        o->vol = vol;
        o->pitch = pitch;
        o->loop = loop;
        o->active = 1; /* last: the mixer thread starts reading it now */
        return v;
    }
    return -1;
}

void audio_free(Audio *a)
{
    if (a->music)
        fclose(a->music);
    for (int i = 0; i < a->n_loops; i++)
        free(a->loop[i].pcm);
    for (int i = 0; i < a->n_sfx; i++)
        free(a->sfx[i].pcm);
    memset(a, 0, sizeof *a);
}

float audio_speed_to_rpm(const Audio *a, float mph)
{
    if (!a->n_gears)
        return 0;
    for (int g = 0; g < a->n_gears; g++) {
        const float *G = a->gear[g];
        if (mph <= G[1] || g == a->n_gears - 1) {
            float t = G[1] > G[0] ? (mph - G[0]) / (G[1] - G[0]) : 0;
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            return G[2] + (G[3] - G[2]) * t;
        }
    }
    return 0;
}

/* piecewise-linear key lookup (keys are sorted by rpm, duplicates allowed) */
static void loop_params(const EngineLoop *l, float rpm, float *vol, float *rate)
{
    int n = l->n_keys;
    if (!n) {
        *vol = 0;
        *rate = 0;
        return;
    }
    if (rpm <= l->key_rpm[0]) {
        *vol = l->key_vol[0];
        *rate = l->key_rate[0];
        return;
    }
    for (int i = 1; i < n; i++) {
        if (rpm <= l->key_rpm[i]) {
            float span = l->key_rpm[i] - l->key_rpm[i - 1];
            float t = span > 0 ? (rpm - l->key_rpm[i - 1]) / span : 1;
            *vol = l->key_vol[i - 1] + (l->key_vol[i] - l->key_vol[i - 1]) * t;
            *rate = l->key_rate[i - 1] + (l->key_rate[i] - l->key_rate[i - 1]) * t;
            return;
        }
    }
    *vol = l->key_vol[n - 1];
    *rate = l->key_rate[n - 1];
}

void audio_mix(Audio *a, int16_t *out, int frames)
{
    float vols[AUDIO_MAX_LOOPS], steps[AUDIO_MAX_LOOPS];
    for (int i = 0; i < a->n_loops; i++) {
        float rate;
        loop_params(&a->loop[i], a->rpm, &vols[i], &rate);
        vols[i] *= a->engine_vol;
        float hz = a->loop[i].base_rate + rate;
        steps[i] = (hz > 0 ? hz : 0) / (float)AUDIO_RATE;
    }
    uint8_t buf[2048];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > (int)sizeof buf)
            n = (int)sizeof buf;
        /* music: stereo IMA, one byte per frame (low nibble L, high nibble R) */
        int got = 0;
        if (a->music && a->music_left) {
            int want = n < (int)a->music_left ? n : (int)a->music_left;
            got = (int)fread(buf, 1, (size_t)want, a->music);
            a->music_left -= (uint32_t)got;
        }
        for (int i = 0; i < n; i++) {
            float l = 0, r = 0;
            if (i < got) {
                l = ima_step(&a->ml, buf[i] & 15) * a->music_vol;
                r = ima_step(&a->mr, buf[i] >> 4) * a->music_vol;
            }
            float e = 0;
            for (int k = 0; k < a->n_loops; k++) {
                EngineLoop *lp = &a->loop[k];
                if (vols[k] <= 0.001f || !lp->n)
                    continue;
                uint32_t p0 = (uint32_t)lp->pos, p1 = p0 + 1 < lp->n ? p0 + 1 : 0;
                float f = (float)(lp->pos - p0);
                e += (lp->pcm[p0] + (lp->pcm[p1] - lp->pcm[p0]) * f) * vols[k];
                lp->pos += steps[k];
                if (lp->pos >= lp->n)
                    lp->pos -= lp->n;
            }
            for (int v = 0; v < AUDIO_MAX_VOICES; v++) {
                Voice *o = &a->voice[v];
                if (!o->active)
                    continue;
                const Sfx *x = &a->sfx[o->sfx];
                uint32_t p0 = (uint32_t)o->pos;
                if (p0 >= x->n) {
                    if (!o->loop) {
                        o->active = 0;
                        continue;
                    }
                    o->pos -= x->n;
                    p0 = (uint32_t)o->pos;
                }
                e += x->pcm[p0] * o->vol * a->sfx_vol;
                o->pos += o->pitch * x->rate / (float)AUDIO_RATE;
            }
            l += e;
            r += e;
            out[2 * (done + i)] = (int16_t)(l < -32768 ? -32768 : (l > 32767 ? 32767 : l));
            out[2 * (done + i) + 1] = (int16_t)(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
        }
        if (a->music && (!a->music_left || got < n))
            audio_next_track(a); /* track finished: next in the playlist */
        done += n;
    }
}
