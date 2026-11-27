// s1_seq.c - see s1_seq.h.
#include "s1_seq.h"

#include <string.h>

void s1_seq_init(s1_seq_t *q) {
    memset(q, 0, sizeof *q);
    for (int l = 0; l < S1_LANES; l++) {
        q->lane[l].step = -1;
        q->lane[l].queued = -1;
        q->lane[l].chain_i = -1;
    }
}

void s1_seq_start(s1_seq_t *q, const s1_state_t *s, uint32_t t) {
    uint32_t rnd = q->rnd ? q->rnd : 0x2545F491u;
    int8_t queued[S1_LANES];
    for (int l = 0; l < S1_LANES; l++) queued[l] = q->lane[l].queued;
    memset(q, 0, sizeof *q);
    q->rnd = rnd;
    uint32_t n16 = t / S1_TICKS;
    for (int l = 0; l < S1_LANES; l++) {
        s1_lane_rt_t *r = &q->lane[l];
        int len = s->lane[l].length;
        // the step before the one due on this 1/16 (or on the next, mid-1/16)
        uint32_t next = t % S1_TICKS ? n16 + 1 : n16;
        r->step = (int16_t)((int)(next % (uint32_t)len) - 1);
        r->queued = queued[l];
        r->chain_i = -1;
    }
}

static uint32_t rnd(s1_seq_t *q) {
    q->rnd ^= q->rnd << 13;
    q->rnd ^= q->rnd >> 17;
    q->rnd ^= q->rnd << 5;
    return q->rnd;
}

static int put(s1_ev_t *out, int n, int max, int type, int lane, int note, int vel, int first) {
    if (n < max) out[n] = (s1_ev_t){(uint8_t)type, (uint8_t)lane, (uint8_t)note, (uint8_t)vel, (uint8_t)first};
    return n < max ? n + 1 : n;
}

static int release(s1_lane_rt_t *r, int lane, s1_ev_t *out, int n, int max) {
    for (int i = 0; i < r->nheld; i++) n = put(out, n, max, S1_EV_OFF, lane, r->held[i], 0, 0);
    r->nheld = 0;
    return n;
}

// the lane comes back round: a queued pattern, or the chain's next
static void crossover(s1_lane_t *ln, s1_lane_rt_t *r) {
    if (r->queued >= 0) {
        ln->slot = (uint8_t)r->queued;
        r->queued = -1;
        // a chain carries on from the queued slot when it's in it
        for (int i = 0; i < ln->nchain; i++)
            if (ln->chain[i] == ln->slot) r->chain_i = (int8_t)i;
    } else if (ln->nchain) {
        r->chain_i = (int8_t)((r->chain_i + 1) % ln->nchain);
        ln->slot = ln->chain[r->chain_i];
    }
}

static void arp_order(s1_seq_t *q, const s1_lane_t *ln, s1_lane_rt_t *r, const uint8_t *notes, int n) {
    r->arp_n = 0;
    switch (ln->arp_mode) {
    case ARP_DOWN:
        for (int i = n - 1; i >= 0; i--) r->arp_notes[r->arp_n++] = notes[i];
        break;
    case ARP_UPDOWN:   // 1 2 3 2: the ends once each
        for (int i = 0; i < n; i++) r->arp_notes[r->arp_n++] = notes[i];
        for (int i = n - 2; i > 0; i--) r->arp_notes[r->arp_n++] = notes[i];
        break;
    case ARP_RANDOM:
        for (int i = 0; i < n; i++) r->arp_notes[r->arp_n++] = notes[i];
        for (int i = n - 1; i > 0; i--) {   // shuffled once per step
            int j = (int)(rnd(q) % (uint32_t)(i + 1));
            uint8_t t = r->arp_notes[i];
            r->arp_notes[i] = r->arp_notes[j];
            r->arp_notes[j] = t;
        }
        break;
    default:   // UP, ORDER (the chord as built: low to high too)
        for (int i = 0; i < n; i++) r->arp_notes[r->arp_n++] = notes[i];
        break;
    }
    r->arp_i = 0;
}

int s1_seq_tick(s1_seq_t *q, s1_state_t *s, uint32_t t, s1_ev_t *out, int max) {
    int n = 0;
    for (int l = 0; l < S1_LANES; l++) {
        s1_lane_t *ln = &s->lane[l];
        s1_lane_rt_t *r = &q->lane[l];
        // what ends here
        if (r->nheld && t >= r->off_at) n = release(r, l, out, n, max);
        if (r->arp_n && t >= r->arp_end) r->arp_n = 0;
        if (t % S1_TICKS == 0) {   // a new 1/16 for this lane
            int len = ln->length;
            if (++r->step >= len || r->step < 0) r->step = 0;
            if (r->step == 0) crossover(ln, r);   // round again (or the very start)
            const s1_step_t *st = &s->pat[l][ln->slot].st[r->step];
            uint8_t notes[S1_MAX_NOTES];
            int nn = ln->mute ? 0 : s1_chord_notes(st, notes);
            if (s->scale != SC_CHROMATIC) {   // the scale: each note to its nearest, no repeats
                int k = 0;
                for (int i = 0; i < nn; i++) {
                    uint8_t q = (uint8_t)s1_quantise(notes[i], s->scale_root, s->scale);
                    if (!k || notes[k - 1] != q) notes[k++] = q;
                }
                nn = k;
            }
            if (nn) {
                // don't run past the lane's end
                int steps = st->len;
                if (r->step + steps > len) steps = len - r->step;
                uint32_t dur = (uint32_t)steps * S1_TICKS;
                n = release(r, l, out, n, max);   // a new note: a new gate
                r->arp_n = 0;
                if (ln->arp) {
                    arp_order(q, ln, r, notes, nn);
                    r->arp_vel = st->vel;
                    r->arp_next = t;
                    r->arp_end = t + dur;
                } else {
                    for (int i = 0; i < nn; i++) {
                        n = put(out, n, max, S1_EV_ON, l, notes[i], st->vel, notes[0]);
                        r->held[i] = notes[i];
                    }
                    r->nheld = (uint8_t)nn;
                    r->off_at = t + dur - 1;   // a tick short: the gate falls before the next
                }
            }
        }
        // the arpeggiator's next note
        if (r->arp_n && t >= r->arp_next && t < r->arp_end) {
            n = release(r, l, out, n, max);
            uint8_t note = r->arp_notes[r->arp_i];
            r->arp_i = (uint8_t)((r->arp_i + 1) % r->arp_n);
            n = put(out, n, max, S1_EV_ON, l, note, r->arp_vel, note);
            r->held[0] = note;
            r->nheld = 1;
            int rt = s1_rate_ticks(ln->arp_rate);
            uint32_t off = t + (uint32_t)(rt > 1 ? rt - 1 : 1);
            r->off_at = off < r->arp_end ? off : r->arp_end - 1;
            if (r->off_at <= t) r->off_at = t + 1;
            r->arp_next = t + (uint32_t)rt;
        }
    }
    return n;
}

int s1_seq_all_off(s1_seq_t *q, s1_ev_t *out, int max) {
    int n = 0;
    for (int l = 0; l < S1_LANES; l++) {
        n = release(&q->lane[l], l, out, n, max);
        q->lane[l].arp_n = 0;
    }
    return n;
}
