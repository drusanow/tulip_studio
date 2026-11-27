// jn_patch.c - JUNO-6's controls: their names, defaults, how they read, and
// the Juno-106 patch dump. Not on the audio path, so it stays in flash.
#include <stdio.h>

#include "jn_curves.h"
#include "jn_dsp.h"

const jn_param_info_t JN_PARAMS[JN_NPARAM] = {
    [JP_LFO_RATE] = {"lfo_rate", "RATE", 0.4f, 0},     [JP_LFO_DELAY] = {"lfo_delay", "DELAY", 0.0f, 0},
    [JP_DCO_LFO] = {"dco_lfo", "LFO", 0.0f, 0},        [JP_DCO_PWM] = {"dco_pwm", "PWM", 0.0f, 0},
    [JP_DCO_NOISE] = {"dco_noise", "NOISE", 0.0f, 0},  [JP_VCF_FREQ] = {"vcf_freq", "FREQ", 0.6f, 0},
    [JP_VCF_RES] = {"vcf_res", "RES", 0.1f, 0},        [JP_VCF_ENV] = {"vcf_env", "ENV", 0.25f, 0},
    [JP_VCF_LFO] = {"vcf_lfo", "LFO", 0.0f, 0},        [JP_VCF_KBD] = {"vcf_kbd", "KYBD", 0.5f, 0},
    [JP_VCA_LEVEL] = {"vca_level", "LEVEL", 0.7f, 0},  [JP_ENV_A] = {"env_a", "A", 0.0f, 0},
    [JP_ENV_D] = {"env_d", "D", 0.4f, 0},              [JP_ENV_S] = {"env_s", "S", 0.6f, 0},
    [JP_ENV_R] = {"env_r", "R", 0.25f, 0},             [JP_DCO_SUB] = {"dco_sub", "SUB", 0.0f, 0},
    [JP_RANGE] = {"range", "RANGE", 0.5f, 3},          [JP_PULSE] = {"pulse", "PULSE", 0.0f, 2},
    [JP_SAW] = {"saw", "SAW", 1.0f, 2},                [JP_PWM_MAN] = {"pwm_man", "PWM MAN", 0.0f, 2},
    [JP_VCF_NEG] = {"vcf_neg", "ENV -", 0.0f, 2},      [JP_VCA_GATE] = {"vca_gate", "GATE", 0.0f, 2},
    [JP_HPF] = {"hpf", "HPF", 1.0f / 3, 4},            [JP_CHORUS] = {"chorus", "CHORUS", 1.0f / 3, 4},
    [JP_PORTA] = {"porta", "PORTA", 0.0f, 0},          [JP_VEL] = {"vel", "VEL", 1.0f, 0},
    [JP_BEND] = {"bend", "BEND", 2.0f / 12, 0},
};

void jn_patch_default(jn_patch_t *p) {
    for (int i = 0; i < JN_NPARAM; i++) p->v[i] = JN_PARAMS[i].def;
}

void jn_from_sysex(jn_patch_t *p, const uint8_t sx[18]) {
    for (int i = 0; i < JN_NSLIDERS; i++) p->v[i] = (sx[i] & 127) / 127.0f;
    uint8_t a = sx[16], b = sx[17];
    jn_set_step(p, JP_RANGE, (a & 1) ? 0 : (a & 4) ? 2 : 1);
    jn_set_step(p, JP_PULSE, (a >> 3) & 1);
    jn_set_step(p, JP_SAW, (a >> 4) & 1);
    static const uint8_t cho[4] = {2, 0, 1, 0};   // bit 5 is "chorus off", bit 6 "I, not II"
    jn_set_step(p, JP_CHORUS, cho[(a >> 5) & 3]);
    jn_set_step(p, JP_PWM_MAN, b & 1);
    jn_set_step(p, JP_VCF_NEG, (b >> 1) & 1);
    jn_set_step(p, JP_VCA_GATE, (b >> 2) & 1);
    jn_set_step(p, JP_HPF, 3 - ((b >> 3) & 3));
}

void jn_factory(jn_patch_t *p, int i) {
    jn_patch_default(p);
    jn_from_sysex(p, JN_FACTORY[i].sx);
    p->v[JP_VCA_LEVEL] *= JN_FACTORY[i].gain;
}

static void ms_text(char *o, int n, float ms) {
    if (ms < 1000)
        snprintf(o, n, "%d ms", (int)(ms + 0.5f));
    else
        snprintf(o, n, "%.1f s", ms / 1000);
}

void jn_param_text(const jn_patch_t *p, int i, char *o, int n) {
    float v = p->v[i];
    switch (i) {
    case JP_RANGE: snprintf(o, n, "%s", (const char *[]){"16'", "8'", "4'"}[jn_step(p, i)]); return;
    case JP_HPF: snprintf(o, n, "%d", jn_step(p, i)); return;
    case JP_CHORUS: snprintf(o, n, "%s", (const char *[]){"OFF", "I", "II", "I+II"}[jn_step(p, i)]); return;
    case JP_PULSE: case JP_SAW: case JP_PWM_MAN: case JP_VCF_NEG: case JP_VCA_GATE:
        snprintf(o, n, "%s", jn_step(p, i) ? "ON" : "OFF");
        return;
    case JP_ENV_A: ms_text(o, n, attack_ms(v)); return;
    case JP_ENV_D: ms_text(o, n, decay_ms(v)); return;
    case JP_ENV_R: ms_text(o, n, release_ms(v)); return;
    case JP_LFO_RATE: snprintf(o, n, "%.1f Hz", lfo_hz(v)); return;
    case JP_LFO_DELAY: ms_text(o, n, lfo_delay_ms(v)); return;
    case JP_PORTA: if (v < 0.004f) snprintf(o, n, "OFF"); else ms_text(o, n, porta_ms(v)); return;
    case JP_VEL: snprintf(o, n, "%d%%", (int)(v * 100 + 0.5f)); return;
    case JP_BEND: snprintf(o, n, "%d st", (int)(v * 12 + 0.5f)); return;
    case JP_VCF_FREQ: {
        float f = clampf(vcf_hz(v), 0, 99999);
        if (f < 1000)
            snprintf(o, n, "%d Hz", (int)f);
        else
            snprintf(o, n, "%.1fk", f / 1000);
        return;
    }
    default: snprintf(o, n, "%d", (int)(v * 127 + 0.5f)); return;   // the Juno's 0..127
    }
}
