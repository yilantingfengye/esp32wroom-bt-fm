/*
* ============================================================================
*  bt_player.c —— 蓝牙 A2DP 接收（手机 → ESP32 → MAX98357A）
* ============================================================================
*
*  手机的音乐通过蓝牙推过来，从【同一个功放】放出来
*  （和互联网电台共用 I2S，两者互斥，见下面的音源仲裁）。
*
*  【音频通路】
*    手机 --BT/SBC--> Bluedroid 协议栈 --SBC 解码--> 16bit PCM
*         --环形缓冲--> 蓝牙音频任务 --I2S--> MAX98357A
*
*  【为什么不用自己写 SBC 解码器】
*  Bluedroid 协议栈内部自带 Open Interface 的 SBC 解码器
*  （CONFIG_BT_A2DP_USE_EXTERNAL_CODEC 保持关闭就会用它），
*  应用层拿到的直接就是 PCM。
*
*  【线程】
*  - 蓝牙协议栈任务：跑栈的回调（bt_audio_data_cb 在这里被调用）
*  - bt_audio 任务（本文件创建）：把 PCM 从环形缓冲搬进 I2S
*    优先级 6（比电台任务高），保证蓝牙不断流
*/

#include "bt_player.h"
#include "radio_player.h"
#include "wifi_station.h"   // wifi_suspend/wifi_resume（蓝牙播放期间关掉 WiFi 射频）

#include "esp_log.h"
#include "esp_system.h"     // esp_get_free_heap_size（看剩余堆）
#include "esp_timer.h"      // esp_timer_get_time（速率统计）

/* ---- 只有在编译期打开了蓝牙 + Bluedroid + A2DP 时才有真正的实现 ---- */
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_BLUEDROID_ENABLED) && \
    defined(CONFIG_BT_A2DP_ENABLE)
#define BT_SUPPORTED 1
#else
#define BT_SUPPORTED 0
#endif

#if BT_SUPPORTED

#include <string.h>
#include <math.h>       // sqrtf（响度计算）

#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"   // esp_a2d_sink_register_data_callback（内部解码器用）
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bt";

/* ================= 编解码能力 =================
*
* 我们不自己注册流端点（SEP）：内部 SBC 解码器模式下，协议栈在
* esp_a2d_sink_init() 时就注册好了默认的 SBC 端点（44.1kHz / 立体声 /
* 16bit，官方 a2dp_sink_stream 示例同样不注册）。
*
* 自己再注册一个会顶掉栈的默认端点 —— 栈就不知道要解码我们这条流，
* 结果是"连上了、码流起来了，但一个采样都收不到"。
*
* 下面是栈默认端点的能力位（A2DP 规范 4.3.2 的 4 字节 CIE），留作参考：
*
*   第 0 字节: bit 0-3 采样率 (16k/32k/44.1k/48k)
*             bit 4-7 声道模式 (单/双/立体声/联合立体声)
*   第 1 字节: bit 0-3 块长 (4/8/12/16)
*             bit 4-5 子带数 (4/8/12)
*             bit 6-7 分配方式 (信噪比/响度)
*   第 2 字节: 最小 bitpool
*   第 3 字节: 最大 bitpool
*/
#define BT_SAMPLE_RATE    44100  /* 蓝牙音频固定 44.1kHz */

/* ================= pcm 环形缓冲 =================
* 单生产者（蓝牙任务）/ 单消费者（音频任务）。
* 读写指针各自只由自己的主人写，所以不需要加锁。 */
/* 【环要多大】蓝牙栈每次回调交过来 2560 字节 = 1280 采样 = 640 帧
 * （立体声下 14.5 ms）。但它不是匀速喂的 —— WiFi/BT 调度一抖动，
 * 就会一次吐好几包（实测能把 8KB 的环瞬间填满）。
 *
 * 环满了就只能丢数据（听起来是"破音"），环空了 I2S 就断粮（也是破音）。
 * 实测：4KB 丢 9.3%、8KB 丢 2.2%、16KB 丢 0.5%。
 *
 * 【为什么不能开到 32KB】堆一共才 ~93KB，要同时装下：
 *   蓝牙协议栈（控制器+主机）~40KB + WiFi ~10KB + 电台 18KB + 这个环。
 * 环开到 32KB 的话，电台一放歌堆里就只剩 23KB，协议栈连接时申请
 * L2CAP 控制块失败 → l2cu_allocate_ccb() 空指针 → **整机崩溃重启**
 * （回溯：l2cu_allocate_ccb ← process_l2cap_cmd ← btu_hci_msg_process）。
 * 所以环保持 16KB，把余量留给协议栈。 */
#define BT_PCM_RING_SAMPLES 8192          /* 16 kB ≈ 93 ms。曾试 16384(32kB)：堆被吃光，
                                             手机一连上蓝牙栈就申请不到入站缓冲而崩溃重启
                                             （hci_hal_h4.c:646 assert）。16kB 实测可正常连接，
                                             突发吸收靠消费者(core 1, prio 21)把数据灌进 DMA 补足。 */

/* 环形缓冲放在堆上：只有手机连着时才需要，电台播放器可能想要这块内存。 */
static int16_t            *s_pcm_ring;    /* 环形缓冲本体（在堆上） */
static volatile uint32_t    s_pcm_head;   /* 写指针：蓝牙任务写 */
static volatile uint32_t    s_pcm_tail;   /* 读指针：音频任务写 */
static volatile uint32_t    s_pcm_overrun; /* 溢出（被迫丢数据）次数 */
static volatile uint32_t    s_ring_peak;  /* 环里水位最高到过多少（= 蓝牙栈的突发大小） */

/* 【掩码必须是运行时变量】内存不够时环会退到更小的尺寸，
 * 而掩码要跟着实际尺寸走 —— 用编译期常量的话，
 * 往 16KB 的环里按下标 16383 写，就直接踩坏堆 → LoadProhibited panic。 */
static volatile uint32_t    s_ring_mask;  /* 环大小 - 1（取位掩码） */
static volatile uint32_t    s_ring_size;  /* 环的采样数 */

/* ---- 诊断计数器（"bt" 命令里能看到） ---- */
static volatile uint32_t    s_data_cbs;     /* 协议栈交过来的 PCM 回调次数 */
static volatile uint32_t    s_bytes_in;     /* 收到的 PCM 字节数 */
static volatile uint32_t    s_samples_out;  /* 写进 I2S 的采样数 */

/* ---- 协商出来的音频参数（手机可能给 48kHz 或单声道） ---- */
static volatile int        s_bt_rate     = 44100;   /* 采样率 */
static volatile int        s_bt_channels = 2;       /* 声道数（1=单声道 2=立体声） */

/* ---- 统计用的时间戳（每 30 秒报一次速率） ---- */
static int64_t             s_last_stat_us;
static uint32_t            s_last_bytes_in;
static uint32_t            s_last_samples_out;
#if BT_VERBOSE
static bool                s_wave_dumped;   /* 波形只 dump 一次 */
#endif
static bool                s_drop_stale;    /* 第一次搬运前要丢掉积压的过期数据 */

/**
* @brief 分配 PCM 环形缓冲（手机连上时调用）。
*
* @return 成功 true，内存不足 false
*/
static bool bt_ring_alloc(void)
{
    if (s_pcm_ring != NULL) {
        return true;                    /* 已经分配过了 */
    }
    /* 环要 32KB 才够（蓝牙栈不是匀速喂数据，突发时会一次吐好几包）。
     * 但堆不一定有 32KB（电台可能还占着），所以拿不到就往下退一档。
     * 【关键】退档时掩码必须跟着变，否则会写越界踩坏堆。 */
    static const uint32_t sizes[] = {
        BT_PCM_RING_SAMPLES, BT_PCM_RING_SAMPLES / 2, BT_PCM_RING_SAMPLES / 4,
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        s_pcm_ring = malloc(sizes[i] * sizeof(int16_t));
        if (s_pcm_ring != NULL) {
            s_ring_size = sizes[i];     /* 记住实际尺寸 */
            s_ring_mask = sizes[i] - 1; /* 掩码跟着实际尺寸走 */
            s_drop_stale = true;        /* 第一次搬运时丢掉积压的过期数据 */
            /* 蓝牙栈紧接着还要申请几 KB（媒体线程 + SBC 解码器），
             * 这里报一下剩余堆，方便判断是不是内存又不够了。 */
            ESP_LOGI(TAG, "pcm ring: %u bytes (%u samples), free heap after alloc: %d",
                     (unsigned)(sizes[i] * sizeof(int16_t)), (unsigned)sizes[i],
                     (int)esp_get_free_heap_size());
            return true;
        }
        ESP_LOGW(TAG, "no memory for a %u byte pcm ring, trying smaller",
                 (unsigned)(sizes[i] * sizeof(int16_t)));
    }
    ESP_LOGE(TAG, "out of memory for the pcm ring buffer");
    return false;
}


/**
* @brief 复位环形缓冲的读写指针（重新连接时用）。
*
* 缓冲本身不释放 —— 它是开机时一次性拿到的 32KB 连续块，
* 堆后来碎了就再也申请不到这么大一块了。
*/
static void bt_ring_reset(void)
{
    s_pcm_head = 0;                     /* 读写指针复位 */
    s_pcm_tail = 0;
    s_drop_stale = true;                /* 下次开搬时丢掉积压的过期数据 */
}

/* ---------------- 状态 ---------------- */
static bt_player_state_t s_state = BT_STATE_IDLE;   /* 当前状态 */
static char              s_peer[24] = "-";          /* 对端蓝牙地址 */
static char              s_peer_name[32] = "-";     /* 对端设备名 */
static volatile bool     s_radio_was_playing;        /* 断开前电台是否在播 */
static TaskHandle_t      s_audio_task;              /* 蓝牙音频任务句柄 */
static volatile bool     s_connect_work;             /* 连上后的重活待办（停电台/关WiFi/兜底申请环），
                                                      * 由音频任务代劳 —— 回调跑在蓝牙栈任务里，
                                                      * 干重活会把 AVDTP 信令卡死，手机等超时就断链
                                                      * （症状：上电第一次自动重连必掉，第二次才正常）。 */
static volatile bool     s_bt_closed;                /* close 状态：协议栈保留、控制器已关（可秒级重开） */
static volatile bool     s_audio_quit;               /* release 流程请求音频任务退出 */
static volatile bool     s_audio_exited;             /* 音频任务已确认退出 */
static bool               s_started;                 /* 栈是否已经启动过 */

/* ------------------------------------------------------------------ */

/**
* @brief 更新状态。
*/
static void bt_update_state(bt_player_state_t st)
{
    s_state = st;
}

/**
* @brief 当前状态。
*/
bt_player_state_t bt_player_state(void)
{
    return s_state;
}

/**
* @brief 当前状态的文字说明。
*/
const char *bt_player_state_str(void)
{
    switch (s_state) {
    case BT_STATE_DISABLED: return "disabled";     /* 固件没编译蓝牙 */
    case BT_STATE_IDLE:     return "idle";         /* 栈在跑，没人连 */
    case BT_STATE_CONNECTED:return "connected";    /* 手机连上了 */
    case BT_STATE_PLAYING:  return "playing";      /* 正在出声 */
    default:                return "?";            /* 未知状态 */
    }
}

/**
* @brief 已连接设备的蓝牙地址；没有则返回 "-"。
*/
const char *bt_player_peer_addr(void)
{
    return s_peer;
}

/**
* @brief 已连接设备的名称；不知道则返回 "-"。
*/
const char *bt_player_peer_name(void)
{
    return s_peer_name;
}

/**
* @brief 收到的音频流的采样率（正常是 44100）。
*/
int bt_player_sample_rate(void)
{
    return s_bt_rate;
}

/**
* @brief 环形缓冲溢出、不得不丢掉的音频块数。
*/
uint32_t bt_player_overruns(void)
{
    return s_pcm_overrun;
}

/**
* @brief 协议栈交过来几次 PCM（诊断用）。
*/
uint32_t bt_player_data_cbs(void)
{
    return s_data_cbs;
}

/**
* @brief 一共收到多少字节 PCM（诊断用）。
*/
uint32_t bt_player_bytes_in(void)
{
    return s_bytes_in;
}

/**
* @brief 一共往 I2S 写了多少个采样（诊断用）。
*/
uint32_t bt_player_samples_out(void)
{
    return s_samples_out;
}

/**
* @brief 固件是否带蓝牙功能。
*/
bool bt_player_available(void)
{
    return true;
}

/* ================= 响度归一化 + 峰值限幅（蓝牙通路专用） =================
 *
 * 电台那条通路有 AGC（跟平滑 RMS 走），蓝牙这条没有 ——
 * 而手机推过来的电平千差万别（各家 App 的 EQ、手机本身的输出电平），
 * 直接按原样放很容易出现"手机在响、喇叭不响"。
 *
 * 【第一版：呼吸】早先的版本每 5.8ms 拿一块（512 采样）算 RMS，
 * 增益每块最多动 12.5%（≈1dB）—— 折合 **167 dB/s**。音乐一秒之内就有
 * 十几 dB 的起伏，于是增益跟着音乐一起"呼吸"，听感就是忽大忽小。
 *
 * 【第二版：破音】把增益改成双向都慢（20 dB/s）后不呼吸了，
 * 但换来另一个毛病：**音乐从安静段落突然进到高潮时，增益还没降下来**
 * （18dB 的目标变化要 1 秒），这期间输出直接冲到 8000 rms，
 * 峰值顶到 32767 → **硬削波** → 听感就是"破音"。
 * 实测（之前的日志）：rms 2253 × 增益 3.54 = 输出 7977 rms，峰值 ~32000。
 *
 * 【现在：AGC + 限幅器，各管一件事】这是专业音频里的标准结构：
 *   1) 响度归一化（AGC）管"平均多响"：跟平滑 RMS 走，
 *      **增益往下降快（attack）、往上升慢（release）** ——
 *      降得快才能在高潮段落及时退让，升得慢才是"呼吸"的元凶。
 *   2) 峰值限幅（limiter）管"瞬时别超载"：每块先看它的峰值，
 *      峰值会顶到天花板就**立刻**把这一块的增益压下来（attack 瞬时），
 *      然后慢慢恢复（release）。输出因此**永远不会削波**，
 *      也不会有呼吸感。
 */
#define BT_AGC_TARGET_RMS  5000              /* 目标 -16 dBFS */
#define BT_AGC_MAX_GAIN    (8 << 8)          /* 最多 +18 dB（手机推过来的音乐往往很轻） */
#define BT_AGC_MIN_GAIN    (1 << 6)          /* 最少 0.25 倍（-12 dB）：大音量段落要能压下来 */
#define BT_AGC_ATTACK_STEP 24                /* 增益下降：每块最多 9%（快，及时退让） */
#define BT_AGC_RELEASE_STEP 3                /* 增益上升：每块最多 1%（慢，避免呼吸） */
#define BT_AGC_SMOOTH      16                /* RMS 平滑 ~175 ms */
#define BT_LIMIT_CEILING   30000             /* 限幅天花板（满量程 32767，留 8% 余量） */
#define BT_LIMIT_RELEASE   8                 /* 限幅器恢复：每块回到差距的 1/8（≈46 ms） */
#define BT_USE_GAIN        1                 /* 1 = AGC+限幅+音量；0 = 原样透传（音量全交给手机） */
#define BT_VERBOSE         0                 /* 1 = 打印每包回调/波形等调试细节；0 = 只留关键事件 */
#define BT_STAT_INTERVAL_US 30000000u        /* 速率统计打印间隔（微秒）：30 秒一次，别刷屏 */

static int32_t s_bt_gain = 256;   /* Q8 定点，1.0 = 256 */
static int32_t s_bt_rms;          /* 平滑后的短时 RMS */
static bool    s_bt_primed;       /* 首帧直接设定增益 */
#if BT_USE_GAIN
static int32_t s_bt_lim = BT_AGC_MAX_GAIN;  /* 限幅器：当前允许的最大总增益（Q8） */
#endif
static volatile uint32_t s_bt_clipped;      /* 输出被硬限幅（削波）的采样数 —— 正常应该是 0 */
static volatile uint32_t s_bt_out_peak;     /* 输出峰值（诊断用，2 秒窗口） */

#if BT_USE_GAIN
/**
* @brief 算这一批 PCM 的响度增益（蓝牙通路专用）。
*
* @param src   PCM 数据
* @param total 采样数
* @return 增益（Q8 定点，1.0 = 256）
*/
static int32_t bt_gain_update(const int16_t *src, int total)
{
    if (total <= 0) {
        return s_bt_gain;
    }
    int64_t sum = 0;                            // 平方和
    for (int i = 0; i < total; i++) {
        sum += (int32_t)src[i] * (int32_t)src[i];
    }
    int rms = (int)sqrtf((float)sum / (float)total);   /* 均方根 */

    if (s_bt_rms == 0) {                        /* 第一个有效值 */
        s_bt_rms = rms;
    } else {
        s_bt_rms = (s_bt_rms * (BT_AGC_SMOOTH - 1) + rms) / BT_AGC_SMOOTH;
    }
    if (s_bt_rms < 32) {
        /* 静音：把 RMS 估计器复位，但**不动**增益。
         * 不复位的话 rms 会趋近 0，目标增益一路冲到 8x 顶格，
         * 音乐回来时再慢慢降下来 —— 又是"呼吸"。
         * 复位后下一个有声块会重新算出正确的目标，
         * 增益按 20dB/s 平滑地跟过去（不会突变，所以不会有咔哒声）。 */
        s_bt_rms = 0;
        return s_bt_gain;                       /* 太安静，增益先不动 */
    }

    int32_t target = ((int32_t)BT_AGC_TARGET_RMS << 8) / s_bt_rms;
    if (target > BT_AGC_MAX_GAIN) {
        target = BT_AGC_MAX_GAIN;
    }
    if (target < BT_AGC_MIN_GAIN) {
        target = BT_AGC_MIN_GAIN;
    }
    if (!s_bt_primed) {                         /* 首帧直接跳到位 */
        s_bt_gain = target;
        s_bt_primed = true;
        return s_bt_gain;
    }
    /* 【非对称限速】这是"破音"和"呼吸"之间的平衡点：
     *   - 增益往下降（音乐变响）：每块最多 9% —— 10dB 的目标变化
     *     42ms 就跟上了，来得及在削波之前退让。
     *   - 增益往上升（音乐变轻）：每块最多 1% —— 慢。
     *     "呼吸感"就是升得太快造成的（一个鼓点把增益顶上去，
     *     音乐一停增益还来不及落回来），慢了就听不出来。 */
    int32_t diff = target - s_bt_gain;
    if (diff < 0) {                             /* 要压低：快 */
        if (diff < -BT_AGC_ATTACK_STEP) {
            diff = -BT_AGC_ATTACK_STEP;
        }
    } else {                                    /* 要抬高：慢 */
        if (diff > BT_AGC_RELEASE_STEP) {
            diff = BT_AGC_RELEASE_STEP;
        }
    }
    s_bt_gain += diff;
    return s_bt_gain;
}

/**
* @brief 峰值限幅：算出这一块允许的最大总增益，保证输出不会被削波。
*
* 【为什么必须有它】AGC 跟的是**平滑后的** RMS（175ms 窗口），
* 它对音乐里的瞬态（鼓点、镲片、人声爆音）反应不过来：
* 目标增益还没降下来，瞬态峰值就顶到 32767 → 硬削波 → "破音"。
* 限幅器直接看这一块的真实峰值：
*   - 峰值会超 → 立刻把这一块的增益压到刚好不超（attack 瞬时，无咔哒）
*   - 之后慢慢把增益放回去（release ≈46ms，不会呼吸）
* 于是输出永远是"圆的"，听不到任何削波。
*
* @param g     这一块的总增益（AGC × 音量，Q8）
* @param src   原始 PCM（还没乘增益）
* @param total 采样数
* @return 实际应该用的总增益（Q8）
*/
static int32_t bt_limit_apply(int32_t g, const int16_t *src, int total)
{
    if (total <= 0) {
        return g;
    }
    int32_t peak = 0;                           /* 这一块的原始峰值 */
    for (int i = 0; i < total; i++) {
        int32_t a = (src[i] < 0) ? -(int32_t)src[i] : (int32_t)src[i];
        if (a > peak) {
            peak = a;
        }
    }
    /* 这一块的原始峰值最多允许多大的总增益（Q8）：
     *   peak × gain/256 ≤ 天花板  =>  gain ≤ 天花板 × 256 / peak */
    int32_t cap = BT_AGC_MAX_GAIN;              /* 默认不限幅 */
    if (peak > 0) {
        int64_t v = ((int64_t)BT_LIMIT_CEILING << 8) / peak;
        if (v < cap) {
            cap = (int32_t)v;
        }
    }
    if (cap < s_bt_lim) {
        s_bt_lim = cap;                         /* 立刻压下去（attack） */
    } else {
        s_bt_lim += (cap - s_bt_lim) / BT_LIMIT_RELEASE;   /* 慢慢恢复（release） */
    }
    return (g < s_bt_lim) ? g : s_bt_lim;       /* 总增益不能超过限幅器给的上限 */
}
#endif

/* ------------------------------------------------------------------ * 蓝牙任务把解码好的 PCM 交给我们。这个回调必须很快返回，* 所以这里只把采样放进环形缓冲。 * ------------------------------------------------------------------ */

/**
* @brief 蓝牙栈把解码后的 PCM 交给我们（跑在蓝牙协议栈任务里）。
*
* 【IDF 6.x 的坑：两个回调别搞混】
*   - 内部 SBC 解码器（CONFIG_BT_A2DP_USE_EXTERNAL_CODEC 关闭，默认）
*     → 用 esp_a2d_sink_register_data_callback()，签名是 (const uint8_t *buf, uint32_t len)，
*       交给我们的是**已经解码好的 PCM**。
*   - 外部解码器（CONFIG_BT_A2DP_USE_EXTERNAL_CODEC 打开）
*     → 用 esp_a2d_sink_register_audio_data_callback()，签名是 (conn_hdl, audio_buf)，
*       交给我们的是**未解码的 SBC 帧**。
* 用错的话回调**永远不会被调用**（症状：连上了、码流起来了，就是没声音）。
*
* 这个回调必须**尽快返回**（栈在等它），所以这里只把数据塞进环形缓冲，
* 真正的 I2S 写入由 bt_audio_task 去做。
*/
static void bt_audio_data_cb(const uint8_t *buf, uint32_t len)
{
    if (s_pcm_ring == NULL) {
        return;   /* not connected (or out of memory) */
    }
    if (buf == NULL || len == 0) {
        return;                                 // 空的包，忽略
    }
    uint32_t bytes = len;                       // 这一包有多少字节
    uint32_t samples = bytes / 2;               // 16bit 采样 = 2 字节
    s_data_cbs++;                               // 计数（诊断用）
    s_bytes_in += bytes;
#if BT_VERBOSE
    if (s_data_cbs <= 3 || (s_data_cbs % 200) == 0) {
        ESP_LOGI(TAG, "bt pcm in: cb #%u, %u bytes (%u samples)",
                 s_data_cbs, bytes, samples);
    }

    /* 【诊断】把这一包的波形原样打出来：
     * 一眼能分出"正常波形" / "乱码"（通道错位、字节序不对）/ "幅度极小"。 */
    {
        int32_t mn = 32767, mx = -32768;
        int64_t sum = 0;
        int64_t diff = 0;                /* 左右声道差值（判断通道是否错位） */
        const int16_t *pcm = (const int16_t *)buf;
        int ch = s_bt_channels;
        for (uint32_t i = 0; i + (uint32_t)ch <= samples; i += (uint32_t)ch) {
            int32_t l = pcm[i];
            int32_t r = (ch == 2) ? pcm[i + 1] : l;
            if (l < mn) mn = l;
            if (l > mx) mx = l;
            sum += (int64_t)l * l;
            diff += (int64_t)(l - r) * (l - r);
        }
        int n = (int)(samples / (uint32_t)ch);
        int rms = (n > 0) ? (int)sqrtf((float)sum / (float)n) : 0;
        int drms = (n > 0) ? (int)sqrtf((float)diff / (float)n) : 0;
        if (s_data_cbs == 1) {
            ESP_LOGI(TAG, "packet stats: %d samples/ch, min=%d max=%d rms=%d, "
                     "L-R rms=%d", n, (int)mn, (int)mx, rms, drms);
        }
        /* 声音起来了（rms > 2000）再 dump 一次波形 —— 这时才有意义 */
        if (rms > 2000 && !s_wave_dumped) {
            s_wave_dumped = true;
            ESP_LOGI(TAG, "waveform (%d Hz, %d ch, rms=%d):", s_bt_rate, ch, rms);
            for (int k = 0; k < 4; k++) {
                ESP_LOGI(TAG, "  %5d %5d %5d %5d %5d %5d %5d %5d",
                         pcm[k * 8 + 0], pcm[k * 8 + 1], pcm[k * 8 + 2], pcm[k * 8 + 3],
                         pcm[k * 8 + 4], pcm[k * 8 + 5], pcm[k * 8 + 6], pcm[k * 8 + 7]);
            }
        }
    }
#endif /* BT_VERBOSE */

    uint32_t head = s_pcm_head;                       // 当前的写位置
    uint32_t tail = s_pcm_tail;                       // 当前的读位置
    uint32_t used = (head - tail) & s_ring_mask;      // 环里已有多少
    uint32_t free_samples = (s_ring_size - 1) - used;   // 还能放多少（留一个空槽）

    /* 记一下水位最高到过多少 —— 蓝牙栈一次突发能吐多少，全看这个数 */
    if (used > s_ring_peak) {
        s_ring_peak = used;
    }

    /* 【高水位回落】积压超过 90% 才把最老的数据丢掉、压回 45%：
     * 水位本身有 ±3000 采样的抖动（蓝牙包成簇到达），阈值太低（如 75%）会被
     * 正常抖动频繁触发，每秒跳一次 = 持续咔哒声。真实时钟漂移只有
     * ~0.06%（进出速率长期均值之差），按它算一分钟跳一次就够 —— 不可闻。
     * 90% 阈值给正常抖动留足空间，只有真正快满时才跳。 */
    if (used > s_ring_size * 9 / 10) {
        uint32_t drop = used - s_ring_size * 45 / 100;  /* 丢到 45% 水位 */
        tail = (tail + drop) & s_ring_mask;           // 丢掉最老的数据
        used -= drop;
        free_samples = (s_ring_size - 1) - used;      /* 水位变了，可用空间重算 */
        s_pcm_overrun++;                              /* 记一次跳帧（诊断可见） */
        ESP_LOGD(TAG, "drift skip: dropped %u old samples", (unsigned)drop);
    }

    if (samples > free_samples) {
/* I2S 消费得太慢 —— 丢掉最老的采样 */
        uint32_t drop = samples - free_samples;       // 要丢多少
        tail = (tail + drop) & s_ring_mask;           // 丢掉最老的数据
        s_pcm_overrun++;                              // 记一次溢出
    }

    const int16_t *src = (const int16_t *)buf;
    for (uint32_t i = 0; i < samples; i++) {          // 逐个采样写进环里
        s_pcm_ring[(head + i) & s_ring_mask] = src[i];
    }
    s_pcm_tail = tail;                               // 读指针可能被推进了
    s_pcm_head = (head + samples) & s_ring_mask;     // 写指针前进

    if (s_state != BT_STATE_PLAYING) {
        bt_update_state(BT_STATE_PLAYING);           // 已经在出声了
    }
/* 唤醒音频任务 */
    if (s_audio_task) {
        xTaskNotifyGive(s_audio_task);                // 唤醒音频任务
    }
}

/* ------------------------------------------------------------------ * 音频任务：把环形缓冲里的 PCM 搬进 I2S。 * ------------------------------------------------------------------ */

/**
* @brief 蓝牙音频任务：把环形缓冲里的 PCM 搬进 I2S。
*/
static void bt_audio_task(void *arg)
{
    int16_t out[512];                // 一次最多搬 256 帧（5.8ms，展开后 512 个采样）

    for (;;) {
/* 等第一批采样。有连接重活待办时用短超时轮询
 * （任务可能在手机连接之后才创建，没有通知也得把活干掉）。 */
        if (s_connect_work) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     // 等蓝牙任务通知
        }

/* 【连接重活】以前在连接回调里同步做，会把 AVDTP 信令卡死（手机
 * 等不到响应就断链 —— 上电第一次自动重连必掉），现在挪到这里做。
 * 顺序很关键：
 * 1) wifi_suspend()：用户要求，蓝牙播放期间射频完全归蓝牙独占，
 *    顺带释放几十 KB 堆给蓝牙栈。
 * 2) 停电台 + 锁 I2S + 等它把堆缓冲还回来（最多卡一次 socket 读）。
 * 3) 兜底申请 PCM 环（正常在开机时就分配好了）。 */
        if (s_connect_work) {
            s_connect_work = false;
            wifi_suspend();
            radio_stop();                           // 先让电台停下
            radio_i2s_lock(true);                   // 锁住 I2S，电台不许写
            radio_stop_and_release(1000);           // 等电台把堆缓冲还回来
            if (s_pcm_ring == NULL && !bt_ring_alloc()) {
                ESP_LOGE(TAG, "out of memory for the pcm ring buffer");
            }
/* 协议栈紧接着要申请 4KB 以上（媒体线程 + SBC 解码器 + L2CAP 控制块），
 * 余量不够它会自己崩，这里报一下余量，一眼能看出是不是内存不够。 */
            ESP_LOGI(TAG, "bt ready, free heap for the stack: %d bytes",
                     (int)esp_get_free_heap_size());
        }

        if (s_audio_quit) {
            break;                           /* release：请求任务退出 */
        }

/* 电台可能正在写 I2S —— 稍等它退出。
         * （I2S 已经被 radio_i2s_lock(true) 锁住，电台写不进来了，
         *   这里只是让它手上那一帧写完，别让两路数据交错。）
         * 【注意】不能等太久：等待期间蓝牙栈还在不停交数据，
         *   环会被灌满，而消费端永远追不回来（I2S 和源头都是 44100/s），
         *   环从此一直半满，一有突发就溢出丢音频。 */
        for (int i = 0; i < 5 && radio_is_playing(); i++) {  // 最多等 50ms
            vTaskDelay(pdMS_TO_TICKS(10));
        }

/* 【关键】丢掉积压的"过期"数据。
         * 蓝牙刚连上时，音频任务还没开始搬，栈已经交了好几包过来，
         * 这些数据比实时落后几十到几百毫秒。硬播出去的话：
         *   - 延迟越来越大
         *   - 环一直半满，一有突发就溢出（听起来就是"破音"）
         * 所以每次连接后第一次开搬时，只保留最新的 2048 个采样（23ms）。 */
        if (s_drop_stale) {
            s_drop_stale = false;
            uint32_t used = (s_pcm_head - s_pcm_tail) & s_ring_mask;
            const uint32_t keep = 2048;             // 留最新的 23ms
            if (used > keep) {
                s_pcm_tail = (s_pcm_tail + (used - keep)) & s_ring_mask;
                ESP_LOGI(TAG, "dropped %u stale samples before starting playback",
                         (unsigned)(used - keep));
            }
        }

/* I2S 时钟跟着【手机协商出来的采样率】走 —— 写死 44.1kHz 会破音 */
        audio_i2s_set_rate(s_bt_rate);

        for (;;) {                                    // 一直搬，直到环空
            if (s_pcm_ring == NULL) {
                break;   /* the phone disconnected */
            }
            if (s_audio_quit) {
                break;                           /* release：请求任务退出 */
            }
            uint32_t head = s_pcm_head;               // 写指针
            uint32_t tail = s_pcm_tail;               // 读指针
            uint32_t avail = (head - tail) & s_ring_mask;   // 可用采样数
            if (avail == 0) {
                break;                               // 环空了，等下一次通知
            }

            int      ch     = s_bt_channels;          // 1 = 单声道，2 = 立体声
            uint32_t frames = avail / (uint32_t)ch;    // 每声道的采样数
            if (frames == 0) {
                break;
            }
/* 【关键】一次只搬 256 帧（5.8ms）。
* 写大块会让 i2s_channel_write 阻塞很久（等 DMA 空位），
* 这期间新到的数据全堆在环里，环一满就开始丢音频。
* 小块搬运 = 消费端紧紧跟住源头，环里几乎不积压。 */
            if (frames > 256) {
                frames = 256;                        // out 装得下的最大帧数
            }

/* 把输入采样搬进 out（单声道复制成左右，立体声原样交错） */
            for (uint32_t i = 0; i < frames; i++) {
                if (ch == 1) {
                    int16_t s = s_pcm_ring[(tail + i) & s_ring_mask];
                    out[2 * i]     = s;
                    out[2 * i + 1] = s;
                } else {
                    out[2 * i]     = s_pcm_ring[(tail + 2 * i)     & s_ring_mask];
                    out[2 * i + 1] = s_pcm_ring[(tail + 2 * i + 1) & s_ring_mask];
                }
            }
            uint32_t out_samples = frames * 2;        // 实际写进 I2S 的采样数

#if BT_USE_GAIN
/* 响度归一化（跟电台同一套算法）再乘用户音量 */
            int vol = radio_get_volume();             // 音量（和电台共用）
            int32_t g = (bt_gain_update(out, (int)out_samples) * vol) / 100;   /* Q8 */
/* 峰值限幅：这一块的峰值不许顶到天花板 —— **削波就是"破音"**。
 * 限幅器看的是【还没乘增益】的原始采样，算出这一块该用多大的增益。 */
            g = bt_limit_apply(g, out, out_samples);
            for (uint32_t i = 0; i < out_samples; i++) {   // 乘增益
                int32_t v = ((int32_t)out[i] * g) >> 8;
                if (v > 32767) {
                    v = 32767;                       // 兜底（限幅器正常时走不到这里）
                    s_bt_clipped++;                  // 记一次削波
                } else if (v < -32768) {
                    v = -32768;
                    s_bt_clipped++;
                }
                int32_t a = (v < 0) ? -v : v;
                if ((uint32_t)a > s_bt_out_peak) {
                    s_bt_out_peak = (uint32_t)a;    /* 记下输出峰值（诊断） */
                }
                out[i] = (int16_t)v;
            }
#else
/* 【增益已关：原样透传】按用户要求，蓝牙音频不做 AGC、不限幅、不乘音量，
 * 解码后的 PCM 直接送 I2S，音量改由手机端控制。仍统计原始 RMS/峰值供诊断。 */
            {
                uint64_t sum = 0;
                for (uint32_t i = 0; i < out_samples; i++) {
                    int32_t v = (int32_t)out[i];
                    int32_t a = (v < 0) ? -v : v;
                    if ((uint32_t)a > s_bt_out_peak) {
                        s_bt_out_peak = (uint32_t)a;    /* 原始峰值（诊断） */
                    }
                    sum += (uint64_t)((int64_t)v * v);
                }
                s_bt_rms  = (out_samples > 0)
                            ? (int)sqrtf((float)sum / (float)out_samples) : 0;
                s_bt_gain = 256;                      /* 标记：增益 = 1.0（透传） */
            }
#endif
            s_pcm_tail = (tail + frames * (uint32_t)ch) & s_ring_mask;   // 读指针前进
            if (!audio_i2s_write(out, out_samples * 2, 200)) {   // 写进 I2S
                ESP_LOGW(TAG, "i2s write failed");
                break;                               // 写失败就先退出这轮
            }
            s_samples_out += out_samples;            // 计数（诊断用）

            /* 【诊断】每 30 秒报一次（BT_STAT_INTERVAL_US）—— 慢点刷：
             * - in : 协议栈交过来的速率（应该 = 88200 采样/秒 = 44100 帧/秒）
             * - out: 写进 I2S 的速率（和 in 相等 = 一帧不丢）
             * - ring: 环里的余量。长期贴着 0 = I2S 断粮（破音）；
             *         长期贴着上限 = 环满了在丢数据（也是破音）。
             *         稳定在中间 = 正常。 */
            int64_t now = esp_timer_get_time();
            if (now - s_last_stat_us >= BT_STAT_INTERVAL_US) {
                double dt = (double)(now - s_last_stat_us) / 1e6;
                uint32_t in_b  = s_bytes_in - s_last_bytes_in;
                uint32_t out_s = s_samples_out - s_last_samples_out;
                ESP_LOGI(TAG, "rates: in %.0f samples/s, out %.0f samples/s, "
                         "ring %u/%u, peak %u, overruns %u, gain %d/256, rms %d, "
                         "clip %u, outpk %u",
                         (double)(in_b / 2) / dt, (double)out_s / dt,
                         (unsigned)((s_pcm_head - s_pcm_tail) & s_ring_mask),
                         (unsigned)s_ring_size,
                         (unsigned)s_ring_peak,
                         (unsigned)s_pcm_overrun,
                         (int)s_bt_gain, (int)s_bt_rms,
                         (unsigned)s_bt_clipped,
                         (unsigned)s_bt_out_peak);
                s_last_stat_us     = now;
                s_last_bytes_in    = s_bytes_in;
                s_last_samples_out = s_samples_out;
                s_ring_peak = 0;                 /* 峰值按 2 秒窗口统计 */
                s_bt_out_peak = 0;               /* 输出峰值也按 2 秒窗口统计 */
            }
        }
    }

/* release 流程会请求退出：任务使命结束，自删（先置标志再删，
 * 让 bt_player_release() 能等到"确实退干净了"再拆协议栈） */
    s_audio_exited = true;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ * AVRCP 目标端：让手机能遥控我们（音量条、播放/暂停） * ------------------------------------------------------------------ */

/**
* @brief AVRCP 目标端回调（遥控/音量）。
*
* 这里的 "target" 是被控方：手机是遥控器，ESP32 是被控的播放器。
* 没有 AVRCP 的话，很多手机（尤其 iOS）根本不会把本机列成音频输出设备。
*/
static void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (param == NULL) {
        return;                                  // 没有数据就没法处理
    }
    switch (event) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {   // 手机音量条：0~127
        uint8_t v = param->set_abs_vol.volume;
        int vol = (int)(((uint32_t)v * 100) / 127);   // 换算成 0~100
        radio_set_volume(vol);                       // 直接用我们自己的音量
        ESP_LOGI(TAG, "avrc: phone set the volume to %d%%", vol);
        break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {     // 手机订阅了某个通知
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
/* 订阅了"音量变化"：把当前音量报回去 */
            esp_avrc_rn_param_t rp;
            memset(&rp, 0, sizeof(rp));
            rp.volume = (uint8_t)((radio_get_volume() * 127) / 100);
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                                    ESP_AVRC_RSP_ACCEPT, &rp);
        }
        break;
    }
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {           // 播放/暂停等按键
        if (param->psth_cmd.key_state == 0) {        // 0 = 按下
            ESP_LOGI(TAG, "avrc: key 0x%02x", param->psth_cmd.key_code);
        }
        break;
    }
    default:                                          // 其它事件不关心
        break;
    }
}

/* ------------------------------------------------------------------ * A2DP 状态回调 * ------------------------------------------------------------------ */

/**
* @brief A2DP 状态回调（跑在蓝牙协议栈任务里）。
*
* @param event 事件类型
* @param param 事件数据
*/
static void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (param == NULL) {
        return;                             // 没有数据就没法处理
    }
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {                // 连接状态变了
        esp_a2d_connection_state_t st = param->conn_stat.state;   // 新状态
        const esp_bd_addr_t *bda = &param->conn_stat.remote_bda; // 对端地址
        snprintf(s_peer, sizeof(s_peer), "%02X:%02X:%02X:%02X:%02X:%02X",
                 bda[0][0], bda[0][1], bda[0][2], bda[0][3], bda[0][4], bda[0][5]);
        esp_bd_addr_t peer;                            // 复制一份（回调返回后就没了）
        memcpy(peer, bda, sizeof(peer));
        if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {          // 手机连上了
            ESP_LOGI(TAG, "connected: %s", s_peer);
            bt_update_state(BT_STATE_CONNECTED);
            s_radio_was_playing = radio_is_playing();  // 记下电台原来在不在播
/* 【关键】回调跑在蓝牙栈任务（BTC）里，必须立刻返回。
 * 以前在这里同步干重活 —— radio_stop_and_release(最长 1s) +
 * esp_wifi_stop(几百 ms) —— AVDTP 信令被卡死，手机等超时就主动断链，
 * 症状就是"上电第一次自动重连必掉、第二次才正常"。
 * 重活（关WiFi/停电台/锁I2S/兜底申请环）全部挪给音频任务做，
 * 这里只置标志 + 唤醒，几微秒就返回。 */
            s_connect_work = true;
            s_drop_stale = true;
/* 新的一条连接：响度估计器和限幅器都从头开始（别继承上一条的电平） */
            s_bt_rms    = 0;
            s_bt_primed = false;
            s_bt_gain   = 256;
#if BT_USE_GAIN
            s_bt_lim    = BT_AGC_MAX_GAIN;  /* 限幅器也复位（别带着上一条的压制状态） */
#endif
            s_bt_clipped  = 0;              /* 削波计数清零 */
            s_bt_out_peak = 0;
/* 问设备名（结果在 gap 回调里送到） */
            esp_bt_gap_read_remote_name(peer);         // 问设备名
            if (s_audio_task) {
                xTaskNotifyGive(s_audio_task);         // 唤醒音频任务去干重活
            }
        } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) { // 手机断开了
            ESP_LOGI(TAG, "disconnected");
            bt_update_state(BT_STATE_IDLE);
            snprintf(s_peer, sizeof(s_peer), "-");      // 地址清空
            snprintf(s_peer_name, sizeof(s_peer_name), "-");  // 名字清空
            wifi_resume();                             /* 手机走了，把 WiFi 请回来（重连由 STA_START 事件自动发起） */
            radio_i2s_lock(false);                     // 把 I2S 还给电台
            /* 环形缓冲**不释放**：它是开机时一次性分配的 32KB 连续块，
             * 堆后来被电台反复分配/释放切碎了，再也申请不到这么大一块。
             * 留着它，电台照样能从剩下的 ~23KB 里申请到自己的 17KB ✓ */
            bt_ring_reset();                           // 只复位读写指针
            if (s_radio_was_playing) {                 // 之前在播电台？恢复
                int idx = radio_current_index();
                if (idx >= 0) {
                    ESP_LOGI(TAG, "back to the radio (station %d)", idx);
                    radio_play(idx);
                }
            }
            s_radio_was_playing = false;
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {                      // 编解码参数协商好了
/* 【关键】手机可能协商到 48kHz 或单声道，必须按协商结果来放，
* 否则就是"破音"（采样率不符 → 音调不对；声道数不符 → 左右错位）。 */
        const esp_a2d_cie_sbc_t *sbc = &param->audio_cfg.mcc.cie.sbc_info;
        int rate = 44100;
        if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
            rate = 48000;
        } else if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
            rate = 44100;
        } else if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
            rate = 32000;
        } else if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_16K) {
            rate = 16000;
        }
        int ch = (sbc->ch_mode & (ESP_A2D_SBC_CIE_CH_MODE_MONO |
                                  ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL)) ? 1 : 2;
        s_bt_rate     = rate;
        s_bt_channels = ch;
        ESP_LOGI(TAG, "audio codec configured: %d Hz, %d ch", rate, ch);
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {                    // 手机开始/停止推流
        esp_a2d_audio_state_t st = param->audio_stat.state;
        ESP_LOGI(TAG, "audio stream %s", (st == ESP_A2D_AUDIO_STATE_STARTED)
                 ? "started" : "suspended");
        break;
    }
    case ESP_A2D_SNK_PSC_CFG_EVT:                      // 流能力配置（不需要处理）
        break;
    case ESP_A2D_PROF_STATE_EVT: {                     // A2DP profile 初始化完成
        if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
            ESP_LOGI(TAG, "a2dp profile ready");
/* 【关键】经典蓝牙（BR/EDR）不像 BLE 有"广播"这个概念，
* 必须显式把 GAP 的 scan mode 设成"可连接 + 可被发现"，
* 手机才搜得到我们。漏了这一句 = 手机列表里永远看不到。 */
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    }
    default:                                            // 其它事件不关心
        break;
    }
}

/* ------------------------------------------------------------------ * GAP（经典蓝牙）回调 —— 用来读对端设备名 * ------------------------------------------------------------------ */

/**
* @brief GAP 回调：用来读对端设备名。
*/
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT && param != NULL) {
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS &&   // 读成功
            param->read_rmt_name.rmt_name[0]) {                      // 名字非空
            strlcpy(s_peer_name, (const char *)param->read_rmt_name.rmt_name,
                    sizeof(s_peer_name));
            ESP_LOGI(TAG, "device name: %s", s_peer_name);
        }
    }
}

/* ------------------------------------------------------------------ */

/**
* @brief 主动断开已连接的手机。
*
* 手机可能自己又连回来（这是手机的行为，不是 bug）。
*/
bool bt_player_disconnect(void)
{
    esp_bd_addr_t bda;                     // 目标地址
    if (s_state < BT_STATE_CONNECTED) {
        return false;                      // 没人连着
    }
    if (sscanf(s_peer, "%2x:%2x:%2x:%2x:%2x:%2x",      // 把字符串地址解析回来
               (unsigned *)&bda[0], (unsigned *)&bda[1], (unsigned *)&bda[2],
               (unsigned *)&bda[3], (unsigned *)&bda[4], (unsigned *)&bda[5]) != 6) {
        return false;                      // 解析失败
    }
    return esp_a2d_sink_disconnect(bda) == ESP_OK;
}

/**
* @brief 启动蓝牙控制器、Bluedroid 协议栈和 A2DP sink。
*
* 必须在 NVS 初始化之后调用（蓝牙要用 NVS 存配对信息）。
*/
esp_err_t bt_player_init(void)
{
    if (s_started) {
        return ESP_OK;                     // 已经启动过了
    }

/* ---- 1. 控制器（射频部分）---- */
/* 只用经典蓝牙（BR/EDR），把 BLE 那部分控制器内存释放掉给别的用。
* 失败不致命（只是省不下这块内存），所以只记日志不返回错误。 */
    esp_err_t mem_err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (mem_err != ESP_OK) {
        ESP_LOGW(TAG, "release ble memory failed: %s", esp_err_to_name(mem_err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed");
        return ESP_FAIL;
    }

/* 【关键】IDF 6.x 里 esp_bt_controller_init() 只是把控制器置成 INITED，
* 必须再调 esp_bt_controller_enable() 才会变成 ENABLED；
* 而 esp_bluedroid_init() 要求控制器已经是 ENABLED，
* 否则直接报 "Controller not initialised" 失败（蓝牙整个起不来）。
* 官方 a2dp_sink 示例的顺序也是：init -> enable -> bluedroid_init。 */
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed");
        return ESP_FAIL;
    }

/* ---- 2. Bluedroid 协议栈 ---- */
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if (esp_bluedroid_init_with_cfg(&bluedroid_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed");
        return ESP_FAIL;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed");
        return ESP_FAIL;
    }

    esp_bt_gap_set_device_name("ESP32 Radio");     // 手机里看到的名字
    esp_bt_gap_register_callback(bt_gap_cb);        // 读设备名用

/* 【关键】经典蓝牙（BR/EDR）没有 BLE 那种"广播"，
* 必须显式把 GAP 的 scan mode 设成"可连接 + 可被发现"，
* 否则手机扫不到我们（表现为：其它设备都能搜到，就是搜不到 ESP32）。
* 官方 a2dp_sink 示例也是在 A2DP profile 就绪后做这件事。 */
    esp_err_t scan_err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                  ESP_BT_GENERAL_DISCOVERABLE);
    if (scan_err != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_mode failed: %s", esp_err_to_name(scan_err));
    } else {
        ESP_LOGI(TAG, "discoverable, waiting for a phone ...");
    }

/* ---- 3. AVRCP 目标端（遥控/音量）----
* 必须在 A2DP 之前初始化（IDF 文档的要求）。
* 没有它的话，很多手机（尤其 iOS）根本不会把本机列成音频输出设备。 */
    if (esp_avrc_tg_init() != ESP_OK) {
        ESP_LOGE(TAG, "avrc target init failed");
        return ESP_FAIL;
    }
    if (esp_avrc_tg_register_callback(bt_avrc_tg_cb) != ESP_OK) {
        ESP_LOGE(TAG, "avrc callback registration failed");
        return ESP_FAIL;
    }

/* ---- 4. A2DP sink（接收端） ---- */
    if (esp_a2d_sink_init() != ESP_OK) {
        ESP_LOGE(TAG, "a2dp sink init failed");
        return ESP_FAIL;
    }
    if (esp_a2d_register_callback(bt_a2dp_cb) != ESP_OK) {
        ESP_LOGE(TAG, "a2dp callback registration failed");
        return ESP_FAIL;
    }
/* 【关键】内部 SBC 解码器模式下必须用 esp_a2d_sink_register_data_callback()，
* 它交给我们的是【已经解码好的 PCM】。
* esp_a2d_sink_register_audio_data_callback() 是给【外部解码器】用的
* （交来的是未解码的 SBC 帧），在内部解码器模式下注册它，
* 回调【永远不会被调用】—— 症状就是"连上了、码流起来了，就是没声音"。 */
    if (esp_a2d_sink_register_data_callback(bt_audio_data_cb) != ESP_OK) {
        ESP_LOGE(TAG, "a2dp data callback registration failed");
        return ESP_FAIL;
    }

/* 流端点（SEP）不用自己注册：
* 内部解码器模式下，协议栈在 esp_a2d_sink_init() 时就注册好了默认的
* SBC 端点（官方 a2dp_sink_stream 示例同样不注册）。
* 自己再注册一个反而会顶掉栈的默认端点，导致栈不知道要解码我们这条流。 */

/* ---- 4. 音频任务（把 PCM 搬进 I2S） ----
 * 绑到 core 1（应用核）：蓝牙协议栈 BTC/HCI/控制器全在 core 0 上跑，
 * 所以把消费者放在 core 1 绝不会饿死蓝牙栈，连接/握手不受影响。
 * 优先级 21：高于应用层任务，突发期间能在 BTC 把数据塞进 ring 的同时，
 * 持续把 ring 里的数据灌进 I2S 的 DMA（吸收能力 = 环(186ms)+DMA(87ms)≈273ms），
 * 消除 overruns 丢数据导致的喀哒声。
 * 之前错绑到 core 0（与 BTC 同核、优先级 21 > BTC 19）把 BTC 饿死 → 蓝牙连不上，
 * 换成 core 1 后该问题消失。 */
    if (xTaskCreatePinnedToCore(bt_audio_task, "bt_audio", 4096, NULL, 21, &s_audio_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the audio task");
        return ESP_FAIL;
    }

    /* ---- 5. 现在就分配 PCM 环形缓冲（32KB） ----
     * 【为什么必须现在分配】堆刚启动时还没被切碎，能拿到 32KB 的连续块。
     * 等到手机连上再申请就晚了：这时电台已经反复分配/释放过 17KB 的缓冲，
     * 堆被切碎，malloc(32768) 会失败（明明 free heap 还有 60KB）。
     * 环一直留着不释放；电台用剩下的 ~23KB 申请自己的缓冲，够用。 */
    if (!bt_ring_alloc()) {
        ESP_LOGW(TAG, "no pcm ring buffer yet, will retry when a phone connects");
    }

    bt_update_state(BT_STATE_IDLE);            // 栈起来了，在等手机
    s_started = true;
    ESP_LOGI(TAG, "a2dp sink ready (sbc 44100 hz stereo), name 'ESP32 Radio'");
    return ESP_OK;
}

/* ------------------------------------------------------------------ * 生命周期：open（开启）/ close（关闭）/ release（释放） * ------------------------------------------------------------------ */

/**
* @brief 开启蓝牙（像手机 App 的"打开"）。
*
* 三种起点都能进：
*  - 从没启动过 / 被 release 过 → 完整冷启动（协议栈全部重建）；
*  - close 过 → 秒级热启动（只重新使能控制器 + 恢复可被发现）；
*  - 已经开 → 什么都不做。
* 必须从普通任务调用（控制台/电台任务），不能在蓝牙回调里调。
*
* @return 成功 true
*/
bool bt_player_open(void)
{
    if (!s_started) {
        return bt_player_init() == ESP_OK;     /* 冷启动：完整重建协议栈 */
    }
    if (s_bt_closed) {
        s_bt_closed = false;
        if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
            ESP_LOGE(TAG, "controller enable failed");
            return false;
        }
        /* 重新使能后必须把扫描模式设回来（PROF_STATE 事件不会重发） */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        ESP_LOGI(TAG, "bluetooth open (controller re-enabled), waiting for a phone ...");
    }
    return true;                               /* 本来就开着 */
}

/**
* @brief 关闭蓝牙（像手机 App 的"停止"）。
*
* 断开手机 + 关掉控制器（射频完全让出来），但【协议栈和内存都保留】，
* bt_player_open() 秒级重开。电台开播时自动调用（电台独占射频）。
*
* @return 成功 true
*/
bool bt_player_close(void)
{
    if (!s_started || s_bt_closed) {
        return true;                       /* 没开 / 已经关了 */
    }
    if (bt_player_state() >= BT_STATE_CONNECTED) {
        bt_player_disconnect();            /* 手机还连着：先断开 */
        for (int i = 0; i < 30 && bt_player_state() >= BT_STATE_CONNECTED; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));    /* 最多等 3 秒 */
        }
    }
    s_bt_closed = true;
    if (esp_bt_controller_disable() != ESP_OK) {
        ESP_LOGW(TAG, "controller disable failed");
        return false;
    }
    ESP_LOGW(TAG, "bluetooth closed (stack kept, RF off)");
    return true;
}

/**
* @brief 释放蓝牙（像手机 App 的"卸载"）。
*
* 按启动的相反顺序把整个协议栈拆掉：音频任务退出 → A2DP/AVRC 反初始化
* → Bluedroid 关闭+卸载 → 控制器反初始化 → 释放环形缓冲。
* 所有蓝牙内存归还堆（约 40KB+），电台可以随便用。
* 之后再 bt_player_open() 会完整冷启动重建。
*
* @return 成功 true
*/
bool bt_player_release(void)
{
    if (!s_started) {
        return true;                       /* 本来就没开 */
    }
    bt_player_close();                     /* 先断手机 + 关控制器（不再有回调进来） */
/* 请音频任务退出：它可能正阻塞在 i2s 写上，最多等 500ms（控制器已关，
 * 环最多 93ms 就搬空了），等不到再强删兜底。 */
    if (s_audio_task) {
        s_audio_quit = true;
        xTaskNotifyGive(s_audio_task);
        for (int i = 0; i < 50 && !s_audio_exited; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!s_audio_exited) {
            vTaskDelete(s_audio_task);     /* 兜底强删（罕见：I2S 卡死时） */
        }
        s_audio_task = NULL;
    }
    s_audio_quit  = false;
    s_audio_exited = false;
    radio_i2s_lock(false);                 /* I2S 还给电台（本来锁着的话） */
/* 按启动的相反顺序拆协议栈 */
    esp_a2d_sink_deinit();                 /* A2DP sink 反初始化（顺带注销数据回调） */
    esp_avrc_tg_deinit();                  /* AVRCP 反初始化 */
    esp_bluedroid_disable();
    esp_bluedroid_deinit();                /* GAP/A2DP/AVRC 回调注册全部随栈销毁 */
    esp_bt_controller_deinit();            /* 控制器反初始化（close 已 disable） */
/* 释放 PCM 环：这 16KB 还给堆（电台可以用） */
    if (s_pcm_ring) {
        free(s_pcm_ring);
        s_pcm_ring  = NULL;
        s_ring_size = 0;
        s_ring_mask = 0;
    }
    s_started = false;
    bt_update_state(BT_STATE_DISABLED);
    ESP_LOGW(TAG, "bluetooth released, free heap: %d bytes",
             (int)esp_get_free_heap_size());
    return true;
}

/**
* @brief 当前电源状态：on（开着）/ closed（关着、资源保留）/ released（已释放）。
*/
const char *bt_player_power_str(void)
{
    if (!s_started) {
        return "released";
    }
    return s_bt_closed ? "closed" : "on";
}

#else /* !BT_SUPPORTED */

/*
* 固件没编译蓝牙时的空实现（所有接口都返回"不支持"），
* 这样 main.c 里的调用代码不用到处 #ifdef。
*/

/** @brief 固件没编译蓝牙功能。 */
bool bt_player_available(void)
{
    return false;
}

/** @brief 没有蓝牙功能。 */
esp_err_t bt_player_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

/** @brief 没有蓝牙功能。 */
bt_player_state_t bt_player_state(void)
{
    return BT_STATE_DISABLED;
}

/** @brief 没有蓝牙功能。 */
const char *bt_player_state_str(void)
{
    return "disabled";
}

/** @brief 没有蓝牙功能。 */
const char *bt_player_peer_addr(void)
{
    return "-";
}

/** @brief 没有蓝牙功能。 */
const char *bt_player_peer_name(void)
{
    return "-";
}

/** @brief 没有蓝牙功能。 */
int bt_player_sample_rate(void)
{
    return 0;
}

/** @brief 没有蓝牙功能。 */
uint32_t bt_player_overruns(void)
{
    return 0;
}

/** @brief 没有蓝牙功能。 */
bool bt_player_disconnect(void)
{
    return false;
}

/** @brief 没有蓝牙功能。 */
bool bt_player_open(void)
{
    return false;
}

/** @brief 没有蓝牙功能。 */
bool bt_player_close(void)
{
    return true;
}

/** @brief 没有蓝牙功能。 */
bool bt_player_release(void)
{
    return true;
}

/** @brief 没有蓝牙功能。 */
const char *bt_player_power_str(void)
{
    return "n/a";
}

#endif /* BT_SUPPORTED */
