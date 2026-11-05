/* Co-op horror sounds, synthesised at start-up (no audio files are shipped or read from the game):
 *   ping:      motion-tracker ping, pitch rises as the contact gets closer (three pitches)
 *   heartbeat: two low thumps, for "something is very close" and while spectating
 *   sting:     a short noise swell with a falling tone, when your partner dies
 * Played with PlaySound(SND_MEMORY | SND_ASYNC). AIC_SFX=0 turns them off.
 */
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sfx.h"

#define RATE 22050
static uint8_t *g_wav[SFX_COUNT];
static int g_on = 1;

static uint8_t *make_wav(int n, int16_t **pcm) {
    uint8_t *w = calloc(1, 44 + (size_t)n * 2);
    if (!w) return NULL;
    uint32_t d = (uint32_t)n * 2, v;
    memcpy(w, "RIFF", 4); v = 36 + d; memcpy(w + 4, &v, 4); memcpy(w + 8, "WAVEfmt ", 8);
    v = 16; memcpy(w + 16, &v, 4);
    uint16_t fmt[] = {1, 1}; memcpy(w + 20, fmt, 4);
    v = RATE; memcpy(w + 24, &v, 4); v = RATE * 2; memcpy(w + 28, &v, 4);
    uint16_t ba[] = {2, 16}; memcpy(w + 32, ba, 4);
    memcpy(w + 36, "data", 4); memcpy(w + 40, &d, 4);
    *pcm = (int16_t *)(w + 44);
    return w;
}

static void tone(int16_t *o, int n, float hz, float amp, float decay) {
    for (int i = 0; i < n; i++) {
        float t = (float)i / RATE, e = expf(-t * decay) * (i < 40 ? i / 40.f : 1.f);
        o[i] = (int16_t)(amp * e * 32767.f * sinf(6.2831853f * hz * t));
    }
}

void sfx_init(void) {
    char b[8];
    if (GetEnvironmentVariableA("AIC_SFX", b, sizeof b) && b[0] == '0') { g_on = 0; return; }
    int16_t *p; int n;
    static const float PING_HZ[3] = {880.f, 1175.f, 1568.f};
    for (int k = 0; k < 3; k++) {
        n = RATE * 18 / 100; g_wav[SFX_PING_FAR + k] = make_wav(n, &p);
        if (p && g_wav[SFX_PING_FAR + k]) tone(p, n, PING_HZ[k], 0.35f, 22.f);
    }
    n = RATE * 7 / 10; g_wav[SFX_HEART] = make_wav(n, &p);
    if (g_wav[SFX_HEART]) {
        int16_t *a = p; int m = RATE / 6;
        tone(a, m, 55.f, 0.9f, 18.f);
        tone(a + RATE * 25 / 100, m, 50.f, 0.7f, 18.f);
    }
    n = RATE * 12 / 10; g_wav[SFX_STING] = make_wav(n, &p);
    if (g_wav[SFX_STING]) {
        uint32_t r = 0x1234567u;
        for (int i = 0; i < n; i++) {
            float t = (float)i / RATE, env = t < 0.15f ? t / 0.15f : expf(-(t - 0.15f) * 3.f);
            r = r * 1664525u + 1013904223u;
            float noise = ((float)(r >> 16) / 32768.f - 1.f) * 0.25f;
            float hz = 420.f - 260.f * t, tonev = 0.45f * sinf(6.2831853f * hz * t) + 0.2f * sinf(6.2831853f * hz * 1.5f * t);
            p[i] = (int16_t)(env * 0.8f * 32767.f * (noise + tonev) / 0.9f);
        }
    }
}

void sfx_play(int which) {
    if (!g_on || which < 0 || which >= SFX_COUNT || !g_wav[which]) return;
    PlaySoundA((LPCSTR)g_wav[which], NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
