// click3ds.h -- the shared touch click for Ryan's 3DS ports.
//
// Standard for every 3DS project: each tap on an on-screen key or button
// gives a click sound and a visible pressed animation. This header is the
// sound half. click3ds::play() plays a short synthesized tick on its own
// DSP channel (23), so it works next to whatever audio the game already
// runs (SDL, ScummVM's mixer, or none at all). ndspInit is reference
// counted by libctru, so sharing it with the game is safe.
//
// Identical copy in every 3DS repo. Header-only; C++.
#pragma once
#ifdef __3DS__
#include <3ds.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace click3ds {

static const int kChannel = 23, kRate = 22050, kSamples = 22050 * 28 / 1000;   // 28 ms

struct State { bool tried = false, ok = false; int16_t *pcm = nullptr; ndspWaveBuf wb; };
inline State &state() { static State s; return s; }

inline void shutdown() {
    State &s = state();
    if (!s.ok) return;
    ndspChnWaveBufClear(kChannel);
    ndspExit();
    if (s.pcm) linearFree(s.pcm);
    s.pcm = nullptr;
    s.ok = false;
}

inline bool init() {
    State &s = state();
    if (s.tried) return s.ok;
    s.tried = true;
    if (R_FAILED(ndspInit())) return false;           // no dspfirm.cdc: stay silent
    s.pcm = (int16_t *)linearAlloc(kSamples * sizeof(int16_t));
    if (!s.pcm) { ndspExit(); return false; }
    // a soft "tock": a 1.9 kHz tone plus a little noise, fast decay
    uint32_t seed = 12345;
    for (int i = 0; i < kSamples; ++i) {
        const float t = (float)i / kRate;
        const float env = expf(-t * 260.f);
        seed = seed * 1664525u + 1013904223u;
        const float noise = ((int)(seed >> 16) - 32768) / 32768.f;
        const float v = (0.75f * sinf(2.f * 3.14159265f * 1900.f * t) + 0.25f * noise) * env;
        s.pcm[i] = (int16_t)(v * 20000.f);
    }
    DSP_FlushDataCache(s.pcm, kSamples * sizeof(int16_t));
    s.ok = true;
    atexit(shutdown);
    return true;
}

inline void play() {
    if (!init()) return;
    State &s = state();
    ndspChnWaveBufClear(kChannel);
    ndspChnReset(kChannel);
    ndspChnSetInterp(kChannel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(kChannel, kRate);
    ndspChnSetFormat(kChannel, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 0.8f, 0.8f };
    ndspChnSetMix(kChannel, mix);
    memset(&s.wb, 0, sizeof(s.wb));
    s.wb.data_vaddr = s.pcm;
    s.wb.nsamples = kSamples;
    ndspChnWaveBufAdd(kChannel, &s.wb);
}

} // namespace click3ds
#else
namespace click3ds { inline bool init() { return false; } inline void play() {} inline void shutdown() {} }
#endif
