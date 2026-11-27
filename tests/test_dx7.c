// test_dx7.c - host tests for the DX7 voices.   make -C tests test
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dx_dsp.h"

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

static float run(dx_engine_t *e, int frames) {
    float peak = 0, l[48], r[48];
    for (int i = 0; i < frames; i += 48) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        dx_render(e, l, r, 48);
        for (int j = 0; j < 48; j++) {
            if (!isfinite(l[j])) finite = false;
            if (fabsf(l[j]) > peak) peak = fabsf(l[j]);
        }
    }
    return peak;
}

static int active(const dx_engine_t *e) {
    int n = 0;
    for (int i = 0; i < DX_VOICES; i++) n += e->vc[i].active;
    return n;
}

// a 32-voice bank's packing (VMEM), the inverse of dx_unpack
static void pack(const uint8_t *v, uint8_t *p) {
    for (int op = 0; op < 6; op++, v += 21, p += 17) {
        memcpy(p, v, 11);
        p[11] = (uint8_t)(v[11] | v[12] << 2);
        p[12] = (uint8_t)(v[13] | v[20] << 3);
        p[13] = (uint8_t)(v[14] | v[15] << 2);
        p[14] = v[16];
        p[15] = (uint8_t)(v[17] | v[18] << 1);
        p[16] = v[19];
    }
    memcpy(p, v, 8);
    p[8] = v[8];
    p[9] = (uint8_t)(v[9] | v[10] << 3);
    memcpy(p + 10, v + 11, 4);
    p[14] = (uint8_t)(v[15] | v[16] << 1 | v[17] << 4);
    p[15] = v[18];
    memcpy(p + 16, v + 19, 10);
}

int main(void) {
    static dx_engine_t e;
    // every factory voice: sounds, stays finite and in level, lets go
    for (int i = 0; i < DX_NFACTORY; i++) {
        dx_init(&e);
        memcpy(e.v.d, DX_FACTORY[i], DX_VOICE_LEN);
        for (int k = 0; k < 3; k++) dx_note_on(&e, 0x80, (uint8_t)(48 + 4 * k), 48 + 4 * k, 0.8f);
        float pk = run(&e, 44100);
        dx_all_off(&e, -1);
        run(&e, 44100 * 30);
        if (active(&e)) run(&e, 44100 * 90);   // EXPLOSION's tail is over a minute long
        char nm[DX_NAME_LEN + 1];
        dx_name(&e.v, nm);
        CHECK(pk > 0.005f && pk < 2.0f, "%s peak %.3f", nm, pk);
        // a carrier whose envelope ends above 0 (L4) holds there after the key
        // is let go, as on a DX7 (ST.HELENS): it ends when its voice is taken
        bool holds = false;
        uint8_t car = dx_carriers(e.v.d[DXV_ALGO]);
        for (int op = 0; op < 6; op++)
            if (((car >> op) & 1) && e.v.d[DX_OP(op) + DXO_L1 + 3] > 0 && e.v.d[DX_OP(op) + DXO_LEVEL] > 0) holds = true;
        CHECK(holds || active(&e) == 0, "%s still sounding after release", nm);
    }
    CHECK(finite, "non-finite output");
    printf("voices: %d sound, stay in level and release\n", DX_NFACTORY);

    // the algorithms' carriers
    CHECK(dx_carriers(0) == 0x05, "algorithm 1 carriers %02x", dx_carriers(0));
    CHECK(dx_carriers(4) == 0x15, "algorithm 5 carriers %02x", dx_carriers(4));
    CHECK(dx_carriers(31) == 0x3f, "algorithm 32 carriers %02x", dx_carriers(31));
    printf("algorithms: carriers as on the DX7's panel\n");

    // one sine carrier at ratio 1 plays A4 at 440 Hz; at ratio 2, 880 Hz
    for (int c = 1; c <= 2; c++) {
        dx_init(&e);
        memcpy(e.v.d, DX_FACTORY[0], DX_VOICE_LEN);
        e.v.d[DXV_ALGO] = 31;
        e.v.d[DXV_TRANSPOSE] = 24;
        e.v.d[DXV_LFO_PMD] = 0;
        e.v.d[DXV_LFO_AMD] = 0;
        for (int i = 0; i < 4; i++) e.v.d[DXV_PL1 + i] = 50;
        for (int op = 0; op < 6; op++) {
            uint8_t *o = e.v.d + DX_OP(op);
            o[DXO_LEVEL] = op == 0 ? 99 : 0;
            o[DXO_MODE] = 0;
            o[DXO_COARSE] = (uint8_t)c;
            o[DXO_FINE] = 0;
            o[DXO_DETUNE] = 7;
            for (int k = 0; k < 4; k++) o[DXO_R1 + k] = 99, o[DXO_L1 + k] = 99;
        }
        dx_note_on(&e, 0x80, 69, 69, 1);
        float l[48], r[48], prev = 0;
        int cross = 0, first = -1, last = 0, t = 0;
        for (int b = 0; b < 44100 / 48; b++) {
            memset(l, 0, sizeof l);
            memset(r, 0, sizeof r);
            dx_render(&e, l, r, 48);
            for (int j = 0; j < 48; j++, t++) {
                if (b > 50 && prev < 0 && l[j] >= 0) {
                    if (first < 0) first = t;
                    last = t;
                    cross++;
                }
                prev = l[j];
            }
        }
        float hz = (cross - 1) * 44100.0f / (float)(last - first);
        CHECK(fabsf(hz - 440.0f * c) < 1.0f, "ratio %d: %.2f Hz", c, hz);
    }
    printf("pitch: ratio 1 is A4 = 440 Hz\n");

    // a bank (.syx) of 32 voices reads back as they were
    static uint8_t syx[4104];
    syx[0] = 0xF0, syx[1] = 0x43, syx[2] = 0, syx[3] = 9, syx[4] = 0x20, syx[5] = 0;
    for (int i = 0; i < 32; i++) pack(DX_FACTORY[i + 32], syx + 6 + i * 128);
    syx[4102] = 0, syx[4103] = 0xF7;
    static dx_voice_t bank[32];
    int n = dx_parse_syx(syx, sizeof syx, bank, 32);
    CHECK(n == 32, "bank voices %d", n);
    int same = 0;
    for (int i = 0; i < n; i++) same += !memcmp(bank[i].d, DX_FACTORY[i + 32], DX_VOICE_LEN);
    CHECK(same == 32, "%d of 32 voices the same after a bank round trip", same);
    CHECK(dx_parse_syx(syx, 100, bank, 32) == 0, "a short file is no bank");
    printf("syx: a 32-voice bank reads back exactly\n");

    printf(failures ? "test_dx7: %d FAILED\n" : "test_dx7: all passed\n", failures);
    return failures != 0;
}
