// d9_comp.h - Drumster9's drum-bus compressor (plain C, host-tested).
//
// Drum samples are all transient: they peak near full scale but sit ~16 dB
// lower on average, so next to a synth they sound quiet. This squeezes the
// peaks down and brings the whole kit up (auto make-up), so it's louder
// without clipping. One control, AMOUNT 0..1:
//
//   threshold  0 dB .. -24 dB      ratio  1:1 .. 6:1
//   make-up    so a full-scale peak comes out at about -1 dB
//
// Envelope per block (48 frames): instant attack, ~80 ms release; the gain
// moves smoothly across each block, and a soft clip catches what's left.
#pragma once

typedef struct {
    float env;    // the peak envelope
    float gain;   // the gain applied at the end of the last block
    float gr;     // gain reduction for the meter (0..1, 1 = none), smoothed
} d9_comp_t;

void d9_comp_init(d9_comp_t *c);
void d9_comp_run(d9_comp_t *c, float amount, float *l, float *r, int n, float sr);
