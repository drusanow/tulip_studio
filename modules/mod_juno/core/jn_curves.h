// jn_curves.h - how JUNO-6's 0..1 sliders become times, rates and pitches
// (AMY's juno.py curves), for the engine and the panel's read-outs.
#pragma once

#include <stdint.h>

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x to ~1e-4, without libm (render runs from RAM, and this is cheap)
static inline float fexp2(float x) {
    x = clampf(x, -60.0f, 60.0f);
    int i = (int)x;
    if (x < (float)i) i--;
    float f = x - (float)i;
    float p = 1.0f + f * (0.6951786f + f * (0.2261170f + f * 0.0782177f));
    union {
        float f;
        int32_t i;
    } u = {p};
    u.i += (int32_t)((uint32_t)i << 23);
    return u.f;
}

// --- the curves (AMY's juno.py), from the 0..1 sliders ---------------------------------------

static inline float attack_ms(float v) { return 6.0f + 8.0f * 127.0f * v; }
static inline float decay_ms(float v) { return clampf(80.0f * fexp2(0.085f * 127.0f * v) - 80.0f, 4.0f, 30000.0f); }
static inline float release_ms(float v) { return clampf(70.0f * fexp2(0.066f * 127.0f * v) - 70.0f, 4.0f, 30000.0f); }
static inline float lfo_hz(float v) { return 0.6f * fexp2(0.04f * 127.0f * v) - 0.1f; }
static inline float lfo_delay_ms(float v) { return 18.0f * fexp2(0.066f * 127.0f * v) - 13.0f; }
static inline float vcf_hz(float v) { return 13.0f * fexp2(0.0938f * 127.0f * v); }
static inline float porta_ms(float v) { return v < 0.004f ? 0.0f : 20.0f * fexp2(v * 127.0f / 30.0f); }
