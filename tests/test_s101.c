// test_s101.c - SEQ101's model and player on the PC.
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "s1_model.h"
#include "s1_seq.h"

static int failures;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            failures++;                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static s1_state_t S;
static s1_seq_t Q;

// run ticks [from, to): the ons and offs of one lane as (tick, note) lists
typedef struct {
    int n;
    uint32_t t[256];
    uint8_t note[256], type[256];
} log_t;

static void run(int lane, uint32_t from, uint32_t to, log_t *lg) {
    memset(lg, 0, sizeof *lg);
    s1_ev_t ev[32];
    for (uint32_t t = from; t < to; t++) {
        int n = s1_seq_tick(&Q, &S, t, ev, 32);
        for (int i = 0; i < n; i++)
            if (ev[i].lane == lane && lg->n < 256) {
                lg->t[lg->n] = t;
                lg->note[lg->n] = ev[i].note;
                lg->type[lg->n] = ev[i].type;
                lg->n++;
            }
    }
}

static void fresh(void) {
    memset(&S, 0, sizeof S);
    s1_defaults(&S);
    memset(S.pat, 0, sizeof S.pat);
    s1_seq_init(&Q);
}

static void test_notes_and_lengths(void) {
    fresh();
    s1_step_default(&S.pat[0][0].st[0], 60);               // a 1/16
    s1_step_default(&S.pat[0][0].st[4], 62);
    S.pat[0][0].st[4].len = 4;                              // a 1/4
    s1_seq_start(&Q, &S, 0);
    log_t lg;
    run(0, 0, 16 * S1_TICKS, &lg);
    CHECK(lg.n == 4, "events %d", lg.n);
    CHECK(lg.type[0] == S1_EV_ON && lg.t[0] == 0 && lg.note[0] == 60, "on 60 at 0");
    CHECK(lg.type[1] == S1_EV_OFF && lg.t[1] == S1_TICKS - 1, "off 60 a tick short: %u", lg.t[1]);
    CHECK(lg.type[2] == S1_EV_ON && lg.t[2] == 4 * S1_TICKS && lg.note[2] == 62, "on 62 on step 5");
    CHECK(lg.type[3] == S1_EV_OFF && lg.t[3] == 8 * S1_TICKS - 1, "1/4 held: off at %u", lg.t[3]);
    printf("seq101: steps, 1/16 and 1/4 lengths, gates fall a tick early\n");
}

static void test_chord_and_lane_length(void) {
    fresh();
    s1_step_default(&S.pat[1][0].st[0], 60);
    S.pat[1][0].st[0].chord = CH_MAJ;
    S.lane[1].length = 5;                                   // B loops every 5
    s1_seq_start(&Q, &S, 0);
    log_t lg;
    run(1, 0, 16 * S1_TICKS, &lg);
    int ons = 0, at[8], k = 0;
    for (int i = 0; i < lg.n; i++)
        if (lg.type[i] == S1_EV_ON) {
            ons++;
            if (lg.note[i] == 60 && k < 8) at[k++] = (int)lg.t[i];
        }
    // a chord of 3 on steps 0, 5, 10, 15 of the bar
    CHECK(ons == 12 && k == 4 && at[1] == 5 * S1_TICKS && at[3] == 15 * S1_TICKS, "ons %d, roots %d", ons, k);
    uint8_t notes[4];
    CHECK(s1_chord_notes(&S.pat[1][0].st[0], notes) == 3 && notes[1] == 64 && notes[2] == 67, "C major");
    printf("seq101: chords, a 5-step lane loops on its own\n");
}

static void test_arp(void) {
    fresh();
    s1_step_default(&S.pat[2][0].st[0], 60);
    S.pat[2][0].st[0].chord = CH_MIN;
    S.pat[2][0].st[0].len = 4;                              // 24 ticks
    S.lane[2].arp = 1;
    S.lane[2].arp_rate = ARP_R16;
    S.lane[2].arp_mode = ARP_DOWN;
    s1_seq_start(&Q, &S, 0);
    log_t lg;
    run(2, 0, 8 * S1_TICKS, &lg);
    uint8_t seq[8];
    int k = 0;
    for (int i = 0; i < lg.n; i++)
        if (lg.type[i] == S1_EV_ON && k < 8) seq[k++] = lg.note[i];
    // 4 notes in the 1/4: 67 63 60 67, every 1/16
    CHECK(k == 4 && seq[0] == 67 && seq[1] == 63 && seq[2] == 60 && seq[3] == 67, "arp down: %d notes %d %d %d %d", k,
          seq[0], seq[1], seq[2], seq[3]);
    int offs = 0;
    for (int i = 0; i < lg.n; i++) offs += lg.type[i] == S1_EV_OFF;
    CHECK(offs == 4, "every arp note ends: %d", offs);
    printf("seq101: arpeggiator down at 1/16 over a 1/4\n");
}

static void test_queue_and_chain(void) {
    fresh();
    s1_step_default(&S.pat[0][0].st[0], 60);
    s1_step_default(&S.pat[0][1].st[0], 61);
    s1_step_default(&S.pat[0][2].st[0], 62);
    S.lane[0].length = 4;
    s1_seq_start(&Q, &S, 0);
    log_t lg;
    run(0, 0, 2 * S1_TICKS, &lg);                            // into the first loop
    Q.lane[0].queued = 1;                                    // waits for the crossover
    run(0, 2 * S1_TICKS, 8 * S1_TICKS, &lg);
    CHECK(lg.n >= 1 && lg.note[0] == 61 && lg.t[0] == 4 * S1_TICKS, "queued slot on the loop: %d at %u", lg.note[0],
          lg.t[0]);
    // a chain 2 -> 0: one loop each
    S.lane[0].chain[0] = 2;
    S.lane[0].chain[1] = 0;
    S.lane[0].nchain = 2;
    s1_seq_init(&Q);
    s1_seq_start(&Q, &S, 0);
    run(0, 0, 16 * S1_TICKS, &lg);
    uint8_t seq[8];
    int k = 0;
    for (int i = 0; i < lg.n; i++)
        if (lg.type[i] == S1_EV_ON && k < 8) seq[k++] = lg.note[i];
    CHECK(k == 4 && seq[0] == 62 && seq[1] == 60 && seq[2] == 62 && seq[3] == 60, "chain: %d %d %d %d", seq[0], seq[1],
          seq[2], seq[3]);
    printf("seq101: a queued pattern on the loop crossover, a chain one loop each\n");
}

static void test_start_mid_bar_and_mute(void) {
    fresh();
    s1_step_default(&S.pat[0][0].st[8], 70);
    s1_seq_start(&Q, &S, 6 * S1_TICKS);                     // switched on at the 7th 1/16
    log_t lg;
    run(0, 6 * S1_TICKS, 8 * S1_TICKS + 2, &lg);   // into the note
    CHECK(lg.n >= 1 && lg.t[0] == 8 * S1_TICKS, "in step with the bar: on at %u", lg.n ? lg.t[0] : 0);
    s1_ev_t ev[8];
    CHECK(s1_seq_all_off(&Q, ev, 8) == 1 && ev[0].type == S1_EV_OFF, "all off");
    S.lane[0].mute = 1;
    s1_seq_start(&Q, &S, 0);
    run(0, 0, 16 * S1_TICKS, &lg);
    CHECK(lg.n == 0, "muted: %d", lg.n);
    printf("seq101: starts in step with the bar, all-off, mute\n");
}

static void test_edit_helpers_and_json(void) {
    fresh();
    s1_pat_t *p = &S.pat[0][0];
    s1_step_default(&p->st[2], 60);
    p->st[2].len = 4;
    s1_step_default(&p->st[8], 62);
    CHECK(s1_owner(p, 16, 4) == 2 && s1_owner(p, 16, 6) == -1 && s1_owner(p, 16, 2) == 2, "owner");
    CHECK(s1_fit_len(p, 16, 2, 16) == 6 && s1_fit_len(p, 10, 8, 16) == 2, "fit");
    S.lane[3].nchain = 2;
    S.lane[3].chain[0] = 5;
    S.lane[3].chain[1] = 7;
    S.lane[1].arp = 1;
    S.cv_5v = 1;
    s1_sanitise(&S);
    s1_step_default(&S.pat[3][7].st[15], 100);
    cJSON *j = s1_to_json(&S);
    static s1_state_t T;
    s1_defaults(&T);
    s1_from_json(j, &T);
    cJSON_Delete(j);
    CHECK(memcmp(&S.pat, &T.pat, sizeof S.pat) == 0, "patterns round trip");
    CHECK(T.lane[3].nchain == 2 && T.lane[3].chain[1] == 7 && T.lane[1].arp && T.cv_5v, "lanes round trip");
    char nm[8];
    s1_note_name(60, nm, sizeof nm);
    CHECK(!strcmp(nm, "C4"), "C4: %s", nm);
    printf("seq101: held-note owner, length fitting, JSON round trip\n");
}

int main(void) {
    test_notes_and_lengths();
    test_chord_and_lane_length();
    test_arp();
    test_queue_and_chain();
    test_start_mid_bar_and_mute();
    test_edit_helpers_and_json();
    printf(failures ? "\n%d FAILURES\n" : "\nall seq101 tests passed\n", failures);
    return failures ? 1 : 0;
}
