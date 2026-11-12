/* Host test for vita/src/audio.c: loads the exported audio, sweeps the engine from 0 to
 * 170 mph over the music and writes the mix to a .wav to listen to.
 * Build: gcc -O2 -std=c99 -Ivita/src vita/src/audio.c vita/tests/audio_test.c -lm -o audio_test
 * Run:   ./audio_test extracted/vita/audio/ out.wav */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
    assert(argc > 2);
    /* IMA reference: nibble 7 from zero state steps by 7>>3 + 7 + 3 + 1 = 11, index -> 8 */
    ImaState s = { 0, 0 };
    assert(ima_step(&s, 7) == 11 && s.index == 8);

    Audio a;
    int parts = audio_init(&a, argv[1]);
    printf("parts=%d tracks=%d loops=%d gears=%d music=%s\n", parts, a.n_tracks, a.n_loops, a.n_gears,
           a.music ? a.tracks[a.cur_track] : "-");
    assert(parts == 2 && a.n_loops == 6 && a.n_gears == 5);
    assert(audio_speed_to_rpm(&a, 0) == 1 && audio_speed_to_rpm(&a, 50) == 2000);
    assert(audio_speed_to_rpm(&a, 170) == 5500);
    /* effects: fx.aud roles, a one-shot voice plays and frees itself */
    printf("sfx=%d\n", a.n_sfx);
    assert(a.n_sfx == 10 && audio_sfx(&a, "suspension", 3) >= 0 && audio_sfx(&a, "impact", 3) < 0);
    {
        Audio q = a;
        q.music = NULL; q.n_loops = 0;
        int v = audio_play(&q, audio_sfx(&q, "brake", 0), 1, 1, 0);
        static int16_t buf[2 * 4096];
        audio_mix(&q, buf, 4096);
        long e = 0;
        for (int i = 0; i < 2 * 4096; i++) e += buf[i] < 0 ? -buf[i] : buf[i];
        assert(v >= 0 && e > 0);
        for (int k = 0; k < 40; k++) audio_mix(&q, buf, 4096); /* 1.26 s sample ends */
        assert(!q.voice[v].active);
    }

    const int secs = 10, n = AUDIO_RATE * secs, chunk = 1024;
    int16_t *pcm = malloc(4u * n);
    long long energy = 0;
    for (int i = 0; i < n; i += chunk) {
        float mph = 170.0f * i / n;
        a.rpm = audio_speed_to_rpm(&a, mph);
        int c = n - i < chunk ? n - i : chunk;
        audio_mix(&a, pcm + 2 * i, c);
    }
    for (int i = 0; i < 2 * n; i++)
        energy += pcm[i] < 0 ? -pcm[i] : pcm[i];
    printf("mean |sample| %lld\n", energy / (2LL * n));
    assert(energy / (2LL * n) > 200);

    FILE *f = fopen(argv[2], "wb");
    fwrite("RIFF", 1, 4, f); put32(f, 36 + 4u * n); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, AUDIO_RATE); put32(f, AUDIO_RATE * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, 4u * n); fwrite(pcm, 4, (size_t)n, f);
    fclose(f);
    audio_free(&a);
    free(pcm);
    puts("audio_test ok");
    return 0;
}
