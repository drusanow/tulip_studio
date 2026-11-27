// dx_dsp.c - DX7 voices (see dx_dsp.h).
#include "dx_dsp.h"

#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define DX_HOT IRAM_ATTR
#define DX_RAM DRAM_ATTR
#else
#define DX_HOT
#define DX_RAM
#endif

#define SINE_N 1024
static DX_RAM float s_sine[SINE_N + 1];

// The 32 algorithms, as the MSFA engine has them: per operator, OP6 first,
// where it reads (bus 1 / 2), where it writes (bus 1 / 2 / the output), and
// whether it adds there or overwrites; 0xc0 marks the feedback operator.
enum { OUT1 = 1, OUT2 = 2, ADD = 4, IN1 = 16, IN2 = 32, FB = 0xc0 };
static DX_RAM const uint8_t ALGO[32][6] = {
    {0xc1, 0x11, 0x11, 0x14, 0x01, 0x14}, {0x01, 0x11, 0x11, 0x14, 0xc1, 0x14},
    {0xc1, 0x11, 0x14, 0x01, 0x11, 0x14}, {0x41, 0x11, 0x94, 0x01, 0x11, 0x14},
    {0xc1, 0x14, 0x01, 0x14, 0x01, 0x14}, {0x41, 0x94, 0x01, 0x14, 0x01, 0x14},
    {0xc1, 0x11, 0x05, 0x14, 0x01, 0x14}, {0x01, 0x11, 0xc5, 0x14, 0x01, 0x14},
    {0x01, 0x11, 0x05, 0x14, 0xc1, 0x14}, {0x01, 0x05, 0x14, 0xc1, 0x11, 0x14},
    {0xc1, 0x05, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x05, 0x14, 0xc1, 0x14},
    {0xc1, 0x05, 0x05, 0x14, 0x01, 0x14}, {0xc1, 0x05, 0x11, 0x14, 0x01, 0x14},
    {0x01, 0x05, 0x11, 0x14, 0xc1, 0x14}, {0xc1, 0x11, 0x02, 0x25, 0x05, 0x14},
    {0x01, 0x11, 0x02, 0x25, 0xc5, 0x14}, {0x01, 0x11, 0x11, 0xc5, 0x05, 0x14},
    {0xc1, 0x14, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x14, 0xc1, 0x14, 0x14},
    {0x01, 0x14, 0x14, 0xc1, 0x14, 0x14}, {0xc1, 0x14, 0x14, 0x14, 0x01, 0x14},
    {0xc1, 0x14, 0x14, 0x01, 0x14, 0x04}, {0xc1, 0x14, 0x14, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x14, 0x04, 0x04, 0x04}, {0xc1, 0x05, 0x14, 0x01, 0x14, 0x04},
    {0x01, 0x05, 0x14, 0xc1, 0x14, 0x04}, {0x04, 0xc1, 0x11, 0x14, 0x01, 0x14},
    {0xc1, 0x14, 0x01, 0x14, 0x04, 0x04}, {0x04, 0xc1, 0x11, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x04, 0x04, 0x04, 0x04}, {0xc4, 0x04, 0x04, 0x04, 0x04, 0x04},
};

uint8_t dx_carriers(int algo) {
    uint8_t m = 0;
    for (int i = 0; i < 6; i++)
        if (!(ALGO[algo & 31][i] & (OUT1 | OUT2))) m |= (uint8_t)(1u << (5 - i));
    return m;
}

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static inline float fexp2(float x) {
    x = clampf(x, -60.0f, 60.0f);
    int i = (int)x;
    if (x < (float)i) i--;
    float f = x - (float)i;
    float p = 1.0f + f * (0.6951786f + f * (0.2261170f + f * 0.0782177f));
    union {
        float f;
        int32_t i;
    } u = {p};
    u.i += (int32_t)((uint32_t)i << 23);
    return u.f;
}

// --- the operator envelope (MSFA's, in log2 gain, Q24) -----------------------------------------

static int scale_out(int l) {
    static const uint8_t lut[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
    return l >= 20 ? 28 + l : lut[l];
}

static DX_HOT void env_advance(dx_env_t *e, int ix, int n) {
    e->ix = (uint8_t)ix;
    if (ix >= 4) return;
    int actual = (scale_out(e->levels[ix]) >> 1 << 6) + e->outlevel - 4256;
    if (actual < 16) actual = 16;
    e->target = actual << 16;
    e->rising = e->target > e->level;
    int q = (e->rates[ix] * 41) >> 6;
    q += e->rate_scale;
    if (q > 63) q = 63;
    int32_t inc64 = (4 + (q & 3)) << (2 + 6 + (q >> 2));   // per 64 frames
    e->inc = (int32_t)(((int64_t)inc64 * n) >> 6);
}

static DX_HOT int32_t env_step(dx_env_t *e, int n) {
    if (e->ix < 3 || (e->ix < 4 && !e->down)) {
        if (e->rising) {
            const int32_t jump = 1716 << 16;   // attacks jump up out of silence
            if (e->level < jump) e->level = jump;
            e->level += (((17 << 24) - e->level) >> 24) * e->inc;
            if (e->level >= e->target) {
                e->level = e->target;
                env_advance(e, e->ix + 1, n);
            }
        } else {
            e->level -= e->inc;
            if (e->level <= e->target) {
                e->level = e->target;
                env_advance(e, e->ix + 1, n);
            }
        }
    }
    return e->level;
}

// keyboard level scaling, rate scaling and velocity (MSFA)
static int scale_curve(int group, int depth, int curve) {
    static const uint8_t exp_scale[33] = {0,  1,  2,  3,  4,  5,  6,  7,   8,   9,   11,  14,  16,  19,  23,  27,  33,
                                          39, 47, 56, 66, 80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250};
    int s;
    if (curve == 0 || curve == 3)
        s = (group * depth * 329) >> 12;
    else
        s = (exp_scale[group > 32 ? 32 : group] * depth * 329) >> 15;
    return curve < 2 ? -s : s;
}

static int scale_level(int note, const uint8_t *o) {
    int off = note - o[DXO_BP] - 17;
    if (off >= 0) return scale_curve((off + 1) / 3, o[DXO_RDEPTH], o[DXO_RCURVE]);
    return scale_curve(-(off - 1) / 3, o[DXO_LDEPTH], o[DXO_LCURVE]);
}

static int scale_vel(int vel, int sens) {
    static const uint8_t vd[64] = {0,   70,  86,  97,  106, 114, 121, 126, 132, 138, 142, 148, 152,
                                   156, 160, 163, 166, 170, 173, 174, 178, 181, 184, 186, 189, 190,
                                   194, 196, 198, 200, 202, 205, 206, 209, 211, 214, 216, 218, 220,
                                   222, 224, 225, 227, 229, 230, 232, 233, 235, 237, 238, 240, 241,
                                   242, 243, 244, 246, 246, 248, 249, 250, 251, 252, 253, 254};
    vel = vel < 0 ? 0 : vel > 127 ? 127 : vel;
    return ((sens * (vd[vel >> 1] - 239) + 7) >> 3) << 4;
}

static int scale_rate(int note, int sens) {
    int x = note / 3 - 7;
    x = x < 0 ? 0 : x > 31 ? 31 : x;
    return (sens * x) >> 3;
}

// --- the pitch envelope (MSFA's): Q24 octaves -------------------------------------------------

static DX_RAM const uint8_t P_RATE[100] = {
    1,  2,  3,  3,  4,  4,  5,  5,  6,  6,  7,  7,  8,  8,  9,  9,  10,  10,  11,  11,  12,  12,  13,  13,  14,
    14, 15, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,  30,  31,  33,  34,  36,  37,  38,  39,
    41, 42, 44, 46, 47, 49, 51, 53, 54, 56, 58, 60, 62, 64, 66, 68, 70,  72,  74,  76,  79,  82,  85,  88,  91,
    94, 98, 102, 106, 110, 115, 120, 125, 130, 135, 141, 147, 153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255};
static DX_RAM const int8_t P_LEVEL[100] = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43, -41, -39, -37, -35, -33, -32, -31,
    -30,  -29,  -28,  -27, -26, -25, -24, -23, -22, -21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11,
    -10,  -9,   -8,   -7,  -6,  -5,  -4,  -3,  -2,  -1,  0,   1,   2,   3,   4,   5,   6,   7,   8,   9,
    10,   11,   12,   13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,   31,   32,   33,  34,  35,  38,  40,  43,  46,  49,  53,  58,  65,  73,  82,  92,  103, 115, 127};

static DX_HOT void pitch_advance(dx_vc_t *c, const uint8_t *d, int ix, int n) {
    c->pitch_ix = (uint8_t)ix;
    if (ix >= 4) return;
    c->pitch_target = P_LEVEL[d[DXV_PL1 + ix]] * (1 << 19);
    c->pitch_rising = c->pitch_target > c->pitch_level;
    // MSFA's unit: (1 << 24) / (21.3 * sr) per frame
    c->pitch_inc = (int32_t)(P_RATE[d[DXV_PR1 + ix]] * (float)n * 16777216.0f / (21.3f * DX_SR));
}

static DX_HOT float pitch_step(dx_vc_t *c, const uint8_t *d, int n) {
    if (c->pitch_ix < 3 || (c->pitch_ix < 4 && !c->on)) {
        if (c->pitch_rising) {
            c->pitch_level += c->pitch_inc;
            if (c->pitch_level >= c->pitch_target) {
                c->pitch_level = c->pitch_target;
                pitch_advance(c, d, c->pitch_ix + 1, n);
            }
        } else {
            c->pitch_level -= c->pitch_inc;
            if (c->pitch_level <= c->pitch_target) {
                c->pitch_level = c->pitch_target;
                pitch_advance(c, d, c->pitch_ix + 1, n);
            }
        }
    }
    return (float)c->pitch_level * (1.0f / 16777216.0f);
}

// --- engine ------------------------------------------------------------------------------------

void dx_init(dx_engine_t *e) {
    memset(e, 0, sizeof *e);
    for (int i = 0; i <= SINE_N; i++) s_sine[i] = sinf(6.2831853f * (float)i / SINE_N);
    memcpy(e->v.d, DX_FACTORY[0], DX_VOICE_LEN);
    e->seed = 0x2545F491u;
    e->bend_semis = 2;
    e->level = 1;
}

static dx_vc_t *pick(dx_engine_t *e, uint8_t src, uint8_t key) {
    dx_vc_t *best = NULL;
    for (int i = 0; i < DX_VOICES; i++)
        if (e->vc[i].active && e->vc[i].src == src && e->vc[i].key == key) return &e->vc[i];
    for (int i = 0; i < DX_VOICES; i++)
        if (!e->vc[i].active) return &e->vc[i];
    for (int i = 0; i < DX_VOICES; i++)
        if (!e->vc[i].on && (!best || e->vc[i].age < best->age)) best = &e->vc[i];
    if (best) return best;
    best = &e->vc[0];
    for (int i = 1; i < DX_VOICES; i++)
        if (e->vc[i].age < best->age) best = &e->vc[i];
    return best;
}

void dx_note_on(dx_engine_t *e, uint8_t src, uint8_t key, int note, float vel) {
    const uint8_t *d = e->v.d;
    dx_vc_t *c = pick(e, src, key);
    int nt = note + d[DXV_TRANSPOSE] - 24;
    nt = nt < 0 ? 0 : nt > 127 ? 127 : nt;
    int v127 = (int)(vel * 127.0f + 0.5f);
    bool fresh = !c->active;
    c->src = src;
    c->key = key;
    c->note = (uint8_t)nt;
    c->base_hz = 440.0f * fexp2(((float)nt - 69.0f) / 12.0f);
    c->on = c->active = true;
    c->age = ++e->clock;
    for (int op = 0; op < 6; op++) {
        const uint8_t *o = d + DX_OP(op);
        dx_op_t *p = &c->op[op];
        dx_env_t *en = &p->env;
        int ol = scale_out(o[DXO_LEVEL]) + scale_level(nt, o);
        ol = ol < 0 ? 0 : ol > 127 ? 127 : ol;
        ol = (ol << 5) + scale_vel(v127, o[DXO_VEL]);
        en->outlevel = ol < 0 ? 0 : ol;
        memcpy(en->rates, o + DXO_R1, 4);
        memcpy(en->levels, o + DXO_L1, 4);
        en->rate_scale = (int8_t)scale_rate(nt, o[DXO_RATE_SCALE]);
        if (fresh) {
            en->level = 0;
            p->gain = p->last_gain = 0;
            p->fb[0] = p->fb[1] = 0;
        }
        if (fresh || d[DXV_OSC_SYNC]) p->ph = 0.25f;   // cosine phase, as the DX7
        en->down = true;
        env_advance(en, 0, 48);
    }
    if (fresh) c->pitch_level = P_LEVEL[d[DXV_PL1 + 3]] * (1 << 19);
    pitch_advance(c, d, 0, 48);
    if (fresh || d[DXV_LFO_SYNC]) c->lfo_amt = 0;
    if (d[DXV_LFO_SYNC]) e->lfo_ph = 0;
}

static void release(dx_engine_t *e, dx_vc_t *c) {
    c->on = false;
    for (int op = 0; op < 6; op++) {
        c->op[op].env.down = false;
        env_advance(&c->op[op].env, 3, 48);
    }
    pitch_advance(c, e->v.d, 3, 48);
}

void dx_note_off(dx_engine_t *e, uint8_t src, uint8_t key) {
    for (int i = 0; i < DX_VOICES; i++) {
        dx_vc_t *c = &e->vc[i];
        if (c->on && c->src == src && (c->key == key || src < 0x80)) release(e, c);
    }
}

void dx_all_off(dx_engine_t *e, int src) {
    for (int i = 0; i < DX_VOICES; i++)
        if (e->vc[i].on && (src < 0 || e->vc[i].src == src)) release(e, &e->vc[i]);
}

static float lfo_hz(int s) {   // TX802 measurements (AMY)
    if (s == 0) return 0.064f;
    if (s <= 64) return (float)s / 6.0f;
    if (s <= 85) return (float)s - 64.0f * 5.0f / 6.0f;
    return 31.67f + ((float)s - 85.0f) * 1.33f;
}

static DX_HOT void render_op(dx_op_t *p, const float *in, float *out, bool add, float fb_scale, int n) {
    float g = p->last_gain, dg = (p->gain - p->last_gain) / (float)n;
    float ph = p->ph, inc = p->inc;
    if (p->gain < 1e-5f && p->last_gain < 1e-5f) {   // silent: just keep time
        if (!add) memset(out, 0, sizeof(float) * (size_t)n);
        ph += inc * (float)n;
        p->ph = ph - (float)(int)ph;
        p->fb[0] = p->fb[1] = 0;
        return;
    }
    float f0 = p->fb[0], f1 = p->fb[1];
    for (int i = 0; i < n; i++) {
        float t = ph;
        if (in) t += in[i];
        if (fb_scale > 0) t += (f0 + f1) * fb_scale;
        int ti = (int)t;
        if (t < (float)ti) ti--;
        t = (t - (float)ti) * SINE_N;
        int k = (int)t;
        float fr = t - (float)k;
        if (k >= SINE_N) k -= SINE_N;   // (rounding at the wrap)
        float s = (s_sine[k] + (s_sine[k + 1] - s_sine[k]) * fr) * g;
        f0 = f1;
        f1 = s;
        out[i] = add ? out[i] + s : s;
        g += dg;
        ph += inc;
        if (ph >= 1.0f) ph -= 1.0f;
    }
    p->fb[0] = f0;
    p->fb[1] = f1;
    p->ph = ph;
}

DX_HOT void dx_render(dx_engine_t *e, float *l, float *r, int n) {
    const uint8_t *d = e->v.d;
    if (n > 64) n = 64;
    float bus1[64], bus2[64], tmp[64], out[64];
    int algo = d[DXV_ALGO] & 31;
    const uint8_t *alg = ALGO[algo];
    uint8_t car = dx_carriers(algo);
    float fb_scale = d[DXV_FEEDBACK] ? 1.0f / (float)(1 << (9 - d[DXV_FEEDBACK])) : 0.0f;

    // the LFO (one for every voice, as the DX7's)
    e->lfo_ph += lfo_hz(d[DXV_LFO_SPEED]) * (float)n / DX_SR;
    if (e->lfo_ph >= 1.0f) {
        e->lfo_ph -= 1.0f;
        uint32_t s = e->seed;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        e->seed = s;
        e->lfo_sh = (float)(int32_t)s * (1.0f / 2147483648.0f);
    }
    float ph = e->lfo_ph, lfo;
    switch (d[DXV_LFO_WAVE]) {
    case 1: lfo = 1.0f - 2.0f * ph; break;                 // saw down
    case 2: lfo = 2.0f * ph - 1.0f; break;                 // saw up
    case 3: lfo = ph < 0.5f ? 1.0f : -1.0f; break;         // square
    case 4: lfo = s_sine[(int)(ph * SINE_N)]; break;       // sine
    case 5: lfo = e->lfo_sh; break;                         // sample and hold
    default: lfo = 4.0f * fabsf(ph - 0.5f) - 1.0f; break;  // triangle
    }
    int pms = d[DXV_PMS];
    float pmd = pms ? 0.6f * fexp2(0.7655347f * (float)(pms - 1)) * (float)d[DXV_LFO_PMD] / 1188.0f : 0.0f;  // 1.7^(pms-1)
    float amd = d[DXV_LFO_AMD] ? fexp2(((float)d[DXV_LFO_AMD] - 99.0f) / 8.0f) : 0.0f;
    float dl = (float)d[DXV_LFO_DELAY] / 99.0f;
    float delay_s = 6.0f * dl * dl * dl;
    float lfo_rise = delay_s > 0.001f ? (float)n / (delay_s * DX_SR) : 1.0f;
    float bend = e->bend * e->bend_semis / 12.0f;
    static const float AMS[4] = {0, 0.25f, 0.5f, 1.0f};
    float vol = e->level * 0.06f;

    for (int vi = 0; vi < DX_VOICES; vi++) {
        dx_vc_t *c = &e->vc[vi];
        if (!c->active) continue;
        c->lfo_amt = clampf(c->lfo_amt + lfo_rise, 0, 1);
        float vlfo = lfo * c->lfo_amt;
        float pit = pitch_step(c, d, n) + pmd * vlfo + bend;
        float ratio_hz = c->base_hz * fexp2(pit);
        float lfo_uni = 0.5f * (vlfo + 1.0f);
        bool alive = false;
        for (int op = 0; op < 6; op++) {
            dx_op_t *p = &c->op[op];
            const uint8_t *o = d + DX_OP(op);
            int32_t lv = env_step(&p->env, n);
            float g = fexp2((float)lv * (1.0f / 16777216.0f) - 14.0f);
            if (amd > 0) g *= 1.0f - amd * AMS[o[DXO_AMS]] * lfo_uni;
            p->last_gain = p->gain;
            p->gain = g;
            float hz;
            if (o[DXO_MODE]) {   // fixed: 1, 10, 100, 1000 Hz times 10^(fine/100)
                hz = fexp2(3.3219281f * ((float)(o[DXO_COARSE] & 3) + (float)o[DXO_FINE] / 100.0f));
            } else {
                float co = o[DXO_COARSE] ? (float)o[DXO_COARSE] : 0.5f;
                hz = ratio_hz * co * (1.0f + (float)o[DXO_FINE] / 100.0f) *
                     fexp2(((float)o[DXO_DETUNE] - 7.0f) / 1200.0f);
            }
            p->inc = clampf(hz / DX_SR, 0.0f, 0.49f);
            if (((car >> op) & 1) && (g > 1e-4f || p->env.ix < 3)) alive = true;
        }
        memset(out, 0, sizeof(float) * (size_t)n);
        for (int i = 0; i < 6; i++) {   // OP6 first
            uint8_t f = alg[i];
            dx_op_t *p = &c->op[5 - i];
            const float *in = (f & IN1) ? bus1 : (f & IN2) ? bus2 : NULL;
            float *dst;
            bool add = f & ADD;
            if (f & OUT1)
                dst = (in == bus1 && !add) ? tmp : bus1;
            else if (f & OUT2)
                dst = bus2;
            else
                dst = out, add = true;
            render_op(p, in, dst, add, (f & FB) == FB ? fb_scale : 0.0f, n);
            if (dst == tmp) memcpy(bus1, tmp, sizeof(float) * (size_t)n);
        }
        for (int i = 0; i < n; i++) {
            l[i] += out[i] * vol;
            r[i] += out[i] * vol;
        }
        if (!alive) c->active = false;
    }
}
