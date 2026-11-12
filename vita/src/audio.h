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

typedef struct { int16_t pred; int8_t index; } ImaState;

typedef struct {
    int16_t *pcm;              /* decoded mono samples */
    uint32_t n;
    float base_rate;           /* original sample rate (Hz) */
    int n_keys;
    float key_rpm[AUDIO_MAX_KEYS], key_vol[AUDIO_MAX_KEYS], key_rate[AUDIO_MAX_KEYS];
    double pos;
} EngineLoop;

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
} Audio;

int16_t ima_step(ImaState *s, int nibble);
/* Load music/playlist.txt and engine/engine.txt under root (e.g. "ux0:data/pixcars/audio/").
 * Missing parts are skipped. Returns the number of parts loaded (0..2). */
int audio_init(Audio *a, const char *root);
void audio_free(Audio *a);
void audio_next_track(Audio *a);
/* Engine RPM from road speed in mph, through the profile's gear ranges (MapSpeedToRPM). */
float audio_speed_to_rpm(const Audio *a, float mph);
/* Mix n stereo frames (interleaved L, R) at AUDIO_RATE. */
void audio_mix(Audio *a, int16_t *out, int frames);

#endif
