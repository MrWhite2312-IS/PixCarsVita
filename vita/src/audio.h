/*
 * PixCarsVita - audio (portable C99): the game's headerless IMA ADPCM .wav files
 * (exported by tools/vitaaudio.py), streamed music plus the engine loops crossfaded and
 * pitched by RPM the way carengine.aud describes, mixed to 16-bit stereo.
 */
#ifndef PIXCARS_AUDIO_H
#define PIXCARS_AUDIO_H

#include <stdint.h>
#include <stdio.h>

#define AUDIO_RATE 44100
#define AUDIO_MAX_LOOPS 8
#define AUDIO_MAX_KEYS 16
#define AUDIO_MAX_GEARS 8
#define AUDIO_MAX_TRACKS 32
#define AUDIO_MAX_SFX 64
#define AUDIO_MAX_VOICES 12

typedef struct { int16_t pred; int8_t index; } ImaState;

typedef struct {
    int16_t *pcm;              /* decoded mono samples */
    uint32_t n;
    float base_rate;           /* original sample rate (Hz) */
    int n_keys;
    float key_rpm[AUDIO_MAX_KEYS], key_vol[AUDIO_MAX_KEYS], key_rate[AUDIO_MAX_KEYS];
    double pos;
} EngineLoop;

/* A sound effect and its fx.aud parameters (slip range -> volume % and pitch offset). */
typedef struct {
    char name[32];
    int16_t *pcm;
    uint32_t n, rate;
    float slip_min, slip_max, freq_min, freq_max, vol_min, vol_max, fade;
} Sfx;

typedef struct {
    int sfx, loop;
    double pos;
    volatile float vol, pitch; /* set by the game, read by the mixer */
    volatile int active;
} Voice;

typedef struct {
    /* music */
    char dir[256];
    char tracks[AUDIO_MAX_TRACKS][64];
    int n_tracks, cur_track;
    FILE *music;
    uint32_t music_left;       /* ADPCM bytes (= stereo frames) left in the track */
    uint32_t music_rate;
    ImaState ml, mr;
    float music_vol;
    /* engine */
    EngineLoop loop[AUDIO_MAX_LOOPS];
    int n_loops;
    int n_gears;
    float gear[AUDIO_MAX_GEARS][4]; /* speed_lo, speed_hi (mph), rpm_lo, rpm_hi */
    float rpm;                 /* set by the game loop */
    float engine_vol;
    /* effects (sfx/sfx.txt) */
    Sfx sfx[AUDIO_MAX_SFX];
    int n_sfx;
    Voice voice[AUDIO_MAX_VOICES];
    float sfx_vol;
} Audio;

int16_t ima_step(ImaState *s, int nibble);
/* Load music/playlist.txt and engine/engine.txt under root (e.g. "ux0:data/pixcars/audio/").
 * Missing parts are skipped. Returns the number of parts loaded (0..2). */
int audio_init(Audio *a, const char *root);
void audio_free(Audio *a);
/* Index of the nth effect with this name (several variants may share a name), or -1. */
int audio_sfx(const Audio *a, const char *name, int nth);
/* Start an effect on a free voice; returns the voice (adjust vol/pitch, clear active to stop). */
int audio_play(Audio *a, int sfx, float vol, float pitch, int loop);
void audio_next_track(Audio *a);
/* Engine RPM from road speed in mph, through the profile's gear ranges (MapSpeedToRPM). */
float audio_speed_to_rpm(const Audio *a, float mph);
/* Mix n stereo frames (interleaved L, R) at AUDIO_RATE. */
void audio_mix(Audio *a, int16_t *out, int frames);

#endif
