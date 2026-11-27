// test_juno.c - host tests for the JUNO-6 voices.   make -C tests test
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jn_dsp.h"

static int failures;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static bool finite = true;
static float diff;   // largest |L - R| of the last run

static float run(jn_engine_t *e, int frames) {
    float peak = 0, l[48], r[48];
    diff = 0;
    for (int i = 0; i < frames; i += 48) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        jn_render(e, l, r, 48);
        for (int j = 0; j < 48; j++) {
            if (!isfinite(l[j]) || !isfinite(r[j])) finite = false;
            if (fabsf(l[j]) > peak) peak = fabsf(l[j]);
            if (fabsf(l[j] - r[j]) > diff) diff = fabsf(l[j] - r[j]);
        }
    }
    return peak;
}

static int active(const jn_engine_t *e) {
    int n = 0;
    for (int i = 0; i < JN_VOICES; i++) n += e->v[i].active;
    return n;
}

// pitch of voice 0's output, by rising zero crossings
static float measure_hz(jn_engine_t *e) {
    float l[48], r[48], prev = 0;
    int cross = 0, first = -1, last = 0, t = 0;
    for (int b = 0; b < 44100 / 48; b++) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        jn_render(e, l, r, 48);
        for (int j = 0; j < 48; j++, t++) {
            if (b > 100 && prev < 0 && l[j] >= 0) {
                if (first < 0) first = t;
                last = t;
                cross++;
            }
            prev = l[j];
        }
    }
    return cross > 1 ? (cross - 1) * 44100.0f / (float)(last - first) : 0;
}

int main(void) {
    static jn_engine_t e;
    // A11 Brass Set 1 decodes as the Juno shows it: 16', saw, PWM by hand,
    // HPF 1, chorus I
    jn_patch_t p;
    jn_factory(&p, 0);
    CHECK(jn_step(&p, JP_RANGE) == 0, "range %d", jn_step(&p, JP_RANGE));
    CHECK(jn_step(&p, JP_SAW) == 1 && jn_step(&p, JP_PULSE) == 0, "waves");
    CHECK(jn_step(&p, JP_PWM_MAN) == 1, "pwm manual");
    CHECK(jn_step(&p, JP_HPF) == 1, "hpf %d", jn_step(&p, JP_HPF));
    CHECK(jn_step(&p, JP_CHORUS) == 1, "chorus %d", jn_step(&p, JP_CHORUS));
    CHECK(fabsf(p.v[JP_VCF_FREQ] - 35 / 127.0f) < 1e-6f, "vcf freq");
    printf("sysex: A11 decodes as on the Juno\n");

    // every factory patch: sounds, stays finite and in level, lets go
    for (int i = 0; i < JN_NFACTORY; i++) {
        jn_init(&e);
        jn_factory(&e.p, i);
        for (int k = 0; k < 3; k++) jn_note_on(&e, 0x80, (uint8_t)(48 + 4 * k), 48 + 4 * k, 0.8f, 0);
        float pk = run(&e, 44100);
        jn_all_off(&e, -1);
        run(&e, 44100 * 30);
        // (A82 has no oscillator: only the VCF singing, and quietly)
        CHECK(pk > 0.002f && pk < 2.0f, "%s peak %.3f", JN_FACTORY[i].name, pk);
        CHECK(active(&e) == 0, "%s still sounding after release", JN_FACTORY[i].name);
    }
    CHECK(finite, "non-finite output");
    printf("patches: %d sound, stay in level and release\n", JN_NFACTORY);

    // the DCO is in tune: 8' A4 = 440 Hz, 16' an octave down
    jn_init(&e);
    jn_patch_default(&e.p);
    e.p.v[JP_VCF_FREQ] = 1;
    e.p.v[JP_ENV_S] = 1;
    jn_set_step(&e.p, JP_CHORUS, 0);
    jn_note_on(&e, 0x80, 69, 69, 1, 0);
    float hz = measure_hz(&e);
    CHECK(fabsf(hz - 440) < 2, "8' A4 at %.1f Hz", hz);
    jn_init(&e);
    jn_patch_default(&e.p);
    e.p.v[JP_VCF_FREQ] = 1;
    e.p.v[JP_ENV_S] = 1;
    jn_set_step(&e.p, JP_CHORUS, 0);
    jn_set_step(&e.p, JP_RANGE, 0);
    jn_note_on(&e, 0x80, 69, 69, 1, 0);
    hz = measure_hz(&e);
    CHECK(fabsf(hz - 220) < 1.5f, "16' A4 at %.1f Hz", hz);
    printf("pitch: 8' %.1f Hz\n", 440.0);

    // the chorus makes it stereo; off, it's mono
    jn_init(&e);
    jn_factory(&e.p, 0);
    jn_set_step(&e.p, JP_CHORUS, 0);
    jn_note_on(&e, 0x80, 60, 60, 1, 0);
    run(&e, 22050);
    CHECK(diff == 0, "mono with the chorus off (%.4f)", diff);
    jn_set_step(&e.p, JP_CHORUS, 2);
    run(&e, 22050);
    CHECK(diff > 0.01f, "stereo with chorus II (%.4f)", diff);
    printf("chorus: off is mono, on is stereo\n");

    // GATE: the VCA ignores the envelope - a zero-sustain patch holds
    jn_init(&e);
    jn_factory(&e.p, 0);
    e.p.v[JP_ENV_S] = 0;
    e.p.v[JP_ENV_D] = 0;
    jn_set_step(&e.p, JP_VCA_GATE, 1);
    jn_note_on(&e, 0x80, 60, 60, 1, 0);
    run(&e, 44100);
    float held = run(&e, 4410);
    CHECK(held > 0.005f && active(&e) == 1, "gate holds (%.4f)", held);
    jn_note_off(&e, 0x80, 60);
    run(&e, 44100);
    CHECK(active(&e) == 0, "gate lets go");
    printf("gate: holds while the key is down\n");

    // more notes than voices: the oldest goes, nothing breaks
    jn_init(&e);
    jn_factory(&e.p, 4);
    for (int k = 0; k < 10; k++) jn_note_on(&e, 0x80, (uint8_t)(40 + k), 40 + k, 0.7f, 0);
    run(&e, 4410);
    CHECK(active(&e) == JN_VOICES, "voices %d", active(&e));
    printf("voices: %d at most\n", JN_VOICES);

    printf(failures ? "test_juno: %d FAILED\n" : "test_juno: all passed\n", failures);
    return failures != 0;
}
