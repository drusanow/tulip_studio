// jn_dsp.h - JUNO-6, a 6-voice analogue-style polysynth after the Roland
// Juno-6 / 106, as on the Tulip (AMY's juno.py): one DCO per voice (pulse
// with PWM, saw, a square sub an octave down, noise), a 24 dB low-pass VCF
// with its own ENV / LFO / KYBD amounts, a VCA on the ADSR or the gate, one
// LFO with a delay, a 4-step HPF and the stereo BBD chorus (I, II, I+II).
//
// The controls and their curves follow AMY's, so the 128 factory patches
// (the Juno's 18-byte patch dumps) sound as they do there. Pure C: tested
// on the PC.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define JN_VOICES 6
#define JN_SR 44100.0f

// The first 16 are the Juno's sliders, in the order of its patch dump.
enum {
    JP_LFO_RATE = 0, JP_LFO_DELAY, JP_DCO_LFO, JP_DCO_PWM, JP_DCO_NOISE,
    JP_VCF_FREQ, JP_VCF_RES, JP_VCF_ENV, JP_VCF_LFO, JP_VCF_KBD, JP_VCA_LEVEL,
    JP_ENV_A, JP_ENV_D, JP_ENV_S, JP_ENV_R, JP_DCO_SUB,
    // switches (0..1 in steps)
    JP_RANGE,      // 16' 8' 4'
    JP_PULSE, JP_SAW,
    JP_PWM_MAN,    // PWM from the LFO / by hand (the PWM slider)
    JP_VCF_NEG,    // envelope polarity
    JP_VCA_GATE,   // VCA on the gate, not the envelope
    JP_HPF,        // 0..3
    JP_CHORUS,     // off, I, II, I+II
    // not on the Juno
    JP_PORTA, JP_VEL, JP_BEND,
    JN_NPARAM
};
#define JN_NSLIDERS 16

typedef struct {
    float v[JN_NPARAM];   // 0..1 each (switches: steps)
} jn_patch_t;

typedef struct {
    const char *key, *label;
    float def;
    uint8_t steps;        // > 0: a switch with this many positions
} jn_param_info_t;

extern const jn_param_info_t JN_PARAMS[JN_NPARAM];

void jn_patch_default(jn_patch_t *p);
int jn_step(const jn_patch_t *p, int i);              // a switch's position
void jn_set_step(jn_patch_t *p, int i, int step);
void jn_param_text(const jn_patch_t *p, int i, char *out, int len);

// The Juno-106 patch dump: 16 slider bytes 0..127, then the switches.
void jn_from_sysex(jn_patch_t *p, const uint8_t sx[18]);

#define JN_NFACTORY 128
typedef struct {
    const char *name;
    uint8_t sx[18];
    float gain;           // AMY's trims of the loudest patches
} jn_factory_t;
extern const jn_factory_t JN_FACTORY[JN_NFACTORY];
void jn_factory(jn_patch_t *p, int i);

// --- engine (audio task) --------------------------------------------------------------------

typedef struct {
    bool on, active;
    uint8_t src, key;
    uint32_t age;
    float vel;
    float pitch, target;      // MIDI note numbers
    float ph, sub;            // DCO phase, sub-octave sign
    float env;                // ADSR 0..1
    uint8_t est;              // 0 attack, 1 decay / sustain, 2 release
    float gate;               // the gate, smoothed (VCA GATE)
    float lfo_amt;            // the LFO delay's fade-in, 0..1
    float s[4];               // the VCF's four stages
    uint32_t seed;
} jn_voice_t;

#define JN_CHO_LEN 512        // 11.6 ms of delay line

typedef struct {
    jn_patch_t p;
    jn_voice_t v[JN_VOICES];
    uint32_t clock;
    float lfo_ph, cho_ph;
    float hp_x, hp_y, ls_y;   // the HPF / bass shelf
    float cho_buf[JN_CHO_LEN];
    int cho_w;
    float bend;               // -1..1 (pitch wheel)
    float last_pitch;
} jn_engine_t;

void jn_init(jn_engine_t *e);
void jn_note_on(jn_engine_t *e, uint8_t src, uint8_t key, float note, float vel, int32_t glide_us);
void jn_note_off(jn_engine_t *e, uint8_t src, uint8_t key);
void jn_all_off(jn_engine_t *e, int src);   // src < 0: everyone
void jn_render(jn_engine_t *e, float *l, float *r, int n);   // adds into l / r
