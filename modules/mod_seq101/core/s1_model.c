// s1_model.c - see s1_model.h.
#include "s1_model.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

const char *const S1_CHORD_NAMES[CH_COUNT] = {"NOTE", "MAJ", "MIN", "SUS2", "SUS4",
                                              "7",    "MAJ7", "MIN7", "5TH", "OCT"};
static const int8_t CHORDS[CH_COUNT][S1_MAX_NOTES] = {
    {0, -1, -1, -1}, {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 2, 7, -1}, {0, 5, 7, -1},
    {0, 4, 7, 10},   {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 7, -1, -1}, {0, 12, -1, -1},
};

const char *const S1_SCALE_NAMES[SC_COUNT] = {"CHROMATIC", "MAJOR",      "MINOR",     "DORIAN",
                                              "PHRYGIAN",  "LYDIAN",     "MIXOLYDIAN", "HARM MINOR",
                                              "PENTA MAJ", "PENTA MIN",  "BLUES"};
const char *const S1_ROOT_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
// bit k: semitone k above the root is in the scale
static const uint16_t SCALES[SC_COUNT] = {
    0xfff,                                             // chromatic
    1 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 11,   // major
    1 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10,   // natural minor
    1 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10,   // dorian
    1 | 1 << 1 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10,   // phrygian
    1 | 1 << 2 | 1 << 4 | 1 << 6 | 1 << 7 | 1 << 9 | 1 << 11,   // lydian
    1 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10,   // mixolydian
    1 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 11,   // harmonic minor
    1 | 1 << 2 | 1 << 4 | 1 << 7 | 1 << 9,                      // major pentatonic
    1 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 10,                     // minor pentatonic
    1 | 1 << 3 | 1 << 5 | 1 << 6 | 1 << 7 | 1 << 10,            // blues
};

bool s1_in_scale(int note, int root, int scale) {
    if (scale < 0 || scale >= SC_COUNT) return true;
    return SCALES[scale] >> (((note - root) % 12 + 12) % 12) & 1;
}

int s1_quantise(int note, int root, int scale) {
    for (int d = 0; d < 12; d++) {
        if (note - d >= 0 && s1_in_scale(note - d, root, scale)) return note - d;
        if (note + d <= 127 && s1_in_scale(note + d, root, scale)) return note + d;
    }
    return note;
}

const char *const S1_ARP_NAMES[ARP_MODES] = {"UP", "DOWN", "UP/DN", "RAND", "ORDER"};
const char *const S1_RATE_NAMES[ARP_RATES] = {"1/32", "1/16", "1/8", "1/4"};

int s1_rate_ticks(int rate) {
    static const uint8_t T[ARP_RATES] = {S1_TICKS / 2, S1_TICKS, 2 * S1_TICKS, 4 * S1_TICKS};
    return rate >= 0 && rate < ARP_RATES ? T[rate] : S1_TICKS;
}

const uint8_t S1_LEN_VALUES[S1_LENS] = {1, 2, 3, 4, 6, 8, 12, 16};
const char *const S1_LEN_NAMES[S1_LENS] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "3/4", "BAR"};

const char *s1_len_name(int len) {
    static char b[8];
    for (int i = 0; i < S1_LENS; i++)
        if (S1_LEN_VALUES[i] == len) return S1_LEN_NAMES[i];
    snprintf(b, sizeof b, "%d/16", len);
    return b;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void s1_step_default(s1_step_t *st, int note) {
    *st = (s1_step_t){.on = 1, .note = (uint8_t)clampi(note, 0, 127), .chord = CH_NONE, .vel = 100, .len = 1};
}

void s1_defaults(s1_state_t *s) {
    memset(s, 0, sizeof *s);
    static const int8_t CH[S1_LANES] = {1, 2, 3, 4};
    for (int l = 0; l < S1_LANES; l++) {
        s1_lane_t *ln = &s->lane[l];
        snprintf(ln->name, sizeof ln->name, "Lane %c", 'A' + l);
        ln->length = S1_STEPS;
        ln->arp_rate = ARP_R16;
        ln->midi_ch = CH[l];
        ln->cv = l == 0 ? CV_OUT_A : l == 1 ? CV_OUT_B : CV_OUT_OFF;
    }
    // something to hear on PLAY: a C minor bass line on A
    static const int8_t LINE[S1_STEPS] = {48, -1, -1, 48, -1, -1, 51, -1, 53, -1, -1, 55, -1, 53, 51, -1};
    for (int i = 0; i < S1_STEPS; i++)
        if (LINE[i] >= 0) s1_step_default(&s->pat[0][0].st[i], LINE[i]);
}

void s1_sanitise(s1_state_t *s) {
    for (int l = 0; l < S1_LANES; l++) {
        s1_lane_t *ln = &s->lane[l];
        ln->name[S1_NAME] = 0;
        ln->length = (uint8_t)clampi(ln->length, 1, S1_STEPS);
        ln->arp = ln->arp != 0;
        ln->arp_mode = (uint8_t)clampi(ln->arp_mode, 0, ARP_MODES - 1);
        ln->arp_rate = (uint8_t)clampi(ln->arp_rate, 0, ARP_RATES - 1);
        ln->midi_ch = (int8_t)clampi(ln->midi_ch, 0, 16);
        ln->cv = (uint8_t)clampi(ln->cv, 0, CV_OUT_B);
        ln->mute = ln->mute != 0;
        ln->slot = (uint8_t)clampi(ln->slot, 0, S1_SLOTS - 1);
        ln->nchain = (uint8_t)clampi(ln->nchain, 0, S1_CHAIN);
        for (int i = 0; i < ln->nchain; i++) ln->chain[i] = (uint8_t)clampi(ln->chain[i], 0, S1_SLOTS - 1);
        for (int p = 0; p < S1_SLOTS; p++)
            for (int i = 0; i < S1_STEPS; i++) {
                s1_step_t *st = &s->pat[l][p].st[i];
                if (!st->on) {   // an empty step is all zeros
                    memset(st, 0, sizeof *st);
                    continue;
                }
                st->on = 1;
                st->note = (uint8_t)clampi(st->note, 0, 127);
                st->chord = (uint8_t)clampi(st->chord, 0, CH_COUNT - 1);
                st->vel = (uint8_t)clampi(st->vel, 1, 127);
                st->len = (uint8_t)clampi(st->len, 1, S1_STEPS);
            }
    }
    s->cv_5v = s->cv_5v != 0;
    s->scale_root = (uint8_t)clampi(s->scale_root, 0, 11);
    s->scale = (uint8_t)clampi(s->scale, 0, SC_COUNT - 1);
}

int s1_chord_notes(const s1_step_t *st, uint8_t out[S1_MAX_NOTES]) {
    if (!st->on) return 0;
    int n = 0;
    for (int i = 0; i < S1_MAX_NOTES && CHORDS[st->chord][i] >= 0; i++) {
        int v = st->note + CHORDS[st->chord][i];
        if (v <= 127) out[n++] = (uint8_t)v;
    }
    return n;
}

int s1_owner(const s1_pat_t *p, int length, int step) {
    for (int i = step; i >= 0; i--)
        if (p->st[i].on) return i + p->st[i].len > step || i == step ? i : -1;
    (void)length;
    return -1;
}

int s1_fit_len(const s1_pat_t *p, int length, int step, int len) {
    int max = length - step;
    for (int i = step + 1; i < length; i++)
        if (p->st[i].on) {
            max = i - step;
            break;
        }
    return clampi(len, 1, max < 1 ? 1 : max);
}

bool s1_pat_empty(const s1_pat_t *p) {
    for (int i = 0; i < S1_STEPS; i++)
        if (p->st[i].on) return false;
    return true;
}

void s1_note_name(int note, char *out, int n) {
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(out, n, "%s%d", N[note % 12], note / 12 - 1);
}

// --- JSON --------------------------------------------------------------------------------

static int num(const cJSON *o, const char *k, int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

cJSON *s1_to_json(const s1_state_t *s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seq101", 1);
    cJSON_AddBoolToObject(o, "cv_5v", s->cv_5v);
    cJSON_AddNumberToObject(o, "scale_root", s->scale_root);
    cJSON_AddNumberToObject(o, "scale", s->scale);
    cJSON *lanes = cJSON_AddArrayToObject(o, "lanes");
    for (int l = 0; l < S1_LANES; l++) {
        const s1_lane_t *ln = &s->lane[l];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", ln->name);
        cJSON_AddNumberToObject(j, "length", ln->length);
        cJSON_AddBoolToObject(j, "arp", ln->arp);
        cJSON_AddNumberToObject(j, "arp_mode", ln->arp_mode);
        cJSON_AddNumberToObject(j, "arp_rate", ln->arp_rate);
        cJSON_AddNumberToObject(j, "midi_ch", ln->midi_ch);
        cJSON_AddNumberToObject(j, "cv", ln->cv);
        cJSON_AddBoolToObject(j, "mute", ln->mute);
        cJSON_AddNumberToObject(j, "slot", ln->slot);
        cJSON *ch = cJSON_AddArrayToObject(j, "chain");
        for (int i = 0; i < ln->nchain; i++) cJSON_AddItemToArray(ch, cJSON_CreateNumber(ln->chain[i]));
        // patterns: only the ones with notes, as [slot, [[step, note, chord, vel, len] ...]]
        cJSON *pats = cJSON_AddArrayToObject(j, "patterns");
        for (int p = 0; p < S1_SLOTS; p++) {
            if (s1_pat_empty(&s->pat[l][p])) continue;
            cJSON *pj = cJSON_CreateObject();
            cJSON_AddNumberToObject(pj, "slot", p);
            cJSON *steps = cJSON_AddArrayToObject(pj, "steps");
            for (int i = 0; i < S1_STEPS; i++) {
                const s1_step_t *st = &s->pat[l][p].st[i];
                if (!st->on) continue;
                int v[5] = {i, st->note, st->chord, st->vel, st->len};
                cJSON *a = cJSON_CreateArray();
                for (int k = 0; k < 5; k++) cJSON_AddItemToArray(a, cJSON_CreateNumber(v[k]));
                cJSON_AddItemToArray(steps, a);
            }
            cJSON_AddItemToArray(pats, pj);
        }
        cJSON_AddItemToArray(lanes, j);
    }
    return o;
}

void s1_from_json(const cJSON *o, s1_state_t *s) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "cv_5v");
    if (cJSON_IsBool(v)) s->cv_5v = cJSON_IsTrue(v);
    s->scale_root = (uint8_t)num(o, "scale_root", s->scale_root);
    s->scale = (uint8_t)num(o, "scale", s->scale);
    const cJSON *lanes = cJSON_GetObjectItemCaseSensitive(o, "lanes");
    for (int l = 0; l < S1_LANES && cJSON_IsArray(lanes); l++) {
        const cJSON *j = cJSON_GetArrayItem(lanes, l);
        if (!cJSON_IsObject(j)) continue;
        s1_lane_t *ln = &s->lane[l];
        v = cJSON_GetObjectItemCaseSensitive(j, "name");
        if (cJSON_IsString(v)) snprintf(ln->name, sizeof ln->name, "%s", v->valuestring);
        ln->length = (uint8_t)num(j, "length", ln->length);
        v = cJSON_GetObjectItemCaseSensitive(j, "arp");
        if (cJSON_IsBool(v)) ln->arp = cJSON_IsTrue(v);
        ln->arp_mode = (uint8_t)num(j, "arp_mode", ln->arp_mode);
        ln->arp_rate = (uint8_t)num(j, "arp_rate", ln->arp_rate);
        ln->midi_ch = (int8_t)num(j, "midi_ch", ln->midi_ch);
        ln->cv = (uint8_t)num(j, "cv", ln->cv);
        v = cJSON_GetObjectItemCaseSensitive(j, "mute");
        if (cJSON_IsBool(v)) ln->mute = cJSON_IsTrue(v);
        ln->slot = (uint8_t)num(j, "slot", ln->slot);
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(j, "chain");
        if (cJSON_IsArray(ch)) {
            ln->nchain = 0;
            for (int i = 0; i < cJSON_GetArraySize(ch) && ln->nchain < S1_CHAIN; i++) {
                const cJSON *x = cJSON_GetArrayItem(ch, i);
                if (cJSON_IsNumber(x)) ln->chain[ln->nchain++] = (uint8_t)x->valueint;
            }
        }
        const cJSON *pats = cJSON_GetObjectItemCaseSensitive(j, "patterns");
        if (!cJSON_IsArray(pats)) continue;
        memset(s->pat[l], 0, sizeof s->pat[l]);   // the file says which patterns have notes
        for (int k = 0; k < cJSON_GetArraySize(pats); k++) {
            const cJSON *pj = cJSON_GetArrayItem(pats, k);
            int p = num(pj, "slot", -1);
            const cJSON *steps = cJSON_GetObjectItemCaseSensitive(pj, "steps");
            if (p < 0 || p >= S1_SLOTS || !cJSON_IsArray(steps)) continue;
            for (int i = 0; i < cJSON_GetArraySize(steps); i++) {
                const cJSON *a = cJSON_GetArrayItem(steps, i);
                if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 5) continue;
                int st = cJSON_GetArrayItem(a, 0)->valueint;
                if (st < 0 || st >= S1_STEPS) continue;
                s1_step_t *x = &s->pat[l][p].st[st];
                x->on = 1;
                x->note = (uint8_t)cJSON_GetArrayItem(a, 1)->valueint;
                x->chord = (uint8_t)cJSON_GetArrayItem(a, 2)->valueint;
                x->vel = (uint8_t)cJSON_GetArrayItem(a, 3)->valueint;
                x->len = (uint8_t)cJSON_GetArrayItem(a, 4)->valueint;
            }
        }
    }
    s1_sanitise(s);
}
