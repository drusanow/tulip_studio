// jn_mod.c - JUNO-6 as a Studio module: notes in (MIDI IN on its channel,
// or a routed sequencer track), audio out, its panel and the patch browser.
//
// The panel is laid out like the instrument's, section by section: DCO,
// HPF, VCF, VCA, CHORUS on top; LFO, ENV, the extras and the patch below;
// two octaves of keys at the bottom for trying sounds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "core/jn_dsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mod_juno.h"
#include "studio/widgets.h"

#define ACC    0xdb6dff   // the faders
#define SWC    0xff49aa   // the switches
#define TAB    0x242455   // section tabs
#define TN_BEND 10        // a queued pitch-wheel move (cents = -8192..8191)

static jn_engine_t s_eng;
static QueueHandle_t s_q;
static jn_patch_t s_patch;            // the UI's copy
static volatile uint32_t s_ver;
static uint32_t s_seen;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_midi_ch = 1;             // 0 omni, 1..16, -1 off

// patches: the 128 factory ones, then yours (/files/juno/presets.json)
#define JN_USER_MAX 64
#define JN_NAME_MAX 20
typedef struct {
    char name[JN_NAME_MAX + 1];
    jn_patch_t p;
} jn_user_t;
static jn_user_t *s_user;
static int s_nuser;
static int s_preset;                  // index into factory + yours
static char s_name[JN_NAME_MAX + 1] = "A11 Brass Set 1";
static bool s_dirty;
static int64_t s_dirty_at;

// --- notes ------------------------------------------------------------------------------------

static void note_in(const tnote_t *n) {
    if (s_q) xQueueSend(s_q, n, 0);
}

static void midi_in(const uint8_t *m, int len, int64_t t) {
    int ch = (m[0] & 15) + 1;
    if (s_midi_ch < 0 || (s_midi_ch && s_midi_ch != ch) || len < 3) return;
    uint8_t st = m[0] & 0xF0;
    tnote_t n = {.src = (uint8_t)(0x80 | (ch - 1)), .key = m[1], .cents = m[1] * 100, .t = t};
    if (st == 0x90 && m[2]) {
        n.type = TN_ON;
        n.vel = m[2];
    } else if (st == 0x80 || st == 0x90) {
        n.type = TN_OFF;
    } else if (st == 0xB0 && (m[1] == 123 || m[1] == 120)) {
        n.type = TN_ALL_OFF;
    } else if (st == 0xE0) {
        n.type = TN_BEND;
        n.cents = ((m[2] << 7) | m[1]) - 8192;
    } else {
        return;
    }
    note_in(&n);
}

// --- audio -------------------------------------------------------------------------------------

static void render(float *l, float *r, const studio_block_t *b) {
    if (s_ver != s_seen) {
        taskENTER_CRITICAL(&s_mux);
        s_seen = s_ver;
        s_eng.p = s_patch;
        taskEXIT_CRITICAL(&s_mux);
    }
    tnote_t n;
    while (xQueueReceive(s_q, &n, 0) == pdTRUE) {
        if (n.type == TN_ON)
            jn_note_on(&s_eng, n.src, n.key, n.cents / 100.0f, n.vel / 127.0f, n.glide_us);
        else if (n.type == TN_OFF)
            jn_note_off(&s_eng, n.src, n.key);
        else if (n.type == TN_BEND)
            s_eng.bend = n.cents / 8192.0f;
        else
            jn_all_off(&s_eng, n.src == 0xFF ? -1 : n.src);
    }
    jn_render(&s_eng, l, r, b->n);
}

static void enable(bool on, int64_t now) {
    tnote_t off = {.type = TN_ALL_OFF, .src = 0xFF};
    if (!on) note_in(&off);
}

// --- files -------------------------------------------------------------------------------------

static bool jn_dir(char *out, int len) {
    const char *root = studio_files_root();
    if (!root) return false;
    snprintf(out, len, "%s/juno", root);
    mkdir(out, 0777);
    return true;
}

static void jn_path(char *out, int len, const char *file) {
    const char *root = studio_files_root();
    snprintf(out, len, "%s/juno/%s", root ? root : "/nonexistent", file);
}

static cJSON *patch_json(const jn_patch_t *pt) {
    cJSON *p = cJSON_CreateObject();
    for (int i = 0; i < JN_NPARAM; i++)
        cJSON_AddNumberToObject(p, JN_PARAMS[i].key, (int)(pt->v[i] * 10000.0f + 0.5f) / 10000.0);
    return p;
}

static void patch_from(const cJSON *p, jn_patch_t *pt) {
    for (int i = 0; i < JN_NPARAM; i++) {
        const cJSON *x = cJSON_GetObjectItem(p, JN_PARAMS[i].key);
        if (cJSON_IsNumber(x)) pt->v[i] = x->valuedouble < 0 ? 0 : (x->valuedouble > 1 ? 1 : (float)x->valuedouble);
    }
}

static cJSON *read_json(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 && n < 256 * 1024 ? malloc((size_t)n + 1) : NULL;
    cJSON *o = NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        buf[n] = 0;
        o = cJSON_Parse(buf);
    }
    free(buf);
    fclose(f);
    return o;
}

static bool write_json(const char *path, const cJSON *o) {
    char *txt = cJSON_PrintUnformatted(o);
    FILE *f = txt ? fopen(path, "w") : NULL;
    bool ok = f && fputs(txt, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    cJSON_free(txt);
    return ok;
}

static void save_now(void) {
    char path[64];
    if (!jn_dir(path, sizeof path)) return;
    jn_path(path, sizeof path, "juno.json");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddStringToObject(o, "name", s_name);
    cJSON_AddItemToObject(o, "patch", patch_json(&s_patch));
    write_json(path, o);
    cJSON_Delete(o);
}

static void user_load(void) {
    if (!s_user) s_user = calloc(JN_USER_MAX, sizeof *s_user);
    s_nuser = 0;
    if (!s_user) return;
    char path[64];
    jn_path(path, sizeof path, "presets.json");
    cJSON *o = read_json(path);
    const cJSON *e;
    cJSON_ArrayForEach(e, o) {
        const cJSON *n = cJSON_GetObjectItem(e, "name");
        if (!cJSON_IsString(n) || s_nuser >= JN_USER_MAX) continue;
        jn_user_t *u = &s_user[s_nuser++];
        snprintf(u->name, sizeof u->name, "%s", n->valuestring);
        jn_patch_default(&u->p);
        patch_from(cJSON_GetObjectItem(e, "patch"), &u->p);
    }
    cJSON_Delete(o);
}

static bool user_write(void) {
    char path[64];
    if (!jn_dir(path, sizeof path)) return false;
    jn_path(path, sizeof path, "presets.json");
    cJSON *o = cJSON_CreateArray();
    for (int i = 0; i < s_nuser; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", s_user[i].name);
        cJSON_AddItemToObject(e, "patch", patch_json(&s_user[i].p));
        cJSON_AddItemToArray(o, e);
    }
    bool ok = write_json(path, o);
    cJSON_Delete(o);
    return ok;
}

static int npresets(void) { return JN_NFACTORY + s_nuser; }
static bool is_user(int i) { return i >= JN_NFACTORY && i < npresets(); }
static const char *preset_name(int i) { return i < JN_NFACTORY ? JN_FACTORY[i].name : s_user[i - JN_NFACTORY].name; }
static int find_preset(const char *name) {
    for (int i = npresets() - 1; i >= 0; i--)
        if (!strcmp(preset_name(i), name)) return i;
    return -1;
}

static void load(void) {
    jn_factory(&s_patch, 0);
    user_load();
    char path[64];
    jn_path(path, sizeof path, "juno.json");
    cJSON *o = read_json(path);
    if (!o) return;
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "name");
    if (cJSON_IsString(v)) snprintf(s_name, sizeof s_name, "%s", v->valuestring);
    int i = find_preset(s_name);
    s_preset = i < 0 ? 0 : i;
    patch_from(cJSON_GetObjectItem(o, "patch"), &s_patch);
    cJSON_Delete(o);
}

static void changed(void) {
    taskENTER_CRITICAL(&s_mux);
    s_ver++;
    taskEXIT_CRITICAL(&s_mux);
    s_dirty = true;
    s_dirty_at = studio_now_us();
}

// --- UI ---------------------------------------------------------------------------------------

// switches: (param, position) per button
typedef struct {
    uint8_t param, pos;
} jn_sw_t;

#define MAX_SW 24
static struct {
    sw_fader_t *fd[JN_NPARAM];
    lv_obj_t *sw[MAX_SW];
    jn_sw_t swd[MAX_SW];
    int nsw;
    lv_obj_t *name_lbl, *code_lbl, *ch_lbl, *del_b, *msg, *voice[JN_VOICES];
    lv_obj_t *keys[25];
    lv_obj_t *grid, *gbtn[64], *gtab[3];
    int gbank;                        // 0 A, 1 B, 2 yours
    bool del_armed, built;
    uint8_t voices_lit;
} U;

static float get_p(void *ud) { return s_patch.v[(intptr_t)ud]; }
static void set_p(void *ud, float v) {
    s_patch.v[(intptr_t)ud] = v;
    changed();
}
static void text_p(void *ud, char *b, int n) { jn_param_text(&s_patch, (int)(intptr_t)ud, b, n); }
static const sw_slider_ops_t OPS = {get_p, set_p, text_p};

static void say(const char *t) {
    if (U.msg) lv_label_set_text(U.msg, t);
}

static void refresh_switches(void) {
    for (int i = 0; i < U.nsw; i++) {
        jn_sw_t d = U.swd[i];
        int st = jn_step(&s_patch, d.param);
        bool on;
        if (d.param == JP_CHORUS)   // I and II: both on is I+II
            on = d.pos == 1 ? (st == 1 || st == 3) : (st == 2 || st == 3);
        else
            on = st == d.pos;
        sw_btn_set(U.sw[i], on);
    }
}

static void split_name(const char *nm, char *code, int cn, const char **rest) {
    // factory names start with their bank and number: "A11 Brass Set 1"
    const char *sp = strchr(nm, ' ');
    if (sp && sp - nm == 3 && (nm[0] == 'A' || nm[0] == 'B')) {
        snprintf(code, cn, "%.3s", nm);
        *rest = sp + 1;
    } else {
        snprintf(code, cn, "%s", is_user(s_preset) ? "YOURS" : "");
        *rest = nm;
    }
}

static void refresh_grid(void);

static void refresh_all(void) {
    for (int i = 0; i < JN_NPARAM; i++)
        if (U.fd[i]) sw_fader_refresh(U.fd[i]);
    refresh_switches();
    char code[8];
    const char *rest;
    split_name(s_name, code, sizeof code, &rest);
    lv_label_set_text(U.code_lbl, code);
    lv_label_set_text(U.name_lbl, rest);
    char t[16];
    if (s_midi_ch == 0)
        snprintf(t, sizeof t, "OMNI");
    else if (s_midi_ch < 0)
        snprintf(t, sizeof t, "OFF");
    else
        snprintf(t, sizeof t, "CH %d", s_midi_ch);
    lv_label_set_text(U.ch_lbl, t);
    if (U.grid && !lv_obj_has_flag(U.grid, LV_OBJ_FLAG_HIDDEN)) refresh_grid();
}

static void switch_cb(void *ud) {
    jn_sw_t d = U.swd[(intptr_t)ud];
    if (d.param == JP_CHORUS) {   // two latching buttons make the four settings
        int st = jn_step(&s_patch, JP_CHORUS);
        bool one = st == 1 || st == 3, two = st == 2 || st == 3;
        if (d.pos == 1) one = !one;
        else two = !two;
        jn_set_step(&s_patch, JP_CHORUS, (one ? 1 : 0) + (two ? 2 : 0));
    } else if (JN_PARAMS[d.param].steps == 2 && (d.param == JP_PULSE || d.param == JP_SAW)) {
        jn_set_step(&s_patch, d.param, !jn_step(&s_patch, d.param));   // each its own on / off
    } else {
        jn_set_step(&s_patch, d.param, d.pos);
    }
    changed();
    refresh_switches();
    if (U.fd[JP_HPF]) sw_fader_refresh(U.fd[JP_HPF]);
}

static void del_disarm(void) {
    U.del_armed = false;
    sw_btn_text(U.del_b, "DEL");
    sw_btn_set(U.del_b, false);
}

static void pick_preset(int i) {
    del_disarm();
    s_preset = i;
    if (is_user(i))
        s_patch = s_user[i - JN_NFACTORY].p;
    else
        jn_factory(&s_patch, i);
    snprintf(s_name, sizeof s_name, "%s", preset_name(i));
    changed();
    refresh_all();
    say(is_user(i) ? "yours" : "factory");
}

static void preset_cb(void *ud) { pick_preset((s_preset + (int)(intptr_t)ud + npresets()) % npresets()); }

static void saved_as(const char *name, void *ud) {
    char nm[JN_NAME_MAX + 1];
    snprintf(nm, sizeof nm, "%s", name);
    int i = -1;
    for (int k = 0; k < s_nuser; k++)
        if (!strcmp(s_user[k].name, nm)) i = k;
    if (i < 0) {
        if (!s_user || s_nuser >= JN_USER_MAX) return say("full: delete one first");
        i = s_nuser++;
    }
    snprintf(s_user[i].name, sizeof s_user[i].name, "%s", nm);
    s_user[i].p = s_patch;
    bool ok = user_write();
    snprintf(s_name, sizeof s_name, "%s", nm);
    s_preset = JN_NFACTORY + i;
    s_dirty = true;
    s_dirty_at = studio_now_us();
    refresh_all();
    say(ok ? "saved" : "couldn't write the file");
}

static void save_as_cb(void *ud) {
    del_disarm();
    sw_text_entry("SAVE JUNO PATCH", is_user(s_preset) ? s_name : "MY PATCH", JN_NAME_MAX, ACC, saved_as, NULL);
}

static void save_cb(void *ud) {
    del_disarm();
    if (is_user(s_preset))
        saved_as(s_user[s_preset - JN_NFACTORY].name, NULL);
    else
        save_as_cb(NULL);
}

static void del_cb(void *ud) {
    if (!is_user(s_preset)) return say("factory: can't delete");
    if (!U.del_armed) {
        U.del_armed = true;
        sw_btn_text(U.del_b, "SURE?");
        sw_btn_set(U.del_b, true);
        return;
    }
    del_disarm();
    int i = s_preset - JN_NFACTORY;
    memmove(&s_user[i], &s_user[i + 1], (size_t)(s_nuser - i - 1) * sizeof *s_user);
    s_nuser--;
    bool ok = user_write();
    s_preset = 0;
    refresh_all();
    say(ok ? "deleted" : "couldn't write the file");
}

static void ch_cb(void *ud) {
    int c = s_midi_ch + (int)(intptr_t)ud;
    s_midi_ch = c < -1 ? 16 : (c > 16 ? -1 : c);
    s_dirty = true;
    s_dirty_at = studio_now_us();
    refresh_all();
}

// --- the patch browser: 8 x 8, as the Juno-106's banks -----------------------------------------

static void refresh_grid(void) {
    for (int t = 0; t < 3; t++) sw_btn_set(U.gtab[t], t == U.gbank);
    for (int k = 0; k < 64; k++) {
        int i = U.gbank < 2 ? U.gbank * 64 + k : JN_NFACTORY + k;
        lv_obj_t *b = U.gbtn[k];
        if (i >= npresets()) {
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        const char *nm = preset_name(i);
        char t[JN_NAME_MAX + 4];
        if (U.gbank < 2)
            snprintf(t, sizeof t, "%.2s %s", nm + 1, nm + 4);   // "11 Brass Set 1"
        else
            snprintf(t, sizeof t, "%s", nm);
        sw_btn_text(b, t);
        sw_btn_set(b, i == s_preset);
    }
}

static void grid_pick_cb(void *ud) {
    int k = (int)(intptr_t)ud;
    pick_preset(U.gbank < 2 ? U.gbank * 64 + k : JN_NFACTORY + k);
}

static void grid_tab_cb(void *ud) {
    U.gbank = (int)(intptr_t)ud;
    refresh_grid();
}

static void grid_show_cb(void *ud) {
    bool show = ud != NULL;
    if (show) {
        U.gbank = s_preset < JN_NFACTORY ? s_preset / 64 : 2;
        refresh_grid();
        lv_obj_remove_flag(U.grid, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(U.grid, LV_OBJ_FLAG_HIDDEN);
    }
}

static void grid_create(lv_obj_t *s) {
    U.grid = sw_box(s, 0, 57, 1024, 430, SW_BG, 0);
    lv_obj_add_flag(U.grid, LV_OBJ_FLAG_CLICKABLE);   // nothing under it takes the touch
    static const char *const TABS[3] = {"BANK A", "BANK B", "YOURS"};
    for (int t = 0; t < 3; t++)
        U.gtab[t] = sw_btn(U.grid, TABS[t], 12 + t * 128, 6, 120, 34, ACC, grid_tab_cb, (void *)(intptr_t)t, NULL);
    sw_label(U.grid, "tap a patch to play it", SW_F_SMALL, SW_DIM, 410, 16, 0, false);
    sw_btn(U.grid, "CLOSE", 1024 - 12 - 120, 6, 120, 34, SWC, grid_show_cb, NULL, NULL);
    for (int k = 0; k < 64; k++) {
        int c = k % 8, rw = k / 8;
        U.gbtn[k] = sw_btn(U.grid, "", 12 + c * 125, 48 + rw * 47, 120, 42, ACC, grid_pick_cb, (void *)(intptr_t)k,
                           SW_F_SMALL);
        lv_obj_t *lb = sw_btn_label(U.gbtn[k]);
        lv_obj_set_width(lb, 112);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    }
    lv_obj_add_flag(U.grid, LV_OBJ_FLAG_HIDDEN);
}

// --- the panel ------------------------------------------------------------------------------------

#define FW 52            // a fader's column
#define BW 60            // a column of switches
#define R1_TAB 64
#define R1_Y 90
#define R1_H 196
#define R2_TAB 296
#define R2_Y 322
#define R2_H 156

static void tab(lv_obj_t *s, const char *title, int x, int y, int w) {
    lv_obj_t *b = sw_box(s, x, y, w, 20, TAB, 4);
    lv_obj_t *l = sw_label(b, title, SW_F_SMALL, SW_DIM, 0, 3, w, true);
    (void)l;
}

static void fader(lv_obj_t *s, int p, int x, int y, int h) {
    U.fd[p] = sw_fader(s, JN_PARAMS[p].label, x, y, FW, h, ACC, &OPS, (void *)(intptr_t)p);
}

// a column of switch buttons under a heading
static void switches(lv_obj_t *s, const char *head, int x, int w, int n, const char *const *labels, const int *params,
                     const int *pos) {
    if (head) sw_label(s, head, SW_F_SMALL, SW_TEXT, x, R1_Y, w, true);
    int h = n >= 4 ? 34 : 36, gap = n >= 4 ? 6 : 10, y = R1_Y + 22;
    for (int i = 0; i < n && U.nsw < MAX_SW; i++) {
        int k = U.nsw++;
        U.swd[k] = (jn_sw_t){(uint8_t)params[i], (uint8_t)pos[i]};
        U.sw[k] = sw_btn(s, labels[i], x + 4, y + i * (h + gap), w - 8, h, SWC, switch_cb, (void *)(intptr_t)k, SW_F_SMALL);
    }
}

static void key_event(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    tnote_t n = {.src = 0xF0, .key = (uint8_t)(48 + k), .cents = (48 + k) * 100, .vel = 100};
    n.type = lv_event_get_code(e) == LV_EVENT_PRESSED ? TN_ON : TN_OFF;
    note_in(&n);
}

static void keys_create(lv_obj_t *s, int y, int kh) {
    static const bool black[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int nw = 0;
    for (int k = 0; k < 25; k++)
        if (!black[k % 12]) nw++;
    int ww = (1024 - 24) / nw;
    for (int pass = 0; pass < 2; pass++) {
        int wx = 12;
        for (int k = 0; k < 25; k++) {
            bool b = black[k % 12];
            if (pass == 0 && !b)
                U.keys[k] = sw_btn(s, k == 0 ? "C3" : k == 12 ? "C4" : k == 24 ? "C5" : "", wx, y, ww - 4, kh, ACC, NULL,
                                   NULL, SW_F_SMALL);
            if (pass == 1 && b) {
                U.keys[k] = sw_btn(s, "", wx - ww / 3, y, ww * 2 / 3 - 2, kh * 6 / 10, ACC, NULL, NULL, NULL);
                lv_obj_set_style_bg_color(U.keys[k], sw_c(0x242424), 0);
            }
            if (!b) wx += ww;
        }
    }
    for (int k = 0; k < 25; k++) {
        lv_obj_add_event_cb(U.keys[k], key_event, LV_EVENT_PRESSED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(U.keys[k], key_event, LV_EVENT_RELEASED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(U.keys[k], key_event, LV_EVENT_PRESS_LOST, (void *)(intptr_t)k);
    }
}

static void ui_create(lv_obj_t *s) {
    // header
    sw_label(s, "JUNO-6", SW_F_BIG, SW_TEXT, 12, 8, 0, false);
    sw_label(s, "POLYSYNTH", SW_F_SMALL, ACC, 128, 22, 0, false);
    sw_btn(s, "SAVE", 418, 8, 62, 40, ACC, save_cb, NULL, NULL);
    sw_btn(s, "SAVE AS", 484, 8, 76, 40, ACC, save_as_cb, NULL, NULL);
    U.del_b = sw_btn(s, "DEL", 564, 8, 52, 40, SW_RED, del_cb, NULL, NULL);
    sw_label(s, "MIDI IN", SW_F_SMALL, SW_DIM, 632, 22, 0, false);
    sw_btn(s, "-", 690, 8, 40, 40, ACC, ch_cb, (void *)-1, SW_F_MID);
    U.ch_lbl = sw_label(s, "", SW_F_MID, SW_TEXT, 732, 18, 76, true);
    sw_btn(s, "+", 810, 8, 40, 40, ACC, ch_cb, (void *)1, SW_F_MID);
    U.msg = sw_label(s, "", SW_F_SMALL, SW_DIM, 228, 22, 180, false);
    lv_label_set_long_mode(U.msg, LV_LABEL_LONG_CLIP);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);

    // row 1: DCO, HPF, VCF, VCA, CHORUS
    int x = 12;
    tab(s, "DCO", x, R1_TAB, 400);
    fader(s, JP_DCO_LFO, x, R1_Y, R1_H);
    fader(s, JP_DCO_PWM, x + FW, R1_Y, R1_H);
    {
        static const char *const L[] = {"LFO", "MAN"};
        static const int P[] = {JP_PWM_MAN, JP_PWM_MAN}, Q[] = {0, 1};
        switches(s, "PWM", x + 2 * FW, BW, 2, L, P, Q);
    }
    {
        static const char *const L[] = {"16'", "8'", "4'"};
        static const int P[] = {JP_RANGE, JP_RANGE, JP_RANGE}, Q[] = {0, 1, 2};
        switches(s, "RANGE", x + 2 * FW + BW, BW, 3, L, P, Q);
    }
    {
        static const char *const L[] = {"PULSE", "SAW"};
        static const int P[] = {JP_PULSE, JP_SAW}, Q[] = {1, 1};
        switches(s, "WAVE", x + 2 * FW + 2 * BW, BW, 2, L, P, Q);
    }
    fader(s, JP_DCO_SUB, x + 2 * FW + 3 * BW, R1_Y, R1_H);
    fader(s, JP_DCO_NOISE, x + 3 * FW + 3 * BW, R1_Y, R1_H);
    x = 426;
    tab(s, "HPF", x, R1_TAB, BW);
    {
        static const char *const L[] = {"3", "2", "1", "0"};
        static const int P[] = {JP_HPF, JP_HPF, JP_HPF, JP_HPF}, Q[] = {3, 2, 1, 0};
        switches(s, "FREQ", x, BW, 4, L, P, Q);
    }
    x = 500;
    tab(s, "VCF", x, R1_TAB, 320);
    fader(s, JP_VCF_FREQ, x, R1_Y, R1_H);
    fader(s, JP_VCF_RES, x + FW, R1_Y, R1_H);
    {
        static const char *const L[] = {"ENV +", "ENV -"};
        static const int P[] = {JP_VCF_NEG, JP_VCF_NEG}, Q[] = {0, 1};
        switches(s, "POL", x + 2 * FW, BW, 2, L, P, Q);
    }
    fader(s, JP_VCF_ENV, x + 2 * FW + BW, R1_Y, R1_H);
    fader(s, JP_VCF_LFO, x + 3 * FW + BW, R1_Y, R1_H);
    fader(s, JP_VCF_KBD, x + 4 * FW + BW, R1_Y, R1_H);
    x = 830;
    tab(s, "VCA", x, R1_TAB, BW + FW);
    {
        static const char *const L[] = {"ENV", "GATE"};
        static const int P[] = {JP_VCA_GATE, JP_VCA_GATE}, Q[] = {0, 1};
        switches(s, "MODE", x, BW, 2, L, P, Q);
    }
    fader(s, JP_VCA_LEVEL, x + BW, R1_Y, R1_H);
    x = 952;
    tab(s, "CHORUS", x, R1_TAB, 62);
    {
        static const char *const L[] = {"I", "II"};
        static const int P[] = {JP_CHORUS, JP_CHORUS}, Q[] = {1, 2};
        switches(s, "", x, 62, 2, L, P, Q);
    }

    // row 2: LFO, ENV, the extras, the patch
    x = 12;
    tab(s, "LFO", x, R2_TAB, 2 * FW);
    fader(s, JP_LFO_RATE, x, R2_Y, R2_H);
    fader(s, JP_LFO_DELAY, x + FW, R2_Y, R2_H);
    x = 130;
    tab(s, "ENV", x, R2_TAB, 4 * FW);
    fader(s, JP_ENV_A, x, R2_Y, R2_H);
    fader(s, JP_ENV_D, x + FW, R2_Y, R2_H);
    fader(s, JP_ENV_S, x + 2 * FW, R2_Y, R2_H);
    fader(s, JP_ENV_R, x + 3 * FW, R2_Y, R2_H);
    x = 352;
    tab(s, "PERFORM", x, R2_TAB, 3 * FW);
    fader(s, JP_PORTA, x, R2_Y, R2_H);
    fader(s, JP_VEL, x + FW, R2_Y, R2_H);
    fader(s, JP_BEND, x + 2 * FW, R2_Y, R2_H);
    x = 524;
    tab(s, "PATCH", x, R2_TAB, 1012 - x);
    U.code_lbl = sw_label(s, "", SW_F_LABEL, ACC, x, R2_Y + 4, 0, false);
    U.name_lbl = sw_label(s, "", SW_F_BIG, SW_TEXT, x, R2_Y + 24, 1012 - x, false);
    lv_label_set_long_mode(U.name_lbl, LV_LABEL_LONG_DOT);
    sw_btn(s, "<", x, R2_Y + 70, 60, 48, ACC, preset_cb, (void *)-1, SW_F_MID);
    sw_btn(s, ">", x + 66, R2_Y + 70, 60, 48, ACC, preset_cb, (void *)1, SW_F_MID);
    sw_btn(s, "ALL PATCHES", x + 136, R2_Y + 70, 150, 48, ACC, grid_show_cb, (void *)1, NULL);
    sw_label(s, "VOICES", SW_F_SMALL, SW_DIM, x + 306, R2_Y + 76, 0, false);
    for (int i = 0; i < JN_VOICES; i++) U.voice[i] = sw_box(s, x + 306 + i * 26, R2_Y + 98, 20, 12, 0x242424, 3);
    sw_label(s, "pitch wheel bends; sliders read 0-127 as on the Juno", SW_F_SMALL, SW_DIM, x, R2_Y + 132, 0, false);

    sw_line(s, 0, 488, 1024, 1);
    keys_create(s, 496, 96);
    grid_create(s);
    U.built = true;
    refresh_all();
}

static void ui_poll(bool visible) {
    if (s_dirty && studio_now_us() - s_dirty_at > 1500000) {
        s_dirty = false;
        save_now();
    }
    if (!visible || !U.built) return;
    uint8_t lit = 0;   // which voices sound (single-byte reads of the engine's state)
    for (int i = 0; i < JN_VOICES; i++)
        if (s_eng.v[i].active) lit |= (uint8_t)(1u << i);
    if (lit != U.voices_lit) {
        for (int i = 0; i < JN_VOICES; i++)
            if (((lit ^ U.voices_lit) >> i) & 1)
                lv_obj_set_style_bg_color(U.voice[i], sw_c((lit >> i) & 1 ? ACC : 0x242424), 0);
        U.voices_lit = lit;
    }
}

static void init(void) {
    s_q = xQueueCreate(64, sizeof(tnote_t));
    jn_init(&s_eng);
    load();
    s_eng.p = s_patch;
    s_seen = s_ver;
}

static void midi_desc(char *b, int n) {
    if (s_midi_ch < 0)
        snprintf(b, n, "off (routed tracks only)");
    else if (s_midi_ch == 0)
        snprintf(b, n, "notes on every channel");
    else
        snprintf(b, n, "notes on ch %d", s_midi_ch);
}

static cJSON *patch_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s_name);
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddItemToObject(o, "patch", patch_json(&s_patch));
    return o;
}

static void patch_load(const cJSON *o) {
    jn_factory(&s_patch, 0);
    patch_from(cJSON_GetObjectItem(o, "patch"), &s_patch);
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "name");
    if (cJSON_IsString(v)) snprintf(s_name, sizeof s_name, "%s", v->valuestring);
    int i = find_preset(s_name);
    s_preset = i < 0 ? 0 : i;
    changed();
    if (U.built) refresh_all();
}

static bool busy(void) {
    if (s_dirty) {
        s_dirty = false;
        save_now();
    }
    return false;
}

tmod_t mod_juno = {
    .name = "juno",
    .title = "JUNO-6",
    .blurb = "Juno-6 / 106 style polysynth: DCO, 24 dB VCF, chorus, 128 factory patches",
    .role = TMOD_SYNTH,
    .flags = TMOD_F_AUDIO | TMOD_F_NOTES_IN,
    .init = init,
    .rt_enable = enable,
    .render = render,
    .note = note_in,
    .midi = midi_in,
    .midi_desc = midi_desc,
    .ui_create = ui_create,
    .ui_poll = ui_poll,
    .busy = busy,
    .patch_save = patch_save,
    .patch_load = patch_load,
};

TULIP_APP(mod_juno)
