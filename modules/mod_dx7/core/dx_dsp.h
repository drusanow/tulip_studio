// dx_dsp.h - DX7, a 6-operator FM synth that plays DX7 voices, as the Tulip
// does with AMY: the 32 algorithms, the operators' 4-rate / 4-level
// envelopes, keyboard level and rate scaling, velocity, the pitch envelope
// and the LFO (pitch and amplitude). 8 voices.
//
// A sound is the DX7's own 155-byte voice data (VCED), so the 128 voices
// the Tulip has, and any DX7 bank (.syx), play as they are. The envelope,
// scaling and velocity follow the open-source MSFA engine (as in Dexed and
// AMY). Pure C: tested on the PC.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DX_VOICES 8
#define DX_SR 44100.0f
#define DX_VOICE_LEN 155     // VCED: 6 x 21 operator bytes (op 6 first), 29 more
#define DX_NAME_LEN 10

// where things are in the voice data
#define DX_OP(op) ((5 - (op)) * 21)          // op 0..5 = the DX7's OP1..OP6
enum {
    DXO_R1 = 0, DXO_L1 = 4, DXO_BP = 8, DXO_LDEPTH, DXO_RDEPTH, DXO_LCURVE, DXO_RCURVE,
    DXO_RATE_SCALE, DXO_AMS, DXO_VEL, DXO_LEVEL, DXO_MODE, DXO_COARSE, DXO_FINE, DXO_DETUNE,
};
enum {
    DXV_PR1 = 126, DXV_PL1 = 130, DXV_ALGO = 134, DXV_FEEDBACK, DXV_OSC_SYNC,
    DXV_LFO_SPEED, DXV_LFO_DELAY, DXV_LFO_PMD, DXV_LFO_AMD, DXV_LFO_SYNC, DXV_LFO_WAVE,
    DXV_PMS, DXV_TRANSPOSE, DXV_NAME,
};

typedef struct {
    uint8_t d[DX_VOICE_LEN + 1];
} dx_voice_t;

#define DX_NFACTORY 128
extern const uint8_t DX_FACTORY[DX_NFACTORY][DX_VOICE_LEN];

void dx_name(const dx_voice_t *v, char out[DX_NAME_LEN + 1]);   // trimmed
void dx_set_name(dx_voice_t *v, const char *name);
// Clamp every byte to its range (voices from files).
void dx_sanitise(dx_voice_t *v);
// One voice of a 32-voice bank's packed (VMEM, 128-byte) data.
void dx_unpack(const uint8_t *packed, dx_voice_t *v);
// A .syx file's voices: a 32-voice bank (4104 bytes) or one voice (163).
// Returns how many went into out (max of them).
int dx_parse_syx(const uint8_t *data, int len, dx_voice_t *out, int max);
// The carriers of an algorithm (bit k: OP k+1 is heard), 0..31.
uint8_t dx_carriers(int algo);

// --- engine (audio task) --------------------------------------------------------------------

typedef struct {
    int32_t level, target, inc;   // log2 gain, Q24
    uint8_t ix;                   // segment 0..3, 4 = done
    bool rising, down;
    int32_t outlevel;             // the operator's level with scaling, velocity
    uint8_t rates[4], levels[4];
    int8_t rate_scale;
} dx_env_t;

typedef struct {
    float ph, inc, gain, last_gain, fb[2];
    dx_env_t env;
} dx_op_t;

typedef struct {
    bool on, active;
    uint8_t src, key, note;
    uint32_t age;
    float base_hz;                // the note's pitch (transpose included)
    dx_op_t op[6];
    int32_t pitch_level, pitch_target, pitch_inc;   // the pitch envelope, Q24 octaves
    uint8_t pitch_ix;
    bool pitch_rising;
    float lfo_amt;                // the LFO delay's fade-in
} dx_vc_t;

typedef struct {
    dx_voice_t v;                 // the sound
    dx_vc_t vc[DX_VOICES];
    uint32_t clock;
    float lfo_ph, lfo_sh;
    uint32_t seed;
    float bend;                   // -1..1
    float bend_semis;
    float level;                  // output gain
} dx_engine_t;

void dx_init(dx_engine_t *e);
void dx_note_on(dx_engine_t *e, uint8_t src, uint8_t key, int note, float vel);
void dx_note_off(dx_engine_t *e, uint8_t src, uint8_t key);
void dx_all_off(dx_engine_t *e, int src);   // src < 0: everyone
void dx_render(dx_engine_t *e, float *l, float *r, int n);   // adds into l / r, n <= 64
