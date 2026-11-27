// s1_ui.c - SEQ101's screen: a header and three pages.
//
//   STEPS     4 lanes x 16 steps. Tap an empty step to put a note there,
//             tap a note to pick it and edit it on the row above (note,
//             octave, chord, velocity, length). A held note spans its steps.
//             Each lane's arpeggiator is on the right; the picked lane's
//             length, MIDI channel and CV output are along the bottom.
//   PATTERNS  each lane's 12 patterns. Tap one: it comes in on the lane's
//             next loop (at once while stopped). PLAY NOW, COPY / PASTE /
//             CLEAR, and a chain: + CHAIN adds the picked pattern to the
//             lane's chain, which then plays one loop each.
//   SET       the clock and MIDI clock (the runtime's panel), the CV range.
#include <stdio.h>
#include <string.h>

#include "s1_app.h"
#include "mod_seq101.h"
#include "s1_ui.h"
#include "studio/widgets.h"

#define ACC 0x24dbaa   // SEQ101's colour
#define PAGE_Y (STUDIO_HDR_LINE_Y + 8)
#define PAGE_H (600 - PAGE_Y)

static const uint32_t LANE_C[S1_LANES] = {SW_ORANGE, SW_YELLOW, 0xff4848, SW_CYAN};

// the step grid
#define CELL_X0 66
#define CELL_P 53       // pitch
#define CELL_W 50
#define ROW_Y0 68
#define ROW_P 80
#define ROW_H 76
// the keyboard under the lanes: 2 octaves of wide, short keys
#define KB_Y 432
#define KB_H 100
#define KB_X0 50
#define KB_WW 66        // a white key's pitch
#define KB_OCTS 2
#define KB_WHITES (7 * KB_OCTS)

enum { PG_STEPS, PG_PATS, PG_SET, PG_COUNT };

typedef struct {
    lv_obj_t *box, *txt, *vel;
} cell_t;

static struct {
    bool built;
    int page;
    lv_obj_t *play, *bpm, *info, *tabs[PG_COUNT], *pages[PG_COUNT];
    int play_shown, bpm_shown;
    uint32_t say_until;
    // STEPS
    cell_t cell[S1_LANES][S1_STEPS];
    lv_obj_t *lane_b[S1_LANES], *mute_b[S1_LANES], *arp_b[S1_LANES], *mode_b[S1_LANES], *rate_b[S1_LANES];
    lv_obj_t *e_note, *e_chord, *e_vel, *e_len, *e_sel;
    lv_obj_t *b_title, *b_len, *b_ch, *b_cv[3], *b_slot;
    int sel_lane, sel_step;     // the picked note (step -1: none)
    int last_note[S1_LANES];    // what a new note starts as
    int head[S1_LANES];         // playhead shown (owner cell)
    lv_obj_t *key[KB_OCTS * 12], *key_dot[KB_OCTS * 12], *key_lbl[KB_OCTS * 12], *kb_oct;
    int kb_base;                // the keyboard's lowest C
    // PATTERNS
    lv_obj_t *slot[S1_LANES][S1_SLOTS], *slot_sub[S1_LANES][S1_SLOTS], *p_lane[S1_LANES], *p_info[S1_LANES];
    lv_obj_t *p_sel;
    int psel_lane, psel_slot;
    s1_pat_t clip;
    bool have_clip;
    int q_shown[S1_LANES], slot_shown[S1_LANES];
    bool blink;
    // SET
    lv_obj_t *s_range, *s_cv, *s_root, *s_scale;
    volatile bool reload;
} U = {.page = -1, .play_shown = -1, .bpm_shown = -1, .sel_step = -1, .psel_slot = 0,
       .last_note = {48, 60, 64, 72}, .head = {-1, -1, -1, -1}, .kb_base = 48};

static uint32_t now_ms(void) { return (uint32_t)(studio_now_us() / 1000); }

static void say(const char *t) {
    if (!U.built) return;
    lv_label_set_text(U.info, t);
    U.say_until = now_ms() + 2500;
}

static s1_pat_t *cur_pat(int l) { return &g_s1.pat[l][g_s1.lane[l].slot]; }

// --- STEPS: drawing ----------------------------------------------------------------------

static void step_text(const s1_step_t *st, char *out, int n) {
    char nm[8];
    s1_note_name(st->note, nm, sizeof nm);
    if (st->chord == CH_NONE)
        snprintf(out, n, "%s", nm);
    else
        snprintf(out, n, "%s\n%s", nm, S1_CHORD_NAMES[st->chord]);
}

static void paint_cell(int l, int i) {
    cell_t *c = &U.cell[l][i];
    const s1_pat_t *p = cur_pat(l);
    int len = g_s1.lane[l].length, owner = s1_owner(p, len, i);
    bool in = i < len;
    if (owner >= 0 && owner != i && in) {   // under a held note: drawn by its owner
        lv_obj_add_flag(c->box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(c->box, LV_OBJ_FLAG_HIDDEN);
    const s1_step_t *st = &p->st[i];
    bool note = in && st->on;
    int span = note ? s1_fit_len(p, len, i, st->len) : 1;
    lv_obj_set_width(c->box, span * CELL_P - (CELL_P - CELL_W));
    bool sel = l == U.sel_lane && i == U.sel_step;
    bool head = U.head[l] == i;
    lv_obj_set_style_bg_color(c->box, sw_c(note ? LANE_C[l] : in ? 0x141414 : 0x050505), 0);
    lv_obj_set_style_border_color(c->box, sw_c(sel    ? SW_TEXT
                                               : head ? ACC
                                               : note ? LANE_C[l]
                                               : in   ? SW_LINE
                                                      : 0x1c1c1c),   // past the lane's length
                                  0);
    lv_obj_set_style_border_width(c->box, sel || head ? 3 : 1, 0);
    if (note) {
        char t[16];
        step_text(st, t, sizeof t);
        lv_label_set_text(c->txt, t);
        snprintf(t, sizeof t, "v%d", st->vel);
        lv_label_set_text(c->vel, t);
    } else {
        lv_label_set_text(c->txt, "");
        lv_label_set_text(c->vel, "");
    }
}

static void paint_lane(int l) {
    for (int i = 0; i < S1_STEPS; i++) paint_cell(l, i);
    const s1_lane_t *ln = &g_s1.lane[l];
    sw_btn_set(U.lane_b[l], l == U.sel_lane);
    sw_btn_set(U.mute_b[l], ln->mute);
    sw_btn_set(U.arp_b[l], ln->arp);
    sw_btn_text(U.arp_b[l], ln->arp ? "ARP ON" : "ARP OFF");
    sw_btn_text(U.mode_b[l], S1_ARP_NAMES[ln->arp_mode]);
    sw_btn_text(U.rate_b[l], S1_RATE_NAMES[ln->arp_rate]);
}

static void paint_editor(void);

// --- the keyboard ---------------------------------------------------------------------------

static bool is_black(int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

static void paint_keys(void) {
    const s1_step_t *st = U.sel_step >= 0 ? &cur_pat(U.sel_lane)->st[U.sel_step] : NULL;
    int cur = st && st->on ? st->note : U.last_note[U.sel_lane];
    bool scaled = g_s1.scale != SC_CHROMATIC;
    for (int k = 0; k < KB_OCTS * 12; k++) {
        int note = U.kb_base + k, pc = k % 12;
        bool in = s1_in_scale(note, g_s1.scale_root, g_s1.scale);
        uint32_t bg = note == cur ? ACC : is_black(pc) ? 0x202020 : (scaled && !in ? 0x9a9a9a : 0xf2f2f2);
        lv_obj_set_style_bg_color(U.key[k], sw_c(bg), 0);
        if (scaled && in)
            lv_obj_remove_flag(U.key_dot[k], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(U.key_dot[k], LV_OBJ_FLAG_HIDDEN);
        if (U.key_lbl[k]) {
            char t[8];
            s1_note_name(note, t, sizeof t);
            lv_label_set_text(U.key_lbl[k], t);
        }
    }
    char t[24];
    char a[8], b[8];
    s1_note_name(U.kb_base, a, sizeof a);
    s1_note_name(U.kb_base + KB_OCTS * 12 - 1, b, sizeof b);
    snprintf(t, sizeof t, "%s-%s", a, b);
    lv_label_set_text(U.kb_oct, t);
}

static void key_event(lv_event_t *e) {
    int note = U.kb_base + (int)(intptr_t)lv_event_get_user_data(e);
    U.last_note[U.sel_lane] = note;
    if (U.sel_step >= 0 && cur_pat(U.sel_lane)->st[U.sel_step].on) {
        cur_pat(U.sel_lane)->st[U.sel_step].note = (uint8_t)note;
        paint_lane(U.sel_lane);
        s1_changed();
    } else {
        char t[40], nm[8];
        s1_note_name(note, nm, sizeof nm);
        snprintf(t, sizeof t, "new notes on lane %c: %s", 'A' + U.sel_lane, nm);
        say(t);
    }
    paint_keys();
    paint_editor();
}

static void kb_oct_cb(void *ud) {
    int b = U.kb_base + 12 * (int)(intptr_t)ud;
    if (b < 0 || b + KB_OCTS * 12 > 128) return;
    U.kb_base = b;
    paint_keys();
}

static void keyboard_create(lv_obj_t *p) {
    sw_btn(p, "<", 6, KB_Y, 40, KB_H, ACC, kb_oct_cb, (void *)-1, SW_F_MID);
    sw_btn(p, ">", KB_X0 + KB_WHITES * KB_WW + 4, KB_Y, 40, KB_H, ACC, kb_oct_cb, (void *)1, SW_F_MID);
    // white keys first, the black ones on top
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0, w = 0; k < KB_OCTS * 12; k++) {
            bool black = is_black(k % 12);
            if (!black) w++;
            if (black != (pass == 1)) continue;
            int x = black ? KB_X0 + w * KB_WW - 20 : KB_X0 + (w - 1) * KB_WW;
            int kw = black ? 40 : KB_WW - 2, kh = black ? KB_H * 6 / 10 : KB_H;
            lv_obj_t *b = U.key[k] = sw_box(p, x, KB_Y, kw, kh, 0xf2f2f2, 3);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, sw_c(0x303030), 0);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, key_event, LV_EVENT_PRESSED, (void *)(intptr_t)k);
            U.key_dot[k] = sw_box(b, kw / 2 - 3, kh - (black ? 12 : 34), 6, 6, black ? 0x24dbaa : 0x24a080, 3);
            if (!black) U.key_lbl[k] = sw_label(b, "", SW_F_SMALL, 0x202020, 0, kh - 20, kw, true);
        }
    U.kb_oct = sw_label(p, "", SW_F_SMALL, SW_DIM, KB_X0 + KB_WHITES * KB_WW + 4, KB_Y - 18, 60, false);
}

static void paint_editor(void) {
    char t[32];
    bool has = U.sel_step >= 0;
    const s1_step_t *st = has ? &cur_pat(U.sel_lane)->st[U.sel_step] : NULL;
    if (has) {
        char nm[8];
        s1_note_name(st->note, nm, sizeof nm);
        lv_label_set_text(U.e_note, nm);
        lv_label_set_text(U.e_chord, S1_CHORD_NAMES[st->chord]);
        snprintf(t, sizeof t, "%d", st->vel);
        lv_label_set_text(U.e_vel, t);
        lv_label_set_text(U.e_len, s1_len_name(st->len));
        snprintf(t, sizeof t, "LANE %c  STEP %d", 'A' + U.sel_lane, U.sel_step + 1);
    } else {
        lv_label_set_text(U.e_note, "--");
        lv_label_set_text(U.e_chord, "--");
        lv_label_set_text(U.e_vel, "--");
        lv_label_set_text(U.e_len, "--");
        snprintf(t, sizeof t, "tap a step");
    }
    lv_label_set_text(U.e_sel, t);
    if (U.key[0]) paint_keys();
}

static void paint_bottom(void) {
    const s1_lane_t *ln = &g_s1.lane[U.sel_lane];
    char t[48];
    snprintf(t, sizeof t, "LANE %c", 'A' + U.sel_lane);
    lv_label_set_text(U.b_title, t);
    lv_obj_set_style_text_color(U.b_title, sw_c(LANE_C[U.sel_lane]), 0);
    snprintf(t, sizeof t, "%d", ln->length);
    lv_label_set_text(U.b_len, t);
    if (ln->midi_ch)
        snprintf(t, sizeof t, "CH %d", ln->midi_ch);
    else
        snprintf(t, sizeof t, "OFF");
    lv_label_set_text(U.b_ch, t);
    for (int i = 0; i < 3; i++) sw_btn_set(U.b_cv[i], ln->cv == i);
    if (ln->nchain)
        snprintf(t, sizeof t, "pattern %d  (chain of %d)", ln->slot + 1, ln->nchain);
    else
        snprintf(t, sizeof t, "pattern %d", ln->slot + 1);
    lv_label_set_text(U.b_slot, t);
}

static void steps_refresh(void) {
    for (int l = 0; l < S1_LANES; l++) paint_lane(l);
    paint_editor();
    paint_bottom();
}

static void edited(int l) {
    paint_lane(l);
    paint_editor();
    s1_changed();
}

// --- STEPS: touch ---------------------------------------------------------------------------

static void cell_event(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    int l = id / S1_STEPS, i = id % S1_STEPS;
    s1_pat_t *p = cur_pat(l);
    int len = g_s1.lane[l].length;
    if (i >= len) return;
    int owner = s1_owner(p, len, i);
    int old = U.sel_lane;
    U.sel_lane = l;
    if (owner >= 0) {
        U.sel_step = owner;
    } else {
        s1_step_default(&p->st[i], U.last_note[l]);
        U.sel_step = i;
        s1_changed();
    }
    if (old != l) paint_lane(old);
    paint_lane(l);
    paint_editor();
    paint_bottom();
}

static s1_step_t *sel_step(void) {
    if (U.sel_step < 0) return NULL;
    s1_step_t *st = &cur_pat(U.sel_lane)->st[U.sel_step];
    return st->on ? st : NULL;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void note_cb(void *ud) {
    s1_step_t *st = sel_step();
    if (!st) return;
    int d = (int)(intptr_t)ud, n = st->note + d;
    if (d == 1 || d == -1)   // a semitone: in a scale, its next note
        while (n > 0 && n < 127 && !s1_in_scale(n, g_s1.scale_root, g_s1.scale)) n += d;
    st->note = (uint8_t)clampi(n, 0, 127);
    U.last_note[U.sel_lane] = st->note;
    edited(U.sel_lane);
}

static void chord_cb(void *ud) {
    s1_step_t *st = sel_step();
    if (!st) return;
    st->chord = (uint8_t)((st->chord + (int)(intptr_t)ud + CH_COUNT) % CH_COUNT);
    edited(U.sel_lane);
}

static void vel_cb(void *ud) {
    s1_step_t *st = sel_step();
    if (!st) return;
    st->vel = (uint8_t)clampi(st->vel + 5 * (int)(intptr_t)ud, 5, 127);
    edited(U.sel_lane);
}

// the lengths in S1_LEN_VALUES, as far as the next note / the lane's end
static void len_cb(void *ud) {
    s1_step_t *st = sel_step();
    if (!st) return;
    int d = (int)(intptr_t)ud, k = 0;
    while (k < S1_LENS - 1 && S1_LEN_VALUES[k] < st->len) k++;
    k = clampi(k + d, 0, S1_LENS - 1);
    int want = S1_LEN_VALUES[k];
    int fit = s1_fit_len(cur_pat(U.sel_lane), g_s1.lane[U.sel_lane].length, U.sel_step, want);
    if (fit < want && d > 0) say("next note or the lane's end is in the way");
    st->len = (uint8_t)fit;
    edited(U.sel_lane);
}

static void del_cb(void *ud) {
    s1_step_t *st = sel_step();
    if (!st) return;
    memset(st, 0, sizeof *st);
    U.sel_step = -1;
    edited(U.sel_lane);
}

static void lane_cb(void *ud) {
    int old = U.sel_lane;
    U.sel_lane = (int)(intptr_t)ud;
    U.sel_step = -1;
    paint_lane(old);
    paint_lane(U.sel_lane);
    paint_editor();
    paint_bottom();
}

static void mute_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_s1.lane[l].mute = !g_s1.lane[l].mute;
    edited(l);
}

static void arp_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_s1.lane[l].arp = !g_s1.lane[l].arp;
    edited(l);
}

static void mode_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_s1.lane[l].arp_mode = (uint8_t)((g_s1.lane[l].arp_mode + 1) % ARP_MODES);
    edited(l);
}

static void rate_cb(void *ud) {
    int l = (int)(intptr_t)ud;
    g_s1.lane[l].arp_rate = (uint8_t)((g_s1.lane[l].arp_rate + 1) % ARP_RATES);
    edited(l);
}

static void llen_cb(void *ud) {
    s1_lane_t *ln = &g_s1.lane[U.sel_lane];
    ln->length = (uint8_t)clampi(ln->length + (int)(intptr_t)ud, 1, S1_STEPS);
    if (U.sel_step >= ln->length) U.sel_step = -1;
    paint_bottom();
    edited(U.sel_lane);
}

static void ch_cb(void *ud) {
    s1_lane_t *ln = &g_s1.lane[U.sel_lane];
    ln->midi_ch = (int8_t)clampi(ln->midi_ch + (int)(intptr_t)ud, 0, 16);
    paint_bottom();
    s1_changed();
}

static void cv_cb(void *ud) {
    g_s1.lane[U.sel_lane].cv = (uint8_t)(intptr_t)ud;
    paint_bottom();
    s1_changed();
}

static lv_obj_t *value(lv_obj_t *p, int x, int w) {
    lv_obj_t *l = sw_label(p, "", SW_F_LABEL, SW_TEXT, x, 6 + 12, w, true);
    return l;
}

static lv_obj_t *stepper(lv_obj_t *p, const char *t, int x, sw_cb_t cb, intptr_t d, bool repeat) {
    lv_obj_t *b = sw_btn(p, t, x, 6, 40, 40, ACC, cb, (void *)d, SW_F_MID);
    if (repeat) sw_btn_repeat(b);
    return b;
}

static void caption(lv_obj_t *p, const char *t, int x, int w) {
    sw_label(p, t, SW_F_SMALL, SW_DIM, x, 0, w, true);
}

static void steps_create(lv_obj_t *p) {
    // the picked note
    caption(p, "NOTE", 48, 62);
    stepper(p, "-", 6, note_cb, -1, true);
    U.e_note = value(p, 48, 62);
    stepper(p, "+", 112, note_cb, 1, true);
    sw_btn(p, "OCT-", 160, 6, 56, 40, ACC, note_cb, (void *)-12, NULL);
    sw_btn(p, "OCT+", 220, 6, 56, 40, ACC, note_cb, (void *)12, NULL);
    caption(p, "CHORD", 330, 70);
    stepper(p, "<", 288, chord_cb, -1, false);
    U.e_chord = value(p, 330, 70);
    stepper(p, ">", 402, chord_cb, 1, false);
    caption(p, "VEL", 496, 54);
    stepper(p, "-", 454, vel_cb, -1, true);
    U.e_vel = value(p, 496, 54);
    stepper(p, "+", 552, vel_cb, 1, true);
    caption(p, "LENGTH", 646, 64);
    stepper(p, "-", 604, len_cb, -1, false);
    U.e_len = value(p, 646, 64);
    stepper(p, "+", 712, len_cb, 1, false);
    sw_btn(p, "DELETE", 762, 6, 80, 40, SW_RED, del_cb, NULL, NULL);
    U.e_sel = sw_label(p, "", SW_F_SMALL, SW_DIM, 852, 20, 166, false);

    // step numbers
    for (int i = 0; i < S1_STEPS; i++) {
        char t[4];
        snprintf(t, sizeof t, "%d", i + 1);
        sw_label(p, t, SW_F_SMALL, i % 4 ? SW_DIM : ACC, CELL_X0 + i * CELL_P, 52, CELL_W, true);
    }
    // the lanes
    for (int l = 0; l < S1_LANES; l++) {
        int y = ROW_Y0 + l * ROW_P;
        char t[4] = {(char)('A' + l), 0};
        U.lane_b[l] = sw_btn(p, t, 6, y, 54, 38, LANE_C[l], lane_cb, (void *)(intptr_t)l, SW_F_MID);
        U.mute_b[l] = sw_btn(p, "M", 6, y + 42, 54, ROW_H - 42, SW_RED, mute_cb, (void *)(intptr_t)l, NULL);
        for (int i = 0; i < S1_STEPS; i++) {
            cell_t *c = &U.cell[l][i];
            c->box = sw_box(p, CELL_X0 + i * CELL_P, y, CELL_W, ROW_H, 0x141414, 0);
            lv_obj_set_style_border_width(c->box, 1, 0);
            lv_obj_add_flag(c->box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(c->box, cell_event, LV_EVENT_PRESSED, (void *)(intptr_t)(l * S1_STEPS + i));
            c->txt = sw_label(c->box, "", SW_F_SMALL, SW_BG, 5, 5, 0, false);
            c->vel = sw_label(c->box, "", SW_F_SMALL, SW_BG, 0, ROW_H - 20, 0, false);
            lv_obj_align(c->vel, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
        }
        U.arp_b[l] = sw_btn(p, "ARP OFF", 918, y, 100, 36, ACC, arp_cb, (void *)(intptr_t)l, SW_F_SMALL);
        U.mode_b[l] = sw_btn(p, "UP", 918, y + 40, 48, 36, ACC, mode_cb, (void *)(intptr_t)l, SW_F_SMALL);
        U.rate_b[l] = sw_btn(p, "1/16", 970, y + 40, 48, 36, ACC, rate_cb, (void *)(intptr_t)l, SW_F_SMALL);
    }
    // the picked lane
    int y = ROW_Y0 + S1_LANES * ROW_P - 2;
    U.b_title = sw_label(p, "", SW_F_MID, SW_TEXT, 6, y + 8, 84, false);
    sw_label(p, "STEPS", SW_F_SMALL, SW_DIM, 96, y + 14, 0, false);
    sw_btn_repeat(sw_btn(p, "-", 140, y, 40, 40, ACC, llen_cb, (void *)-1, SW_F_MID));
    U.b_len = sw_label(p, "", SW_F_LABEL, SW_TEXT, 182, y + 12, 40, true);
    sw_btn_repeat(sw_btn(p, "+", 224, y, 40, 40, ACC, llen_cb, (void *)1, SW_F_MID));
    sw_label(p, "MIDI", SW_F_SMALL, SW_DIM, 282, y + 14, 0, false);
    sw_btn_repeat(sw_btn(p, "-", 316, y, 40, 40, ACC, ch_cb, (void *)-1, SW_F_MID));
    U.b_ch = sw_label(p, "", SW_F_LABEL, SW_TEXT, 358, y + 12, 56, true);
    sw_btn_repeat(sw_btn(p, "+", 416, y, 40, 40, ACC, ch_cb, (void *)1, SW_F_MID));
    sw_label(p, "CV", SW_F_SMALL, SW_DIM, 474, y + 14, 0, false);
    static const char *const CVN[3] = {"OFF", "CV A", "CV B"};
    for (int i = 0; i < 3; i++)
        U.b_cv[i] = sw_btn(p, CVN[i], 498 + i * 62, y, 58, 40, ACC, cv_cb, (void *)(intptr_t)i, NULL);
    U.b_slot = sw_label(p, "", SW_F_SMALL, SW_DIM, 700, y + 14, 318, false);
    keyboard_create(p);
}

// the playhead: one cell outlined per lane
static void steps_tick(void) {
    for (int l = 0; l < S1_LANES; l++) {
        int s = s1_play_step(l), h = -1;
        if (s >= 0) {
            h = s1_owner(cur_pat(l), g_s1.lane[l].length, s);
            if (h < 0) h = s;
        }
        if (h == U.head[l]) continue;
        int old = U.head[l];
        U.head[l] = h;
        if (old >= 0) paint_cell(l, old);
        if (h >= 0) paint_cell(l, h);
    }
}

// --- PATTERNS -----------------------------------------------------------------------------

#define SLOT_X0 168
#define SLOT_P 71
#define PROW_P 86

static void paint_slot(int l, int k) {
    const s1_lane_t *ln = &g_s1.lane[l];
    bool active = ln->slot == k, used = !s1_pat_empty(&g_s1.pat[l][k]);
    bool queued = s1_queued(l) == k, sel = l == U.psel_lane && k == U.psel_slot;
    lv_obj_t *b = U.slot[l][k];
    lv_obj_set_style_bg_color(b, sw_c(active ? LANE_C[l] : 0x141414), 0);
    uint32_t edge = sel ? SW_TEXT : queued ? (U.blink ? SW_YELLOW : 0x141414) : used ? LANE_C[l] : SW_LINE;
    lv_obj_set_style_border_color(b, sw_c(edge), 0);
    lv_obj_set_style_border_width(b, sel || queued ? 3 : 1, 0);
    lv_obj_t *num = lv_obj_get_child(b, 0);
    lv_obj_set_style_text_color(num, sw_c(active ? SW_BG : used ? SW_TEXT : SW_DIM), 0);
    // where it sits in the chain: C1 C3 ...
    char t[24] = "";
    int n = 0;
    for (int i = 0; i < ln->nchain && n < (int)sizeof t - 5; i++)
        if (ln->chain[i] == k) n += snprintf(t + n, sizeof t - n, "%sC%d", n ? " " : "", i + 1);
    lv_label_set_text(U.slot_sub[l][k], t);
    lv_obj_set_style_text_color(U.slot_sub[l][k], sw_c(active ? SW_BG : 0xdb49ff), 0);
}

static void paint_plane(int l) {
    const s1_lane_t *ln = &g_s1.lane[l];
    char t[64];
    int q = s1_queued(l);
    if (q >= 0)
        snprintf(t, sizeof t, "playing P%d, P%d next", ln->slot + 1, q + 1);
    else if (ln->nchain)
        snprintf(t, sizeof t, "playing P%d, chain of %d", ln->slot + 1, ln->nchain);
    else
        snprintf(t, sizeof t, "playing P%d", ln->slot + 1);
    lv_label_set_text(U.p_info[l], t);
    for (int k = 0; k < S1_SLOTS; k++) paint_slot(l, k);
    U.q_shown[l] = q;
    U.slot_shown[l] = ln->slot;
}

static void paint_psel(void) {
    char t[64];
    snprintf(t, sizeof t, "LANE %c  PATTERN %d%s", 'A' + U.psel_lane, U.psel_slot + 1,
             s1_pat_empty(&g_s1.pat[U.psel_lane][U.psel_slot]) ? "  (empty)" : "");
    lv_label_set_text(U.p_sel, t);
}

static void pats_refresh(void) {
    for (int l = 0; l < S1_LANES; l++) paint_plane(l);
    paint_psel();
}

static bool playing(void) {
    studio_clock_status_t st;
    studio_clock_status(&st);
    return st.playing;
}

static void slot_event(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    int l = id / S1_SLOTS, k = id % S1_SLOTS, ol = U.psel_lane, ok = U.psel_slot;
    U.psel_lane = l;
    U.psel_slot = k;
    if (playing()) {
        s1_queue(l, k == g_s1.lane[l].slot ? -1 : k);   // the playing one again: cancel
    } else {
        s1_activate(l, k);
    }
    paint_slot(ol, ok);
    paint_plane(l);
    paint_psel();
}

static void now_cb(void *ud) {
    s1_activate(U.psel_lane, U.psel_slot);
    paint_plane(U.psel_lane);
    say("playing now");
}

static void copy_cb(void *ud) {
    U.clip = g_s1.pat[U.psel_lane][U.psel_slot];
    U.have_clip = true;
    say("copied");
}

static void paste_cb(void *ud) {
    if (!U.have_clip) return say("COPY first");
    g_s1.pat[U.psel_lane][U.psel_slot] = U.clip;
    s1_changed();
    paint_plane(U.psel_lane);
    paint_psel();
    say("pasted");
}

static void clear_cb(void *ud) {
    memset(&g_s1.pat[U.psel_lane][U.psel_slot], 0, sizeof(s1_pat_t));
    s1_changed();
    paint_plane(U.psel_lane);
    paint_psel();
    say("cleared");
}

static void chain_cb(void *ud) {
    s1_lane_t *ln = &g_s1.lane[U.psel_lane];
    if (ln->nchain >= S1_CHAIN) return say("the chain is full");
    ln->chain[ln->nchain] = (uint8_t)U.psel_slot;
    ln->nchain++;   // after the slot is there: the rt task reads nchain
    s1_changed();
    paint_plane(U.psel_lane);
}

static void unchain_cb(void *ud) {
    g_s1.lane[U.psel_lane].nchain = 0;
    s1_changed();
    paint_plane(U.psel_lane);
    say("chain cleared");
}

static void pats_create(lv_obj_t *p) {
    for (int l = 0; l < S1_LANES; l++) {
        int y = 8 + l * PROW_P;
        char t[16];
        snprintf(t, sizeof t, "LANE %c", 'A' + l);
        U.p_lane[l] = sw_label(p, t, SW_F_MID, LANE_C[l], 12, y + 10, 150, false);
        U.p_info[l] = sw_label(p, "", SW_F_SMALL, SW_DIM, 12, y + 44, 150, false);
        for (int k = 0; k < S1_SLOTS; k++) {
            lv_obj_t *b = U.slot[l][k] = sw_box(p, SLOT_X0 + k * SLOT_P, y, SLOT_P - 5, PROW_P - 8, 0x141414, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, slot_event, LV_EVENT_PRESSED, (void *)(intptr_t)(l * S1_SLOTS + k));
            snprintf(t, sizeof t, "%d", k + 1);
            lv_obj_t *n = sw_label(b, t, SW_F_MID, SW_TEXT, 0, 14, SLOT_P - 5, true);
            (void)n;
            U.slot_sub[l][k] = sw_label(b, "", SW_F_SMALL, 0xdb49ff, 0, 48, SLOT_P - 5, true);
        }
    }
    int y = 8 + S1_LANES * PROW_P + 8;
    sw_line(p, 8, y - 6, 1008, 1);
    U.p_sel = sw_label(p, "", SW_F_LABEL, SW_TEXT, 12, y + 12, 330, false);
    sw_btn(p, "PLAY NOW", 350, y, 110, 44, ACC, now_cb, NULL, NULL);
    sw_btn(p, "COPY", 466, y, 80, 44, ACC, copy_cb, NULL, NULL);
    sw_btn(p, "PASTE", 552, y, 80, 44, ACC, paste_cb, NULL, NULL);
    sw_btn(p, "CLEAR", 638, y, 80, 44, SW_RED, clear_cb, NULL, NULL);
    sw_btn(p, "+ CHAIN", 744, y, 130, 44, 0xdb49ff, chain_cb, NULL, NULL);
    sw_btn(p, "CLR CHAIN", 880, y, 130, 44, 0xdb49ff, unchain_cb, NULL, NULL);
    sw_label(p,
             "Tap a pattern: it comes in when its lane next goes round (at once while stopped); tap the "
             "playing one to cancel.\n+ CHAIN adds the picked pattern to its lane's chain, which then "
             "plays one loop of each, in order (C1, C2 ...). Patterns save themselves.",
             SW_F_SMALL, SW_DIM, 12, y + 58, 1000, false);
}

static void pats_tick(uint32_t ms) {
    bool b = (ms / 300) & 1;
    for (int l = 0; l < S1_LANES; l++) {
        int q = s1_queued(l);
        if (q != U.q_shown[l] || g_s1.lane[l].slot != U.slot_shown[l]) {
            paint_plane(l);
        } else if (q >= 0 && b != U.blink) {
            U.blink = b;
            paint_slot(l, q);
        }
    }
    U.blink = b;
}

// --- SET ----------------------------------------------------------------------------------

static void set_refresh(void) {
    sw_btn_text(U.s_range, g_s1.cv_5v ? "PITCH 0-5 V (AMYboard)" : "PITCH 0-10 V");
    sw_btn_set(U.s_range, g_s1.cv_5v);
    char t[96];
    s1_cv_status(t, sizeof t);
    lv_label_set_text(U.s_cv, t);
    lv_label_set_text(U.s_root, S1_ROOT_NAMES[g_s1.scale_root]);
    lv_label_set_text(U.s_scale, S1_SCALE_NAMES[g_s1.scale]);
}

static void root_cb(void *ud) {
    g_s1.scale_root = (uint8_t)((g_s1.scale_root + (int)(intptr_t)ud + 12) % 12);
    s1_changed();
    set_refresh();
}

static void scale_cb(void *ud) {
    g_s1.scale = (uint8_t)((g_s1.scale + (int)(intptr_t)ud + SC_COUNT) % SC_COUNT);
    s1_changed();
    set_refresh();
}

static void range_cb(void *ud) {
    g_s1.cv_5v = !g_s1.cv_5v;
    s1_changed();
    set_refresh();
}

static void set_create(lv_obj_t *p) {
    sw_label(p, "CLOCK & MIDI", SW_F_LABEL, ACC, 12, 10, 0, false);
    studio_io_panel(p, 12, 36, &mod_seq101, ACC);
    const int x = 380;
    sw_label(p, "CV", SW_F_LABEL, ACC, x, 10, 0, false);
    U.s_range = sw_btn(p, "PITCH 0-10 V", x, 36, 260, 44, ACC, range_cb, NULL, NULL);
    U.s_cv = sw_label(p, "", SW_F_SMALL, SW_TEXT, x, 90, 630, false);
    sw_label(p,
             "Each lane picks CV A (the Mabee DAC at 0x59) or CV B (0x58), its MIDI channel and its\n"
             "length along the bottom of the STEPS page. VOUT0 is the gate (5 V), VOUT1 the pitch,\n"
             "1 V/oct with 0 V = C0. A chord sends its lowest note to CV and to Studio routing;\n"
             "MIDI gets the whole chord. 0-5 V suits AMYboard inputs (set its 0V NOTE to 12):\n"
             "notes above 5 V drop by octaves.\n\n"
             "The DACs belong to one sequencer at a time: the first one switched on.",
             SW_F_SMALL, SW_DIM, x, 120, 630, false);

    sw_label(p, "SCALE", SW_F_LABEL, ACC, x, 270, 0, false);
    sw_label(p, "ROOT", SW_F_SMALL, SW_DIM, x, 310, 0, false);
    sw_btn(p, "<", x + 60, 298, 40, 40, ACC, root_cb, (void *)-1, SW_F_MID);
    U.s_root = sw_label(p, "", SW_F_LABEL, SW_TEXT, x + 102, 310, 56, true);
    sw_btn(p, ">", x + 160, 298, 40, 40, ACC, root_cb, (void *)1, SW_F_MID);
    sw_label(p, "SCALE", SW_F_SMALL, SW_DIM, x + 220, 310, 0, false);
    sw_btn(p, "<", x + 270, 298, 40, 40, ACC, scale_cb, (void *)-1, SW_F_MID);
    U.s_scale = sw_label(p, "", SW_F_LABEL, SW_TEXT, x + 312, 310, 120, true);
    sw_btn(p, ">", x + 434, 298, 40, 40, ACC, scale_cb, (void *)1, SW_F_MID);
    sw_label(p,
             "Every lane's notes snap to the nearest note of the scale as they play (down on a tie),\n"
             "chords included. The keyboard marks the scale's notes with a dot; NOTE -/+ steps\n"
             "through them. CHROMATIC leaves every note as it is.",
             SW_F_SMALL, SW_DIM, x, 350, 630, false);
}

// --- the header and the pages -----------------------------------------------------------

static void refresh_page(int i) {
    if (i == PG_STEPS) steps_refresh();
    if (i == PG_PATS) pats_refresh();
    if (i == PG_SET) set_refresh();
}

static void show_page(int i) {
    U.page = i;
    for (int k = 0; k < PG_COUNT; k++) {
        sw_btn_set(U.tabs[k], k == i);
        if (k == i)
            lv_obj_remove_flag(U.pages[k], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(U.pages[k], LV_OBJ_FLAG_HIDDEN);
    }
    refresh_page(i);
}

static void tab_cb(void *ud) { show_page((int)(intptr_t)ud); }
static void play_cb(void *ud) { studio_toggle_play(); }

static void bpm_cb(void *ud) {
    int d = (intptr_t)ud == 1 ? 1 : -1;
    studio_set_bpm_x100((studio_bpm_x100() + 50) / 100 * 100 + d * 100);
}

void s1_ui_create(lv_obj_t *s) {
    lv_obj_set_style_bg_color(s, sw_c(SW_BG), 0);
    const int y = STUDIO_HDR_Y, h = STUDIO_HDR_BTN_H;
    sw_label(s, "SEQ101", SW_F_MID, SW_TEXT, 12, 16, 0, false);
    U.play = sw_btn(s, "PLAY", 110, y, 80, h, SW_GREEN, play_cb, NULL, SW_F_MID);
    sw_btn_repeat(sw_btn(s, "-", 198, y, 40, h, ACC, bpm_cb, (void *)0, SW_F_MID));
    U.bpm = sw_label(s, "", SW_F_LABEL, SW_TEXT, 240, 20, 76, true);
    sw_btn_repeat(sw_btn(s, "+", 318, y, 40, h, ACC, bpm_cb, (void *)1, SW_F_MID));
    static const char *const names[PG_COUNT] = {"STEPS", "PATTERNS", "SET"};
    static const int w[PG_COUNT] = {90, 110, 70};
    for (int i = 0, x = 372; i < PG_COUNT; x += w[i] + 6, i++)
        U.tabs[i] = sw_btn(s, names[i], x, y, w[i], h, ACC, tab_cb, (void *)(intptr_t)i, NULL);
    U.info = sw_label(s, "", SW_F_SMALL, SW_DIM, 668, 22, STUDIO_SQ_X - 668 - 6, false);
    lv_label_set_long_mode(U.info, LV_LABEL_LONG_CLIP);
    studio_chrome(s);
    sw_line(s, 0, STUDIO_HDR_LINE_Y, 1024, 1);
    void (*make[PG_COUNT])(lv_obj_t *) = {steps_create, pats_create, set_create};
    for (int i = 0; i < PG_COUNT; i++) {
        lv_obj_t *g = U.pages[i] = sw_box(s, 0, PAGE_Y, 1024, PAGE_H, SW_BG, 0);
        lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
        make[i](g);
        lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    }
    U.built = true;
    show_page(PG_STEPS);
}

void s1_ui_show(bool shown) {
    if (shown && U.built) refresh_page(U.page);
}

void s1_ui_reload(void) { U.reload = true; }

void s1_ui_poll(bool visible) {
    if (!U.built) return;
    if (U.reload) {
        U.reload = false;
        U.sel_step = -1;
        if (visible) refresh_page(U.page);
    }
    if (!visible) return;
    uint32_t ms = now_ms();
    studio_clock_status_t st;
    studio_clock_status(&st);
    int ps = st.playing ? 1 : st.armed ? 2 : 0;
    if (ps != U.play_shown) {
        U.play_shown = ps;
        static const char *const txt[3] = {"PLAY", "STOP", "ARMED"};
        sw_btn_text(U.play, txt[ps]);
        sw_btn_set(U.play, ps != 0);
        sw_btn_accent(U.play, ps == 2 ? SW_YELLOW : SW_GREEN);
    }
    int bv = st.ext ? -1 - st.ext_bpm_x10 : (int)((studio_bpm_x100() + 50) / 100);
    if (bv != U.bpm_shown) {
        U.bpm_shown = bv;
        char t[16];
        if (bv >= 0)
            snprintf(t, sizeof t, "%d BPM", bv);
        else if (st.ext_bpm_x10)
            snprintf(t, sizeof t, "MIDI %d", (st.ext_bpm_x10 + 5) / 10);
        else
            snprintf(t, sizeof t, "MIDI --");
        lv_label_set_text(U.bpm, t);
    }
    if (U.say_until && (int32_t)(ms - U.say_until) > 0) {
        U.say_until = 0;
        lv_label_set_text(U.info, "");
    }
    if (U.page == PG_STEPS) steps_tick();
    if (U.page == PG_PATS) pats_tick(ms);
}
