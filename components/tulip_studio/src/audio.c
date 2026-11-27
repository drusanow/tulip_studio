// audio.c - the Studio audio engine: every running app's render() mixed
// into one stereo stream to the Tulip's I2S DAC (44.1 kHz, 16 bit).
//
// DMA ring of 8 x 48 frames (8.7 ms), task on core 1 below the rt task.
// Each block is rendered just before it's queued, so its first frame
// reaches the DAC about 8 ms later - and the engine knows WHEN, in clock
// microseconds: every block carries t0, the clock time its frame 0 is
// heard. That mapping is a phase-locked estimate (below), so an app can put
// a drum hit on the exact sample where the master clock says the step is.
//
// Sample clock vs clock: i2s_channel_write() returns the moment a DMA
// buffer has been played and refilled; at that instant the next block will
// start after the DESC buffers now queued. Each block measures that, and a
// slow first-order loop (1/16 per block) follows it: jitter in the task's
// wake-up is filtered out, and any rate difference between the I2S clock
// and the 1 MHz timer is tracked (it settles to a few us).
//
// Two cores: a worker on core 0 (above the UI) renders about half the apps
// each block - the costliest first, each to whichever core has less so
// far, by what each app's render took last time - while this task renders
// the rest. Then this task mixes them all. An app is only ever rendered by
// one task at a time, and the two meet once per block.
#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "studio_priv.h"
#include "tulip/board.h"

static const char *TAG = "audio";

#define DESC        8   // deeper than the 4 it was: slack for a busy moment
#define B           STUDIO_BLOCK
#define AUDIO_CORE  1
#define AUDIO_PRIO  (configMAX_PRIORITIES - 4)
#define RELOCK_US   2000     // an error bigger than this: re-lock at once

static i2s_chan_handle_t s_tx;
static float s_l[B], s_r[B];
// each app's block, before its fader (rendered on either core)
static float s_ml[STUDIO_MAX_MODULES][B], s_mr[STUDIO_MAX_MODULES][B];
static float s_cost[STUDIO_MAX_MODULES];          // its render time, us (smoothed)
static volatile int s_load_mod[STUDIO_MAX_MODULES];   // ... as % of the block
static uint8_t s_on_core0[STUDIO_MAX_MODULES];    // this block: the worker renders it
static TaskHandle_t s_audio_task, s_worker;
static const studio_block_t *volatile s_blk;      // the block being rendered

static void render_one(int i, const studio_block_t *blk) {
    memset(s_ml[i], 0, sizeof s_ml[i]);
    memset(s_mr[i], 0, sizeof s_mr[i]);
    int64_t t = esp_timer_get_time();
    g_mods[i]->render(s_ml[i], s_mr[i], blk);
    float us = (float)(esp_timer_get_time() - t);
    s_cost[i] += (us - s_cost[i]) * 0.05f;
}

static bool wants_render(int i) { return g_enabled[i] && g_mods[i]->render; }

static void worker_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const studio_block_t *blk = s_blk;
        for (int i = 0; i < g_nmods; i++)
            if (s_on_core0[i]) render_one(i, blk);
        xTaskNotifyGive(s_audio_task);
    }
}

// split this block's apps between the cores: costliest first, each to the
// core with less so far (core 1 also mixes: it starts a little behind)
static int split(void) {
    float c0 = 0, c1 = 60.0f;
    bool done[STUDIO_MAX_MODULES] = {0};
    int n0 = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < g_nmods; i++)
            if (!done[i] && wants_render(i) && (best < 0 || s_cost[i] > s_cost[best])) best = i;
        if (best < 0) break;
        done[best] = true;
        bool zero = s_worker && c0 < c1;
        s_on_core0[best] = zero;
        if (zero) {
            c0 += s_cost[best] + 5.0f;
            n0++;
        } else {
            c1 += s_cost[best] + 5.0f;
        }
    }
    for (int i = 0; i < g_nmods; i++)
        if (!done[i]) s_on_core0[i] = 0;
    return n0;
}
static volatile float s_mgain[STUDIO_MAX_MODULES];
static volatile float s_peak[STUDIO_MAX_MODULES];
// loudness: the sum of squares since the UI last asked, and its frames
static volatile float s_sq[STUDIO_MAX_MODULES];
static volatile uint32_t s_sqn[STUDIO_MAX_MODULES];
static int16_t s_out[B * 2];
static volatile int s_load_pct;
static volatile uint32_t s_underruns;
static volatile float s_gain;
static volatile uint32_t s_blocks;    // blocks handed to the DAC so far
static volatile esp_err_t s_err;      // why audio couldn't start, if it couldn't
static const char *s_err_at = "";

static float vol_gain(int v) {
    if (v <= 0) return 0.0f;
    return powf(10.0f, (float)(v - 10) * 3.0f / 20.0f);   // 3 dB per step, 10 = 0 dB
}

void studio_set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    g_cfg.volume = (uint8_t)v;
    s_gain = vol_gain(v);
    cfg_changed();
}

int studio_volume(void) { return g_cfg.volume; }

// fader: 0..100 %, a dB taper with STUDIO_MIX_UNITY = 0 dB and 0 = off
static float mix_gain(int m) {
    if (g_mix.mute & (1u << m)) return 0.0f;
    int v = g_mix.level[m];
    if (v <= 0) return 0.0f;
    // -40 dB at 1 %, 0 dB at UNITY, +2 dB at 100 %
    float db = v >= STUDIO_MIX_UNITY
                   ? 2.0f * (v - STUDIO_MIX_UNITY) / (100 - STUDIO_MIX_UNITY)
                   : -40.0f * (float)(STUDIO_MIX_UNITY - v) / (STUDIO_MIX_UNITY - 1);
    return powf(10.0f, db / 20.0f);
}

static void mix_update(int m) { s_mgain[m] = mix_gain(m); }

void studio_mix_set_level(int m, int pct) {
    if (m < 0 || m >= STUDIO_MAX_MODULES) return;
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    if (g_mix.level[m] == pct) return;
    g_mix.level[m] = (uint8_t)pct;
    mix_update(m);
    cfg_changed();
}

int studio_mix_level(int m) { return m >= 0 && m < STUDIO_MAX_MODULES ? g_mix.level[m] : 0; }

void studio_mix_set_mute(int m, bool on) {
    if (m < 0 || m >= STUDIO_MAX_MODULES) return;
    if (on)
        g_mix.mute |= (uint8_t)(1u << m);
    else
        g_mix.mute &= (uint8_t)~(1u << m);
    mix_update(m);
    cfg_changed();
}

bool studio_mix_muted(int m) { return m >= 0 && m < STUDIO_MAX_MODULES && (g_mix.mute & (1u << m)); }

float studio_mix_peak(int m) {
    if (m < 0 || m >= STUDIO_MAX_MODULES) return 0;
    float p = s_peak[m];
    s_peak[m] = 0;   // (a race with the audio task only loses one block's peak)
    return p;
}
float studio_mix_rms(int m) {
    if (m < 0 || m >= STUDIO_MAX_MODULES || !s_sqn[m]) return 0;
    float r = sqrtf(s_sq[m] / (float)s_sqn[m]);
    s_sq[m] = 0;   // (a race with the audio task only loses one block)
    s_sqn[m] = 0;
    return r;
}

int studio_audio_load_pct(void) { return s_load_pct; }
int studio_mix_load(int m) { return m >= 0 && m < STUDIO_MAX_MODULES ? s_load_mod[m] : 0; }
uint32_t studio_audio_underruns(void) { return s_underruns; }

// Shown on screen: no sound is otherwise a silent failure. NULL = all well.
const char *studio_audio_problem(void) {
    static char b[64];
    static uint32_t last_blocks;
    static int64_t last_change;
    if (s_err != ESP_OK) {
        snprintf(b, sizeof b, "AUDIO OFF: %s %s", s_err_at, esp_err_to_name(s_err));
        return b;
    }
    int64_t now = esp_timer_get_time();
    uint32_t n = s_blocks;
    if (n != last_blocks || !last_change) {
        last_blocks = n;
        last_change = now;
        return NULL;
    }
    if (now - last_change > 500000) {   // ~450 blocks should have gone by
        snprintf(b, sizeof b, "AUDIO STALLED (%u blocks)", (unsigned)n);
        return b;
    }
    return NULL;
}

static bool IRAM_ATTR on_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx) {
    s_underruns++;
    return false;
}

// Gentle limiter: linear up to 0.8, then a soft knee into +-1.
static inline float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float z = (a - 0.8f) * 5.0f;
    float t = z >= 3.0f ? 1.0f : z * (27.0f + z * z) / (27.0f + 9.0f * z * z);
    float y = 0.8f + 0.2f * t;
    return x < 0 ? -y : y;
}

static void audio_task(void *arg) {
    // frame-accurate clock mapping: frame f plays at base_t + (f - base_f) / SR
    uint64_t frame = 0, base_f = 0;
    int64_t base_t = studio_now_us() + DESC * B * 1000000LL / STUDIO_SR;
    float base_frac = 0;
    float load = 0;
    for (;;) {
        studio_block_t blk = {
            .n = B,
            .t0 = base_t + (int64_t)((frame - base_f) * 1000000ULL / STUDIO_SR),
            .frame = frame,
        };
        int64_t c0 = studio_now_us();
        memset(s_l, 0, sizeof s_l);
        memset(s_r, 0, sizeof s_r);
        // each app renders on its own (on one core or the other) ...
        int n0 = split();
        if (n0) {
            s_blk = &blk;
            xTaskNotifyGive(s_worker);
        }
        for (int i = 0; i < g_nmods; i++)
            if (wants_render(i) && !s_on_core0[i]) render_one(i, &blk);
        if (n0) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // the worker's share is done
        // ... then goes through its fader into the mix
        for (int i = 0; i < g_nmods; i++) {
            if (!wants_render(i)) {
                s_load_mod[i] = 0;
                continue;
            }
            s_load_mod[i] = (int)(s_cost[i] * (100.0f * STUDIO_SR / 1e6f / B) + 0.5f);
            float mg = s_mgain[i], pk = s_peak[i], sq = 0;
            for (int k = 0; k < B; k++) {
                float a = s_ml[i][k] * mg, b = s_mr[i][k] * mg;
                s_l[k] += a;
                s_r[k] += b;
                float x = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
                if (x > pk) pk = x;
                sq += 0.5f * (a * a + b * b);
            }
            s_peak[i] = pk;
            s_sq[i] += sq;
            s_sqn[i] += B;
        }
        float g = s_gain;
        for (int i = 0; i < B; i++) {
            s_out[2 * i] = (int16_t)(soft_clip(s_l[i] * g) * 32000.0f);
            s_out[2 * i + 1] = (int16_t)(soft_clip(s_r[i] * g) * 32000.0f);
        }
        int64_t c1 = studio_now_us();
        load += ((float)(c1 - c0) * (100.0f * STUDIO_SR / 1e6f / B) - load) * 0.02f;
        s_load_pct = (int)(load + 0.5f);

        size_t written;
        esp_err_t e = i2s_channel_write(s_tx, s_out, sizeof s_out, &written, pdMS_TO_TICKS(100));
        if (e != ESP_OK) {
            // the DAC isn't taking data: say so, and don't spin on it
            static uint32_t nerr;
            if (nerr++ % 50 == 0) ESP_LOGE(TAG, "I2S write: %s", esp_err_to_name(e));
            vTaskDelay(1);
            continue;
        }
        int64_t tw = studio_now_us();
        frame += B;
        s_blocks++;

        // when the next block (frame) will be heard, measured vs predicted
        int64_t measured = tw + DESC * B * 1000000LL / STUDIO_SR;
        int64_t predicted = base_t + (int64_t)((frame - base_f) * 1000000ULL / STUDIO_SR);
        int64_t err = measured - predicted;
        if (err > RELOCK_US || err < -RELOCK_US) {
            base_t = measured;   // start-up, or after an underrun
            base_f = frame;
            base_frac = 0;
        } else {
            base_frac += (float)err / 16.0f;
            int64_t whole = (int64_t)base_frac;
            base_t += whole;
            base_frac -= (float)whole;
        }
        // keep the base recent so the multiply never overflows
        if (frame - base_f > (uint64_t)STUDIO_SR * 600) {
            base_t += (int64_t)((frame - base_f) * 1000000ULL / STUDIO_SR);
            base_f = frame;
        }
    }
}

static esp_err_t i2s_init(void) {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = DESC;
    cc.dma_frame_num = B;
    cc.auto_clear = true;   // an underrun plays silence, not a stuck buffer
    esp_err_t e;
    s_err_at = "channel";
    if ((e = i2s_new_channel(&cc, &s_tx, NULL)) != ESP_OK) return e;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(STUDIO_SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    s_err_at = "std mode";
    if ((e = i2s_channel_init_std_mode(s_tx, &sc)) != ESP_OK) return e;
    i2s_event_callbacks_t cbs = {.on_send_q_ovf = on_ovf};
    i2s_channel_register_event_callback(s_tx, &cbs, NULL);
    s_err_at = "enable";
    return i2s_channel_enable(s_tx);
}

// I2S is set up from the audio task itself, so its interrupt is on core 1
// with the rest of the audio, away from the panel's on core 0.
static void audio_boot(void *arg) {
    s_audio_task = xTaskGetCurrentTaskHandle();
    // the second core's share: above the UI on core 0 (no audio, no worker:
    // everything renders here, as before)
    if (xTaskCreatePinnedToCore(worker_task, "audio1", 6144, NULL, AUDIO_PRIO, &s_worker, 0) != pdPASS) {
        s_worker = NULL;
        ESP_LOGW(TAG, "no audio worker on core 0: one core renders everything");
    }
    esp_err_t e = i2s_init();
    if (e != ESP_OK) {
        s_err = e;
        ESP_LOGE(TAG, "I2S %s failed: %s - no audio", s_err_at, esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "%d Hz, %d x %d frames, BCLK %d WS %d DOUT %d, volume %d", STUDIO_SR, DESC, B,
             PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT, g_cfg.volume);
    audio_task(arg);
}

void audio_start(void) {
    s_gain = vol_gain(g_cfg.volume);
    for (int m = 0; m < STUDIO_MAX_MODULES; m++) mix_update(m);
    if (xTaskCreatePinnedToCore(audio_boot, "audio", 8192, NULL, AUDIO_PRIO, NULL, AUDIO_CORE) !=
        pdPASS) {
        s_err = ESP_ERR_NO_MEM;
        s_err_at = "task";
        ESP_LOGE(TAG, "no memory for the audio task - no audio");
    }
}
