// d9_comp.c - see d9_comp.h.
#include "d9_comp.h"

#include <math.h>

void d9_comp_init(d9_comp_t *c) {
    c->env = 0;
    c->gain = 1;
    c->gr = 1;
}

static float soft(float x) {   // linear to 0.9, then a knee into +-1
    float a = fabsf(x);
    if (a <= 0.9f) return x;
    float y = 0.9f + 0.1f * tanhf((a - 0.9f) * 10.0f);
    return x < 0 ? -y : y;
}

void d9_comp_run(d9_comp_t *c, float amount, float *l, float *r, int n, float sr) {
    if (amount <= 0.001f) {   // off: as it was
        c->gain = 1;
        c->gr = 1;
        return;
    }
    float pk = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(l[i]), b = fabsf(r[i]);
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    float rel = expf(-(float)n / (0.08f * sr));
    c->env = pk > c->env ? pk : c->env * rel + pk * (1.0f - rel);
    float thr = powf(10.0f, -24.0f * amount / 20.0f);
    float k = 1.0f - 1.0f / (1.0f + 5.0f * amount);   // 1 - 1/ratio
    float g = c->env > thr ? powf(thr / c->env, k) : 1.0f;
    float makeup = 0.89f * powf(thr, -k);
    float g0 = c->gain, g1 = g * makeup;
    for (int i = 0; i < n; i++) {
        float gi = g0 + (g1 - g0) * (float)(i + 1) / (float)n;
        l[i] = soft(l[i] * gi);
        r[i] = soft(r[i] * gi);
    }
    c->gain = g1;
    c->gr += (g - c->gr) * 0.2f;
}
