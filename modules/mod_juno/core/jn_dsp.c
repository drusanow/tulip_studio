// jn_dsp.c - JUNO-6 voices, VCF, LFO, HPF and chorus (see jn_dsp.h).
#include "jn_dsp.h"
#include "jn_curves.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define JN_HOT IRAM_ATTR
#else
#define JN_HOT
#endif

#define CTL 8   // pitch, PWM and the VCF move every 8 frames (0.18 ms)


int jn_step(const jn_patch_t *p, int i) {
    int n = JN_PARAMS[i].steps;
    int s = (int)(p->v[i] * (n - 1) + 0.5f);
    return s < 0 ? 0 : (s >= n ? n - 1 : s);
}

void jn_set_step(jn_patch_t *p, int i, int step) {
    int n = JN_PARAMS[i].steps;
    p->v[i] = n > 1 ? (float)step / (float)(n - 1) : 0.0f;
}






// --- engine ------------------------------------------------------------------------------------

void jn_init(jn_engine_t *e) {
    memset(e, 0, sizeof *e);
    jn_patch_default(&e->p);
    for (int i = 0; i < JN_VOICES; i++) {
        e->v[i].seed = 0x9E3779B9u * (uint32_t)(i + 1);
        e->v[i].sub = 1;
    }
    e->last_pitch = 60;
}

static jn_voice_t *pick(jn_engine_t *e, uint8_t src, uint8_t key) {
    jn_voice_t *best = NULL;
    for (int i = 0; i < JN_VOICES; i++)   // the same key again: retrigger it
        if (e->v[i].active && e->v[i].src == src && e->v[i].key == key) return &e->v[i];
    for (int i = 0; i < JN_VOICES; i++)
        if (!e->v[i].active) return &e->v[i];
    for (int i = 0; i < JN_VOICES; i++)   // the oldest released, else the oldest
        if (!e->v[i].on && (!best || e->v[i].age < best->age)) best = &e->v[i];
    if (best) return best;
    best = &e->v[0];
    for (int i = 1; i < JN_VOICES; i++)
        if (e->v[i].age < best->age) best = &e->v[i];
    return best;
}

void jn_note_on(jn_engine_t *e, uint8_t src, uint8_t key, float note, float vel, int32_t glide_us) {
    jn_voice_t *v = pick(e, src, key);
    // portamento slides from the last note played; a routed track's slide
    // from that track's
    float from = e->last_pitch;
    if (src < 0x80)
        for (int i = 0; i < JN_VOICES; i++)
            if (e->v[i].active && e->v[i].src == src) from = e->v[i].pitch;
    bool glide = glide_us > 0 || porta_ms(e->p.v[JP_PORTA]) > 0;
    v->target = note;
    v->pitch = glide ? from : note;
    v->src = src;
    v->key = key;
    v->vel = vel;
    if (!v->active) {
        v->env = v->gate = v->lfo_amt = 0;
        memset(v->s, 0, sizeof v->s);
    }
    v->on = v->active = true;
    v->est = 0;
    v->age = ++e->clock;
    e->last_pitch = note;
}

void jn_note_off(jn_engine_t *e, uint8_t src, uint8_t key) {
    for (int i = 0; i < JN_VOICES; i++) {
        jn_voice_t *v = &e->v[i];
        if (v->on && v->src == src && (v->key == key || src < 0x80)) {
            v->on = false;
            v->est = 2;
        }
    }
}

void jn_all_off(jn_engine_t *e, int src) {
    for (int i = 0; i < JN_VOICES; i++)
        if (src < 0 || e->v[i].src == src) {
            e->v[i].on = false;
            e->v[i].est = 2;
        }
}

static inline float blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// tan(x) for the VCF's prewarp, x up to ~1.3 (Pade, within 1 %)
static inline float ftan(float x) {
    float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

// The chorus: a delay line read at two points swung in opposite directions
// by one triangle, one per side (as the Juno's BBD chorus).
static inline float cho_read(const jn_engine_t *e, float d) {
    float rp = (float)e->cho_w - d;
    while (rp < 0) rp += JN_CHO_LEN;
    int i0 = (int)rp;
    float f = rp - (float)i0;
    if (i0 >= JN_CHO_LEN) i0 -= JN_CHO_LEN;   // (rounding at the wrap)
    int i1 = i0 + 1 == JN_CHO_LEN ? 0 : i0 + 1;
    return e->cho_buf[i0] + (e->cho_buf[i1] - e->cho_buf[i0]) * f;
}

JN_HOT void jn_render(jn_engine_t *e, float *l, float *r, int n) {
    const jn_patch_t *p = &e->p;
    float mono[64];
    if (n > 64) n = 64;
    memset(mono, 0, sizeof(float) * (size_t)n);

    // the block's settings
    float range = (float)(jn_step(p, JP_RANGE) - 1);   // octaves
    float g_pulse = (float)jn_step(p, JP_PULSE), g_saw = (float)jn_step(p, JP_SAW);
    float g_sub = p->v[JP_DCO_SUB], g_noise = p->v[JP_DCO_NOISE];
    bool pwm_man = jn_step(p, JP_PWM_MAN), gate_mode = jn_step(p, JP_VCA_GATE);
    float pwm = p->v[JP_DCO_PWM], dco_lfo = 0.03f * p->v[JP_DCO_LFO];
    float kbd = p->v[JP_VCF_KBD];
    float fc_base = vcf_hz(p->v[JP_VCF_FREQ]) * fexp2(0.75f * kbd);
    float env_oct = (jn_step(p, JP_VCF_NEG) ? -11.0f : 11.0f) * p->v[JP_VCF_ENV];
    float lfo_oct = 1.25f * p->v[JP_VCF_LFO];
    // ladder feedback: 4 rings; past it (the top few steps) the VCF sings on
    // its own, as the Juno's does - patches like B35 Tomita play only that
    float res = p->v[JP_VCF_RES];
    float k = 4.0f * res + (res > 0.9f ? (res - 0.9f) * 12.0f : 0.0f);
    float comp = 1.0f + 0.35f * k;           // keep some body as it resonates
    float a_inc = 1000.0f / (attack_ms(p->v[JP_ENV_A]) * JN_SR);
    float d_k = fexp2(-3.0f * 1.442695f * 1000.0f / (decay_ms(p->v[JP_ENV_D]) * JN_SR));
    float r_k = fexp2(-3.0f * 1.442695f * 1000.0f / (release_ms(p->v[JP_ENV_R]) * JN_SR));
    float sus = p->v[JP_ENV_S];
    float lfo_rise = (float)n * 1000.0f / (lfo_delay_ms(p->v[JP_LFO_DELAY]) * JN_SR);
    // LEVEL 0 is quiet, not silent, as on the Juno (B23, B27 sit there)
    float level = (0.2f + 0.8f * p->v[JP_VCA_LEVEL]) * 0.4f, vsens = p->v[JP_VEL];
    float bend = e->bend * p->v[JP_BEND] * 12.0f / 12.0f;   // octaves
    float pm = porta_ms(p->v[JP_PORTA]);
    float glide_k = pm > 0 ? fexp2(-3.0f * 1.442695f * 1000.0f * CTL / (pm * JN_SR)) : 0.0f;

    // the LFO: one triangle for every voice, as on the Juno
    e->lfo_ph += lfo_hz(p->v[JP_LFO_RATE]) * (float)n / JN_SR;
    if (e->lfo_ph >= 1.0f) e->lfo_ph -= 1.0f;
    float lfo = 4.0f * fabsf(e->lfo_ph - 0.5f) - 1.0f;

    for (int vi = 0; vi < JN_VOICES; vi++) {
        jn_voice_t *v = &e->v[vi];
        if (!v->active) continue;
        float vg = level * (1.0f - vsens + vsens * v->vel);
        v->lfo_amt = clampf(v->lfo_amt + lfo_rise, 0.0f, 1.0f);
        float vlfo = lfo * v->lfo_amt;
        float duty = pwm_man ? 0.5f + 0.45f * pwm : 0.5f + 0.45f * pwm * vlfo;
        for (int i = 0; i < n; i += CTL) {
            v->pitch = glide_k > 0 ? v->target + (v->pitch - v->target) * glide_k : v->target;
            float oct = (v->pitch - 69.0f) / 12.0f;
            float dt = clampf(440.0f * fexp2(oct + range + dco_lfo * vlfo + bend) / JN_SR, 0.0f, 0.4f);
            float fc = fc_base * fexp2(kbd * oct + env_oct * v->env + lfo_oct * vlfo);
            float g = ftan(3.14159265f * clampf(fc, 20.0f, 0.4f * JN_SR) / JN_SR);
            float G = g / (1.0f + g), G2 = G * G, G4 = G2 * G2, inv = 1.0f - G;
            float den = 1.0f / (1.0f + k * G4);
            int m = n - i < CTL ? n - i : CTL;
            for (int j = 0; j < m; j++) {
                // the DCO
                float ph = v->ph, x = 0;
                if (g_saw > 0) x += (2.0f * ph - 1.0f) - blep(ph, dt);
                if (g_pulse > 0) {
                    float q = ph - duty + 1.0f;
                    if (q >= 1.0f) q -= 1.0f;
                    x += (ph < duty ? 1.0f : -1.0f) + blep(ph, dt) - blep(q, dt);
                }
                if (g_sub > 0) {
                    float s = v->sub;
                    if (ph < dt) s += v->sub * blep(ph, dt);
                    else if (ph > 1.0f - dt) s -= v->sub * blep(ph, dt);
                    x += g_sub * s;
                }
                if (g_noise > 0) {
                    uint32_t s = v->seed;
                    s ^= s << 13;
                    s ^= s >> 17;
                    s ^= s << 5;
                    v->seed = s;
                    x += g_noise * ((float)(int32_t)s * (1.0f / 2147483648.0f));
                }
                ph += dt;
                if (ph >= 1.0f) {
                    ph -= 1.0f;
                    v->sub = -v->sub;
                }
                v->ph = ph;
                // the VCF: a 4-pole ladder, solved without a delay in its loop
                float S = G2 * G * inv * v->s[0] + G2 * inv * v->s[1] + G * inv * v->s[2] + inv * v->s[3];
                // a whisper of noise (-70 dB), as in any analogue circuit: what
                // starts a self-oscillating VCF singing
                uint32_t h = v->seed * 1664525u + 1013904223u;
                v->seed = h;
                float hiss = (float)(int32_t)h * (3e-4f / 2147483648.0f);
                float u = (x * comp + hiss - k * S) * den;
                u = u / (1.0f + 0.15f * fabsf(u));   // a little drive, and no runaway
                for (int st = 0; st < 4; st++) {
                    float w = (u - v->s[st]) * G;
                    float y = w + v->s[st];
                    v->s[st] = y + w;
                    u = y;
                }
                // the envelope (the VCF's, and the VCA's unless on GATE)
                if (v->est == 0) {
                    v->env += a_inc;
                    if (v->env >= 1.0f) {
                        v->env = 1.0f;
                        v->est = 1;
                    }
                } else if (v->est == 1) {
                    v->env = sus + (v->env - sus) * d_k;
                } else {
                    v->env *= r_k;
                }
                v->gate += ((v->on ? 1.0f : 0.0f) - v->gate) * 0.01f;
                mono[i + j] += u * vg * (gate_mode ? v->gate : v->env);
            }
        }
        if (!v->on && (gate_mode ? v->gate : v->env) < 1e-4f) v->active = false;
        if (v->on && !gate_mode && v->est == 1 && sus < 1e-4f && v->env < 1e-4f) v->active = false;
    }

    // the HPF: 0 lifts the bass, 1 is flat, 2 and 3 cut it
    int hpf = jn_step(p, JP_HPF);
    if (hpf != 1) {
        float a = hpf == 0 ? 0.028f : hpf == 2 ? 0.032f : 0.092f;   // ~200, 225, 680 Hz
        for (int i = 0; i < n; i++) {
            float x = mono[i];
            e->ls_y += (x - e->ls_y) * a;   // one-pole low-pass
            mono[i] = hpf == 0 ? x + 0.9f * e->ls_y : x - e->ls_y;
        }
    }

    // the chorus
    int cho = jn_step(p, JP_CHORUS);
    if (!cho) {
        for (int i = 0; i < n; i++) {
            l[i] += mono[i];
            r[i] += mono[i];
            e->cho_buf[e->cho_w] = mono[i];   // keep the line current
            if (++e->cho_w == JN_CHO_LEN) e->cho_w = 0;
        }
        return;
    }
    // I: 0.5 Hz, II: 0.86 Hz, both 1.66..5.35 ms; I+II: 9.75 Hz, shallow
    static const float RATE[4] = {0, 0.513f, 0.863f, 9.75f};
    static const float MID[4] = {0, 3.5f, 3.5f, 3.4f}, SWING[4] = {0, 1.85f, 1.85f, 0.25f};
    float inc = RATE[cho] / JN_SR, mid = MID[cho] * JN_SR / 1000.0f, sw = SWING[cho] * JN_SR / 1000.0f;
    for (int i = 0; i < n; i++) {
        e->cho_buf[e->cho_w] = mono[i];
        e->cho_ph += inc;
        if (e->cho_ph >= 1.0f) e->cho_ph -= 1.0f;
        float t = 4.0f * fabsf(e->cho_ph - 0.5f) - 1.0f;
        float wl = cho_read(e, mid + sw * t), wr = cho_read(e, mid - sw * t);
        l[i] += 0.7f * (mono[i] + wl);
        r[i] += 0.7f * (mono[i] + wr);
        if (++e->cho_w == JN_CHO_LEN) e->cho_w = 0;
    }
}
