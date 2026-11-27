// dx_mod.c - DX7 as a Studio module: notes in (MIDI IN on its channel, or a
// routed sequencer track), audio out, the voice browser and an edit panel
// for the things worth reaching for: the algorithm, feedback, the LFO and
// each operator's level and frequency.
//
// Voices: the 128 the Tulip has, yours (/files/dx7/voices.json), and any
// DX7 bank or single-voice .syx put in /files/dx7/.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "core/dx_dsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mod_dx7.h"
#include "studio/widgets.h"

#define ACC  0x49dbaa   // faders, carriers
#define SWC  0x24b6ff   // buttons
#define TAB  0x242455
#define MODC 0x494955   // a modulator's tab
#define TN_BEND 10

static dx_engine_t s_eng;
static QueueHandle_t s_q;
static dx_voice_t s_voice;            // the UI's copy: the sound
static volatile uint8_t s_mute;       // operators switched off (bit per op)
static volatile uint32_t s_ver;
static uint32_t s_seen;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_midi_ch = 1;

#define DX_USER_MAX 32
typedef struct {
    dx_voice_t v;
} dx_user_t;
static dx_user_t *s_user;
static int s_nuser;
static int s_preset;                  // factory 0..127, yours after; -1 from a file / edited
static bool s_dirty;
static int64_t s_dirty_at;

// .syx files in /files/dx7/
#define DX_FILES_MAX 32
#define DX_FNAME 40
static char (*s_files)[DX_FNAME];
static int s_nfiles;
static dx_voice_t *s_fbank;           // the voices of the file opened
static int s_nfbank;
static char s_fname[DX_FNAME];

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
        s_eng.v = s_voice;
        uint8_t m = s_mute;
        taskEXIT_CRITICAL(&s_mux);
        for (int op = 0; op < 6; op++)
            if ((m >> op) & 1) s_eng.v.d[DX_OP(op) + DXO_LEVEL] = 0;
    }
    tnote_t n;
    while (xQueueReceive(s_q, &n, 0) == pdTRUE) {
        if (n.type == TN_ON)
            dx_note_on(&s_eng, n.src, n.key, n.cents / 100, n.vel / 127.0f);
        else if (n.type == TN_OFF)
            dx_note_off(&s_eng, n.src, n.key);
        else if (n.type == TN_BEND)
            s_eng.bend = n.cents / 8192.0f;
        else
            dx_all_off(&s_eng, n.src == 0xFF ? -1 : n.src);
    }
    dx_render(&s_eng, l, r, b->n);
}

static void enable(bool on, int64_t now) {
    tnote_t off = {.type = TN_ALL_OFF, .src = 0xFF};
    if (!on) note_in(&off);
}

// --- files -------------------------------------------------------------------------------------

static bool dx_dir(char *out, int len) {
    const char *root = studio_files_root();
    if (!root) return false;
    snprintf(out, len, "%s/dx7", root);
    mkdir(out, 0777);
    return true;
}

static void dx_path(char *out, int len, const char *file) {
    const char *root = studio_files_root();
    snprintf(out, len, "%s/dx7/%s", root ? root : "/nonexistent", file);
}

static cJSON *voice_json(const dx_voice_t *v) {
    int n[DX_VOICE_LEN];
    for (int i = 0; i < DX_VOICE_LEN; i++) n[i] = v->d[i];
    return cJSON_CreateIntArray(n, DX_VOICE_LEN);
}

static bool voice_from(const cJSON *a, dx_voice_t *v) {
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < DX_VOICE_LEN) return false;
    for (int i = 0; i < DX_VOICE_LEN; i++) {
        const cJSON *x = cJSON_GetArrayItem(a, i);
        v->d[i] = cJSON_IsNumber(x) && x->valueint >= 0 && x->valueint < 128 ? (uint8_t)x->valueint : 0;
    }
    dx_sanitise(v);
    return true;
}

static uint8_t *read_file(const char *path, long max, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 && n <= max ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = n;
    }
    return buf;
}

static cJSON *read_json(const char *path) {
    long n;
    char *buf = (char *)read_file(path, 256 * 1024, &n);
    cJSON *o = buf ? cJSON_Parse(buf) : NULL;
    free(buf);
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
    if (!dx_dir(path, sizeof path)) return;
    dx_path(path, sizeof path, "dx7.json");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddNumberToObject(o, "preset", s_preset);
    cJSON_AddNumberToObject(o, "mute", s_mute);
    cJSON_AddItemToObject(o, "voice", voice_json(&s_voice));
    write_json(path, o);
    cJSON_Delete(o);
}

static void user_load(void) {
    if (!s_user) s_user = calloc(DX_USER_MAX, sizeof *s_user);
    s_nuser = 0;
    if (!s_user) return;
    char path[64];
    dx_path(path, sizeof path, "voices.json");
    cJSON *o = read_json(path);
    const cJSON *e;
    cJSON_ArrayForEach(e, o) {
        if (s_nuser < DX_USER_MAX && voice_from(e, &s_user[s_nuser].v)) s_nuser++;
    }
    cJSON_Delete(o);
}

static bool user_write(void) {
    char path[64];
    if (!dx_dir(path, sizeof path)) return false;
    dx_path(path, sizeof path, "voices.json");
    cJSON *o = cJSON_CreateArray();
    for (int i = 0; i < s_nuser; i++) cJSON_AddItemToArray(o, voice_json(&s_user[i].v));
    bool ok = write_json(path, o);
    cJSON_Delete(o);
    return ok;
}

static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void files_scan(void) {
    if (!s_files) s_files = calloc(DX_FILES_MAX, DX_FNAME);
    s_nfiles = 0;
    char path[64];
    dx_path(path, sizeof path, "");
    DIR *d = s_files ? opendir(path) : NULL;
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_nfiles < DX_FILES_MAX) {
        size_t l = strlen(e->d_name);
        if (l < 5 || l >= DX_FNAME || strcasecmp(e->d_name + l - 4, ".syx")) continue;
        memcpy(s_files[s_nfiles++], e->d_name, l + 1);   // (l < DX_FNAME, checked)
    }
    closedir(d);
    qsort(s_files, (size_t)s_nfiles, DX_FNAME, cmp_name);
}

static bool file_open(const char *name) {
    if (!s_fbank) s_fbank = calloc(32, sizeof *s_fbank);
    if (!s_fbank) return false;
    char path[96];
    const char *root = studio_files_root();
    snprintf(path, sizeof path, "%s/dx7/%s", root ? root : "/nonexistent", name);
    long n = 0;
    uint8_t *buf = read_file(path, 16 * 1024, &n);
    s_nfbank = buf ? dx_parse_syx(buf, (int)n, s_fbank, 32) : 0;
    free(buf);
    snprintf(s_fname, sizeof s_fname, "%s", name);
    return s_nfbank > 0;
}

static int npresets(void) { return DX_NFACTORY + s_nuser; }
static bool is_user(int i) { return i >= DX_NFACTORY && i < npresets(); }

static void preset_voice(int i, dx_voice_t *v) {
    if (i < DX_NFACTORY) {
        memcpy(v->d, DX_FACTORY[i], DX_VOICE_LEN);
        v->d[DX_VOICE_LEN] = 0;
    } else {
        *v = s_user[i - DX_NFACTORY].v;
    }
}

static void load(void) {
    preset_voice(0, &s_voice);
    user_load();
    char path[64];
    dx_path(path, sizeof path, "dx7.json");
    cJSON *o = read_json(path);
    if (!o) return;
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "preset");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint < npresets()) s_preset = v->valueint;
    v = cJSON_GetObjectItem(o, "mute");
    if (cJSON_IsNumber(v)) s_mute = (uint8_t)(v->valueint & 63);
    voice_from(cJSON_GetObjectItem(o, "voice"), &s_voice);
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

// a fader on one byte of the voice: offset and its top value
typedef struct {
    uint8_t off, max;
    const char *label;
} dx_byte_t;

static const dx_byte_t FD[7] = {
    {DXV_FEEDBACK, 7, "FBACK"},     {DXV_LFO_SPEED, 99, "SPEED"}, {DXV_LFO_DELAY, 99, "DELAY"},
    {DXV_LFO_PMD, 99, "PITCH"},     {DXV_LFO_AMD, 99, "AMP"},     {DXV_PMS, 7, "P.SENS"},
    // per operator: the level (offset added per op)
    {DXO_LEVEL, 99, "LEVEL"},
};

static struct {
    sw_fader_t *fd[6 + 6];            // 6 globals, then OP1..OP6 levels
    lv_obj_t *name_lbl, *num_lbl, *ch_lbl, *del_b, *msg, *algo_lbl, *car_lbl, *wave_b, *trans_lbl;
    lv_obj_t *op_tab[6], *op_tab_lbl[6], *ratio_lbl[6], *mute_b[6], *voice[DX_VOICES];
    lv_obj_t *keys[25];
    lv_obj_t *grid, *gbtn[32], *gtab[7], *gtitle;
    int gmode;                        // 0..3 factory banks, 4 yours, 5 files, 6 a file's voices
    bool del_armed, built;
    uint8_t voices_lit;
} U;

// ud: low byte the FD index, next byte the operator (op faders)
static int fd_off(intptr_t ud) {
    int i = (int)(ud & 0xff), op = (int)(ud >> 8);
    return FD[i].off + (i == 6 ? DX_OP(op) : 0);
}
static float get_b(void *ud) { return (float)s_voice.d[fd_off((intptr_t)ud)] / FD[(intptr_t)ud & 0xff].max; }
static void set_b(void *ud, float v) {
    int max = FD[(intptr_t)ud & 0xff].max;
    uint8_t b = (uint8_t)(v * max + 0.5f);
    uint8_t *p = &s_voice.d[fd_off((intptr_t)ud)];
    if (*p == b) return;
    *p = b;
    changed();
}
static void text_b(void *ud, char *o, int n) { snprintf(o, n, "%d", s_voice.d[fd_off((intptr_t)ud)]); }
static const sw_slider_ops_t OPS = {get_b, set_b, text_b};

static void say(const char *t) {
    if (U.msg) lv_label_set_text(U.msg, t);
}

static void ratio_text(int op, char *o, int n) {
    const uint8_t *p = s_voice.d + DX_OP(op);
    if (p[DXO_MODE]) {
        static const int DEC[4] = {1, 10, 100, 1000};
        float hz = (float)DEC[p[DXO_COARSE] & 3];
        for (int i = 0; i < p[DXO_FINE]; i++) hz *= 1.023293f;   // 10^(1/100)
        snprintf(o, n, hz < 1000 ? "%.1f Hz" : "%.0f Hz", hz);
    } else {
        float r = (p[DXO_COARSE] ? (float)p[DXO_COARSE] : 0.5f) * (1.0f + p[DXO_FINE] / 100.0f);
        snprintf(o, n, "x %.2f", r);
    }
}

static void refresh_ops(void) {
    uint8_t car = dx_carriers(s_voice.d[DXV_ALGO]);
    for (int op = 0; op < 6; op++) {
        bool c = (car >> op) & 1;
        lv_obj_set_style_bg_color(U.op_tab[op], sw_c(c ? ACC : MODC), 0);
        lv_obj_set_style_text_color(U.op_tab_lbl[op], sw_c(c ? SW_BG : SW_TEXT), 0);
        char t[20];
        ratio_text(op, t, sizeof t);
        lv_label_set_text(U.ratio_lbl[op], t);
        sw_btn_set(U.mute_b[op], !((s_mute >> op) & 1));
        sw_fader_refresh(U.fd[6 + op]);
    }
    char t[40];
    snprintf(t, sizeof t, "%d", s_voice.d[DXV_ALGO] + 1);
    lv_label_set_text(U.algo_lbl, t);
    int k = snprintf(t, sizeof t, "heard: OP");
    for (int op = 0; op < 6 && k < (int)sizeof t - 3; op++)
        if ((car >> op) & 1) k += snprintf(t + k, sizeof t - k, " %d", op + 1);
    lv_label_set_text(U.car_lbl, t);
    static const char *const W[6] = {"TRI", "SAW DN", "SAW UP", "SQUARE", "SINE", "S + H"};
    sw_btn_text(U.wave_b, W[s_voice.d[DXV_LFO_WAVE] % 6]);
    snprintf(t, sizeof t, "%+d", s_voice.d[DXV_TRANSPOSE] - 24);
    lv_label_set_text(U.trans_lbl, t);
}

static void refresh_grid(void);

static void refresh_all(void) {
    for (int i = 0; i < 6; i++) sw_fader_refresh(U.fd[i]);
    refresh_ops();
    char nm[DX_NAME_LEN + 1];
    dx_name(&s_voice, nm);
    lv_label_set_text(U.name_lbl, nm[0] ? nm : "(no name)");
    char t[48];
    if (s_preset < 0)
        snprintf(t, sizeof t, "%s", s_fname[0] ? s_fname : "edited");
    else if (is_user(s_preset))
        snprintf(t, sizeof t, "YOURS %d", s_preset - DX_NFACTORY + 1);
    else
        snprintf(t, sizeof t, "BANK %d  VOICE %d", s_preset / 32 + 1, s_preset % 32 + 1);
    lv_label_set_text(U.num_lbl, t);
    if (s_midi_ch == 0)
        snprintf(t, sizeof t, "OMNI");
    else if (s_midi_ch < 0)
        snprintf(t, sizeof t, "OFF");
    else
        snprintf(t, sizeof t, "CH %d", s_midi_ch);
    lv_label_set_text(U.ch_lbl, t);
    if (U.grid && !lv_obj_has_flag(U.grid, LV_OBJ_FLAG_HIDDEN)) refresh_grid();
}

static void del_disarm(void) {
    U.del_armed = false;
    sw_btn_text(U.del_b, "DEL");
    sw_btn_set(U.del_b, false);
}

static void pick_preset(int i) {
    del_disarm();
    s_preset = i;
    preset_voice(i, &s_voice);
    s_fname[0] = 0;
    changed();
    refresh_all();
}

static void preset_cb(void *ud) {
    int i = s_preset < 0 ? 0 : s_preset;
    pick_preset((i + (int)(intptr_t)ud + npresets()) % npresets());
}

static void saved_as(const char *name, void *ud) {
    dx_set_name(&s_voice, name);
    char nm[DX_NAME_LEN + 1];
    dx_name(&s_voice, nm);
    int i = -1;
    for (int k = 0; k < s_nuser; k++) {
        char un[DX_NAME_LEN + 1];
        dx_name(&s_user[k].v, un);
        if (!strcmp(un, nm)) i = k;
    }
    if (i < 0) {
        if (!s_user || s_nuser >= DX_USER_MAX) return say("full: delete one first");
        i = s_nuser++;
    }
    s_user[i].v = s_voice;
    bool ok = user_write();
    s_preset = DX_NFACTORY + i;
    s_fname[0] = 0;
    changed();
    refresh_all();
    say(ok ? "saved" : "couldn't write the file");
}

static void save_as_cb(void *ud) {
    del_disarm();
    char nm[DX_NAME_LEN + 1];
    dx_name(&s_voice, nm);
    sw_text_entry("SAVE DX7 VOICE (10 LETTERS)", nm[0] ? nm : "MY VOICE", DX_NAME_LEN, ACC, saved_as, NULL);
}

static void save_cb(void *ud) {
    del_disarm();
    if (is_user(s_preset)) {
        char nm[DX_NAME_LEN + 1];
        dx_name(&s_voice, nm);
        saved_as(nm, NULL);
    } else {
        save_as_cb(NULL);
    }
}

static void del_cb(void *ud) {
    if (!is_user(s_preset)) return say("only yours can go");
    if (!U.del_armed) {
        U.del_armed = true;
        sw_btn_text(U.del_b, "SURE?");
        sw_btn_set(U.del_b, true);
        return;
    }
    del_disarm();
    int i = s_preset - DX_NFACTORY;
    memmove(&s_user[i], &s_user[i + 1], (size_t)(s_nuser - i - 1) * sizeof *s_user);
    s_nuser--;
    bool ok = user_write();
    s_preset = -1;
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

static void edited(void) {
    changed();
    refresh_ops();
}

static void algo_cb(void *ud) {
    s_voice.d[DXV_ALGO] = (uint8_t)((s_voice.d[DXV_ALGO] + (int)(intptr_t)ud + 32) % 32);
    edited();
}

static void wave_cb(void *ud) {
    s_voice.d[DXV_LFO_WAVE] = (uint8_t)((s_voice.d[DXV_LFO_WAVE] + 1) % 6);
    edited();
}

static void trans_cb(void *ud) {
    int t = s_voice.d[DXV_TRANSPOSE] + (int)(intptr_t)ud;
    s_voice.d[DXV_TRANSPOSE] = (uint8_t)(t < 0 ? 0 : t > 48 ? 48 : t);
    edited();
}

// ud: op << 8 | what (0 coarse -, 1 coarse +, 2 fine -, 3 fine +)
static void freq_cb(void *ud) {
    int op = (int)((intptr_t)ud >> 8), w = (int)((intptr_t)ud & 0xff);
    uint8_t *p = s_voice.d + DX_OP(op);
    if (w < 2) {
        int c = p[DXO_COARSE] + (w ? 1 : -1);
        p[DXO_COARSE] = (uint8_t)(c < 0 ? 0 : c > 31 ? 31 : c);
    } else {
        int f = p[DXO_FINE] + (w == 3 ? 1 : -1);
        p[DXO_FINE] = (uint8_t)(f < 0 ? 0 : f > 99 ? 99 : f);
    }
    edited();
}

static void mute_cb(void *ud) {
    int op = (int)(intptr_t)ud;
    s_mute ^= (uint8_t)(1u << op);
    edited();
}

// --- the voice browser ---------------------------------------------------------------------------

static void grid_btn(int k, const char *text, bool on) {
    lv_obj_remove_flag(U.gbtn[k], LV_OBJ_FLAG_HIDDEN);
    sw_btn_text(U.gbtn[k], text);
    sw_btn_set(U.gbtn[k], on);
}

static void refresh_grid(void) {
    for (int t = 0; t < 7; t++) sw_btn_set(U.gtab[t], t == U.gmode || (t == 5 && U.gmode == 6));
    char nm[DX_NAME_LEN + 1];
    const char *title = "tap a voice to play it";
    for (int k = 0; k < 32; k++) {
        lv_obj_add_flag(U.gbtn[k], LV_OBJ_FLAG_HIDDEN);
        if (U.gmode < 4) {
            int i = U.gmode * 32 + k;
            dx_voice_t v;
            preset_voice(i, &v);
            dx_name(&v, nm);
            grid_btn(k, nm, i == s_preset);
        } else if (U.gmode == 4 && k < s_nuser) {
            dx_name(&s_user[k].v, nm);
            grid_btn(k, nm, DX_NFACTORY + k == s_preset);
        } else if (U.gmode == 5 && k < s_nfiles) {
            grid_btn(k, s_files[k], false);
        } else if (U.gmode == 6 && k < s_nfbank) {
            dx_name(&s_fbank[k], nm);
            grid_btn(k, nm, false);
        }
    }
    if (U.gmode == 4 && !s_nuser) title = "none yet: SAVE AS keeps the sound here";
    if (U.gmode == 5) title = s_nfiles ? "a DX7 bank (.syx) in /files/dx7/" : "put DX7 .syx banks in /files/dx7/";
    if (U.gmode == 6) title = s_fname;
    lv_label_set_text(U.gtitle, title);
}

static void grid_pick_cb(void *ud) {
    int k = (int)(intptr_t)ud;
    if (U.gmode < 4) {
        pick_preset(U.gmode * 32 + k);
    } else if (U.gmode == 4) {
        if (k < s_nuser) pick_preset(DX_NFACTORY + k);
    } else if (U.gmode == 5) {
        if (k < s_nfiles && file_open(s_files[k])) U.gmode = 6;
        else say("not a DX7 bank");
        refresh_grid();
    } else if (k < s_nfbank) {
        del_disarm();
        s_voice = s_fbank[k];
        s_preset = -1;
        changed();
        refresh_all();
    }
}

static void grid_tab_cb(void *ud) {
    U.gmode = (int)(intptr_t)ud;
    if (U.gmode == 5) files_scan();
    refresh_grid();
}

static void grid_show_cb(void *ud) {
    if (ud) {
        U.gmode = s_preset < 0 ? (s_nfbank ? 6 : 0) : s_preset < DX_NFACTORY ? s_preset / 32 : 4;
        refresh_grid();
        lv_obj_remove_flag(U.grid, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(U.grid, LV_OBJ_FLAG_HIDDEN);
    }
}

static void grid_create(lv_obj_t *s) {
    U.grid = sw_box(s, 0, 57, 1024, 430, SW_BG, 0);
    lv_obj_add_flag(U.grid, LV_OBJ_FLAG_CLICKABLE);
    static const char *const TABS[7] = {"1-32", "33-64", "65-96", "97-128", "YOURS", "FILES", ""};
    for (int t = 0; t < 7; t++) {
        U.gtab[t] = sw_btn(U.grid, TABS[t], 12 + t * 92, 6, 86, 34, ACC, grid_tab_cb, (void *)(intptr_t)t, NULL);
        if (t == 6) lv_obj_add_flag(U.gtab[t], LV_OBJ_FLAG_HIDDEN);   // (a file's voices: under FILES)
    }
    U.gtitle = sw_label(U.grid, "", SW_F_SMALL, SW_DIM, 576, 16, 300, false);
    lv_label_set_long_mode(U.gtitle, LV_LABEL_LONG_DOT);
    sw_btn(U.grid, "CLOSE", 1024 - 12 - 120, 6, 120, 34, SWC, grid_show_cb, NULL, NULL);
    for (int k = 0; k < 32; k++) {
        int c = k % 8, rw = k / 8;
        U.gbtn[k] = sw_btn(U.grid, "", 12 + c * 125, 50 + rw * 94, 120, 88, ACC, grid_pick_cb, (void *)(intptr_t)k, NULL);
        lv_obj_t *lb = sw_btn_label(U.gbtn[k]);
        lv_obj_set_width(lb, 112);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    }
    lv_obj_add_flag(U.grid, LV_OBJ_FLAG_HIDDEN);
}

// --- the panel ------------------------------------------------------------------------------------

#define FW 52
#define R1_TAB 64
#define R1_Y 90
#define R1_H 196
#define R2_TAB 296
#define R2_Y 322
#define R2_H 156

static void tab(lv_obj_t *s, const char *title, int x, int y, int w) {
    lv_obj_t *b = sw_box(s, x, y, w, 20, TAB, 4);
    sw_label(b, title, SW_F_SMALL, SW_DIM, 0, 3, w, true);
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
    sw_label(s, "DX7", SW_F_BIG, SW_TEXT, 12, 8, 0, false);
    sw_label(s, "FM SYNTH", SW_F_SMALL, ACC, 78, 22, 0, false);
    U.msg = sw_label(s, "", SW_F_SMALL, SW_DIM, 160, 22, 250, false);
    lv_label_set_long_mode(U.msg, LV_LABEL_LONG_CLIP);
    sw_btn(s, "SAVE", 418, 8, 62, 40, ACC, save_cb, NULL, NULL);
    sw_btn(s, "SAVE AS", 484, 8, 76, 40, ACC, save_as_cb, NULL, NULL);
    U.del_b = sw_btn(s, "DEL", 564, 8, 52, 40, SW_RED, del_cb, NULL, NULL);
    sw_label(s, "MIDI IN", SW_F_SMALL, SW_DIM, 632, 22, 0, false);
    sw_btn(s, "-", 690, 8, 40, 40, ACC, ch_cb, (void *)-1, SW_F_MID);
    U.ch_lbl = sw_label(s, "", SW_F_MID, SW_TEXT, 732, 18, 76, true);
    sw_btn(s, "+", 810, 8, 40, 40, ACC, ch_cb, (void *)1, SW_F_MID);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);

    // row 1: the voice, the algorithm, the LFO
    int x = 12;
    tab(s, "VOICE", x, R1_TAB, 400);
    U.num_lbl = sw_label(s, "", SW_F_LABEL, ACC, x, R1_Y + 2, 400, false);
    U.name_lbl = sw_label(s, "", SW_F_BIG, SW_TEXT, x, R1_Y + 24, 400, false);
    sw_btn(s, "<", x, R1_Y + 70, 60, 48, SWC, preset_cb, (void *)-1, SW_F_MID);
    sw_btn(s, ">", x + 66, R1_Y + 70, 60, 48, SWC, preset_cb, (void *)1, SW_F_MID);
    sw_btn(s, "ALL VOICES", x + 136, R1_Y + 70, 150, 48, SWC, grid_show_cb, (void *)1, NULL);
    sw_label(s, "TRANSPOSE", SW_F_SMALL, SW_DIM, x, R1_Y + 140, 0, false);
    sw_btn(s, "-", x + 86, R1_Y + 130, 44, 36, SWC, trans_cb, (void *)-1, SW_F_MID);
    U.trans_lbl = sw_label(s, "", SW_F_MID, SW_TEXT, x + 132, R1_Y + 136, 52, true);
    sw_btn(s, "+", x + 186, R1_Y + 130, 44, 36, SWC, trans_cb, (void *)1, SW_F_MID);
    sw_label(s, "VOICES", SW_F_SMALL, SW_DIM, x + 250, R1_Y + 140, 0, false);
    for (int i = 0; i < DX_VOICES; i++) U.voice[i] = sw_box(s, x + 250 + i * 18, R1_Y + 160, 14, 10, 0x242424, 3);

    x = 426;
    tab(s, "ALGORITHM", x, R1_TAB, 200);
    U.algo_lbl = sw_label(s, "", &lv_font_montserrat_28, SW_TEXT, x, R1_Y + 10, 140, true);
    sw_btn(s, "-", x + 8, R1_Y + 52, 60, 44, SWC, algo_cb, (void *)-1, SW_F_MID);
    sw_btn(s, "+", x + 74, R1_Y + 52, 60, 44, SWC, algo_cb, (void *)1, SW_F_MID);
    U.car_lbl = sw_label(s, "", SW_F_SMALL, ACC, x, R1_Y + 110, 140, true);
    U.fd[0] = sw_fader(s, FD[0].label, x + 146, R1_Y, FW, R1_H, ACC, &OPS, (void *)0);

    x = 640;
    tab(s, "LFO", x, R1_TAB, 372);
    for (int i = 1; i < 6; i++) U.fd[i] = sw_fader(s, FD[i].label, x + (i - 1) * FW, R1_Y, FW, R1_H, ACC, &OPS, (void *)(intptr_t)i);
    sw_label(s, "WAVE", SW_F_SMALL, SW_TEXT, x + 5 * FW, R1_Y, 100, true);
    U.wave_b = sw_btn(s, "", x + 5 * FW + 4, R1_Y + 22, 104, 40, SWC, wave_cb, NULL, NULL);

    // row 2: the six operators
    for (int op = 0; op < 6; op++) {
        x = 12 + op * 167;
        U.op_tab[op] = sw_box(s, x, R2_TAB, 160, 20, MODC, 4);
        char t[8];
        snprintf(t, sizeof t, "OP %d", op + 1);
        U.op_tab_lbl[op] = sw_label(U.op_tab[op], t, SW_F_SMALL, SW_TEXT, 0, 3, 160, true);
        U.fd[6 + op] = sw_fader(s, "LEVEL", x, R2_Y, FW, R2_H, ACC, &OPS, (void *)(intptr_t)(6 | (op << 8)));
        int bx = x + FW + 4;
        U.ratio_lbl[op] = sw_label(s, "", SW_F_LABEL, SW_TEXT, bx, R2_Y + 2, 104, true);
        sw_label(s, "COARSE", SW_F_SMALL, SW_DIM, bx, R2_Y + 22, 104, true);
        sw_btn(s, "-", bx, R2_Y + 38, 50, 32, SWC, freq_cb, (void *)(intptr_t)(op << 8 | 0), SW_F_MID);
        sw_btn(s, "+", bx + 54, R2_Y + 38, 50, 32, SWC, freq_cb, (void *)(intptr_t)(op << 8 | 1), SW_F_MID);
        sw_label(s, "FINE", SW_F_SMALL, SW_DIM, bx, R2_Y + 74, 104, true);
        lv_obj_t *b = sw_btn(s, "-", bx, R2_Y + 90, 50, 32, SWC, freq_cb, (void *)(intptr_t)(op << 8 | 2), SW_F_MID);
        sw_btn_repeat(b);
        b = sw_btn(s, "+", bx + 54, R2_Y + 90, 50, 32, SWC, freq_cb, (void *)(intptr_t)(op << 8 | 3), SW_F_MID);
        sw_btn_repeat(b);
        U.mute_b[op] = sw_btn(s, "ON", bx, R2_Y + 128, 104, 28, ACC, mute_cb, (void *)(intptr_t)op, SW_F_SMALL);
    }

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
    uint8_t lit = 0;
    for (int i = 0; i < DX_VOICES; i++)
        if (s_eng.vc[i].active) lit |= (uint8_t)(1u << i);
    if (lit != U.voices_lit) {
        for (int i = 0; i < DX_VOICES; i++)
            if (((lit ^ U.voices_lit) >> i) & 1)
                lv_obj_set_style_bg_color(U.voice[i], sw_c((lit >> i) & 1 ? ACC : 0x242424), 0);
        U.voices_lit = lit;
    }
}

static void init(void) {
    s_q = xQueueCreate(64, sizeof(tnote_t));
    dx_init(&s_eng);
    load();
    s_ver++;   // the engine takes the voice on its first block
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
    cJSON_AddNumberToObject(o, "midi_ch", s_midi_ch);
    cJSON_AddNumberToObject(o, "preset", s_preset);
    cJSON_AddNumberToObject(o, "mute", s_mute);
    cJSON_AddItemToObject(o, "voice", voice_json(&s_voice));
    return o;
}

static void patch_load(const cJSON *o) {
    const cJSON *v = cJSON_GetObjectItem(o, "midi_ch");
    if (cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint <= 16) s_midi_ch = v->valueint;
    v = cJSON_GetObjectItem(o, "preset");
    s_preset = cJSON_IsNumber(v) && v->valueint >= -1 && v->valueint < npresets() ? v->valueint : -1;
    v = cJSON_GetObjectItem(o, "mute");
    s_mute = cJSON_IsNumber(v) ? (uint8_t)(v->valueint & 63) : 0;
    if (!voice_from(cJSON_GetObjectItem(o, "voice"), &s_voice)) preset_voice(s_preset < 0 ? 0 : s_preset, &s_voice);
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

tmod_t mod_dx7 = {
    .name = "dx7",
    .title = "DX7",
    .blurb = "6-operator FM synth: 32 algorithms, 128 DX7 voices, loads DX7 .syx banks",
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

TULIP_APP(mod_dx7)
