/*
* ============================================================================
*  radio_player.c —— 互联网收音机核心
* ============================================================================
*
*  【数据通路】
*    HTTP(S) 流 -> [ICY 元数据解析] -> [MP3 解码] -> [音量/响度] -> I2S
*                                                                    |
*                                                              MAX98357A
*
*  【线程】
*  - radio（电台任务）：本文件创建，负责收流、解码、写 I2S
*  - console（控制台任务，在 main.c）：只调用本文件的接口改状态
*  两者通过事件组 s_evt 和互斥锁 s_station_mutex 通信。
*
*  【三种输出方式】（Kconfig 里三选一，默认是 I2S）
*   - I2S  ：接数字功放 MAX98357A（主线，能出声、音质最好）
*   - PWM  ：LEDC 直驱耳机/小喇叭（诊断用）
*   - DAC  ：芯片内部 8 位 DAC 直驱（诊断用）
*  PWM/DAC 是"直驱输出模式"，只是用来在没有功放的情况下验证声音通路，
*  没有功放时用它们。
*
*  【编码】
*  本文件是 UTF-8（无 BOM）。电台发来的曲名（ICY 元数据）通常是
*  GBK，固件原样透传，打印效果取决于终端代码页（见 README）。
*/

#define MINIMP3_ONLY_MP3          // 只要 MP3 解码（不要 wav/flac）
#define MINIMP3_IMPLEMENTATION    // 在本文件里实现解码器（单文件版）
#include "minimp3.h"              // 第三方 MP3 解码库（components/minimp3）

#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp / strncasecmp
#include <stdlib.h>    // malloc / free

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"   // 事件组（控制台 -> 电台任务）
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include <math.h>            // sqrtf（响度计算）
#include "esp_http_client.h" // HTTP 客户端
#include "esp_timer.h"       // 定时器（节流日志用）
#include "esp_system.h"      // esp_get_free_heap_size（看剩余堆）
#include "driver/i2s_std.h"  // I2S 驱动
#include "driver/gpio.h"     // GPIO 驱动
#include "nvs.h"             // NVS 键值存储

#include "radio_player.h"   // 本模块的对外接口
#include "bt_player.h"      // 蓝牙状态（蓝牙连着时暂停电台重试，避免射频共存干扰）
#include "wifi_station.h"   // wifi_resume（蓝牙期间用户主动切台时恢复 WiFi）
#include "board_pins.h"     // 引脚定义（唯一来源）
#include "sdkconfig.h"      // 编译期配置

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
/* 用了 mbedTLS 证书包，https 流才能通过证书校验 */
extern esp_err_t esp_crt_bundle_attach(void *conf);
#endif

static const char *TAG = "radio";

/* ---- 事件组标志位：控制台用它们给电台任务下指令 ---- */
#define EVT_REQUEST     BIT0   /* 请求播放某个电台 */
#define EVT_STOP        BIT1   /* 请求停止 */
#define EVT_TONE        BIT2   /* 请求输出测试音 */

#define STREAM_BUF_SIZE ((size_t)CONFIG_RADIO_STREAM_BUF_SIZE)  // 流缓冲（堆上）
/* 每次从 socket 读多少字节。
 *
 * 【为什么不能太大】IDF 的 esp_http_client_read() 要【读满】才返回
 * （内部是 while (need_read > 0) 的循环），所以它阻塞的时间 = 攒满
 * len 字节的时间。128kbps 的流 4KB 要 256ms —— 电台的循环周期
 * 就是"读 256ms + 解码 256ms"≈512ms，停止指令要等这么久才能被看到。
 * 改成 2KB 之后周期只有 ~256ms，电台能在一个周期内停下来、
 * 把堆缓冲还给蓝牙栈。 */
#define HTTP_RD_SIZE    2048    // 每次从 socket 读多少字节
/* 电台表最多能放几个。编译进来的默认电台占前几个位置，其余的用 "add" 命令在运行时加。 */
#define MAX_STATIONS    16      // 电台表最大条数

static mp3dec_t            s_mp3d;     // 解码器状态
static size_t              s_fill;     // 流缓冲里已经攒了多少字节
static mp3dec_frame_info_t s_info;     // 最后一帧的解码信息（采样率/声道…）

/* 电台的三个大缓冲放在【堆】上，不播时释放 —— 内存按需占用。
 *
 * 【为什么现在是堆】静态 DRAM 只剩 12%，三个缓冲要 18KB，硬塞下去
 * 会导致蓝牙栈申请不到内存而崩溃重启（已发生过）。
 * 关键点是【电台和蓝牙互斥】：
 *   - 电台开播时，先把蓝牙彻底释放（bt_player_release，归还约 40KB），
 *     再分配这 18KB —— 此刻堆里只有电台一家；
 *   - 电台停止时，先释放这 18KB，再重新开启蓝牙。
 * 这样堆上永远只有一方的大块，不会互相挤压、也不会被反复切碎。
 * 电台和蓝牙的互斥本身由 I2S 锁（radio_i2s_lock）负责，与内存无关。 */
#define BUFS_PCM_OFF  0
#define BUFS_OUT_OFF  (BUFS_PCM_OFF + MINIMP3_MAX_SAMPLES_PER_FRAME * (int)sizeof(mp3d_sample_t))
#define BUFS_STREAM_OFF (BUFS_OUT_OFF + MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * (int)sizeof(int16_t))
#define BUFS_TOTAL    (BUFS_STREAM_OFF + (int)STREAM_BUF_SIZE)

static uint8_t            *s_bufs;                       // 播放时才从堆上取，不播为 NULL
#define s_pcm        ((mp3d_sample_t *)(s_bufs + BUFS_PCM_OFF))   // 解码输出
#define s_out        ((int16_t *)(s_bufs + BUFS_OUT_OFF))         // 立体声输出
#define s_stream_buf (s_bufs + BUFS_STREAM_OFF)                   // 未解码的 MP3 数据

/**
* @brief 启用音频缓冲（开始播放时调用）。
*
* 缓冲本体从【堆】上取，只有电台在播的这段时间才占内存；
* 停止时 radio_bufs_free() 真正 free 掉，把内存还给蓝牙。
*
* @return 成功 true，堆不够 false
*/
static bool radio_bufs_alloc(void)
{
    if (s_bufs != NULL) {
        return true;                    // 已经启用过了
    }
    s_bufs = malloc(BUFS_TOTAL);        // 播放期间才占用堆
    if (s_bufs == NULL) {
        ESP_LOGW(TAG, "not enough heap for the %d byte audio buffers",
                 (int)BUFS_TOTAL);
        return false;
    }
    memset(s_bufs, 0, BUFS_TOTAL);      // 清零（避免读到脏数据）
    return true;
}

/**
* @brief 停用音频缓冲（停止播放时调用）：真正 free，把内存还给堆。
*
* 电台和蓝牙互斥，所以停播时把这块还出来，蓝牙连上来就有充足内存。
*/
static void radio_bufs_free(void)
{
    if (s_bufs != NULL) {
        free(s_bufs);                    // 归还堆（电台不播时）
        s_bufs = NULL;
    }
    s_fill = 0;                           // 流缓冲的已用长度归零
}

static int64_t             s_last_decode_err_us;  // 上次解码错误日志的时刻
static int64_t             s_last_overflow_us;    // 上次缓冲区溢出日志的时刻
static int64_t             s_last_rate_log_us;    // 上次采样率变化日志的时刻
static int                 s_404_count;   /* consecutive http 404s on the current station */
static volatile int        s_tone_freq = 1000;    // 测试音频率
static volatile int        s_tone_ms   = 2000;    // 测试音时长

/**
* @brief 限流打印：同一个 interval_ms 内最多打一条。
*
* 避免网络抖动时刷屏把串口输出淹掉。
*
* @param last_us     上次打印的时刻（会被更新）
* @param interval_ms 最小间隔（毫秒）
* @return 该不该打印
*/
static bool log_throttled(int64_t *last_us, int64_t interval_ms)
{
    int64_t now = esp_timer_get_time();            // 当前时刻（微秒）
    if (now - *last_us < interval_ms * 1000) {     // 还没到间隔
        return false;
    }
    *last_us = now;                                // 记下这次的时刻
    return true;
}

static i2s_chan_handle_t   s_i2s;                    // I2S 通道句柄
static int                 s_i2s_rate;               // 当前 I2S 采样率
static bool                s_rate_locked;   /* i2s clock is reconfigured at most once */

static EventGroupHandle_t  s_evt;                   // 事件组：控制台 -> 电台任务
static SemaphoreHandle_t   s_station_mutex; /* guards the station list (console vs radio task) */
static volatile int        s_volume    = CONFIG_RADIO_INITIAL_VOLUME;  // 音量 0~100
static volatile int        s_target    = 0;         // 要播的电台下标
static volatile int        s_cur       = -1;        // 当前电台下标
static volatile bool       s_playing   = false;     // 是否在播放
static char                s_track[128];             // 当前曲名

static char                s_station_names[MAX_STATIONS][48];  // 电台名
static char                s_station_urls[MAX_STATIONS][160];  // 电台地址
static radio_station_t     s_stations[MAX_STATIONS];          // 对外的电台表
static int                 s_station_count;                    // 电台个数

/* I2S 引脚映射表。默认那套是【已实测能出声】的接线，另一套给重新接过线的人用。 * 选择结果存进 NVS，所以任何一次重烧都不会再把功放改哑。 */
typedef struct {
    int         bck, ws, data;   // 三个 I2S 引脚
    const char *desc;            // 描述（显示用）
} i2s_pinmap_t;

static const i2s_pinmap_t s_pinmaps[] = {
    { PIN_I2S_BCK,      PIN_I2S_WS,      PIN_I2S_DATA,      "default (verified)" },
    { PIN_I2S_BCK_ALT,  PIN_I2S_WS_ALT,  PIN_I2S_DATA_ALT,  "legacy" },
};
#define PINMAP_COUNT (sizeof(s_pinmaps) / sizeof(s_pinmaps[0]))
static int s_pinmap = 0;                            // 当前用的映射编号
static radio_event_cb_t       s_cb;                  // 事件回调
static void                  *s_cb_ctx;              // 回调上下文

/**
* @brief 向上层报告一个状态变化。
*/
static void notify(radio_event_t event, const char *msg)
{
    if (s_cb) {
        s_cb(event, msg ? msg : "", s_cb_ctx);
    }
}

/* ================= 直驱输出模式（PWM / DAC，仅诊断用） ================= *
*
*  两个后端，一套引擎：
*    CONFIG_RADIO_AUDIO_PWM : LEDC PWM 输出到 GPIO，100kHz 载波，8 位占空比
*    CONFIG_RADIO_AUDIO_DAC : ESP32 内部 8 位 DAC（GPIO25 / GPIO26）
*
*  两者都由硬件定时器中断（gptimer，16kHz）采样：每次中断取一个采样、
*  写一次寄存器。完全不经过 esp_timer 任务，所以 WiFi 协议栈不会影响
*  音频时序，也不会把 idle 任务饿死（那正是任务/中断看门狗 panic 的原因）。
*
*  特意【不】在中断里调 ledc_update_duty()：它会在关中断的临界区里忙等
*  硬件的 duty_start 标志（ledc_ll.h:571），16kHz 下会把 CPU 卡死并触发看门狗。
*  直接写寄存器是安全的：PWM 周期是 10us 而采样周期是 62us，新的占空比更新
*  不可能和上一次重叠，永远不需要等。
*
*  接线（PWM）：gpio -> 22R -> 耳机 L+R 并联，gnd -> gnd。
*  接线（DAC）：gpio -> 隔直电容（1~100uF）-> 耳机 L+R，gnd -> gnd。
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

/* ================= 后端：一个采样 -> 一次寄存器写 ================= */

#if defined(CONFIG_RADIO_AUDIO_PWM)     // ---- PWM 直驱后端 ----

#include "driver/ledc.h"
#include "soc/ledc_struct.h"      // 直接操作 LEDC 寄存器

#define PWM_CARRIER_HZ      100000     // PWM 载波频率 100kHz
#define PWM_DUTY_RES        LEDC_TIMER_8_BIT   // 占空比分辨率 8 位

/**
* @brief 把一个 16 位采样写进 PWM 占空比（跑在中断里）。
*
* 直接写寄存器，绕开 ledc_update_duty()（它会忙等，见上面的说明）。
*/
static inline void IRAM_ATTR audio_write_sample(int16_t sample)
{
    uint32_t duty = (uint32_t)(((int32_t)sample + 32768) >> 8); /* 0..255 */
/* 就是 LEDC 驱动的那两行寄存器写，只是不带忙等 */
    LEDC.channel_group[0].channel[0].duty.duty       = duty << 4;   // 写占空比
    LEDC.channel_group[0].channel[0].conf1.duty_start = 1;          // 启动输出
}

#else /* CONFIG_RADIO_AUDIO_DAC */     // ---- DAC 直驱后端 ----

#include "driver/dac_oneshot.h"
#include "soc/rtc_io_struct.h"    // 直接操作 RTC IO 寄存器

static dac_oneshot_handle_t s_dac_chan;   // DAC 通道句柄
static int                  s_dac_ch = DAC_CHAN_0;   // 用的哪个 DAC 通道

/**
* @brief 把一个 16 位采样写进内部 DAC（跑在中断里）。
*/
static inline void IRAM_ATTR audio_write_sample(int16_t sample)
{
    uint8_t v = (uint8_t)(((int32_t)sample + 32768) >> 8);   // 16 位 -> 8 位
    if (s_dac_ch == DAC_CHAN_0) {
        RTCIO.pad_dac[0].dac = v;     // 通道 0 = GPIO25
    } else {
        RTCIO.pad_dac[1].dac = v;     // 通道 1 = GPIO26
    }
}

#endif

/* ================= 引擎 ================= */

/**
* @brief 硬件定时器的中断服务函数：每次中断取一个采样输出。
*
* @return false = 不需要让出（不需要 yield）
*/
static bool IRAM_ATTR audio_sample_isr(gptimer_handle_t timer,
                                       const gptimer_alarm_event_data_t *edata,
                                       void *user_ctx)
{
    int16_t sample = 0;                          // 默认输出静音
    if (s_audio_head != s_audio_tail) {           // 环里还有数据？
        sample = s_audio_buf[s_audio_tail];       // 取一个
        s_audio_tail = (s_audio_tail + 1) & AUDIO_BUF_MASK;   // 读指针前进
    }
    audio_write_sample(sample);                   // 输出
    return false; /* no yield needed */
}

/**
* @brief 环形缓冲里还剩多少空间（可写的采样数）。
*/
static int audio_out_free(void)
{
    return AUDIO_BUF_MASK - ((s_audio_head - s_audio_tail) & AUDIO_BUF_MASK);
}

/**
* @brief 等到环形缓冲有足够空间（起反压作用）。
*
* 中断按采样率取数据，所以这里就把生产者的速度压到音频时钟上；
* 网络抖动则由 MP3 流缓冲吸收，不会变成声音里的断断续续。
*
* @param need       需要的空间（采样数）
* @param timeout_ms 最长等待（毫秒）
* @return true = 有空间了；false = 超时或被要求停止
*/
static bool audio_out_wait_space(int need, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;  // 截止时刻
    while (audio_out_free() < need) {              // 空间还不够就一直等
        if (xEventGroupWaitBits(s_evt, EVT_STOP, pdFALSE, pdFALSE, 0) & EVT_STOP) {
            return false;                          // 收到停止指令
        }
        if (esp_timer_get_time() >= deadline) {
            return false;                          // 超时
        }
/* 注意：tick 频率是 100Hz，所以 pdMS_TO_TICKS(2) 会得到 0 tick，* 而 vTaskDelay(0) 只是让出 CPU —— 任务会空转、永不阻塞，* 把 idle 任务饿死（触发任务看门狗 panic）。1 个 tick = 10ms。 */
        vTaskDelay(1);                             // 让出 1 个 tick（10ms）
    }
    return true;
}

/**
* @brief 把一个采样放进环形缓冲（满了就先等）。
*/
static void audio_push(int16_t sample)
{
/* 反压：环形缓冲不能溢出。I2S 版本靠阻塞的 DMA 写天然就有这个保证；* 寄存器直驱的话生产者会跑在前面，环溢出会静默丢采样 —— 听感就是一顿一顿。 */
    if (audio_out_free() < 64) {                   // 剩余空间不足 64 个采样
        if (!audio_out_wait_space(64, 1000)) {     // 等最多 1 秒
            return; /* timed out or stopped: drop the sample */
        }
    }
    s_audio_buf[s_audio_head] = sample;            // 写进环里
    s_audio_head = (s_audio_head + 1) & AUDIO_BUF_MASK;   // 写指针前进
}

/**
* @brief 初始化直驱输出（LEDC PWM 或内部 DAC）+ 采样定时器。
*/
static void audio_out_init(void)
{
#if defined(CONFIG_RADIO_AUDIO_PWM)
    ledc_timer_config_t timer_cfg = {                 // PWM 定时器配置
        .speed_mode      = LEDC_HIGH_SPEED_MODE,      // 高速模式
        .duty_resolution = PWM_DUTY_RES,              // 8 位占空比
        .timer_num       = LEDC_TIMER_0,              // 用定时器 0
        .freq_hz         = PWM_CARRIER_HZ,            // 100kHz 载波
        .clk_cfg         = LEDC_AUTO_CLK,             // 自动选时钟源
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
/* 定时器没配好，通道就是死的，更新占空比会卡在驱动的临界区里（触发中断看门狗 panic）。 */
        ESP_LOGE(TAG, "ledc timer config failed: %s", esp_err_to_name(err));
        return;                                       // 定时器都没了，后面的别做了
    }

    ledc_channel_config_t ch_cfg = {                 // PWM 通道配置
        .gpio_num   = PIN_PWM,                        // 输出引脚
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = AUDIO_SILENCE,                  // 初始占空比 = 静音
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel config failed: %s", esp_err_to_name(err));
        return;
    }
/* 最强驱动能力：耳机是低阻负载，GPIO 自身的输出阻抗（几十欧）会吃掉大部分摆幅。 */
    gpio_set_drive_capability(PIN_PWM, GPIO_DRIVE_CAP_3);   // 最强驱动能力
#else
    s_dac_ch = (PIN_DAC_CHANNEL == 1) ? DAC_CHAN_1 : DAC_CHAN_0;   // 选通道
    dac_oneshot_config_t dac_cfg = {
        .chan_id = (dac_channel_t)s_dac_ch,
    };
    esp_err_t err = dac_oneshot_new_channel(&dac_cfg, &s_dac_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac channel init failed: %s", esp_err_to_name(err));
        return;
    }
/* 打开 pad 输出并清掉连续波模式 */
    dac_oneshot_output_voltage(s_dac_chan, AUDIO_SILENCE);      // 输出静音电平
#endif

    gptimer_config_t gcfg = {                        // 采样定时器配置
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
        .on_alarm = audio_sample_isr,                // 每次闹钟触发采样中断
    };
    err = gptimer_register_event_callbacks(s_audio_timer, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_register_event_callbacks failed: %s", esp_err_to_name(err));
        return;
    }

    gptimer_alarm_config_t acfg = {
        .alarm_count = 8000000 / AUDIO_SAMPLE_RATE, /* 500 -> exactly 16 kHz */
        .flags.auto_reload_on_alarm = true,         // 自动重装 = 周期触发
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

/**
* @brief 把交错的立体声 PCM 重采样成 16kHz 单声道并送进环形缓冲。
*
* 直驱模式只有 8 位、16kHz，所以要降采样；这里用最简单的"取左声道 + 丢样"
* 方式（够用于诊断，不追求音质）。
*
* @param stereo 交错的左右声道数据
* @param frames 帧数（一帧 = 左右各一个采样）
* @param rate   源采样率
*/
static void audio_out_write(const int16_t *stereo, int frames, int rate)
{
    if (rate <= 0) {
        rate = AUDIO_SAMPLE_RATE;                   // 源采样率无效就按 16kHz 处理
    }
    float step = (float)AUDIO_SAMPLE_RATE / rate;   // 降采样步长
    for (int i = 0; i < frames; i++) {              // 逐帧处理
        s_resample_pos += step;                     // 累加相位
        if (s_resample_pos >= 1.0f) {               // 够了就输出一个采样
            s_resample_pos -= 1.0f;                 // 相位回绕
            audio_push(stereo[2 * i]); /* left channel */
        }
    }
}

#endif /* CONFIG_RADIO_AUDIO_DAC || CONFIG_RADIO_AUDIO_PWM */

/* ================= 状态指示灯 ================= */

/**
* @brief 状态灯开关（播放时点亮）。
*/
static void led_set(bool on)
{
    gpio_set_level(PIN_STATUS_LED, on ? 1 : 0);
}

/* ---------------- I2S ---------------- */

/**
* @brief 初始化 I2S 通道（标准模式 / Philips 格式 / 16 位 / 立体声）。
*
* MCLK 不接（MAX98357A 只需要 BCLK/LRC/DIN），省一根线。
*/
static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    /* 【为什么 DMA 要这么深】蓝牙栈不是匀速喂数据的：调度一抖动就一次吐
     * 好几包（实测一次突发 ≈ 7000 采样 ≈ 160ms）。I2S 时钟固定 44100/s，
     * 消费端【永远追不上】突发，只能靠缓冲吸收。
     *
     * 堆里放不下更大的环形缓冲（蓝牙协议栈要 ~40KB，环超过 16KB
     * 协议栈连接时就会 OOM 崩溃），但 **DMA 缓冲不占堆** —— 它在内部
     * DMA RAM 里。16 × 240 帧 = 3840 帧 ≈ 87ms，叠上 16KB 的环
     * 一共 ≈ 260ms 的吸收能力，突发就不会把环撑爆
     * （撑爆 = 丢音频 = 听感上的"破音"）。 */
    chan_cfg.dma_desc_num  = 16;     // 16 个 DMA 描述符
    chan_cfg.dma_frame_num = 240;    // 每个描述符 240 帧
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s, NULL), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),   // 初始 44.1kHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    };
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;             // 不接主时钟
    std_cfg.gpio_cfg.bclk = s_pinmaps[s_pinmap].bck;     // 位时钟
    std_cfg.gpio_cfg.ws   = s_pinmaps[s_pinmap].ws;       // 左右声道选择
    std_cfg.gpio_cfg.dout = s_pinmaps[s_pinmap].data;    // 数据输出
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;             // 不接收

    s_i2s_rate = 44100;                                 // 记住当前时钟
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s, &std_cfg), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s), TAG, "i2s enable failed");
    ESP_LOGI(TAG, "i2s ready (bck=%d ws=%d dout=%d) [%s]",
             s_pinmaps[s_pinmap].bck, s_pinmaps[s_pinmap].ws, s_pinmaps[s_pinmap].data,
             s_pinmaps[s_pinmap].desc);
    return ESP_OK;
}

/**
* @brief 不重启就把 I2S 换到另一组引脚。
*
* 用来在几种可能的接线里找出真正能出声的那一组。
*
* @param idx 引脚映射编号
* @return 成功 true
*/
static bool i2s_apply_pinmap(int idx)
{
    if (idx < 0 || idx >= (int)PINMAP_COUNT || s_i2s == NULL) {
        return false;                          // 编号非法或还没初始化
    }
    const i2s_pinmap_t *pm = &s_pinmaps[idx]; // 要用的那组引脚

    i2s_std_gpio_config_t gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = pm->bck,
        .ws   = pm->ws,
        .dout = pm->data,
        .din  = I2S_GPIO_UNUSED,
    };

    i2s_channel_disable(s_i2s);                // 先关掉通道才能改引脚
    esp_err_t err = i2s_channel_reconfig_std_gpio(s_i2s, &gpio_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_i2s);      // 再打开
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s pin switch failed: %s", esp_err_to_name(err));
        return false;
    }
    s_pinmap = idx;                            // 记下当前映射
    ESP_LOGI(TAG, "i2s pins -> [%s] bck=%d ws=%d dout=%d", pm->desc, pm->bck, pm->ws, pm->data);
    return true;
}

/**
* @brief 按流的采样率重新设置 I2S 时钟。
*
* 换台/重连后，【第一条音频帧】会重新锁定时钟（此时还没有数据流动，
* 不会出现和 DMA 中断抢资源的问题）。
* 一条流播到一半再变采样率就只打日志、不动时钟（限流打印）。
*/
static void i2s_set_rate(int rate)
{
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    return; /* direct output runs at a fixed rate */
#else
    if (rate <= 0) {
        return;                               // 非法采样率，忽略
    }
    if (s_rate_locked) {                      // 这条流已经锁定过时钟了
        if (rate != s_i2s_rate && log_throttled(&s_last_rate_log_us, 5000)) {
            ESP_LOGW(TAG, "stream rate %d Hz, keeping i2s at %d Hz", rate, s_i2s_rate);
        }
        return;
    }

    if (rate != s_i2s_rate) {                 // 采样率不一样：重新配置时钟
        ESP_LOGI(TAG, "stream sample rate %d Hz, reconfiguring i2s", rate);
        i2s_channel_disable(s_i2s);
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        if (i2s_channel_reconfig_std_clock(s_i2s, &clk_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "i2s reconfig failed");
        }
        i2s_channel_enable(s_i2s);
        s_i2s_rate = rate;                    // 记下新采样率
    } else {
        ESP_LOGI(TAG, "i2s clock stays at %d Hz (stream wants %d Hz)",
                 s_i2s_rate, rate);
    }
    s_rate_locked = true; /* first frame of this stream decides the clock */
#endif
}

/* ================= 共用的 I2S 输出 =================
*
* 蓝牙和电台写的是同一个 I2S 通道，同一时刻只能有一个占着：
* 手机连上时蓝牙接管（电台停），手机断开时电台拿回来。
*/
static volatile bool s_i2s_locked;   /* 蓝牙占着 I2S 时为 true */


/**
* @brief 锁住/解锁 I2S（蓝牙调用）。
*/
void radio_i2s_lock(bool locked)
{
    s_i2s_locked = locked;
}

/**
* @brief I2S 当前是否被蓝牙占着。
*/
bool radio_i2s_locked(void)
{
    return s_i2s_locked;
}

/**
* @brief 往 I2S 写原始 PCM（蓝牙播放器用）。

*
* @param data        数据
* @param bytes       字节数
* @param timeout_ms  超时（毫秒）
* @return 写成功 true
*/
bool audio_i2s_write(const void *data, size_t bytes, uint32_t timeout_ms)
{
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    (void)data;                              // 直驱模式没有 I2S
    (void)bytes;
    (void)timeout_ms;
    return false;
#else
    if (s_i2s == NULL) {
        return false;                        // 还没初始化
    }
    size_t written = 0;                      // 实际写了多少
    esp_err_t err = i2s_channel_write(s_i2s, data, bytes, &written, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
        return false;
    }
    return written == bytes;                 // 必须全部写完
#endif
}

/**
* @brief 强制把 I2S 时钟设成指定采样率（蓝牙流固定 44.1kHz）。
*/
bool audio_i2s_set_rate(int rate)
{
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    (void)rate;
    return false;
#else
    if (s_i2s == NULL || rate <= 0) {
        return false;
    }
    /* 已经是这个采样率了就不用动。
     * （以前每次调用都会清掉 s_rate_locked 再进 i2s_set_rate()，
     *   结果每被唤醒一次就打一行日志 —— 115200 波特率下这些字
     *   会阻塞音频任务，反而把音频搞出破音。） */
    if (rate == s_i2s_rate) {
        return true;
    }
    s_rate_locked = false;   /* allow the reconfiguration */
    i2s_set_rate(rate);
    return true;
#endif
}

/* ================= 功放 SD 脚控制（诊断用） =================
*
* 用 GPIO 驱动 MAX98357A 的 SD 脚，不用示波器就能判断模块死活：
*
*   mode -1 : gpio 高阻     -> 模块默认（1M 上拉：(L+R)/2 混合模式）
*   mode  0 : gpio 拉低     -> 功放关断，输出应掉到 ~0V —— 说明芯片是活的
*   mode  1 : gpio 拉高     -> 只放左声道，15dB 增益
*
* 平时这个脚是输入（高阻），不影响正常播放。
* 接线：gpio -> 22R -> SD 脚。22R 够用：它能把模块的 1M 上拉拉到 ~0.07V，
* 远低于 0.16V 的关断门限。
*/
bool radio_set_amp_sd(int mode)

{
#if (PIN_AMP_SD >= 0)
    const int pin = PIN_AMP_SD;
    if (mode < 0) {
        gpio_set_direction(pin, GPIO_MODE_INPUT);   // 悬空 = 模块自己决定
        ESP_LOGI(TAG, "amp sd: float (module default)");
    } else {
        gpio_set_level(pin, mode ? 1 : 0);           // 先设置电平
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);   // 再切成输出
        ESP_LOGI(TAG, "amp sd: %s", mode ? "high (left channel, 15 dB)" : "low (shutdown)");
    }
    return true;
#else
    (void)mode;
    ESP_LOGW(TAG, "amp sd control not configured (PIN_AMP_SD)");
    return false;                                      // 没配这个功能
#endif
}

/* ================= PCM 输出 ================= */

/* ================= 响度（自动增益） =================
*
* 峰值归一化（跟基线版本一致）：让输出峰值稳定落在 SAFE_PEAK 附近。
* 基线一直用这套，声音正常 —— 关键在于【峰值】永远不超过 32767，
* 不会触发 output_frame 里的 32767 硬削波（那才是"破音"的真凶）。
*
* 之前改成"RMS 自动增益"后，对鼓点/镲片这类高峰值因子瞬态，RMS 算出的
* 增益会把峰值顶过 32767 → 硬削波 → 破音。所以改回峰值归一化。
*
* 平滑：快攻（瞬态一来立刻压住）/4、慢放（之后慢慢回来）/32，避免"抽吸感"。
*/
#define SAFE_PEAK        30000   /* 输出峰值目标（略低于 32767，永不触发硬削波） */
#define SAFE_GAIN_MAX    (12 << 8)  /* 最高 +21.5 dB（很轻的台也能拉得响） */
#define SAFE_GAIN_MIN    (64)       /* 最低 0.25× */

static int32_t s_audio_gain = 256;   /* Q8 定点，1.0 = 256 */

static bool    s_agc_enabled = true;
static bool    s_stream_info_logged = false;

/**
* @brief 开关自动响度增益（"norm 0/1" 命令）。
*
* 关掉后完全不做自动增益 —— 电平保持录制时的样子。
*/
void radio_set_normalizer(bool on)
{
    s_agc_enabled = on;
/* 状态清零重来，让改动立刻能听出来 */
    s_audio_gain = 256;              // 增益回到 1.0
    ESP_LOGI(TAG, "auto gain: %s", on ? "on (loudness normalized)" : "off (raw level)");
}

/**
* @brief 自动响度增益当前是开还是关。
*/
bool radio_normalizer_enabled(void)
{
    return s_agc_enabled;
}

/**
* @brief 峰值归一化：算出这一帧的增益，让输出峰值稳定落在 SAFE_PEAK 附近。
*
* 用【峰值】而不是 RMS —— 这样输出峰值永远 ≤ SAFE_PEAK < 32767，
* 不会触发 output_frame 里的 32767 硬削波（那才是"破音"）。
*
* @param src   这一帧的 PCM
* @param total 采样数（声道数已算进去）
*/
static void audio_gain_update(const int16_t *src, int total)
{
    if (!s_agc_enabled) {
        s_audio_gain = 256;   /* no automatic gain at all */
        return;
    }
    if (total <= 0) {
        return;               // 没有数据
    }

    int32_t peak = 0;                           // 这一帧的真实峰值
    for (int i = 0; i < total; i++) {
        int32_t a = (src[i] < 0) ? -(int32_t)src[i] : (int32_t)src[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak < 256) {
        return;   /* 太安静，保持当前增益 */
    }

    int32_t target = (SAFE_PEAK << 8) / peak;    /* 让这一帧的峰值 = SAFE_PEAK */
    if (target > SAFE_GAIN_MAX) {
        target = SAFE_GAIN_MAX;                  /* 很轻的台最多拉到 +21.5dB */
    }
    if (target < SAFE_GAIN_MIN) {
        target = SAFE_GAIN_MIN;                  /* 很响的台最多压到 0.25× */
    }

/* 快攻慢放：瞬态一来立刻压住（/4），之后慢慢回来（/32），不抽吸 */
    if (target > s_audio_gain) {
        s_audio_gain += (target - s_audio_gain) / 4;    /* 快攻 */
    } else {
        s_audio_gain += (target - s_audio_gain) / 32;   /* 慢放 */
    }
}

/**
* @brief 把一帧解码好的 PCM 处理后送进输出（I2S 或直驱）。
*
* @param samples_per_ch 每声道的采样数
*/
static void output_frame(int samples_per_ch)
{
    int ch = s_info.channels > 1 ? 2 : 1;      // 声道数（1 或 2）
    const int16_t *src = (const int16_t *)s_pcm;// 解码输出
    int vol = s_volume;                        // 用户音量
    int total = samples_per_ch * ch;           // 总采样数

    if (vol < 0)   vol = 0;                    // 音量限幅
    if (vol > 100) vol = 100;

/* 先做响度归一化，再乘用户音量 */
    audio_gain_update(src, total);             // 先算响度增益
    int32_t g = (s_audio_gain * vol) / 100;   /* Q8, gain * volume */

    if (ch == 1) {                             // 单声道 -> 复制成左右
        for (int i = 0; i < samples_per_ch; i++) {
            int32_t v = ((int32_t)src[i] * g) >> 8;   // 定点乘法
            if (v > 32767) {
                v = 32767;                     // 限幅（防溢出回绕）
            } else if (v < -32768) {
                v = -32768;
            }
            s_out[2 * i]     = (int16_t)v;     // 左
            s_out[2 * i + 1] = (int16_t)v;     // 右
        }
        total *= 2;                            // 输出变成双声道
    } else {                                   // 立体声：直接处理
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

    size_t written = 0;                       // 实际写出的字节数
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    audio_out_write(s_out, samples_per_ch, s_info.hz);   // 直驱模式
    (void)written;
#else
/* 蓝牙接管期间，I2S 是它的 —— 电台绝不能同时往里写，
* 否则两个写者的数据会在 DMA 里交错，听起来就是"破音"。 */
    if (radio_i2s_locked()) {
        return;
    }
    i2s_channel_write(s_i2s, s_out, total * sizeof(int16_t), &written, pdMS_TO_TICKS(2000));
#endif
}

/* 流缓冲里有多少 MP3 帧就解多少帧 */
static void decode_loop(void)
{
    size_t consumed = 0;

/* 跳过流开头的 ID3v2 标签 */
    if (s_fill >= 10 && memcmp(s_stream_buf, "ID3", 3) == 0) {   // 有 ID3v2 标签？
        uint32_t size = 0;                    // 标签长度是 4 个 7 位字节
        for (int i = 6; i < 10; i++) {
            size = (size << 7) | (s_stream_buf[i] & 0x7F);
        }
        size_t total = (size_t)10 + size;    // 标签总长（含 10 字节头）
        if (total <= s_fill) {
            memmove(s_stream_buf, s_stream_buf + total, s_fill - total);  // 跳过标签
            s_fill -= total;
        } else {
            s_fill = 0; /* tag not complete yet, drop what we have */
            return;                             // 标签还没收全，先扔掉等下次
        }
    }

    while (consumed < s_fill) {               // 逐帧解码
        int avail = (int)(s_fill - consumed); // 还没解析的字节数
        int frame_bytes = 0;                  // 这一帧占多少字节

/* 返回这一帧在缓冲里的偏移，frame_bytes = 帧长 + 填充 */
        int off = mp3d_find_frame(s_stream_buf + consumed, avail, &s_mp3d.free_format_bytes, &frame_bytes);
        if (off >= avail || off + frame_bytes > avail) {
            break; /* no complete frame yet, wait for more data */
        }

        memset(&s_info, 0, sizeof(s_info));   // 清空解码信息
        int samples = mp3dec_decode_frame(&s_mp3d, s_stream_buf + consumed + off, avail - off, s_pcm, &s_info);
        if (samples > 0) {                    // 解码成功
            if (!s_stream_info_logged) {      // 每条流只打一次参数
                s_stream_info_logged = true;
                ESP_LOGI(TAG, "  mp3: %d Hz, %d ch, %d kbps",
                         s_info.hz, s_info.channels, s_info.bitrate_kbps);
            }
            i2s_set_rate(s_info.hz);          // 按这一帧的采样率锁定 I2S 时钟
            consumed += off + frame_bytes;    // 这一帧处理完
            output_frame(samples);            // 送进输出
        } else {                              // 解码失败：跳过这一帧
            if (log_throttled(&s_last_decode_err_us, 2000)) {
                ESP_LOGW(TAG, "mp3 decode error, skipping frame");
            }
            consumed += off + frame_bytes;
        }
    }

    if (consumed > 0) {                      // 把没用的数据挪走
        memmove(s_stream_buf, s_stream_buf + consumed, s_fill - consumed);
        s_fill -= consumed;                   // 剩余字节数
    }
}

/* ================= 电台表（运行时可改，存 NVS） ================= */

/* NVS 命名空间：电台表、音量、I2S 引脚映射都存在这里。
 * 【音量持久化默认关闭】只有在 menuconfig 里打开
 * CONFIG_RADIO_VOLUME_PERSIST 才会把 "vol <n>" 存进 NVS 并在开机时
 * 恢复；否则每次重启都用 CONFIG_RADIO_INITIAL_VOLUME 这个默认音量，
 * 想要多大声音自己调。 */
#define STATION_NVS_NS "radio"

/* ---- 音量是否重启后保持（默认不保持，见 Kconfig 的 RADIO_VOLUME_PERSIST） ---- */
#if CONFIG_RADIO_VOLUME_PERSIST
#define VOLUME_PERSIST 1
#else
#define VOLUME_PERSIST 0
#endif

#if VOLUME_PERSIST

/**
* @brief 从 NVS 读出用户设置的音量。
*
* @return 音量值；没存过返回 -1（此时用 Kconfig 的默认值）
*/
static int volume_nvs_load(void)
{
    nvs_handle_t handle;                       // NVS 句柄
    int32_t value = -1;                        // 默认"没有"
    if (nvs_open(STATION_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return -1;                             // 命名空间都打不开
    }
    if (nvs_get_i32(handle, "volume", &value) != ESP_OK) {
        value = -1;                            // 键不存在
    }
    nvs_close(handle);
    return (int)value;
}

/**
* @brief 把音量写进 NVS（"vol" 命令调用，重启后仍然有效）。
*/
static void volume_nvs_save(int volume)
{
    nvs_handle_t handle;
    if (nvs_open(STATION_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;                                // 打不开就放弃（不影响播放）
    }
    nvs_set_i32(handle, "volume", (int32_t)volume);
    nvs_commit(handle);                        // 提交才落盘
    nvs_close(handle);
}

#endif /* VOLUME_PERSIST */

/**
* @brief 从 NVS 读出第 idx 个电台（名字 + 地址）。
*/
static void station_nvs_load(int idx)
{
    nvs_handle_t handle;
    if (nvs_open(STATION_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    char key[16];                              // 键名缓冲
    size_t len = sizeof(s_station_urls[idx]);  // 目标缓冲区大小
    snprintf(key, sizeof(key), "url%d", idx);  // 键名 = "url0" / "url1" ...
    if (nvs_get_str(handle, key, s_station_urls[idx], &len) == ESP_OK) {
        len = sizeof(s_station_names[idx]);
        snprintf(key, sizeof(key), "name%d", idx);
        nvs_get_str(handle, key, s_station_names[idx], &len);   // 名字读不到就保持原样
    }
    nvs_close(handle);
}

/**
* @brief 把第 idx 个电台（名字 + 地址）写进 NVS。
*/
static void station_nvs_save(int idx)
{
    nvs_handle_t handle;
    if (nvs_open(STATION_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    char key[16];
    snprintf(key, sizeof(key), "name%d", idx);
    nvs_set_str(handle, key, s_station_names[idx]);   // 存名字
    snprintf(key, sizeof(key), "url%d", idx);
    nvs_set_str(handle, key, s_station_urls[idx]);    // 存地址
    nvs_commit(handle);
    nvs_close(handle);
}

/* 电台个数也存起来：运行时加的电台重启后还在，删掉的也保持删掉。 */

/**
* @brief 读出电台个数。
*
* @return 个数；没存过返回 -1（用编译进固件的默认表）
*/
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

/**
* @brief 存下电台个数（add/del 之后调用）。
*/
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

/* I2S 引脚映射会被记住，这样改掉默认引脚的固件升级也不会把已经能出声的功放改哑。存的是引脚本身（不是表里的序号）—— 以后重排或扩充映射表，也不会悄悄改变已经保存的选择的含义。 */

/**
* @brief 读出已保存的 I2S 引脚。
*
* 存的是【引脚本身】而不是表里的序号 —— 以后增删/重排映射表也不会
* 让已经保存的选择含义漂移，任何一次烧录都不会再把能出声的功放改哑。
*
* @return 存过返回 true
*/
static bool pinmap_nvs_load(int *bck, int *ws, int *data)
{
    nvs_handle_t handle;
    int32_t b = 0, w = 0, d = 0;
    if (nvs_open(STATION_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_get_i32(handle, "pin_bck", &b) == ESP_OK) &&    // 三个都要有
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

/**
* @brief 存下当前使用的 I2S 引脚。
*/
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

/* ================= HTTP / ICY（ICY 元数据） ================= */

typedef struct {
    int  metaint;
    char content_type[32];
/* 服务端自己说它在放什么 —— 这正是"同一个地址内容不一样"的线索：* CDN 可能给了另一个源、另一个码率，或者重定向到别的边缘节点。 */
    char icy_br[16];
    char icy_name[64];
    char icy_genre[32];
    char location[192];
} http_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (ctx == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            ctx->metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "content-type") == 0) {
            strlcpy(ctx->content_type, evt->header_value, sizeof(ctx->content_type));
        } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
            strlcpy(ctx->icy_br, evt->header_value, sizeof(ctx->icy_br));
        } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
            strlcpy(ctx->icy_name, evt->header_value, sizeof(ctx->icy_name));
        } else if (strcasecmp(evt->header_key, "icy-genre") == 0) {
            strlcpy(ctx->icy_genre, evt->header_value, sizeof(ctx->icy_genre));
        } else if (strcasecmp(evt->header_key, "location") == 0) {
            strlcpy(ctx->location, evt->header_value, sizeof(ctx->location));
        }
    }
    return ESP_OK;
}

/* ================= HTTP 客户端辅助函数 ================= */

/**
* @brief 建一个 HTTP 客户端（带统一的请求头）。
*
* @param url  要请求的地址
* @param ctx  事件回调的上下文（ICY 元数据信息往这里填）
* @return 客户端句柄；失败 NULL
*/
static esp_http_client_handle_t http_client_new(const char *url, http_ctx_t *ctx)
{
    esp_http_client_config_t cfg = {
        .url             = url,
        .event_handler   = http_event_cb,      // 响应头回调
        .user_data       = ctx,                // 回调的上下文
        .timeout_ms      = 10000,              // 10 秒超时
        .buffer_size     = 1024,               // 接收缓冲
        .keep_alive_enable = true,             // 保持连接
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
/* IDF 6.x 去掉了 esp_http_client 的 skip_verify：https 流必须校验服务器证书。* 根证书由 mbedTLS 证书包提供（sdkconfig 里 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 已打开）。 */
        .crt_bundle_attach = esp_crt_bundle_attach,   // 挂上根证书
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;                            // 初始化失败
    }
    esp_http_client_set_header(client, "Icy-MetaData", "1");   // 索取 ICY 元数据
    esp_http_client_set_header(client, "User-Agent", "esp32-radio/1.0");
    return client;
}

/* ================= 播放列表（.m3u） ================= */

/* 播放列表最多几条（内存有限：每条 192+48 字节） */
#define PL_MAX_ENTRIES 8

static char s_pl_urls[PL_MAX_ENTRIES][192];    // 解析出来的地址
static char s_pl_titles[PL_MAX_ENTRIES][48];   // 解析出来的歌名
static int  s_pl_count;                        // 条目数
static char s_pl_text[3072];   /* static: the radio task stack is small */

/**
* @brief 播放列表的条目数（"pl" 命令用）。
*/
int radio_playlist_count(void)
{
    return s_pl_count;
}

/**
* @brief 播放列表第 idx 条的标题。
*/
const char *radio_playlist_title(int idx)
{
    if (idx < 0 || idx >= s_pl_count) {
        return NULL;                            // 越界
    }
    return s_pl_titles[idx];
}

/**
* @brief 播放列表第 idx 条的地址。
*/
const char *radio_playlist_url(int idx)
{
    if (idx < 0 || idx >= s_pl_count) {
        return NULL;                            // 越界
    }
    return s_pl_urls[idx];
}

/**
* @brief 这个地址是不是一个播放列表（.m3u / .m3u8）。
*/
static bool url_is_playlist(const char *url)
{
    size_t n = strlen(url);
    if (n >= 5 && strcasecmp(url + n - 5, ".m3u8") == 0) {   // .m3u8
        return true;
    }
    if (n >= 4 && strcasecmp(url + n - 4, ".m3u") == 0) {    // .m3u
        return true;
    }
    return false;
}

/**
* @brief 判断是不是一个十六进制数字（判断 %xx 用）。
*/
static bool pl_is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* 把 URL 路径里不能直接出现的字符做百分号编码（空格、非 ASCII、控制符…）。* 已经是 %xx 的序列原样保留，所以不会二次编码。 */

/**
* @brief 把文件名做 URL 百分号编码。
*
* 空格、中文在 HTTP 请求行里是非法字符，必须编码成 %20 / %E6%99%B4。
* 已经编码过的 %xx 原样保留（不会二次编码）。
*/
static void pl_encode(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;                              // 输出位置
    for (size_t i = 0; src[i] && o + 4 < dstlen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%' && pl_is_hex(src[i + 1]) && pl_is_hex(src[i + 2])) {
            dst[o++] = src[i++];               // 已经是 %xx：原样复制
            dst[o++] = src[i++];
            dst[o++] = src[i];
            continue;
        }
        if (c <= 0x20 || c == 0x7f || c >= 0x80 || c == '"' || c == '<' ||
            c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' ||
            c == '}' || c == '|' || c == '[' || c == ']') {
            o += snprintf(dst + o, dstlen - o, "%%%02X", c);   // 编码成一个 %xx
        } else {
            dst[o++] = (char)c;                // 合法字符：直接复制
        }
    }
    dst[o] = '\0';
}

/* 把歌单里的一条目解析成完整地址：绝对地址原样使用，相对地址按歌单所在目录拼接（本地音乐文件夹就靠这个）。 */

/**
* @brief 把播放列表里的一条目解析成完整的 URL。
*
* 绝对地址原样使用；相对地址按播放列表所在目录拼接
* （所以换一个 IP 也能用，不用重新生成歌单）。
*/
static void pl_resolve(char *dst, size_t dstlen, const char *base, const char *entry)
{
    char enc[256];                             // 编码后的条目
    pl_encode(enc, sizeof(enc), entry);

    if (strncasecmp(enc, "http://", 7) == 0 || strncasecmp(enc, "https://", 8) == 0) {
        strlcpy(dst, enc, dstlen);             // 本身就是完整地址
        return;
    }
    if (enc[0] == '/') {
/* 同一主机上的绝对路径 */
        char host[192];
        strlcpy(host, base, sizeof(host));
        char *scheme = strstr(host, "://");     // 找 "://"
        if (scheme) {
            char *slash = strchr(scheme + 3, '/');   // 主机名后面的第一个 '/'
            if (slash) {
                *slash = '\0';                 // 只留主机名
                snprintf(dst, dstlen, "%s%s", host, enc);
                return;
            }
        }
    }
/* 相对路径：按歌单所在目录拼接 */
    char root[192];
    strlcpy(root, base, sizeof(root));
    char *slash = strrchr(root, '/');          // 最后一个 '/'（目录分隔符）
    if (slash) {
        slash[1] = '\0';                       // 只保留到目录为止
    }
    snprintf(dst, dstlen, "%s%s", root, enc);
}

/* 抓取文本歌单并解析进 s_pl_urls / s_pl_titles。返回条目数（0 = 不是歌单或失败）。 */

/**
* @brief 抓取并解析一个文本播放列表（.m3u）。
*
* @param url      歌单地址
* @param http_buf 复用的 HTTP 接收缓冲
* @return 解析出的条目数；0 = 不是歌单或失败
*/
static int playlist_fetch(const char *url, uint8_t *http_buf)
{
    http_ctx_t ctx = { .metaint = -1 };         // 歌单不是音频流：没有 ICY 元数据
    esp_http_client_handle_t client = http_client_new(url, &ctx);
    if (!client) {
        return 0;                               // 客户端建不起来
    }

    esp_err_t err = esp_http_client_open(client, 0);       // 建立连接
    if (err == ESP_OK) {
        err = esp_http_client_fetch_headers(client);       // 读响应头
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "playlist fetch failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return 0;
    }
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {                        // 只接受 200
        ESP_LOGE(TAG, "playlist http %d for %s", status, url);
        esp_http_client_cleanup(client);
        return 0;
    }

    size_t used = 0;                            // 已读字节数
    bool truncated = false;                     // 有没有被缓冲区截断
    for (;;) {
        int r = esp_http_client_read(client, (char *)http_buf, HTTP_RD_SIZE);
        if (r <= 0) {
            break;                              // 读完了
        }
        if (used + (size_t)r >= sizeof(s_pl_text)) {   // 缓冲区快满了
            r = (int)(sizeof(s_pl_text) - used - 1);   // 只收得下这么多
            truncated = true;
            if (r <= 0) {
                break;
            }
        }
        memcpy(s_pl_text + used, http_buf, r);  // 追加到文本缓冲
        used += r;
    }
    esp_http_client_cleanup(client);
    s_pl_text[used] = '\0';                     // 补结尾的 0

/* 解析："#EXTINF:<秒数>,<标题>" 描述紧随其后的那个地址 */
    s_pl_count = 0;                             // 从头解析
    char pending[48] = "";                      // 待用的歌名（#EXTINF 的）
    char *line = s_pl_text;
    while (line && *line && s_pl_count < PL_MAX_ENTRIES) {
        char *nl = strchr(line, '\n');          // 找行尾
        bool no_newline = (nl == NULL);        // 最后一行可能没有换行符
        if (nl) {
            *nl = '\0';                         // 截断成一行
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') {       // 去掉行首空白
            p++;
        }
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[--len] = '\0';                    // 去掉行尾空白和 CR
        }
        if (no_newline && truncated) {
/* 最后一行被缓冲区截断了（半行）-> 丢弃 */
/* 最后一行被缓冲区截断了（半行）-> 丢弃，否则会拿半截 URL 去连 */
        } else if (p[0] == '#') {               // 注释行
            if (strncasecmp(p, "#EXTINF:", 8) == 0) {       // "#EXTINF:-1,歌名"
                char *comma = strchr(p + 8, ',');            // 逗号后面是歌名
                if (comma) {
                    strlcpy(pending, comma + 1, sizeof(pending));
                }
            }
        } else if (p[0] != '\0') {              // 普通行 = 一个地址
            pl_resolve(s_pl_urls[s_pl_count], sizeof(s_pl_urls[0]), url, p);
            strlcpy(s_pl_titles[s_pl_count], pending, sizeof(s_pl_titles[0]));
            pending[0] = '\0';                  // 歌名用掉了，清空
            s_pl_count++;
        }
        line = nl ? nl + 1 : NULL;              // 下一行
    }
    ESP_LOGI(TAG, "playlist: %d entries from %s%s", s_pl_count, url,
             truncated ? " (truncated)" : "");
    return s_pl_count;
}

/* ================= 断点续传状态 ================= */

/* 当前文件的字节偏移 + 文件总长（-1 = 未知，也就是直播流）。* 只有长度已知的文件才发 Range 续传请求 —— 直播流必须从头读。 */
static long s_resume_offset = 0;      // 当前文件已经收了多少字节
static long s_resume_total  = -1;     // 文件总长（-1 = 未知 = 直播流）
static int  s_resume_station = -1;    // 断点属于哪个电台（换台就作废）

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

/* 播一条流。 * 返回 ESP_OK = 被用户打断（停止/换台），ESP_FAIL = 硬错误， * ESP_ERR_NOT_FINISHED = 服务器把流关了（一首播完/直播断了）。 */
static esp_err_t play_url(const char *url, const char *name, uint8_t *http_buf)
{
    http_ctx_t ctx = { .metaint = -1 };

/* 文件已经下完了 -> 从头再播一遍。 */
    if (s_resume_total > 0 && s_resume_offset >= s_resume_total) {
        ESP_LOGI(TAG, "file complete (%ld bytes), restarting from the top", s_resume_total);
        s_resume_offset = 0;
    }

    esp_http_client_handle_t client = http_client_new(url, &ctx);
    if (!client) {
        return ESP_FAIL;
    }

/* 续传下了一半的文件，而不是从头再来。 * 只有服务器给了总长度才能这么做（是文件而不是直播流 —— * 直播流没有长度，必须从头读）。 */
    if (s_resume_offset > 0 && s_resume_total > 0) {   // 要续传？
        char range[48];
        snprintf(range, sizeof(range), "bytes=%ld-", s_resume_offset);  // Range 头
        esp_http_client_set_header(client, "Range", range);
        ESP_LOGI(TAG, "resuming at byte %ld/%ld", s_resume_offset, s_resume_total);
    }

    esp_err_t err = esp_http_client_open(client, 0);       // 建立连接
    if (err == ESP_OK) {
        err = esp_http_client_fetch_headers(client);       // 读响应头
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect to %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int status = esp_http_client_get_status_code(client);  // HTTP 状态码
    ESP_LOGI(TAG, "stream: %s (http %d, metaint=%d, type=%s)",
             name, status, ctx.metaint, ctx.content_type);
    if (ctx.icy_br[0] || ctx.icy_name[0] || ctx.icy_genre[0]) {
/* 服务端自己说的码率/台名/类型 —— 用来判断"同一地址内容不一样" */
        ESP_LOGI(TAG, "  server says: br=%s name=%s genre=%s",
                 ctx.icy_br[0] ? ctx.icy_br : "-",
                 ctx.icy_name[0] ? ctx.icy_name : "-",
                 ctx.icy_genre[0] ? ctx.icy_genre : "-");
    }
    if (ctx.location[0]) {
        ESP_LOGI(TAG, "  redirect: %s", ctx.location);      // 有重定向
    }
    {
        char final_url[224] = "";                          // 最终落地的地址
        if (esp_http_client_get_url(client, final_url, sizeof(final_url)) == ESP_OK &&
            final_url[0] && strcmp(final_url, url) != 0) {
            ESP_LOGI(TAG, "  final url: %s", final_url);   // 和请求的不一样 = CDN 调度
        }
    }
    if (status == 404 || status == 403 || status == 410) {
/* 地址本身废了（蜻蜓会轮换频道 id）：一直重试同一个地址没有意义。 */
        ESP_LOGE(TAG, "http %d: station url is dead, set another with 'url <n> <url>'", status);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_SUPPORTED;                     // 地址本身废了，别再重试
    }
    if (status == 206) {
/* 206 Partial Content：服务器接受了我们的 Range 请求。 */
        long remaining = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "  resumed: %ld bytes left", remaining);
    } else if (status == 200) {
/* 服务器忽略了 Range 头（或者我们本来就没发）：正文从头开始，所以偏移必须清零。 */
        if (s_resume_offset > 0) {
            ESP_LOGW(TAG, "  server ignored the range request, restarting");
            s_resume_offset = 0;                            // 从头开始
        }
        long len = esp_http_client_get_content_length(client);
        s_resume_total = (len > 0) ? (long)len : -1;        // 有长度 = 文件，没有 = 直播
    } else {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    notify(RADIO_EVENT_STREAMING, name);                    // 通知上层"开始推流"
    led_set(true);                                          // 点亮状态灯

    mp3dec_init(&s_mp3d);                                  // 解码器复位
    s_fill   = 0;                                           // 流缓冲清空
    s_track[0] = '\0';                                      // 曲名清空
    s_rate_locked = false; /* new stream: first frame re-locks the i2s clock */
    s_stream_info_logged = false;
/* 新的流：增益从头开始，否则新电台的起始响度会由上一个电台决定。 */
    s_audio_gain = 256;                                    // 增益回到 1.0

    const int metaint    = ctx.metaint;   /* <= 0: no metadata */
    int       until_meta = metaint;       /* audio bytes until next meta block */
    int       meta_len   = -1;            /* -1: read length byte next */
    int       meta_pos   = 0;             /* 已收的元数据字节数 */
    char      meta_buf[512];              /* 元数据缓冲 */

    for (;;) {
/* 看看控制台有没有下新指令（换台/停止） */
        EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0);
        if (bits & (EVT_REQUEST | EVT_STOP)) {
            esp_http_client_cleanup(client);
            return ESP_OK;                                // 被用户打断
        }

/* 反压：流缓冲快满时绝不去读 socket。TCP 接收窗口会自己关掉，服务器会等，一个 MP3 字节都不会被丢掉。扔掉字节会破坏帧对齐 —— 解码器会报"mp3 decode error, skipping frame"，杂音非常明显。decode_loop() 由阻塞的 I2S 写操作控制节奏，腾缓冲的速度正好等于音频播放的速度。 */
        if (s_fill + HTTP_RD_SIZE > STREAM_BUF_SIZE) {   // 缓冲快满了
            decode_loop();                               // 先解码腾地方
            if (s_fill + HTTP_RD_SIZE > STREAM_BUF_SIZE) {
                vTaskDelay(pdMS_TO_TICKS(10));            // 还是满，等一下
                continue;
            }
        }

        int r = esp_http_client_read(client, (char *)http_buf, HTTP_RD_SIZE);  // 读数据
        if (r < 0) {
            ESP_LOGE(TAG, "read error: %s", esp_err_to_name(r));
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            ESP_LOGW(TAG, "stream closed by server");     // 服务器断流了
            esp_http_client_cleanup(client);
            return ESP_ERR_NOT_FINISHED;                  // 一首播完/直播断了
        }
        if (s_resume_total > 0) {
/* 只有文件能续传，直播流没有长度 */
            s_resume_offset += r;                        // 累加已收字节
        }

        int pos = 0;                                     // 这一批数据的位置
        while (pos < r) {
            if (metaint > 0 && until_meta == 0) {        // 到元数据块了？
                if (meta_len < 0) {                      // 还没读长度字节
                    meta_len = (uint8_t)http_buf[pos++] * 16;   // 长度 = 1 字节 × 16
                    meta_pos = 0;
                    if (meta_len == 0) {                 // 长度 0 = 空的元数据
                        until_meta = metaint;
                        meta_len   = -1;
                        continue;
                    }
                }
                int take = r - pos;                      // 这次能拿多少
                if (take > meta_len - meta_pos) {
                    take = meta_len - meta_pos;          // 不能超过元数据长度
                }
                if (meta_len < (int)sizeof(meta_buf)) {
                    memcpy(meta_buf + meta_pos, http_buf + pos, take);
                }
                meta_pos += take;                        // 累计
                pos      += take;
                if (meta_pos >= meta_len) {              // 一整块元数据收齐了
                    if (meta_len < (int)sizeof(meta_buf)) {
                        meta_buf[meta_len] = '\0';
                        char title[128];                 // 曲名
                        parse_track(meta_buf, title, sizeof(title));
                        if (title[0] && strcmp(title, s_track) != 0) {  // 变了才报
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

/* 这里是纯音频数据 */
            int take = r - pos;                      // 这次能收多少音频
            if (metaint > 0 && take > until_meta) {
                take = until_meta;                  // 不能越过元数据块
            }
            size_t space = STREAM_BUF_SIZE - s_fill; // 流缓冲剩余空间
            if ((size_t)take > space) {
/* 解码跟不上：丢掉最老的字节 */
                size_t drop = (size_t)take - space;  // 要丢多少
                if (drop > s_fill) {
                    drop = s_fill;
                }
                memmove(s_stream_buf, s_stream_buf + drop, s_fill - drop);  // 丢最老的
                s_fill -= drop;
                if (log_throttled(&s_last_overflow_us, 2000)) {
                    ESP_LOGW(TAG, "stream buffer full, dropped %u bytes", (unsigned)drop);
                }
                space = STREAM_BUF_SIZE - s_fill;
                if ((size_t)take > space) {
                    take = (int)space;               // 剩下的只能收这么多
                }
            }
            if (take > 0) {
                memcpy(s_stream_buf + s_fill, http_buf + pos, take);  // 存进流缓冲
                s_fill += take;                      // 已用长度增加
                pos     += take;                     // 这一批处理完
            }
            if (metaint > 0) {
                until_meta -= take;                 // 距离下一个元数据块还剩多少音频
            }

            decode_loop();                          // 解码并输出
        }
    }
}

/* 播放一个电台：普通流就播一条；地址是 .m3u 歌单就按顺序逐首播。 */

/**
* @brief 播放第 idx 个电台。
*
* 如果地址是 .m3u，就按歌单逐首播；否则当成普通流播。
*
* @param idx      电台下标
* @param http_buf HTTP 接收缓冲
* @return ESP_OK = 被用户打断；ESP_ERR_NOT_FINISHED = 一条流/整个歌单播完了
*/
static esp_err_t play_station(int idx, uint8_t *http_buf)
{
/* 手机连着的时候，蓝牙播放器占着功放。 */
    if (radio_i2s_locked()) {
        ESP_LOGW(TAG, "bluetooth owns the amplifier, radio paused");
        return ESP_OK;   /* treated as "interrupted", no retry storm */
    }

/* 播放期间音频缓冲从堆上取（蓝牙栈需要同一块内存）。 */
    if (!radio_bufs_alloc()) {
        return ESP_FAIL;                            // 内存不足
    }

/* 在互斥锁下取名字/地址的快照：控制台任务可能正在改它们（url/name 命令），* 读到写了一半的字符串会让 HTTP 客户端的 URL 解析器崩溃。 */
    char url[160];                                  // 地址快照
    char name[48];                                  // 名字快照
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_FAIL;                            // 拿不到锁（控制台正忙）
    }
    strlcpy(url, s_station_urls[idx], sizeof(url));
    strlcpy(name, s_station_names[idx], sizeof(name));
    xSemaphoreGive(s_station_mutex);

/* 换台 = 换文件，断点作废。 */
    if (idx != s_resume_station) {                 // 换台了
        s_resume_offset = 0;                        // 断点作废
        s_resume_total  = -1;
        s_resume_station = idx;
    }

    if (url_is_playlist(url)) {                    // 是歌单？
        int n = playlist_fetch(url, http_buf);     // 抓下来解析
        if (n > 0) {
            for (int i = 0; i < n; i++) {          // 逐首播
                const char *title = s_pl_titles[i][0] ? s_pl_titles[i] : name;
                ESP_LOGI(TAG, "playlist %d/%d: %s", i + 1, n, title);
                esp_err_t e = play_url(s_pl_urls[i], title, http_buf);
                if (e != ESP_ERR_NOT_FINISHED) {
/* 被用户打断，或者出了硬错误 */
                    return e;                       // 被打断或出错了
                }
/* 这首正常播完了 -> 直接下一首 */
                if (xEventGroupWaitBits(s_evt, EVT_STOP, pdFALSE, pdFALSE, 0) & EVT_STOP) {
                    return ESP_OK;                 // 用户叫停
                }
            }
/* 整个歌单播完 -> 电台任务会重连，从头再来一遍 */
            return ESP_ERR_NOT_FINISHED;           // 整个歌单播完 -> 从头再来
        }
        ESP_LOGW(TAG, "playlist empty or unreachable, treating as a plain stream");
    }

    return play_url(url, name, http_buf);           // 普通流
}

/* ================= 电台任务 ================= */

/* 诊断用：方波直接走音频通路，不联网、不解码，用来区分"喇叭不响"这类硬件问题。duration_ms == 0 表示一直响到 stop —— 万用表测量需要，2 秒太短来不及读数。 */

/**
* @brief 输出方波测试音（不联网、不解码，直接走音频通路）。
*
* 用来区分"硬件没出声"和"软件没解码"这类问题。
* duration_ms == 0 表示一直响到 stop（万用表测量需要，2 秒太短）。
*/
static void play_tone_blocking(int freq, int duration_ms)
{
#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    const int rate = AUDIO_SAMPLE_RATE;            // 直驱模式的采样率
    const int half_period = rate / (2 * freq);      // 半周期（采样数）
    const bool continuous = (duration_ms == 0);    // 一直响？
    int total = continuous ? 0 : rate * duration_ms / 1000;   // 总采样数
    int s = 0;                                     // 已输出多少采样

    while (continuous || s < total) {
/* 接近满幅：PWM 的 GPIO 最多只能出 ~40mA，驱动低阻耳机时幅度要收敛一点 */
        audio_push((int16_t)(((s / half_period) % 2) ? 20000 : -20000));  // 方波
        s++;
        if ((s % 256) == 0) {                      // 每 256 个采样查一次停止指令
            if (xEventGroupWaitBits(s_evt, EVT_STOP, pdFALSE, pdFALSE, 0) & EVT_STOP) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(15));         // 让出一点 CPU
        }
    }
/* 让队列放完，测试音才完整 */
    int queued = (s_audio_head - s_audio_tail) & AUDIO_BUF_MASK;  // 环里还有多少
    vTaskDelay(pdMS_TO_TICKS(queued * 1000 / AUDIO_SAMPLE_RATE + 30));  // 等它放完
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
/* 接近满幅：功放还会再叠加 9~15dB 增益 */
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
/* 证明 I2S 的 DMA 真的把采样搬走了：如果这里是 0，问题在 ESP32 这边，不是功放。 */
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

/* 链接时解析到同一组件里的 wifi_station.c。如果那里没有这个符号，就认为网络没连上。 */
extern bool wifi_is_connected(void) __attribute__((weak));

static bool net_is_up(void)
{
    return wifi_is_connected ? wifi_is_connected() : false;
}

/**
* @brief 电台任务：等控制台的指令，然后收流、解码、输出。
*
* 这是本模块的核心循环：
*   等 EVT_REQUEST/EVT_STOP/EVT_TONE -> 等网络 -> 播 -> 断了重连
*/
static void radio_task(void *arg)
{
    uint8_t *http_buf = malloc(HTTP_RD_SIZE);    // HTTP 接收缓冲（堆上）
    if (!http_buf) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);                       // 内存都不够，直接退出
        return;
    }

    for (;;) {
/* 阻塞等控制台的指令（换台/停止/测试音） */
        EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP | EVT_TONE, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & EVT_TONE) {                   // 要放测试音
            s_playing = true;
            led_set(true);
            ESP_LOGI(TAG, "test tone: %d Hz, %d ms", s_tone_freq, s_tone_ms);
            notify(RADIO_EVENT_CONNECTING, "test tone");
            play_tone_blocking(s_tone_freq, s_tone_ms);   // 阻塞输出
            s_playing = false;
            led_set(false);
            notify(RADIO_EVENT_STOPPED, "");
            continue;
        }
        if (!(bits & EVT_REQUEST)) {             // 只是停止
            led_set(false);
            continue;
        }

        int idx = s_target;                      // 要播的电台
        s_cur     = idx;                         // 记下当前电台
        s_playing = true;
        s_404_count = 0;                         // 404 计数清零
        ESP_LOGI(TAG, "station [%d/%d]: %s", idx + 1, s_station_count, s_station_names[idx]);
        notify(RADIO_EVENT_CONNECTING, s_station_names[idx]);
/* 【用户要求】电台模式彻底关闭蓝牙（和"蓝牙播放时关 WiFi"对称）。
 * 这里用【释放】而不是只关控制器：释放会把蓝牙协议栈占用的约 40KB
 * 堆内存全部归还，电台随后分配自己那 18KB 缓冲就不会和蓝牙抢堆 ——
 * 抢堆的后果是蓝牙栈申请不到入站缓冲而崩溃重启（之前已经发生过）。
 * 顺序很关键：先释放蓝牙 → 再由 play_station() 分配电台缓冲。
 * 电台会话结束时（下面）先释放电台缓冲，再重新开启蓝牙。 */
        bt_player_release();

        while (1) {                              /* 播这个台，直到换台/停止 */
            EventBits_t b = xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0);
            if (b & (EVT_REQUEST | EVT_STOP)) {
                break; /* switch or stop */
            }

/* 没 IP 就等着，别去空敲 DNS。 */
            if (!net_is_up()) {                  // 还没联网
                notify(RADIO_EVENT_ERROR, "waiting for wifi ...");
                int ticks = 0;
                while (!net_is_up()) {           // 一直等
                    if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                        break;
                    }
                    wifi_resume();   /* 用户在蓝牙期间主动要电台：把挂起的 WiFi 请回来（幂等） */
                    led_set((ticks % 10) < 5); /* slow blink */
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ticks++;
                    if (ticks % 300 == 0) {     // 每 30 秒报一次
                        ESP_LOGW(TAG, "still waiting for wifi ...");
                    }
                }
                if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                    break;
                }
                led_set(true);
                notify(RADIO_EVENT_CONNECTING, s_station_names[idx]);
                continue;                        // 网络好了，重新连
            }

            esp_err_t err = play_station(idx, http_buf);   // 播
            /* 流一停就停用音频缓冲（缓冲本体是静态的，不占堆 ✓）。
             * 这样蓝牙连上来时堆是干净的，协议栈申请内存不会失败。 */
            if (s_bufs != NULL) {
                radio_bufs_free();
                ESP_LOGI(TAG, "radio: stream ended, audio buffers idle "
                         "(free heap: %d)", (int)esp_get_free_heap_size());
            }
            if (err == ESP_OK) {
                break;                           // 被用户打断
            }

            if (err == ESP_ERR_NOT_SUPPORTED) {  // 404/403/410：地址废了
/* 404 不一定代表地址废了：蜻蜓的 CDN 会限制快速重连（这个地址第一次是能连上的）。所以退避久一点，连着几次 404 才放弃。 */
                s_404_count++;
                if (s_404_count >= 5) {          // 连着 5 次才放弃
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
                continue;                        // 再试一次
            }

            if (err == ESP_ERR_NOT_FINISHED) {
                s_404_count = 0; /* url works, the server just closed the stream */
            }

            if (!net_is_up()) {                  // 断网了
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
/* 【关键】蓝牙连着时先别重试电台：WiFi+BT 共用同一条 2.4GHz 射频，
 * 每 3 秒一次的 HTTP/TLS 重试会把蓝牙数据包的到达节奏打成 ±2% 的
 * 秒级波动，蓝牙的环形缓冲兜不住 → 播放全程持续喀哒声。
 * 等手机断开蓝牙再恢复重试；用户显式切台（EVT_REQUEST）随时可打断。 */
            while (bt_player_state() >= BT_STATE_CONNECTED) {
                led_set(false);
                if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, pdMS_TO_TICKS(500)) & (EVT_REQUEST | EVT_STOP)) {
                    break;
                }
            }
            if (xEventGroupWaitBits(s_evt, EVT_REQUEST | EVT_STOP, pdFALSE, pdFALSE, 0) & (EVT_REQUEST | EVT_STOP)) {
                break;
            }
            notify(RADIO_EVENT_CONNECTING, s_station_names[idx]);
        }

        s_playing = false;
        led_set(false);
        notify(RADIO_EVENT_STOPPED, "");
/* 把音频缓冲还给堆 —— 蓝牙栈现在可能需要这块内存。
 * 顺序：先释放电台缓冲，再重新开启蓝牙，让蓝牙拿到干净的堆。 */
        radio_bufs_free();

/* 会话结束：真停了（EVT_STOP）才把蓝牙请回来，回到蓝牙待命模式。
 * 如果只是"切台"（EVT_REQUEST 还挂着就不开）—— 下一个会话一上来
 * 又要释放它，反复冷启动/卸载 Bluedroid 纯属折腾，且有稳定性风险。 */
        if (!(xEventGroupWaitBits(s_evt, EVT_REQUEST, pdFALSE, pdFALSE, 0) & EVT_REQUEST)) {
            bt_player_open();
        }
    }
}

/* ================= 电台任务 ================= */

/**
* @brief 初始化电台模块：装入电台表、初始化 I2S、创建电台任务。
*
* @param stations 编译进固件的默认电台表
* @param count    默认电台个数
* @param cb       事件回调
* @param ctx      回调上下文
*/
void radio_init(const radio_station_t *stations, int count, radio_event_cb_t cb, void *ctx)
{
    s_station_count = count > MAX_STATIONS ? MAX_STATIONS : count;  // 别超过表的大小
    for (int i = 0; i < s_station_count; i++) {
        strlcpy(s_station_names[i], stations[i].name, sizeof(s_station_names[i]));
        strlcpy(s_station_urls[i], stations[i].url, sizeof(s_station_urls[i]));
        station_nvs_load(i); /* stored config overrides the defaults */
        s_stations[i].name = s_station_names[i];
        s_stations[i].url  = s_station_urls[i];
    }

/* NVS 里存过个数 = 用户加过/删过电台，它优先于编译进固件的默认表。 */
    int stored = station_nvs_load_count();
    if (stored >= 0 && stored <= MAX_STATIONS) {   // NVS 里存过个数
        for (int i = s_station_count; i < stored; i++) {  // 补上多出来的那些
            s_station_names[i][0] = '\0';
            s_station_urls[i][0] = '\0';
            station_nvs_load(i);
            s_stations[i].name = s_station_names[i];
            s_stations[i].url  = s_station_urls[i];
        }
        s_station_count = stored;
    }
    s_cb     = cb;                                // 事件回调
    s_cb_ctx = ctx;                              // 回调上下文
    s_volume = CONFIG_RADIO_INITIAL_VOLUME;   // 出厂/默认音量
#if VOLUME_PERSIST
/* 打开持久化后：NVS 里存过就恢复上次调的音量。 */
    {
        int stored_vol = volume_nvs_load();           // 运行时设过？
        if (stored_vol >= 0) {
            s_volume = stored_vol;                   // 用存的那个
        }
    }
#endif
    ESP_LOGI(TAG, "initial volume: %d%% (%s)", s_volume,
             VOLUME_PERSIST ? "persistent" : "default, not saved");
    s_evt    = xEventGroupCreate();               // 控制台 -> 电台任务的通道
    s_station_mutex = xSemaphoreCreateMutex();    // 保护电台表
    s_rate_locked = false;                        // 时钟还没锁定

    gpio_config_t led_io = {                     // 状态灯
        .pin_bit_mask = 1ULL << PIN_STATUS_LED,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_io);
    led_set(false);                              // 初始熄灭

#if (PIN_AMP_SD >= 0)
/* SD 控制脚默认是高阻输入，所以功放保持模块自己的默认模式（模块内部上拉优先），直到用户用 "sd" 命令去覆盖。 */
    gpio_config_t sd_io = {                      // 功放 SD 脚：默认高阻输入
        .pin_bit_mask = 1ULL << PIN_AMP_SD,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&sd_io);
    ESP_LOGI(TAG, "amp sd control on gpio %d (idle: float)", PIN_AMP_SD);
#endif

#if defined(CONFIG_RADIO_AUDIO_DAC) || defined(CONFIG_RADIO_AUDIO_PWM)
    audio_out_init();                            // 直驱模式
#else
/* 运行时用 "pins" 命令选过的引脚优先于编译进固件的默认值，这样升级固件也不会把能出声的功放改哑。存的是引脚本身，去表里比对，所以以后重排映射表也不会让选择含义漂移。 */
    int bck = 0, ws = 0, data = 0;
    if (pinmap_nvs_load(&bck, &ws, &data)) {    // NVS 里存过引脚？
        bool found = false;
        for (int i = 0; i < (int)PINMAP_COUNT; i++) {
            if (s_pinmaps[i].bck == bck && s_pinmaps[i].ws == ws && s_pinmaps[i].data == data) {
                s_pinmap = i;                     // 找到对应的那组引脚
                found = true;
                break;
            }
        }
        if (found) {
            ESP_LOGI(TAG, "i2s pin map from nvs: [%s] bck=%d ws=%d dout=%d",
                     s_pinmaps[s_pinmap].desc, bck, ws, data);
        } else {
            ESP_LOGW(TAG, "stored i2s pins (%d/%d/%d) not in the table, using default",
                     bck, ws, data);               // 表里没有：用默认的
        }
    }
    i2s_init();                                   // 建 I2S 通道
#endif

/* mp3dec_decode_frame() 会在栈上留 ~16KB 的暂存空间（mp3dec_scratch_t），再加上 HTTP 客户端的调用链。32KB 栈是安全的。 */
    xTaskCreate(radio_task, "radio", 32768, NULL, 5, NULL);   // 起电台任务
}

/**
* @brief 播放第 index 个电台。
*/
void radio_play(int index)
{
    if (s_station_count <= 0) {
        return;                                   // 一个电台都没有
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= s_station_count) {
        index = s_station_count - 1;              // 越界就取最后一个
    }
    s_target = index;                             // 目标电台
    xEventGroupClearBits(s_evt, EVT_STOP);        // 清掉停止标志
    xEventGroupSetBits(s_evt, EVT_REQUEST);       // 通知电台任务：开始播
}

/**
* @brief 下一个电台（末尾绕回第一个）。
*/
void radio_next(void)
{
    if (s_station_count <= 0) {
        return;
    }
    radio_play((s_target + 1) % s_station_count);
}

/**
* @brief 上一个电台（开头绕回最后一个）。
*/
void radio_prev(void)
{
    if (s_station_count <= 0) {
        return;
    }
    radio_play((s_target + s_station_count - 1) % s_station_count);
}

/**
* @brief 停止播放（不换台）。
*/
void radio_stop(void)
{
    xEventGroupClearBits(s_evt, EVT_REQUEST);     // 清掉播放请求
    xEventGroupSetBits(s_evt, EVT_STOP);          // 通知电台任务：停下
}

/**
* @brief 让电台立刻停下，并同步等它把堆上的音频缓冲释放掉（蓝牙接管时用）。
*
* 蓝牙栈在连接后会马上申请几 KB 内存（媒体线程 + SBC 解码器）。
* 如果这时堆太紧张，栈的申请就会失败（串口里是
* "BT_OSI: malloc failed (size=4112)"），结果是一个音频采样都收不到，
* 甚至协议栈自己崩在 l2cu_allocate_ccb() 里 → 整机重启。
*
* 电台的缓冲现在是静态内存（不占堆 ✓），所以这里要等的不是"内存还回来"，
* 而是"电台真的不再往 I2S 里写了" —— 两个写者的数据在 DMA 里交错也是破音。
* 电台任务是异步的：它要先退出收流循环（最多一个读取周期 ≈ 256ms）。
*/
bool radio_stop_and_release(uint32_t timeout_ms)
{
    radio_stop();                                  // 先让电台停下
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int64_t last_report = 0;
    while (radio_is_playing()) {                   // 电台还在播？
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            ESP_LOGW(TAG, "the radio did not stop within %u ms", timeout_ms);
            return false;                          // 超时：交出去
        }
        /* 每 300ms 报一次，免得看不出电台卡在哪 */
        if (now - last_report >= 300000) {
            last_report = now;
            ESP_LOGI(TAG, "waiting for the radio to stop ...");
        }
        vTaskDelay(pdMS_TO_TICKS(10));              // 让电台任务有机会跑
    }
    /* 蓝牙栈马上就要申请几 KB，这里报一下还剩多少堆 */
    ESP_LOGI(TAG, "radio stopped, free heap: %d",
             (int)esp_get_free_heap_size());
    return true;
}


/**
* @brief 输出方波测试音（不联网、不解码）。
*/
void radio_play_tone(int freq_hz, int duration_ms)
{
    if (freq_hz < 50)        freq_hz = 50;        // 频率下限
    if (freq_hz > 8000)      freq_hz = 8000;      // 频率上限
    if (duration_ms < 0)     duration_ms = 0;   /* 0 = continuous until stop */
    if (duration_ms > 60000) duration_ms = 60000; // 最长 1 分钟
    s_tone_freq = freq_hz;                        // 频率
    s_tone_ms   = duration_ms;                    // 时长
/* 先停掉正在播的流，测试音才能立刻响。 */
    xEventGroupClearBits(s_evt, EVT_REQUEST);
    xEventGroupSetBits(s_evt, EVT_STOP | EVT_TONE);   // 停下 + 放测试音
}

/**
* @brief 设置音量（0~100），立即生效。
*
* 只有打开 CONFIG_RADIO_VOLUME_PERSIST 时才会写进 NVS；
* 默认只改内存里的值，重启回到默认音量。
*/
void radio_set_volume(int volume)
{
    if (volume < 0)   volume = 0;                 // 下限
    if (volume > 100) volume = 100;               // 上限
    s_volume = volume;                             // 立即生效
#if VOLUME_PERSIST
    volume_nvs_save(volume);                       // 存进 NVS（默认关闭）
#endif
}

/**
* @brief 当前音量（0~100）。
*/
int radio_get_volume(void)
{
    return s_volume;
}

/**
* @brief 当前电台下标（没有则 -1）。
*/
int radio_current_index(void)
{
    return s_cur;
}

/**
* @brief 改第 index 个电台的名字和/或地址（存 NVS）。
*/
void radio_set_station(int index, const char *name, const char *url)
{
    if (index < 0 || index >= s_station_count) {
        return;                                   // 越界
    }
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (url && url[0] && strlen(url) < sizeof(s_station_urls[0])) {
            strlcpy(s_station_urls[index], url, sizeof(s_station_urls[index]));  // 改地址
        }
        if (name && name[0] && strlen(name) < sizeof(s_station_names[0])) {
            strlcpy(s_station_names[index], name, sizeof(s_station_names[index]));  // 改名字
        }
        xSemaphoreGive(s_station_mutex);
    }
    station_nvs_save(index);                      // 存进 NVS
    ESP_LOGI(TAG, "station [%d] set: %s (%s)", index, s_station_names[index], s_station_urls[index]);
    if (index == s_cur || index == s_target) {    // 正在播这个台？
        radio_play(index); /* reconnect with the new url right away */
    }
}

/**
* @brief 取第 index 个电台的描述；下标越界返回 NULL。
*/
const radio_station_t *radio_get_station(int index)
{
    if (index < 0 || index >= s_station_count) {
        return NULL;
    }
    return &s_stations[index];
}

/**
* @brief 电台表里的电台个数（随 add/del 变化）。
*/
int radio_station_count(void)
{
    return s_station_count;
}

/**
* @brief 追加一个电台（存 NVS）。
*
* @param name 电台名，给 NULL 就从 URL 末段自动取
* @param url  流地址
* @return 新电台的下标；失败返回 -1
*/
int radio_add_station(const char *name, const char *url)
{
    if (!url || !url[0] || strlen(url) >= sizeof(s_station_urls[0])) {
        return -1;                                // 地址非法
    }
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -1;                                // 拿不到锁
    }

/* 找第一个空位 */
    int idx = -1;                                 // 找第一个空位
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (s_station_urls[i][0] == '\0') {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        xSemaphoreGive(s_station_mutex);
        ESP_LOGW(TAG, "station list full (%d)", MAX_STATIONS);
        return -1;                                // 表满了
    }

    strlcpy(s_station_urls[idx], url, sizeof(s_station_urls[idx]));   // 存地址
    if (name && name[0]) {
        strlcpy(s_station_names[idx], name, sizeof(s_station_names[idx]));  // 存名字
    } else {
/* 没给名字：取 URL 末段当名字 */
        const char *last = strrchr(url, '/');     // 最后一个 '/'
        const char *p = last ? last + 1 : url;    // 它后面那段
        char tmp[48];
        size_t n = strcspn(p, "?");               // 截掉查询参数
        if (n >= sizeof(tmp)) {
            n = sizeof(tmp) - 1;
        }
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        strlcpy(s_station_names[idx], tmp[0] ? tmp : "radio", sizeof(s_station_names[idx]));
    }
    s_stations[idx].name = s_station_names[idx];  // 对外的表指向真实字符串
    s_stations[idx].url  = s_station_urls[idx];
    if (idx + 1 > s_station_count) {
        s_station_count = idx + 1;                // 个数增加了
    }
    station_nvs_save(idx);                        // 存进 NVS
    station_nvs_save_count(s_station_count);
    xSemaphoreGive(s_station_mutex);

    ESP_LOGI(TAG, "station [%d] added: %s (%s)", idx, s_station_names[idx], s_station_urls[idx]);
    return idx;
}

/**
* @brief 删除一个电台，后面的自动前移（存 NVS）。
*/
bool radio_del_station(int index)
{
    if (index < 0 || index >= s_station_count) {
        return false;                             // 越界
    }
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

/* 后面的电台往前挪，保证表是紧凑的 */
    for (int i = index; i < s_station_count - 1; i++) {   // 后面的往前挪
        strlcpy(s_station_names[i], s_station_names[i + 1], sizeof(s_station_names[i]));
        strlcpy(s_station_urls[i], s_station_urls[i + 1], sizeof(s_station_urls[i]));
    }
    s_station_count--;                            // 个数减一
    s_station_names[s_station_count][0] = '\0';   // 末位清空
    s_station_urls[s_station_count][0]  = '\0';
    for (int i = 0; i < s_station_count; i++) {   // 全部重新存一遍
        s_stations[i].name = s_station_names[i];
        s_stations[i].url  = s_station_urls[i];
        station_nvs_save(i);
    }
    station_nvs_save_count(s_station_count);
    xSemaphoreGive(s_station_mutex);

    ESP_LOGI(TAG, "station [%d] deleted, %d left", index, s_station_count);
    return true;
}

/* ================= I2S 引脚映射表 ================= */

/**
* @brief 可用的引脚映射数量。
*/
int radio_i2s_pinmap_count(void)
{
    return (int)PINMAP_COUNT;
}

/**
* @brief 当前正在用的映射编号。
*/
int radio_i2s_pinmap_current(void)
{
    return s_pinmap;
}

/**
* @brief 读出第 idx 套映射的三个引脚。
*/
bool radio_i2s_pinmap_get(int idx, int *bck, int *ws, int *data)
{
    if (idx < 0 || idx >= (int)PINMAP_COUNT) {
        return false;                             // 越界
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

/**
* @brief 切换到第 idx 套引脚映射（会先停播，选择结果存进 NVS）。
*/
bool radio_set_i2s_pinmap(int idx)
{
    if (idx < 0 || idx >= (int)PINMAP_COUNT) {
        return false;                             // 越界
    }
    if (idx == s_pinmap) {
        ESP_LOGI(TAG, "i2s pins already [%s]", s_pinmaps[idx].desc);
        return true;                              // 已经是这一套了
    }
/* 重新配置 I2S 的时候电台任务不能往里写 —— 先停播，等它退出写操作。 */
    radio_stop();                                 // 先停播
    vTaskDelay(pdMS_TO_TICKS(300));               // 等它退出写操作

    if (!i2s_apply_pinmap(idx)) {
        return false;                             // 切换失败
    }
    pinmap_nvs_save(s_pinmaps[idx].bck, s_pinmaps[idx].ws, s_pinmaps[idx].data);
    ESP_LOGI(TAG, "i2s pins (bck=%d ws=%d dout=%d) stored in nvs",
             s_pinmaps[idx].bck, s_pinmaps[idx].ws, s_pinmaps[idx].data);
    return true;
}

/**
* @brief 当前曲名（ICY 元数据里的标题，没有则空串）。
*/
const char *radio_current_track(void)
{
    return s_track;
}

/**
* @brief 当前是否正在播放。
*/
bool radio_is_playing(void)
{
    return s_playing;
}