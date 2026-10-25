/*
 * ============================================================================
 *  radio_player.h —— 互联网收音机核心模块接口
 * ============================================================================
 *
 *  【这个模块负责什么】
 *  从 HTTP 拉流 -> 解析 ICY 元数据 -> MP3 解码 -> 音量/响度处理 ->
 *  I2S 输出到 MAX98357A 功放。控制台（main.c）只调用这里的接口。
 *
 *  【线程模型】
 *  - 电台任务（radio task）：负责收流和解码，唯一写 I2S 的一方（蓝牙不在线时）
 *  - 控制台任务：只调用这里的接口改状态，不直接碰 I2S
 *
 *  【内存说明】
 *  大缓冲（流缓冲、解码缓冲）不在 .bss 里，而是播放时从堆上申请、
 *  停止时释放 —— 蓝牙栈需要同一块内存，两者互斥。
 */

#pragma once  // 头文件只包含一次

#include <stdbool.h>   // bool 类型
#include <stddef.h>   // size_t
#include <stdint.h>   // uint32_t

/* ===================== 电台描述 ===================== */

/** 一个电台的名字和流地址。 */
typedef struct {
    const char *name;   // 电台名（显示用）
    const char *url;    // 流地址（http/https）
} radio_station_t;

/* ===================== 事件回调 ===================== */

/** 电台模块向上层报告的状态变化。 */
typedef enum {
    RADIO_EVENT_CONNECTING = 0,  // 正在连接（msg = 电台名）
    RADIO_EVENT_STREAMING,       // 已经连上、开始推流（msg = 电台名）
    RADIO_EVENT_TRACK,           // 曲名更新（msg = ICY 元数据里的标题）
    RADIO_EVENT_ERROR,           // 出错（msg = 错误描述）
    RADIO_EVENT_STOPPED,         // 已停止（msg = 空串）
} radio_event_t;

/** 事件回调函数原型：event = 事件类型，msg = 附带信息，ctx = 注册时传进来的上下文。 */
typedef void (*radio_event_cb_t)(radio_event_t event, const char *msg, void *ctx);

/* ===================== 生命周期与控制 ===================== */

/**
 * @brief 初始化模块：建立电台表、创建电台任务、初始化 I2S。
 *
 * @param stations  固件内置的默认电台表（会被复制走，调用后可以释放）
 * @param count     默认电台个数
 * @param cb        事件回调（可为 NULL）
 * @param ctx       回调上下文
 */
void radio_init(const radio_station_t *stations, int count,
                radio_event_cb_t cb, void *ctx);

/** @brief 切到第 index 个电台并开始播放。 */
void radio_play(int index);

/** @brief 下一个电台（到末尾绕回第一个）。 */
void radio_next(void);

/** @brief 上一个电台（到开头绕回最后一个）。 */
void radio_prev(void);

/** @brief 停止播放（不换台）。 */
void radio_stop(void);

/**
 * @brief 让电台立刻停下，并同步等它把堆上的音频缓冲释放掉（蓝牙接管时用）。
 *
 * 蓝牙栈在连接后会马上申请几 KB 内存（媒体线程 + SBC 解码器）。
 * 如果这时电台还占着 25KB 的堆缓冲，栈的申请就会失败
 * （串口里是 "BT_OSI: malloc failed"），结果是一个音频采样都收不到。
 * 所以这里先把电台停下，**等它真的把内存还回来**再让蓝牙继续。
 *
 * @param timeout_ms 最长等待时间（毫秒）
 * @return true = 缓冲已经释放；false = 超时（电台可能卡在网络读上）
 */
bool radio_stop_and_release(uint32_t timeout_ms);

/* ===================== 诊断功能 ===================== */

/**
 * @brief 用方波测试 I2S 通路（不联网、不解码），用来判断功放和接线是否正常。
 *
 * @param freq_hz      频率，50~8000 Hz
 * @param duration_ms  时长，100~10000 ms；传 0 表示一直响到 stop
 */
void radio_play_tone(int freq_hz, int duration_ms);

/**
 * @brief 用 GPIO 驱动功放的 SD 脚，检测模块死活（不用示波器）。
 *
 * @param mode  -1 = 悬空（模块默认，(L+R)/2 混合模式）
 *               0 = 拉低（功放关断，输出应掉到 ~0V）
 *               1 = 拉高（只放左声道，15dB 增益）
 * @return 成功返回 true；没配置这个功能（PIN_AMP_SD < 0）返回 false
 */
bool radio_set_amp_sd(int mode);

/* ===================== 电台表（存在 NVS 里） ===================== */

/**
 * @brief 运行时改电台名和/或地址（存 NVS）。
 *
 * @param index 电台下标
 * @param name  新名字，传 NULL 表示不改
 * @param url   新地址，传 NULL 表示不改
 * 如果改的是当前正在播的电台，会立即重连。
 */
void radio_set_station(int index, const char *name, const char *url);

/** @brief 电台表里的电台个数（随 add/del 变化）。 */
int radio_station_count(void);

/**
 * @brief 追加一个电台（存 NVS）。
 *
 * @param name 电台名，传 NULL 或空串则从 URL 末段自动取名
 * @param url  流地址
 * @return 新电台的下标；失败（地址非法 / 表已满）返回 -1
 */
int radio_add_station(const char *name, const char *url);

/**
 * @brief 删除一个电台，后面的自动前移（存 NVS）。
 *
 * @return 删除成功返回 true；下标越界返回 false
 */
bool radio_del_station(int index);

/* ===================== 播放列表（.m3u） ===================== */

/** @brief 最近一次解析出来的播放列表条目数（"pl" 命令用）。 */
int radio_playlist_count(void);

/** @brief 播放列表第 idx 条的标题（可能是 NULL）。 */
const char *radio_playlist_title(int idx);

/** @brief 播放列表第 idx 条的地址（可能是 NULL）。 */
const char *radio_playlist_url(int idx);

/* ===================== I2S 引脚映射 ===================== */

/** @brief 可用的引脚映射数量。 */
int  radio_i2s_pinmap_count(void);

/** @brief 当前正在用的映射编号。 */
int  radio_i2s_pinmap_current(void);

/** @brief 读出第 idx 套映射的三个引脚（BCK/WS/DATA）。 */
bool radio_i2s_pinmap_get(int idx, int *bck, int *ws, int *data);

/**
 * @brief 切换到第 idx 套引脚映射（会先停播，选择结果存进 NVS）。
 *
 * 存的是引脚本身而不是表序号，所以以后增删/重排映射表也不会让
 * 已保存的选择含义漂移 —— 任何一次烧录都不会再把能出声的功放改哑。
 */
bool radio_set_i2s_pinmap(int idx);

/* ===================== 与其它音源共用 I2S ===================== */

/**
 * @brief I2S 输出被蓝牙播放器占用了（true）或者释放了（false）。
 *
 * 蓝牙和电台共用同一个功放/同一个 I2S 通道，不能同时写。
 */
void radio_i2s_lock(bool locked);

/** @brief 当前 I2S 是否被蓝牙占着。 */
bool radio_i2s_locked(void);

/**
 * @brief 往 I2S 写原始 PCM（16 位立体声）。蓝牙播放器用。
 *
 * @param data        音频数据
 * @param bytes       字节数
 * @param timeout_ms  写超时（毫秒）
 * @return 写成功返回 true，出错返回 false
 */
bool audio_i2s_write(const void *data, size_t bytes, uint32_t timeout_ms);

/**
 * @brief 强制把 I2S 时钟设成指定采样率。
 *
 * 蓝牙流固定 44.1kHz；电台流则在每条流的第一帧按自己的采样率锁定。
 */
bool audio_i2s_set_rate(int rate);

/* ===================== 音量与响度 ===================== */

/**
 * @brief 设置音量（0~100），并写进 NVS（重启后仍然有效）。
 */
void radio_set_volume(int volume);

/**
 * @brief 自动响度增益开关。
 *
 * on （默认）：增益跟着流的短时 RMS 走，音量恒定、安静的台会被提起来；
 * off：完全不做自动增益 —— 电平保持录制时的样子（广播流本身 -12dBFS，
 *       会比较轻）。
 * 这个选择不存 NVS，重启后回到默认。
 */
void radio_set_normalizer(bool on);

/** @brief 自动响度增益当前是开还是关。 */
bool radio_normalizer_enabled(void);

/** @brief 当前音量（0~100）。 */
int  radio_get_volume(void);

/* ===================== 状态查询 ===================== */

/** @brief 当前电台下标（没有则 -1）。 */
int         radio_current_index(void);

/** @brief 当前曲名（ICY 元数据里的标题，没有则空串）。 */
const char *radio_current_track(void);

/** @brief 当前是否正在播放。 */
bool        radio_is_playing(void);

/** @brief 取第 index 个电台的描述；下标越界返回 NULL。 */
const radio_station_t *radio_get_station(int index);
