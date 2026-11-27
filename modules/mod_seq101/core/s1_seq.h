// s1_seq.h - SEQ101's player: from the master clock's ticks (S1_TICKS to a
// 1/16) to note on / off events. Plain C, host-tested.
//
// Each lane counts its own 1/16s over its length. When a lane comes back to
// its first step (its loop crossover) a queued pattern comes in, or the next
// one in its chain. A step's note sounds for its length (a little short of
// it, so the next note is a new gate); with the arpeggiator on, the chord's
// notes take turns over that length at the arp's rate.
#pragma once

#include "s1_model.h"

enum { S1_EV_ON = 1, S1_EV_OFF };

typedef struct {
    uint8_t type;   // S1_EV_*
    uint8_t lane;
    uint8_t note;
    uint8_t vel;
    uint8_t first;  // ON: the lowest note of the step (the one CV and routing play)
} s1_ev_t;

typedef struct {
    int16_t step;              // the step playing, -1 before the first
    int8_t chain_i;            // place in the chain
    volatile int8_t queued;    // a slot waiting for the loop crossover, -1 none (UI sets)
    uint8_t held[S1_MAX_NOTES];
    uint8_t nheld;
    uint32_t off_at;           // tick the held notes end
    // the arpeggiator over the current step
    uint8_t arp_notes[S1_MAX_NOTES * 2];
    uint8_t arp_n, arp_i, arp_vel;
    uint32_t arp_next, arp_end;
} s1_lane_rt_t;

typedef struct {
    s1_lane_rt_t lane[S1_LANES];
    uint32_t rnd;
} s1_seq_t;

// Once, before anything else: nothing queued.
void s1_seq_init(s1_seq_t *q);

// Start (or start again) at tick t: each lane in step with the bar, so a
// lane switched on mid-song lands where it would have been.
void s1_seq_start(s1_seq_t *q, const s1_state_t *s, uint32_t t);

// Everything due on tick t, in order (note-offs before note-ons). Returns
// how many events were written to out (at most max).
int s1_seq_tick(s1_seq_t *q, s1_state_t *s, uint32_t t, s1_ev_t *out, int max);

// Note-offs for whatever is held (stop, switched off). Returns the count.
int s1_seq_all_off(s1_seq_t *q, s1_ev_t *out, int max);

// The step each lane is on, for the playhead (-1 stopped).
static inline int s1_seq_step(const s1_seq_t *q, int lane) { return q->lane[lane].step; }
