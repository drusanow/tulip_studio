// s1_mod.c - SEQ101 as a Studio module: plays to the master clock's
// timeline, sends each lane to MIDI OUT, the CV DACs and the Studio's
// routing, keeps its state in /files/seq101/.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "core/s1_seq.h"
#include "esp_log.h"
#include "mod_seq101.h"
#include "s1_app.h"
#include "ui/s1_ui.h"

static const char *TAG = "seq101";

#define TICK_U (TC_U16 / S1_TICKS)   // timeline units in a tick
#define CV_HIGH_V 5.0f

s1_state_t g_s1;
static s1_seq_t s_q;
static uint32_t s_last_tick, s_epoch;
static bool s_running;
static int8_t s_cv_note[2] = {-1, -1};          // the note each DAC's gate holds
static int8_t s_route_note[S1_LANES] = {-1, -1, -1, -1};

int s1_play_step(int lane) { return s_running ? s1_seq_step(&s_q, lane) : -1; }
void s1_queue(int lane, int slot) { s_q.lane[lane].queued = (int8_t)slot; }
int s1_queued(int lane) { return s_q.lane[lane].queued; }

void s1_activate(int lane, int slot) {
    s_q.lane[lane].queued = -1;
    g_s1.lane[lane].slot = (uint8_t)slot;
    s1_changed();
}

void s1_cv_status(char *out, int n) {
    const tmod_t *o = studio_cv_owner();
    if (o && o != &mod_seq101) {
        snprintf(out, n, "%s has the CV DACs (switch it off to use them here)", o->title);
        return;
    }
    snprintf(out, n, "CV A (0x59) %s   CV B (0x58) %s", studio_cv_present(0) ? "ready" : "-",
             studio_cv_present(1) ? "ready" : "-");
}

// --- the outputs (rt task) -------------------------------------------------------------------

// 1 V/oct, 0 V = C0; above the range a note drops by octaves (stays in key)
static float note_volts(int note) {
    float v = (note - 12) / 12.0f, max = g_s1.cv_5v ? 5.0f : 10.0f;
    if (v < 0) v = 0;
    while (v > max + 0.0001f) v -= 1.0f;
    return v;
}

static void emit(const s1_ev_t *e, int64_t t) {
    const s1_lane_t *ln = &g_s1.lane[e->lane];
    bool on = e->type == S1_EV_ON;
    if (ln->midi_ch > 0) {
        uint8_t m[3] = {(uint8_t)((on ? 0x90 : 0x80) | (ln->midi_ch - 1)), e->note, on ? e->vel : 0};
        studio_midi_out_at(t, m, 3);
    }
    // the Studio's routing and the CV take one note a lane: the chord's lowest
    // (or the arp's note)
    if (on && e->note == e->first) {
        tnote_t n = {.type = TN_ON, .vel = e->vel, .cents = e->note * 100, .t = t};
        studio_note_out(&mod_seq101, e->lane, &n);
        s_route_note[e->lane] = (int8_t)e->note;
        if (ln->cv) {
            int dac = ln->cv - 1;
            studio_cv_write(&mod_seq101, dac, 1, note_volts(e->note));   // pitch, then the gate
            studio_cv_write(&mod_seq101, dac, 0, CV_HIGH_V);
            s_cv_note[dac] = (int8_t)e->note;
        }
    } else if (!on) {
        if (s_route_note[e->lane] == e->note) {
            tnote_t n = {.type = TN_OFF, .t = t};
            studio_note_out(&mod_seq101, e->lane, &n);
            s_route_note[e->lane] = -1;
        }
        if (ln->cv && s_cv_note[ln->cv - 1] == e->note) {
            studio_cv_write(&mod_seq101, ln->cv - 1, 0, 0.0f);
            s_cv_note[ln->cv - 1] = -1;
        }
    }
}

static void all_off(int64_t t) {
    s1_ev_t ev[S1_LANES * S1_MAX_NOTES];
    int n = s1_seq_all_off(&s_q, ev, S1_LANES * S1_MAX_NOTES);
    for (int i = 0; i < n; i++) emit(&ev[i], t);
    for (int d = 0; d < 2; d++)
        if (s_cv_note[d] >= 0) {
            studio_cv_write(&mod_seq101, d, 0, 0.0f);
            s_cv_note[d] = -1;
        }
}

// --- the rt hooks: ticks off the timeline ------------------------------------------------

static int64_t m_next_due(void) {
    tclock_tl_t tl;
    studio_timeline(&tl);
    if (!tl.running) return s_running ? studio_now_us() : INT64_MAX;   // stopped: let go now
    if (!s_running || tl.epoch != s_epoch) return studio_now_us();
    return tclock_tl_time(&tl, (int64_t)(s_last_tick + 1) * TICK_U);
}

static void m_advance(int64_t now) {
    tclock_tl_t tl;
    studio_timeline(&tl);
    if (s_running && (!tl.running || tl.epoch != s_epoch)) {   // stop, locate, continue
        all_off(now);
        s_running = false;
    }
    s_epoch = tl.epoch;
    if (!tl.running) return;
    int64_t pos = tclock_tl_pos(&tl, now + TC_LOOKAHEAD_US);
    if (pos < 0) return;
    uint32_t cur = (uint32_t)(pos / TICK_U);
    if (!s_running) {
        s_running = true;
        s1_seq_start(&s_q, &g_s1, cur);
        s_last_tick = cur - 1;
    }
    if (cur - s_last_tick > 64) s_last_tick = cur - 1;   // a long stall: don't burst
    s1_ev_t ev[32];
    for (uint32_t t = s_last_tick + 1; (int32_t)(t - cur) <= 0; t++) {
        int n = s1_seq_tick(&s_q, &g_s1, t, ev, 32);
        int64_t at = tclock_tl_time(&tl, (int64_t)t * TICK_U);
        for (int i = 0; i < n; i++) emit(&ev[i], at);
    }
    s_last_tick = cur;
}

// switched off in the Studio: everything off, and start in step when back
static void m_enable(bool on, int64_t now) {
    if (on) return;
    all_off(now);
    s_running = false;
}

// --- the state file ---------------------------------------------------------------------------

static volatile bool s_dirty;
static int64_t s_dirty_at;

void s1_changed(void) {
    s_dirty = true;
    s_dirty_at = studio_now_us();
}

static void path(char *out, int n, const char *file) {
    const char *root = studio_files_root();
    snprintf(out, n, "%s/seq101/%s", root ? root : "/nonexistent", file);
}

// the state as JSON to p, through a temporary file (a power cut never leaves half a file)
static bool write_state(const char *p) {
    char tmp[112];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    cJSON *o = s1_to_json(&g_s1);
    char *txt = cJSON_Print(o);
    cJSON_Delete(o);
    if (!txt) return false;
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fputs(txt, f) >= 0;
    if (f && fclose(f) != 0) ok = false;
    cJSON_free(txt);
    if (ok) {
        unlink(p);
        ok = rename(tmp, p) == 0;
    }
    if (!ok) ESP_LOGW(TAG, "couldn't write %s", p);
    return ok;
}

// p into the state (keys it lacks keep their values); false if unreadable
static bool read_state(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *txt = n > 0 && n < 256 * 1024 ? malloc((size_t)n + 1) : NULL;
    bool ok = false;
    if (txt && fread(txt, 1, (size_t)n, f) == (size_t)n) {
        txt[n] = 0;
        cJSON *o = cJSON_Parse(txt);
        if (o) {
            s1_from_json(o, &g_s1);
            ok = true;
        }
        cJSON_Delete(o);
    }
    free(txt);
    fclose(f);
    return ok;
}

static void save_now(void) {
    s_dirty = false;
    if (!studio_files_root()) return;
    char p[96];
    path(p, sizeof p, "");
    mkdir(p, 0775);
    path(p, sizeof p, "state.json");
    write_state(p);
}

static void load(void) {
    char p[96];
    path(p, sizeof p, "state.json");
    read_state(p);
}

// --- projects: /files/seq101/projects/<name>.json, the whole state -----------------------

static void proj_path(char *out, int n, const char *name) {
    const char *root = studio_files_root();
    snprintf(out, n, "%s/seq101/projects/%s.json", root ? root : "/nonexistent", name);
}

static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

int s1_proj_list(char (*names)[S1_PROJ_NAME + 1], int max) {
    char p[96];
    path(p, sizeof p, "projects");
    DIR *d = opendir(p);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l - 5 > S1_PROJ_NAME || strcmp(e->d_name + l - 5, ".json")) continue;
        snprintf(names[n], S1_PROJ_NAME + 1, "%.*s", (int)(l - 5), e->d_name);
        n++;
    }
    closedir(d);
    qsort(names, n, sizeof names[0], cmp_name);
    return n;
}

bool s1_proj_save(const char *name) {
    if (!studio_files_root() || !name[0]) return false;
    char p[112];
    path(p, sizeof p, "");
    mkdir(p, 0775);
    path(p, sizeof p, "projects");
    mkdir(p, 0775);
    proj_path(p, sizeof p, name);
    return write_state(p);
}

bool s1_proj_load(const char *name) {
    char p[112];
    proj_path(p, sizeof p, name);
    static s1_state_t keep;
    keep = g_s1;
    if (!read_state(p)) {
        g_s1 = keep;
        return false;
    }
    s1_changed();
    return true;
}

bool s1_proj_delete(const char *name) {
    char p[112];
    proj_path(p, sizeof p, name);
    return unlink(p) == 0;
}

// --- module ------------------------------------------------------------------------------------

static void init(void) {
    s1_defaults(&g_s1);
    load();
    s1_seq_init(&s_q);
}

static void ui_poll(bool visible) {
    if (s_dirty && studio_now_us() - s_dirty_at > 1500000) save_now();
    s1_ui_poll(visible);
}

static bool busy(void) {
    if (s_dirty) save_now();
    return false;
}

static cJSON *patch_save(void) { return s1_to_json(&g_s1); }

static void patch_load(const cJSON *o) {
    s1_from_json(o, &g_s1);
    s1_changed();
    s1_ui_reload();
}

static const char *const OUTS[S1_LANES] = {"LANE A", "LANE B", "LANE C", "LANE D"};

tmod_t mod_seq101 = {
    .name = "seq101",
    .title = "SEQ101",
    .blurb = "4-lane, 16-step note sequencer: chords, lengths, arpeggiator, pattern chains",
    .role = TMOD_SEQUENCER,
    .flags = TMOD_F_CV | TMOD_F_CLOCKED,
    .n_note_outs = S1_LANES,
    .note_out_names = OUTS,
    .init = init,
    .rt_next_due = m_next_due,
    .rt_advance = m_advance,
    .rt_enable = m_enable,
    .ui_create = s1_ui_create,
    .ui_poll = ui_poll,
    .ui_show = s1_ui_show,
    .busy = busy,
    .patch_save = patch_save,
    .patch_load = patch_load,
};
