#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "nvs.h"

#include "radio_player.h"
#include "board_pins.h"
#include "sdkconfig.h"

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

static const char *TAG = "radio";

#define EVT_REQUEST     BIT0
#define EVT_STOP        BIT1
#define EVT_TONE        BIT2

#define STREAM_BUF_SIZE ((size_t)CONFIG_RADIO_STREAM_BUF_SIZE)
#define HTTP_RD_SIZE    4096
/* How many stations the list can hold. The compiled-in defaults fill the
 * first few slots, the rest are added at runtime with the "add" command. */
#define MAX_STATIONS    16

static mp3dec_t            s_mp3d;
static uint8_t             s_stream_buf[STREAM_BUF_SIZE];
static size_t              s_fill;
static mp3d_sample_t       s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
static int16_t             s_out[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
static mp3dec_frame_info_t s_info;
static int64_t             s_last_decode_err_us;
static int64_t             s_last_overflow_us;
static int64_t             s_last_rate_log_us;
static int                 s_404_count;   /* consecutive http 404s on the current station */
static volatile int        s_tone_freq = 1000;
static volatile int        s_tone_ms   = 2000;

/* Print at most one line per interval for repetitive warnings. */
static bool log_throttled(int64_t *last_us, int64_t interval_ms)
{
    int64_t now = esp_timer_get_time();
    if (now - *last_us < interval_ms * 1000) {
        return false;
    }
    *last_us = now;
    return true;
}

static i2s_chan_handle_t   s_i2s;
static int                 s_i2s_rate;
static bool                s_rate_locked;   /* i2s clock is reconfigured at most once */

static EventGroupHandle_t  s_evt;
static SemaphoreHandle_t   s_station_mutex; /* guards the station list (console vs radio task) */
static volatile int        s_volume    = CONFIG_RADIO_INITIAL_VOLUME;
static volatile int        s_target    = 0;
static volatile int        s_cur       = -1;
static volatile bool       s_playing   = false;
static char                s_track[128];

static char                s_station_names[MAX_STATIONS][48];
static char                s_station_urls[MAX_STATIONS][160];
static radio_station_t     s_stations[MAX_STATIONS];
static int                 s_station_count;

/* I2S pin maps. The default one is the wiring that is known to work; the
 * alternative is for a rewired module. The selection is stored in nvs so a
 * reflash can never silence the amplifier again. */
typedef struct {
    int         bck, ws, data;
    const char *desc;
} i2s_pinmap_t;

static const i2s_pinmap_t s_pinmaps[] = {
    { PIN_I2S_BCK,      PIN_I2S_WS,      PIN_I2S_DATA,      "default (verified)" },
    { PIN_I2S_BCK_ALT,  PIN_I2S_WS_ALT,  PIN_I2S_DATA_ALT,  "legacy" },
};
#define PINMAP_COUNT (sizeof(s_pinmaps) / sizeof(s_pinmaps[0]))
static int s_pinmap = 0;
static radio_event_cb_t       s_cb;
static void                  *s_cb_ctx;

static void notify(radio_event_t event, const char *msg)
{
    if (s_cb) {
        s_cb(event, msg ? msg : "", s_cb_ctx);
    }
}

/* ================= direct audio output (pwm or dac) ================= */
/* Two backends, one engine:
 *   CONFIG_RADIO_AUDIO_PWM : ledc pwm on a gpio, 100 kHz carrier, 8 bit duty
 *   CONFIG_RADIO_AUDIO_DAC : esp32 internal 8 bit dac (gpio25 / gpio26)
 *
 * Both are sampled by a hardware timer isr (gptimer, 16 kHz): one sample per
 * interrupt, one register write per sample. No esp_timer task is involved,
 * so the wifi stack cannot distort the audio timing and nothing can starve
 * the idle task (that was the cause of the task/interrupt watchdog panics).
 *
 * The ledc driver is deliberately NOT called from the isr: ledc_update_duty()
 * busy-waits for the hardware to clear the duty_start bit inside an
 * interrupt-disabled critical section (ledc_ll.h:571). At 16 kHz that hung
 * the cpu and triggered the watchdog. Writing the register directly is safe
 * here: the pwm period is 10 us while the sample period is 62 us, so a new
 * duty update can never overlap the previous one and the wait is never
 * needed.
 *
 * Wiring (pwm): gpio -> 22R -> headphone L+R in parallel, gnd -> gnd.
 * Wiring (dac): gpio -> series cap (1..100uF) -> headphone L+R, gnd -> gnd.
 */

#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)

#include "driver/gptimer.h"

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BUF_SAMPLES   2048            /* power of two: mask, no modulo */
#define AUDIO_BUF_MASK      (AUDIO_BUF_SAMPLES - 1)
#define AUDIO_SILENCE       128             /* mid scale / 50% duty = silence */

static int16_t          s_audio_buf[AUDIO_BUF_SAMPLES];
static volatile int     s_audio_head;
static volatile int     s_audio_tail;
static float            s_resample_pos;
static gptimer_handle_t s_audio_timer;

/* ---------------- backend: one sample -> one register write ---------------- */

#if defined(CONFIG_RADIO_AUDIO_PWM)

#include "driver/ledc.h"
#include "soc/ledc_struct.h"

#define PWM_CARRIER_HZ      100000
#define PWM_DUTY_RES        LEDC_TIMER_8_BIT

static inline void IRAM_ATTR audio_write_sample(int16_t sample)
{
    uint32_t duty = (uint32_t)(((int32_t)sample + 32768) >> 8); /* 0..255 */
    /* exactly the two writes the ledc driver does, minus the busy-wait */
    LEDC.channel_group[0].channel[0].duty.duty       = duty << 4;
    LEDC.channel_group[0].channel[0].conf1.duty_start = 1;
}

#else /* CONFIG_RADIO_AUDIO_DAC */

#include "driver/dac_oneshot.h"
#include "soc/rtc_io_struct.h"

static dac_oneshot_handle_t s_dac_chan;
static int                  s_dac_ch = DAC_CHAN_0;

static inline void IRAM_ATTR audio_write_sample(int16_t sample)
{
    uint8_t v = (uint8_t)(((int32_t)sample + 32768) >> 8);
    if (s_dac_ch == DAC_CHAN_0) {
        RTCIO.pad_dac[0].dac = v;
    } else {
        RTCIO.pad_dac[1].dac = v;
    }
}

#endif

/* ---------------- engine ---------------- */

/* One sample per hardware timer interrupt. */
static bool IRAM_ATTR audio_sample_isr(gptimer_handle_t timer,
                                       const gptimer_alarm_event_data_t *edata,
                                       void *user_ctx)
{
    int16_t sample = 0;
    if (s_audio_head != s_audio_tail) {
        sample = s_audio_buf[s_audio_tail];
        s_audio_tail = (s_audio_tail + 1) & AUDIO_BUF_MASK;
    }
    audio_write_sample(sample);
    return false; /* no yield needed */
}

/* Free space in the ring buffer. (head - tail) is the number of *stored*
 * samples; one slot is always kept empty, so the usable free space is
 * MASK - stored. */
static int audio_out_free(void)
{
    return AUDIO_BUF_MASK - ((s_audio_head - s_audio_tail) & AUDIO_BUF_MASK);
}

/* Block until the ring has at least "need" free samples. The isr drains the
 * ring at the sample rate, so this paces the producer to the audio clock.
 * The network jitter is then absorbed by the mp3 stream buffer instead of
 * showing up as dropouts in the audio. */
static bool audio_out_wait_space(int need, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (audio_out_free() < need) {
        if (xEventGroupWaitBits(s_evt, EVT_STOP, pdFALSE, pdFALSE, 0) & EVT_STOP) {
            return false;
        }
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        /* NOTE: the tick rate is 100Hz, so pdMS_TO_TICKS(2) would be 0 ticks
         * and vTaskDelay(0) only yields - the task would spin, never block
         * and starve the idle task (task watchdog panic). One tick = 10ms. */
        vTaskDelay(1);
    }
    return true;
}

static void audio_push(int16_t sample)
{
    /* Backpressure: the ring must not overflow. In the i2s build the blocking
     * dma write provides this for free; with a register based output the
     * producer would run ahead, the ring would overflow and samples would be
     * dropped silently - heard as stuttering. */
    if (audio_out_free() < 64) {
        if (!audio_out_wait_space(64, 1000)) {
            return; /* timed out or stopped: drop the sample */
        }
    }
    s_audio_buf[s_audio_head] = sample;
    s_audio_head = (s_audio_head + 1) & AUDIO_BUF_MASK;
}

static void audio_out_init(void)
{
#if defined(CONFIG_RADIO_AUDIO_PWM)
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_HIGH_SPEED_MODE,
        .duty_resolution = PWM_DUTY_RES,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = PWM_CARRIER_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        /* Without a valid timer the channel is dead and a duty update would
         * hang inside the driver's critical section (interrupt wdt panic). */
        ESP_LOGE(TAG, "ledc timer config failed: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = PIN_PWM,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = AUDIO_SILENCE,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel config failed: %s", esp_err_to_name(err));
        return;
    }
    /* Strongest drive: a headphone is a low impedance load and the gpio
     * output impedance (tens of ohms) would eat most of the swing. */
    gpio_set_drive_capability(PIN_PWM, GPIO_DRIVE_CAP_3);
#else
    s_dac_ch = (PIN_DAC_CHANNEL == 1) ? DAC_CHAN_1 : DAC_CHAN_0;
    dac_oneshot_config_t dac_cfg = {
        .chan_id = (dac_channel_t)s_dac_ch,
    };
    esp_err_t err = dac_oneshot_new_channel(&dac_cfg, &s_dac_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac channel init failed: %s", esp_err_to_name(err));
        return;
    }
    /* sets the pad enable and clears the continuous wave mode */
    dac_oneshot_output_voltage(s_dac_chan, AUDIO_SILENCE);
#endif

    gptimer_config_t gcfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 8000000, /* 125 ns tick */
    };
    err = gptimer_new_timer(&gcfg, &s_audio_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
        return;
    }

    gptimer_event_callbacks_t cbs = {
        .on_alarm = audio_sample_isr,
    };
    err = gptimer_register_event_callbacks(s_audio_timer, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_register_event_callbacks failed: %s", esp_err_to_name(err));
        return;
    }

    gptimer_alarm_config_t acfg = {
        .alarm_count = 8000000 / AUDIO_SAMPLE_RATE, /* 500 -> exactly 16 kHz */
        .flags.auto_reload_on_alarm = true,
    };
    err = gptimer_set_alarm_action(s_audio_timer, &acfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_set_alarm_action failed: %s", esp_err_to_name(err));
        return;
    }
    err = gptimer_enable(s_audio_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_enable failed: %s", esp_err_to_name(err));
        return;
    }
    err = gptimer_start(s_audio_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_start failed: %s", esp_err_to_name(err));
        return;
    }

#if defined(CONFIG_RADIO_AUDIO_PWM)
    ESP_LOGI(TAG, "pwm audio ready (gpio=%d, %d Hz isr, carrier %d Hz, 8 bit)",
             PIN_PWM, AUDIO_SAMPLE_RATE, PWM_CARRIER_HZ);
#else
    ESP_LOGI(TAG, "dac audio ready (ch %d, gpio %d, %d Hz isr)",
             s_dac_ch, s_dac_ch == DAC_CHAN_0 ? 25 : 26, AUDIO_SAMPLE_RATE);
#endif
}

/* Resample interleaved stereo pcm down to AUDIO_SAMPLE_RATE mono and queue. */
static void audio_out_write(const int16_t *stereo, int frames, int rate)
{
    if (rate <= 0) {
        rate = AUDIO_SAMPLE_RATE;
    }
    float step = (float)AUDIO_SAMPLE_RATE / rate;
    for (int i = 0; i < frames; i++) {
        s_resample_pos += step;
        if (s_resample_pos >= 1.0f) {
            s_resample_pos -= 1.0f;
            audio_push(stereo[2 * i]); /* left channel */
        }
    }
}

#endif /* CONFIG_RADIO_AUDIO_DAC || CONFIG_RADIO_AUDIO_PWM */

/* ---------------- status LED ---------------- */

static void led_set(bool on)
{
    gpio_set_level(PIN_STATUS_LED, on ? 1 : 0);
}

/* ---------------- I2S ---------------- */

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s, NULL), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    };
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = s_pinmaps[s_pinmap].bck;
    std_cfg.gpio_cfg.ws   = s_pinmaps[s_pinmap].ws;
    std_cfg.gpio_cfg.dout = s_pinmaps[s_pinmap].data;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

    s_i2s_rate = 44100;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s, &std_cfg), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s), TAG, "i2s enable failed");
    ESP_LOGI(TAG, "i2s ready (bck=%d ws=%d dout=%d) [%s]",
             s_pinmaps[s_pinmap].bck, s_pinmaps[s_pinmap].ws, s_pinmaps[s_pinmap].data,
             s_pinmaps[s_pinmap].desc);
    return ESP_OK;
}

/* Re-point the i2s peripheral at another pin triple without rebooting.
 * Used to find the wiring that actually works, and to survive a firmware
 * update that moves the pins. */
static bool i2s_apply_pinmap(int idx)
{
    if (idx < 0 || idx >= (int)PINMAP_COUNT || s_i2s == NULL) {
        return false;
    }
    const i2s_pinmap_t *pm = &s_pinmaps[idx];

    i2s_std_gpio_config_t gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = pm->bck,
        .ws   = pm->ws,
        .dout = pm->data,
        .din  = I2S_GPIO_UNUSED,
    };

    i2s_channel_disable(s_i2s);
    esp_err_t err = i2s_channel_reconfig_std_gpio(s_i2s, &gpio_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_i2s);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s pin switch failed: %s", esp_err_to_name(err));
        return false;
    }
    s_pinmap = idx;
    ESP_LOGI(TAG, "i2s pins -> [%s] bck=%d ws=%d dout=%d", pm->desc, pm->bck, pm->ws, pm->data);
    return true;
}

/* Each new stream (station switch / reconnect) may re-lock the I2S clock to
 * the stream's sample rate - the reconfiguration happens on the very first
 * frame, before any audio is flowing, so no DMA interrupt race is possible.
 * Mid-stream rate changes keep the clock and only log (rate limited). */
static void i2s_set_rate(int rate)
{
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    return; /* direct output runs at a fixed rate */
#else
    if (rate <= 0) {
        return;
    }
    if (s_rate_locked) {
        if (rate != s_i2s_rate && log_throttled(&s_last_rate_log_us, 5000)) {
            ESP_LOGW(TAG, "stream rate %d Hz, keeping i2s at %d Hz", rate, s_i2s_rate);
        }
        return;
    }

    if (rate != s_i2s_rate) {
        ESP_LOGI(TAG, "stream sample rate %d Hz, reconfiguring i2s", rate);
        i2s_channel_disable(s_i2s);
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        if (i2s_channel_reconfig_std_clock(s_i2s, &clk_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "i2s reconfig failed");
        }
        i2s_channel_enable(s_i2s);
        s_i2s_rate = rate;
    }
    s_rate_locked = true; /* first frame of this stream decides the clock */
#endif
}

/* ---------------- amplifier SD pin control (diagnostic) ---------------- */

/* Drives the MAX98357A "SD" pin from a gpio so the module can be checked
 * without a scope:
 *
 *   mode -1 : gpio high-z      -> module default (1M pull-up: (L+R)/2 mix)
 *   mode  0 : gpio pulled low  -> amplifier shutdown, the outputs must fall
 *                                  to ~0V - this proves the chip is alive
 *   mode  1 : gpio pulled high -> left channel only, 15 dB gain
 *
 * The gpio idles as an input, so it never disturbs normal playback.
 * Wiring: gpio -> 22R -> SD pin of the module. 22R is enough: it drags the
 * module's 1M pull-up down to ~0.07V, well below the 0.16V shutdown limit.
 */
bool radio_set_amp_sd(int mode)
{
#if (PIN_AMP_SD >= 0)
    const int pin = PIN_AMP_SD;
    if (mode < 0) {
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        ESP_LOGI(TAG, "amp sd: float (module default)");
    } else {
        gpio_set_level(pin, mode ? 1 : 0);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        ESP_LOGI(TAG, "amp sd: %s", mode ? "high (left channel, 15 dB)" : "low (shutdown)");
    }
    return true;
#else
    (void)mode;
    ESP_LOGW(TAG, "amp sd control not configured (PIN_AMP_SD)");
    return false;
#endif
}

/* ---------------- PCM output ---------------- */

/* Peak normalizer (gain in Q8, 1.0 = 256). Broadcast mp3 is often far below
 * full scale: a -12 dBFS stream only swings a quarter of the 8 bit duty
 * range and sounds very quiet. Track the peak and adapt the gain (fast
 * attack, slow release) so quiet material still reaches the full range. */
static int32_t s_audio_gain = 256;

static void audio_gain_update(int peak)
{
    if (peak < 256) {
        return; /* too quiet to judge - keep the current gain */
    }
    int32_t target = (32000 << 8) / peak;
    if (target > s_audio_gain) {
        s_audio_gain += (target - s_audio_gain) / 4;   /* fast attack */
    } else {
        s_audio_gain += (target - s_audio_gain) / 32;  /* slow release */
    }
    if (s_audio_gain > (12 << 8)) {
        s_audio_gain = 12 << 8;
    }
    if (s_audio_gain < 64) {
        s_audio_gain = 64;
    }
}

static void output_frame(int samples_per_ch)
{
    int ch = s_info.channels > 1 ? 2 : 1;
    const int16_t *src = (const int16_t *)s_pcm;
    int vol = s_volume;
    int total = samples_per_ch * ch;

    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;

    /* Normalize to full scale, then apply the user volume. */
    int peak = 0;
    for (int i = 0; i < total; i++) {
        int a = src[i] < 0 ? -src[i] : src[i];
        if (a > peak) {
            peak = a;
        }
    }
    audio_gain_update(peak);
    int32_t g = (s_audio_gain * vol) / 100;   /* Q8, gain * volume */

    if (ch == 1) {
        for (int i = 0; i < samples_per_ch; i++) {
            int32_t v = ((int32_t)src[i] * g) >> 8;
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            s_out[2 * i]     = (int16_t)v;
            s_out[2 * i + 1] = (int16_t)v;
        }
        total *= 2;
    } else {
        for (int i = 0; i < total; i++) {
            int32_t v = ((int32_t)src[i] * g) >> 8;
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            s_out[i] = (int16_t)v;
        }
    }

    size_t written = 0;
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    audio_out_write(s_out, samples_per_ch, s_info.hz);
    (void)written;
#else
    i2s_channel_write(s_i2s, s_out, total * sizeof(int16_t), &written, pdMS_TO_TICKS(2000));
#endif
}

/* Decode as many MP3 frames as the stream buffer holds. */
static void decode_loop(void)
{
    size_t consumed = 0;

    /* Skip an ID3v2 tag at the start of a stream. */
    if (s_fill >= 10 && memcmp(s_stream_buf, "ID3", 3) == 0) {
        uint32_t size = 0;
        for (int i = 6; i < 10; i++) {
            size = (size << 7) | (s_stream_buf[i] & 0x7F);
        }
        size_t total = (size_t)10 + size;
        if (total <= s_fill) {
            memmove(s_stream_buf, s_stream_buf + total, s_fill - total);
            s_fill -= total;
        } else {
            s_fill = 0; /* tag not complete yet, drop what we have */
            return;
        }
    }

    while (consumed < s_fill) {
        int avail = (int)(s_fill - consumed);
        int frame_bytes = 0;

        /* returns offset of the frame inside the buffer, frame_bytes = frame size + padding */
        int off = mp3d_find_frame(s_stream_buf + consumed, avail, &s_mp3d.free_format_bytes, &frame_bytes);
        if (off >= avail || off + frame_bytes > avail) {
            break; /* no complete frame yet, wait for more data */
        }

        memset(&s_info, 0, sizeof(s_info));
        int samples = mp3dec_decode_frame(&s_mp3d, s_stream_buf + consumed + off, avail - off, s_pcm, &s_info);
        if (samples > 0) {
            i2s_set_rate(s_info.hz);
            consumed += off + frame_bytes;
            output_frame(samples);
        } else {
            if (log_throttled(&s_last_decode_err_us, 2000)) {
                ESP_LOGW(TAG, "mp3 decode error, skipping frame");
            }
            consumed += off + frame_bytes;
        }
    }

    if (consumed > 0) {
        memmove(s_stream_buf, s_stream_buf + consumed, s_fill - consumed);
        s_fill -= consumed;
    }
}

/* ---------------- station list (runtime editable, persisted in NVS) ---------------- */

#define STATION_NVS_NS "radio"

static void station_nvs_load(int idx)
{
    nvs_handle_t handle;
    if (nvs_open(STATION_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    char key[16];
    size_t len = sizeof(s_station_urls[idx]);
    snprintf(key, sizeof(key), "url%d", idx);
    if (nvs_get_str(handle, key, s_station_urls[idx], &len) == ESP_OK) {
        len = sizeof(s_station_names[idx]);
        snprintf(key, sizeof(key), "name%d", idx);
        nvs_get_str(handle, key, s_station_names[idx], &len);
    }
    nvs_close(handle);
}

static void station_nvs_save(int idx)
{
    nvs_handle_t handle;
    if (nvs_open(STATION_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    char key[16];
    snprintf(key, sizeof(key), "name%d", idx);
    nvs_set_str(handle, key, s_station_names[idx]);
    snprintf(key, sizeof(key), "url%d", idx);
    nvs_set_str(handle, key, s_station_urls[idx]);
    nvs_commit(handle);
    nvs_close(handle);
}

/* The number of stations is stored as well, so that stations added at
 * runtime survive a reboot and deleted ones stay deleted. */
static int station_nvs_load_count(void)
{
    nvs_handle_t handle;
    int32_t value = -1;
    if (nvs_open(STATION_NVS_NS, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_i32(handle, "count", &value) != ESP_OK) {
            value = -1;
        }
        nvs_close(handle);
    }
    return (int)value;
}

static void station_nvs_save_count(int count)
{
    nvs_handle_t handle;
    if (nvs_open(STATION_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_i32(handle, "count", (int32_t)count);
    nvs_commit(handle);
    nvs_close(handle);
}

/* The i2s pin map is remembered so that a firmware update which changes the
 * default pins cannot silence an amplifier that is already working.
 *
 * The pins themselves are stored (not the table index) - so reordering or
 * extending the table later can never silently change the meaning of an
 * already stored selection. */
static bool pinmap_nvs_load(int *bck, int *ws, int *data)
{
    nvs_handle_t handle;
    int32_t b = 0, w = 0, d = 0;
    if (nvs_open(STATION_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_get_i32(handle, "pin_bck", &b) == ESP_OK) &&
              (nvs_get_i32(handle, "pin_ws", &w) == ESP_OK) &&
              (nvs_get_i32(handle, "pin_data", &d) == ESP_OK);
    nvs_close(handle);
    if (!ok) {
        return false;
    }
    *bck = (int)b;
    *ws = (int)w;
    *data = (int)d;
    return true;
}

static void pinmap_nvs_save(int bck, int ws, int data)
{
    nvs_handle_t handle;
    if (nvs_open(STATION_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_i32(handle, "pin_bck", (int32_t)bck);
    nvs_set_i32(handle, "pin_ws", (int32_t)ws);
    nvs_set_i32(handle, "pin_data", (int32_t)data);
    nvs_commit(handle);
    nvs_close(handle);
}

/* ---------------- HTTP / ICY ---------------- */

typedef struct {
    int  metaint;
    char content_type[32];
} http_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            ctx->metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "content-type") == 0) {
            strlcpy(ctx->content_type, evt->header_value, sizeof(ctx->content_type));
        }
    }
    return ESP_OK;
}

static void parse_track(const char *meta, char *out, size_t len)
{
    out[0] = '\0';
    const char *p = strstr(meta, "StreamTitle='");
    if (!p) {
        return;
    }
    p += strlen("StreamTitle='");
    const char *e = strstr(p, "';");
    if (!e) {
        e = p + strlen(p);
    }
    size_t n = (size_t)(e - p);
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Returns ESP_OK when the user interrupted (stop/switch), error otherwise. */
static esp_err_t play_station(int idx, uint8_t *http_buf)
{
    http_ctx_t ctx = { .metaint = -1 };

    /* Snapshot name/url under the mutex: the console task may rewrite them
     * (url/name commands) while we are connecting. Reading a half-written
     * string can crash the http client url parser. */
    char url[160];
    char name[48];
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_FAIL;
    }
    strlcpy(url, s_station_urls[idx], sizeof(url));
    strlcpy(name, s_station_names[idx], sizeof(name));
    xSemaphoreGive(s_station_mutex);

    esp_http_client_config_t cfg = {
        .url             = url,
        .event_handler   = http_event_cb,
        .user_data       = &ctx,
        .timeout_ms      = 10000,
        .buffer_size     = 1024,
        .keep_alive_enable = true,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        /* IDF 6.x removed esp_http_client's skip_verify: https streams must
         * verify the server. The mbedTLS certificate bundle (enabled in
         * sdkconfig, CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) provides the root CAs. */
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "1");
    esp_http_client_set_header(client, "User-Agent", "esp32-radio/1.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        err = esp_http_client_fetch_headers(client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect to %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "stream: %s (http %d, metaint=%d, type=%s)",
             name, status, ctx.metaint, ctx.content_type);
    if (status == 404 || status == 403 || status == 410) {
        /* The url itself is dead (qingting rotates channel ids): retrying
         * the same address forever is pointless. */
        ESP_LOGE(TAG, "http %d: station url is dead, set another with 'url <n> <url>'", status);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (status != 200) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    notify(RADIO_EVENT_STREAMING, name);
    led_set(true);

    mp3dec_init(&s_mp3d);
    s_fill   = 0;
    s_track[0] = '\0';
    s_rate_locked = false; /* new stream: first frame re-locks the i2s clock */

    const int metaint    = ctx.metaint;   /* <= 0: no metadata */
    int       until_meta = metaint;       /* audio bytes until next meta block */
    int       meta_len   = -1;            /* -1: read length byte next */
    int       meta_pos   = 0;
    char      meta_buf[512];

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0);
        if (bits & (EVT_REQUEST | EVT_STOP)) {
            esp_http_client_cleanup(client);
            return ESP_OK;
        }

        /* Backpressure: never read from the socket while the stream buffer is
         * (nearly) full. The tcp receive window closes by itself, the server
         * waits, and not a single mp3 byte is thrown away.
         *
         * Throwing bytes away would break the frame alignment - the decoder
         * then reports "mp3 decode error, skipping frame" and the glitch is
         * clearly audible. decode_loop() is paced by the blocking i2s write,
         * so it frees the buffer exactly at the speed the audio is played. */
        if (s_fill + HTTP_RD_SIZE > STREAM_BUF_SIZE) {
            decode_loop();
            if (s_fill + HTTP_RD_SIZE > STREAM_BUF_SIZE) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        int r = esp_http_client_read(client, (char *)http_buf, HTTP_RD_SIZE);
        if (r < 0) {
            ESP_LOGE(TAG, "read error: %s", esp_err_to_name(r));
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            ESP_LOGW(TAG, "stream closed by server");
            esp_http_client_cleanup(client);
            return ESP_ERR_NOT_FINISHED;
        }

        int pos = 0;
        while (pos < r) {
            if (metaint > 0 && until_meta == 0) {
                if (meta_len < 0) {
                    meta_len = (uint8_t)http_buf[pos++] * 16;
                    meta_pos = 0;
                    if (meta_len == 0) {
                        until_meta = metaint;
                        meta_len   = -1;
                        continue;
                    }
                }
                int take = r - pos;
                if (take > meta_len - meta_pos) {
                    take = meta_len - meta_pos;
                }
                if (meta_len < (int)sizeof(meta_buf)) {
                    memcpy(meta_buf + meta_pos, http_buf + pos, take);
                }
                meta_pos += take;
                pos      += take;
                if (meta_pos >= meta_len) {
                    if (meta_len < (int)sizeof(meta_buf)) {
                        meta_buf[meta_len] = '\0';
                        char title[128];
                        parse_track(meta_buf, title, sizeof(title));
                        if (title[0] && strcmp(title, s_track) != 0) {
                            strlcpy(s_track, title, sizeof(s_track));
                            ESP_LOGI(TAG, "track: %s", s_track);
                            notify(RADIO_EVENT_TRACK, s_track);
                        }
                    }
                    until_meta = metaint;
                    meta_len   = -1;
                }
                continue;
            }

            /* plain audio payload */
            int take = r - pos;
            if (metaint > 0 && take > until_meta) {
                take = until_meta;
            }
            size_t space = STREAM_BUF_SIZE - s_fill;
            if ((size_t)take > space) {
                /* decoder cannot keep up: drop oldest bytes */
                size_t drop = (size_t)take - space;
                if (drop > s_fill) {
                    drop = s_fill;
                }
                memmove(s_stream_buf, s_stream_buf + drop, s_fill - drop);
                s_fill -= drop;
                if (log_throttled(&s_last_overflow_us, 2000)) {
                    ESP_LOGW(TAG, "stream buffer full, dropped %u bytes", (unsigned)drop);
                }
                space = STREAM_BUF_SIZE - s_fill;
                if ((size_t)take > space) {
                    take = (int)space;
                }
            }
            if (take > 0) {
                memcpy(s_stream_buf + s_fill, http_buf + pos, take);
                s_fill += take;
                pos     += take;
            }
            if (metaint > 0) {
                until_meta -= take;
            }

            decode_loop();
        }
    }
}

/* ---------------- radio task ---------------- */

/* Diagnostic: square wave straight through the audio path - no network, no
 * decoder. Used to isolate "silent speaker" hardware problems.
 * duration_ms == 0 means "continuous until stop" - needed for multimeter
 * measurements, a 2 s burst is too short to read a meter. */
static void play_tone_blocking(int freq, int duration_ms)
{
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    const int rate = AUDIO_SAMPLE_RATE;
    const int half_period = rate / (2 * freq);
    const bool continuous = (duration_ms == 0);
    int total = continuous ? 0 : rate * duration_ms / 1000;
    int s = 0;

    while (continuous || s < total) {
        /* near full scale: the pwm gpio can source ~40mA, so keep the volume
         * moderate when driving a low impedance headphone */
        audio_push((int16_t)(((s / half_period) % 2) ? 20000 : -20000));
        s++;
        if ((s % 256) == 0) {
            if (xEventGroupWaitBits(s_evt, EVT_STOP, pdFALSE, pdFALSE, 0) & EVT_STOP) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    /* let the queue drain so the tone is fully heard */
    int queued = (s_audio_head - s_audio_tail) & AUDIO_BUF_MASK;
    vTaskDelay(pdMS_TO_TICKS(queued * 1000 / AUDIO_SAMPLE_RATE + 30));
#else
    const int rate = s_i2s_rate;
    const int half_period = rate / (2 * freq);
    const bool continuous = (duration_ms == 0);
    int total = continuous ? 0 : rate * duration_ms / 1000;
    int16_t buf[256];
    size_t total_written = 0;
    int s = 0;

    for (;;) {
        int n = 128;
        if (!continuous && s + n > total) {
            n = total - s;
        }
        if (n <= 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            /* near full scale: the amp adds 9-15 dB on top of this */
            int16_t v = (int16_t)((((s + i) / half_period) % 2) ? 20000 : -20000);
            buf[2 * i]     = v;
            buf[2 * i + 1] = v;
        }
        size_t written = 0;
        esp_err_t werr = i2s_channel_write(s_i2s, buf, n * sizeof(int16_t) * 2, &written, portMAX_DELAY);
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "tone: i2s write failed: %s", esp_err_to_name(werr));
            break;
        }
        total_written += written;
        s += n;
        if (xEventGroupWaitBits(s_evt, EVT_STOP, pdFALSE, pdFALSE, 0) & EVT_STOP) {
            break;
        }
    }
    /* Proof that the i2s dma really moved the samples out. If this is 0 the
     * fault is on the esp32 side, not on the amplifier. */
    if (continuous) {
        ESP_LOGI(TAG, "tone: %u bytes written to i2s at %d Hz (continuous)",
                 (unsigned)total_written, rate);
    } else {
        ESP_LOGI(TAG, "tone: %u bytes written to i2s at %d Hz (expected %u)",
                 (unsigned)total_written, rate,
                 (unsigned)((size_t)total * 2 * sizeof(int16_t)));
    }
#endif
}

/* Resolved to wifi_station.c at link time (same component). If absent,
 * the network is simply considered down. */
extern bool wifi_is_connected(void) __attribute__((weak));

static bool net_is_up(void)
{
    return wifi_is_connected ? wifi_is_connected() : false;
}

static void radio_task(void *arg)
{
    uint8_t *http_buf = malloc(HTTP_RD_SIZE);
    if (!http_buf) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP | EVT_TONE, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & EVT_TONE) {
            s_playing = true;
            led_set(true);
            ESP_LOGI(TAG, "test tone: %d Hz, %d ms", s_tone_freq, s_tone_ms);
            notify(RADIO_EVENT_CONNECTING, "test tone");
            play_tone_blocking(s_tone_freq, s_tone_ms);
            s_playing = false;
            led_set(false);
            notify(RADIO_EVENT_STOPPED, "");
            continue;
        }
        if (!(bits & EVT_REQUEST)) {
            led_set(false);
            continue;
        }

        int idx = s_target;
        s_cur     = idx;
        s_playing = true;
        s_404_count = 0;
        ESP_LOGI(TAG, "station [%d/%d]: %s", idx + 1, s_station_count, s_station_names[idx]);
        notify(RADIO_EVENT_CONNECTING, s_station_names[idx]);

        while (1) {
            EventBits_t b = xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0);
            if (b & (EVT_REQUEST | EVT_STOP)) {
                break; /* switch or stop */
            }

            /* Wait for the network instead of hammering DNS without an ip. */
            if (!net_is_up()) {
                notify(RADIO_EVENT_ERROR, "waiting for wifi ...");
                int ticks = 0;
                while (!net_is_up()) {
                    if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                        break;
                    }
                    led_set((ticks % 10) < 5); /* slow blink */
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ticks++;
                    if (ticks % 300 == 0) {
                        ESP_LOGW(TAG, "still waiting for wifi ...");
                    }
                }
                if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                    break;
                }
                led_set(true);
                notify(RADIO_EVENT_CONNECTING, s_station_names[idx]);
                continue;
            }

            esp_err_t err = play_station(idx, http_buf);
            if (err == ESP_OK) {
                break;
            }

            if (err == ESP_ERR_NOT_SUPPORTED) {
                /* A 404 does not always mean a dead url: qingting's cdn
                 * throttles quick reconnects (the first attempt of this very
                 * url succeeded). Back off long and only give up after
                 * several consecutive 404s. */
                s_404_count++;
                if (s_404_count >= 5) {
                    notify(RADIO_EVENT_ERROR, "station url keeps returning 404, stopped. use 'url <n> <url>'");
                    break;
                }
                notify(RADIO_EVENT_ERROR, "http 404 (reconnect throttled?), retrying in 30 s ...");
                for (int i = 0; i < 300; i++) { /* 30 s backoff */
                    led_set((i % 10) < 5);
                    if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                    break;
                }
                continue;
            }

            if (err == ESP_ERR_NOT_FINISHED) {
                s_404_count = 0; /* url works, the server just closed the stream */
            }

            if (!net_is_up()) {
                notify(RADIO_EVENT_ERROR, "wifi connection lost");
                continue; /* back to the wifi wait loop */
            }

            notify(RADIO_EVENT_ERROR, "connection lost, retrying...");
            for (int i = 0; i < 30; i++) { /* 3 s backoff, blinking */
                led_set((i % 2) == 0);
                if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                break;
            }
            notify(RADIO_EVENT_CONNECTING, s_station_names[idx]);
        }

        s_playing = false;
        led_set(false);
        notify(RADIO_EVENT_STOPPED, "");
    }
}

/* ---------------- radio task ---------------- */

void radio_init(const radio_station_t *stations, int count, radio_event_cb_t cb, void *ctx)
{
    s_station_count = count > MAX_STATIONS ? MAX_STATIONS : count;
    for (int i = 0; i < s_station_count; i++) {
        strlcpy(s_station_names[i], stations[i].name, sizeof(s_station_names[i]));
        strlcpy(s_station_urls[i], stations[i].url, sizeof(s_station_urls[i]));
        station_nvs_load(i); /* stored config overrides the defaults */
        s_stations[i].name = s_station_names[i];
        s_stations[i].url  = s_station_urls[i];
    }

    /* A stored count means the user added or deleted stations - it wins
     * over the compiled-in default list. */
    int stored = station_nvs_load_count();
    if (stored >= 0 && stored <= MAX_STATIONS) {
        for (int i = s_station_count; i < stored; i++) {
            s_station_names[i][0] = '\0';
            s_station_urls[i][0] = '\0';
            station_nvs_load(i);
            s_stations[i].name = s_station_names[i];
            s_stations[i].url  = s_station_urls[i];
        }
        s_station_count = stored;
    }
    s_cb     = cb;
    s_cb_ctx = ctx;
    s_volume = CONFIG_RADIO_INITIAL_VOLUME;
    s_evt    = xEventGroupCreate();
    s_station_mutex = xSemaphoreCreateMutex();
    s_rate_locked = false;

    gpio_config_t led_io = {
        .pin_bit_mask = 1ULL << PIN_STATUS_LED,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_io);
    led_set(false);

#if (PIN_AMP_SD >= 0)
    /* The sd control pin idles as a high-z input, so the amplifier stays in
     * its default mode (module pull-up wins) until the user overrides it
     * with the "sd" command. */
    gpio_config_t sd_io = {
        .pin_bit_mask = 1ULL << PIN_AMP_SD,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&sd_io);
    ESP_LOGI(TAG, "amp sd control on gpio %d (idle: float)", PIN_AMP_SD);
#endif

#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    audio_out_init();
#else
    /* A pin map that was chosen at runtime ("pins" command) wins over the
     * compiled-in default, so a firmware update cannot silence a working
     * amplifier. The stored pins are matched against the table, so the
     * selection keeps its meaning even if the table is reordered. */
    int bck = 0, ws = 0, data = 0;
    if (pinmap_nvs_load(&bck, &ws, &data)) {
        bool found = false;
        for (int i = 0; i < (int)PINMAP_COUNT; i++) {
            if (s_pinmaps[i].bck == bck && s_pinmaps[i].ws == ws && s_pinmaps[i].data == data) {
                s_pinmap = i;
                found = true;
                break;
            }
        }
        if (found) {
            ESP_LOGI(TAG, "i2s pin map from nvs: [%s] bck=%d ws=%d dout=%d",
                     s_pinmaps[s_pinmap].desc, bck, ws, data);
        } else {
            ESP_LOGW(TAG, "stored i2s pins (%d/%d/%d) not in the table, using default",
                     bck, ws, data);
        }
    }
    i2s_init();
#endif

    /* mp3dec_decode_frame() keeps a ~16KB scratch buffer on the stack
     * (mp3dec_scratch_t), plus the http client call chain. 32KB is safe. */
    xTaskCreate(radio_task, "radio", 32768, NULL, 5, NULL);
}

void radio_play(int index)
{
    if (s_station_count <= 0) {
        return;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= s_station_count) {
        index = s_station_count - 1;
    }
    s_target = index;
    xEventGroupClearBits(s_evt, EVT_STOP);
    xEventGroupSetBits(s_evt, EVT_REQUEST);
}

void radio_next(void)
{
    if (s_station_count <= 0) {
        return;
    }
    radio_play((s_target + 1) % s_station_count);
}

void radio_prev(void)
{
    if (s_station_count <= 0) {
        return;
    }
    radio_play((s_target + s_station_count - 1) % s_station_count);
}

void radio_stop(void)
{
    xEventGroupClearBits(s_evt, EVT_REQUEST);
    xEventGroupSetBits(s_evt, EVT_STOP);
}

void radio_play_tone(int freq_hz, int duration_ms)
{
    if (freq_hz < 50)        freq_hz = 50;
    if (freq_hz > 8000)      freq_hz = 8000;
    if (duration_ms < 0)     duration_ms = 0;   /* 0 = continuous until stop */
    if (duration_ms > 60000) duration_ms = 60000;
    s_tone_freq = freq_hz;
    s_tone_ms   = duration_ms;
    /* Stop the current stream first so the tone runs immediately. */
    xEventGroupClearBits(s_evt, EVT_REQUEST);
    xEventGroupSetBits(s_evt, EVT_STOP | EVT_TONE);
}

void radio_set_volume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
}

int radio_get_volume(void)
{
    return s_volume;
}

int radio_current_index(void)
{
    return s_cur;
}

void radio_set_station(int index, const char *name, const char *url)
{
    if (index < 0 || index >= s_station_count) {
        return;
    }
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (url && url[0] && strlen(url) < sizeof(s_station_urls[0])) {
            strlcpy(s_station_urls[index], url, sizeof(s_station_urls[index]));
        }
        if (name && name[0] && strlen(name) < sizeof(s_station_names[0])) {
            strlcpy(s_station_names[index], name, sizeof(s_station_names[index]));
        }
        xSemaphoreGive(s_station_mutex);
    }
    station_nvs_save(index);
    ESP_LOGI(TAG, "station [%d] set: %s (%s)", index, s_station_names[index], s_station_urls[index]);
    if (index == s_cur || index == s_target) {
        radio_play(index); /* reconnect with the new url right away */
    }
}

const radio_station_t *radio_get_station(int index)
{
    if (index < 0 || index >= s_station_count) {
        return NULL;
    }
    return &s_stations[index];
}

int radio_station_count(void)
{
    return s_station_count;
}

int radio_add_station(const char *name, const char *url)
{
    if (!url || !url[0] || strlen(url) >= sizeof(s_station_urls[0])) {
        return -1;
    }
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -1;
    }

    /* first free slot */
    int idx = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (s_station_urls[i][0] == '\0') {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        xSemaphoreGive(s_station_mutex);
        ESP_LOGW(TAG, "station list full (%d)", MAX_STATIONS);
        return -1;
    }

    strlcpy(s_station_urls[idx], url, sizeof(s_station_urls[idx]));
    if (name && name[0]) {
        strlcpy(s_station_names[idx], name, sizeof(s_station_names[idx]));
    } else {
        /* no name given: take the last path element of the url */
        const char *last = strrchr(url, '/');
        const char *p = last ? last + 1 : url;
        char tmp[48];
        size_t n = strcspn(p, "?");
        if (n >= sizeof(tmp)) {
            n = sizeof(tmp) - 1;
        }
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        strlcpy(s_station_names[idx], tmp[0] ? tmp : "radio", sizeof(s_station_names[idx]));
    }
    s_stations[idx].name = s_station_names[idx];
    s_stations[idx].url  = s_station_urls[idx];
    if (idx + 1 > s_station_count) {
        s_station_count = idx + 1;
    }
    station_nvs_save(idx);
    station_nvs_save_count(s_station_count);
    xSemaphoreGive(s_station_mutex);

    ESP_LOGI(TAG, "station [%d] added: %s (%s)", idx, s_station_names[idx], s_station_urls[idx]);
    return idx;
}

bool radio_del_station(int index)
{
    if (index < 0 || index >= s_station_count) {
        return false;
    }
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    /* shift the following stations down so the list stays packed */
    for (int i = index; i < s_station_count - 1; i++) {
        strlcpy(s_station_names[i], s_station_names[i + 1], sizeof(s_station_names[i]));
        strlcpy(s_station_urls[i], s_station_urls[i + 1], sizeof(s_station_urls[i]));
    }
    s_station_count--;
    s_station_names[s_station_count][0] = '\0';
    s_station_urls[s_station_count][0]  = '\0';
    for (int i = 0; i < s_station_count; i++) {
        s_stations[i].name = s_station_names[i];
        s_stations[i].url  = s_station_urls[i];
        station_nvs_save(i);
    }
    station_nvs_save_count(s_station_count);
    xSemaphoreGive(s_station_mutex);

    ESP_LOGI(TAG, "station [%d] deleted, %d left", index, s_station_count);
    return true;
}

/* ---------------- i2s pin maps ---------------- */

int radio_i2s_pinmap_count(void)
{
    return (int)PINMAP_COUNT;
}

int radio_i2s_pinmap_current(void)
{
    return s_pinmap;
}

bool radio_i2s_pinmap_get(int idx, int *bck, int *ws, int *data)
{
    if (idx < 0 || idx >= (int)PINMAP_COUNT) {
        return false;
    }
    if (bck) {
        *bck = s_pinmaps[idx].bck;
    }
    if (ws) {
        *ws = s_pinmaps[idx].ws;
    }
    if (data) {
        *data = s_pinmaps[idx].data;
    }
    return true;
}

bool radio_set_i2s_pinmap(int idx)
{
    if (idx < 0 || idx >= (int)PINMAP_COUNT) {
        return false;
    }
    if (idx == s_pinmap) {
        ESP_LOGI(TAG, "i2s pins already [%s]", s_pinmaps[idx].desc);
        return true;
    }
    /* The radio task must not write to the i2s channel while it is being
     * reconfigured - stop the stream first and give it a moment to leave
     * the write call. */
    radio_stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    if (!i2s_apply_pinmap(idx)) {
        return false;
    }
    pinmap_nvs_save(s_pinmaps[idx].bck, s_pinmaps[idx].ws, s_pinmaps[idx].data);
    ESP_LOGI(TAG, "i2s pins (bck=%d ws=%d dout=%d) stored in nvs",
             s_pinmaps[idx].bck, s_pinmaps[idx].ws, s_pinmaps[idx].data);
    return true;
}

const char *radio_current_track(void)
{
    return s_track;
}

bool radio_is_playing(void)
{
    return s_playing;
}
