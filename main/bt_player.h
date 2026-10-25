/*
 * ============================================================================
 *  bt_player.h —— 蓝牙 A2DP 接收（手机 → ESP32 → MAX98357A）
 * ============================================================================
 *
 *  【音频通路】
 *    手机 --BT/SBC--> Bluedroid 协议栈 --SBC 解码--> 16bit PCM
 *         --环形缓冲--> 本模块的音频任务 --I2S--> 功放
 *
 *  【为什么不用自己写 SBC 解码器】
 *  Bluedroid 协议栈内部自带 Open Interface 的 SBC 解码器
 *  （CONFIG_BT_A2DP_USE_EXTERNAL_CODEC 保持关闭就会用它），
 *  应用层拿到的直接就是 PCM。
 *
 *  【和电台的关系】
 *  蓝牙和电台共用同一个 I2S 通道和同一个功放，不能同时出声：
 *  手机连上 -> 电台让位；手机断开 -> 电台自动恢复。
 */

#pragma once  // 头文件只包含一次

#include <stdbool.h>   // bool 类型
#include <stdint.h>   // uint32_t

#include "esp_err.h"   // esp_err_t

#ifdef __cplusplus
extern "C" {          // 让 C++ 也能包含这个头文件
#endif

/* ===================== 状态 ===================== */

/** 蓝牙音频接收器的状态。 */
typedef enum {
    BT_STATE_DISABLED = 0, /**< 固件没编译蓝牙功能（看 bt_player_available） */
    BT_STATE_IDLE,         /**< 协议栈已启动，但没有手机连上 */
    BT_STATE_CONNECTED,    /**< 手机已连接，还没开始出声 */
    BT_STATE_PLAYING,      /**< 正在往功放里送音频 */
} bt_player_state_t;

/* ===================== 接口 ===================== */

/**
 * @brief 启动蓝牙控制器、Bluedroid 协议栈和 A2DP sink。
 *
 * 必须在 NVS 初始化之后调用（蓝牙要用 NVS 存配对信息）。
 * 固件没编译蓝牙功能时返回 ESP_ERR_NOT_SUPPORTED，
 * 用 bt_player_available() 可以提前判断。
 */
esp_err_t bt_player_init(void);

/** @brief 固件是否带蓝牙功能。 */
bool bt_player_available(void);

/** @brief 当前状态。 */
bt_player_state_t bt_player_state(void);

/** @brief 当前状态的文字说明（"idle" / "connected" / "playing" ...）。 */
const char *bt_player_state_str(void);

/** @brief 已连接设备的蓝牙地址；没有则返回 "-"。 */
const char *bt_player_peer_addr(void);

/** @brief 已连接设备的名称；不知道则返回 "-"。 */
const char *bt_player_peer_name(void);

/** @brief 收到的音频流的采样率（正常是 44100）。 */
int bt_player_sample_rate(void);

/**
 * @brief 环形缓冲溢出、不得不丢掉的音频块数。
 *
 * 一直为 0 说明 I2S 跟得上；数值在涨说明 CPU 或 I2S 写入跟不上。
 */
uint32_t bt_player_overruns(void);

/**
 * @brief 蓝牙音频数据回调被调用了几次（诊断用）。
 *
 * 一直是 0 = 协议栈根本没把解码后的 PCM 交过来。
 */
uint32_t bt_player_data_cbs(void);

/**
 * @brief 一共从协议栈收到了多少字节的 PCM（诊断用）。
 *
 * 有数据但没声音 = 数据进来了、问题在 I2S 那一侧。
 */
uint32_t bt_player_bytes_in(void);

/**
 * @brief 一共往 I2S 写了多少个采样（诊断用）。
 *
 * 和 bt_player_bytes_in() 对比就能看出是不是写不出去。
 */
uint32_t bt_player_samples_out(void);

/** @brief 主动断开已连接的手机（手机可能自己再连回来）。 */
bool bt_player_disconnect(void);

/**
 * @brief 开启蓝牙（像手机 App 的"打开"）。
 *
 * 没启动/被释放过 → 完整冷启动；close 过 → 秒级热启动；已开 → 无操作。
 * 必须从普通任务调用（控制台/电台任务），不要在蓝牙回调里调。
 */
bool bt_player_open(void);

/**
 * @brief 关闭蓝牙（像手机 App 的"停止"）：断开手机 + 关控制器。
 *
 * 协议栈和内存都保留，bt_player_open() 秒级重开。电台开播时自动调用
 * （射频完全让给电台）。
 */
bool bt_player_close(void);

/**
 * @brief 释放蓝牙（像手机 App 的"卸载"）：整个协议栈拆除、内存归还堆。
 *
 * 之后 bt_player_open() 会完整冷启动重建。
 */
bool bt_player_release(void);

/** @brief 电源状态字符串："on" / "closed" / "released"。 */
const char *bt_player_power_str(void);

#ifdef __cplusplus
}
#endif
