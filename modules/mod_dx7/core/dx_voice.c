// dx_voice.c - DX7 voice data: names, ranges, and reading DX7 banks
// (.syx). Not on the audio path, so it stays in flash.
#include <string.h>

#include "dx_dsp.h"

// --- voice data ---------------------------------------------------------------------------------

void dx_name(const dx_voice_t *v, char out[DX_NAME_LEN + 1]) {
    int n = 0;
    for (int i = 0; i < DX_NAME_LEN; i++) {
        uint8_t c = v->d[DXV_NAME + i];
        out[i] = c >= 32 && c < 127 ? (char)c : ' ';
        if (out[i] != ' ') n = i + 1;
    }
    out[n] = 0;
}

void dx_set_name(dx_voice_t *v, const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < DX_NAME_LEN; i++) v->d[DXV_NAME + i] = (uint8_t)(i < (int)n ? name[i] : ' ');
}

static uint8_t lim(uint8_t x, uint8_t max) { return x > max ? max : x; }

void dx_sanitise(dx_voice_t *v) {
    uint8_t *d = v->d;
    for (int op = 0; op < 6; op++) {
        uint8_t *o = d + DX_OP(op);
        static const uint8_t MAX[21] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                        3,  3,  7,  3,  7,  99, 1,  31, 99, 14};
        for (int i = 0; i < 21; i++) o[i] = lim(o[i], MAX[i]);
    }
    for (int i = DXV_PR1; i < DXV_ALGO; i++) d[i] = lim(d[i], 99);
    static const uint8_t G[DXV_NAME - DXV_ALGO] = {31, 7, 1, 99, 99, 99, 99, 1, 5, 7, 48};
    for (int i = DXV_ALGO; i < DXV_NAME; i++) d[i] = lim(d[i], G[i - DXV_ALGO]);
    for (int i = DXV_NAME; i < DXV_NAME + DX_NAME_LEN; i++)
        if (d[i] < 32 || d[i] > 126) d[i] = ' ';
}

void dx_unpack(const uint8_t *p, dx_voice_t *v) {
    uint8_t *o = v->d;
    for (int op = 0; op < 6; op++, p += 17) {
        memcpy(o, p, 11);   // rates, levels, breakpoint, depths
        o[11] = p[11] & 3;
        o[12] = (p[11] >> 2) & 3;
        o[13] = p[12] & 7;
        o[14] = p[13] & 3;
        o[15] = (p[13] >> 2) & 7;
        o[16] = p[14];
        o[17] = p[15] & 1;
        o[18] = (p[15] >> 1) & 31;
        o[19] = p[16];
        o[20] = (p[12] >> 3) & 15;
        o += 21;
    }
    memcpy(o, p, 8);        // pitch envelope
    o[8] = p[8] & 31;
    o[9] = p[9] & 7;
    o[10] = (p[9] >> 3) & 1;
    o[11] = p[10];
    o[12] = p[11];
    o[13] = p[12];
    o[14] = p[13];
    o[15] = p[14] & 1;
    o[16] = (p[14] >> 1) & 7;
    o[17] = (p[14] >> 4) & 7;
    o[18] = p[15];
    memcpy(o + 19, p + 16, DX_NAME_LEN);
    v->d[DX_VOICE_LEN] = 0;
    dx_sanitise(v);
}

int dx_parse_syx(const uint8_t *data, int len, dx_voice_t *out, int max) {
    const uint8_t *pl = data;
    int fmt = -1;
    if (len >= 8 && data[0] == 0xF0) {
        if (data[1] != 0x43) return 0;   // not Yamaha
        fmt = data[3];
        pl = data + 6;
        len -= 8;
    }
    int n = 0;
    if ((fmt == 9 || fmt < 0) && len >= 4096) {   // a 32-voice bank
        for (int i = 0; i < 32 && n < max; i++) dx_unpack(pl + i * 128, &out[n++]);
    } else if ((fmt == 0 || fmt < 0) && len >= DX_VOICE_LEN && max > 0) {   // one voice
        memcpy(out[0].d, pl, DX_VOICE_LEN);
        out[0].d[DX_VOICE_LEN] = 0;
        dx_sanitise(&out[0]);
        n = 1;
    }
    return n;
}

