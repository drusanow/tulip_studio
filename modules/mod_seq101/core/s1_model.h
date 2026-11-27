// s1_model.h - SEQ101's data: 4 lanes of 16 steps, 12 patterns a lane,
// chains. Plain C (no ESP-IDF, no LVGL): host-tested in tests/test_s101.c.
//
// A step holds a note - or a chord built on it - with a velocity and a
// length in 1/16s, so a note can be held for 1/8, 1/4, a bar. Each lane plays
// one of its 12 patterns, loops over its own number of steps, and can run
// through a chain of patterns, one loop each.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define S1_LANES 4
#define S1_STEPS 16
#define S1_SLOTS 12
#define S1_CHAIN 16
#define S1_MAX_NOTES 4    // a chord's notes
#define S1_TICKS 6        // ticks to a 1/16 (24 to a beat)
#define S1_NAME 12

// chords on a step's note
enum { CH_NONE = 0, CH_MAJ, CH_MIN, CH_SUS2, CH_SUS4, CH_7, CH_MAJ7, CH_MIN7, CH_5TH, CH_OCT, CH_COUNT };
extern const char *const S1_CHORD_NAMES[CH_COUNT];

// the arpeggiator
enum { ARP_UP = 0, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM, ARP_ORDER, ARP_MODES };
enum { ARP_R32 = 0, ARP_R16, ARP_R8, ARP_R4, ARP_RATES };
extern const char *const S1_ARP_NAMES[ARP_MODES];
extern const char *const S1_RATE_NAMES[ARP_RATES];
int s1_rate_ticks(int rate);

// quantise scales (notes snap to the nearest in the scale as they play)
enum { SC_CHROMATIC = 0, SC_MAJOR, SC_MINOR, SC_DORIAN, SC_PHRYGIAN, SC_LYDIAN, SC_MIXOLYDIAN,
       SC_HARM_MINOR, SC_PENTA_MAJ, SC_PENTA_MIN, SC_BLUES, SC_COUNT };
extern const char *const S1_SCALE_NAMES[SC_COUNT];
extern const char *const S1_ROOT_NAMES[12];
bool s1_in_scale(int note, int root, int scale);
int s1_quantise(int note, int root, int scale);   // the nearest scale note (down on a tie)

// note lengths offered (in 1/16s), and their names
#define S1_LENS 8
extern const uint8_t S1_LEN_VALUES[S1_LENS];        // 1 2 3 4 6 8 12 16
extern const char *const S1_LEN_NAMES[S1_LENS];      // 1/16 1/8 3/16 1/4 3/8 1/2 3/4 BAR
const char *s1_len_name(int len);                    // any 1..16

typedef struct {
    uint8_t on;
    uint8_t note;     // MIDI 0..127, the chord's root
    uint8_t chord;    // CH_*
    uint8_t vel;      // 1..127
    uint8_t len;      // 1..16 1/16s
} s1_step_t;

typedef struct {
    s1_step_t st[S1_STEPS];
} s1_pat_t;

enum { CV_OUT_OFF = 0, CV_OUT_A, CV_OUT_B };   // the DAC at 0x59 / 0x58

typedef struct {
    char name[S1_NAME + 1];
    uint8_t length;     // steps it loops over, 1..16
    uint8_t arp, arp_mode, arp_rate;
    int8_t midi_ch;     // 0 off, 1..16
    uint8_t cv;         // CV_OUT_*
    uint8_t mute;
    uint8_t slot;       // the pattern playing (0..11)
    uint8_t chain[S1_CHAIN];
    uint8_t nchain;     // 0: no chain
} s1_lane_t;

typedef struct {
    s1_lane_t lane[S1_LANES];
    s1_pat_t pat[S1_LANES][S1_SLOTS];
    uint8_t cv_5v;      // pitch 0-5 V (AMYboard inputs) instead of 0-10 V
    uint8_t scale_root; // 0 = C .. 11 = B
    uint8_t scale;      // SC_*
} s1_state_t;

void s1_defaults(s1_state_t *s);                // four lanes, a starting pattern on A
void s1_sanitise(s1_state_t *s);                // clamp everything after a load
void s1_step_default(s1_step_t *st, int note);  // a 1/16 at velocity 100

// The notes a step plays, low to high; returns how many (0 if off).
int s1_chord_notes(const s1_step_t *st, uint8_t out[S1_MAX_NOTES]);

// The step a 1/16 belongs to, if one held from earlier covers it (its own
// index when it starts there); -1 if none. Notes don't run past the lane end.
int s1_owner(const s1_pat_t *p, int length, int step);

// A note's length, cut so it ends before the next note and the lane's end.
int s1_fit_len(const s1_pat_t *p, int length, int step, int len);

bool s1_pat_empty(const s1_pat_t *p);

// note name, "C4" (C4 = 60)
void s1_note_name(int note, char *out, int n);

// --- JSON (patches and the state file) -----------------------------------------------
struct cJSON;
struct cJSON *s1_to_json(const s1_state_t *s);
void s1_from_json(const struct cJSON *o, s1_state_t *s);   // keys missing keep s's values
