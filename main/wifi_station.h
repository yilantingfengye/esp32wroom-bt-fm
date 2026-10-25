/*
 * ============================================================================
 *  wifi_station.h —— WiFi 客户端（STA）接口
 * ============================================================================
 *
 *  职责：把 ESP32 接到家里的路由器上，并把 SSID/密码存在 NVS 里，
 *  这样断电重启后不用重新配网。
 *
 *  设计要点：
 *   - 只做 STA（客户端），不做 AP（热点）
 *   - 不阻塞等待 IP：连接过程异步完成，电台任务自己轮询 wifi_is_connected()
 *   - 凭据存 NVS（命名空间 "wifi"），和电台列表分开，互不干扰
 */

#pragma once  // 头文件只包含一次

#include <stdbool.h>   // bool 类型

#include "esp_err.h"   // esp_err_t / ESP_OK / ESP_ERR_NOT_FOUND

/**
 * @brief 用 NVS 里存的凭据启动 WiFi STA。
 *
 * 不会阻塞等待拿到 IP：连接是异步的，调用之后用 wifi_is_connected() 轮询。
 *
 * @return ESP_OK              已启动（凭据存在）
 * @return ESP_ERR_NOT_FOUND   NVS 里没有凭据（先用控制台 "wifi <ssid>|<pass>" 配置）
 */
esp_err_t wifi_init_sta(void);

/**
 * @brief 把 SSID/密码写进 NVS 并连接（第一次调用会顺带初始化 WiFi）。
 *
 * @param ssid  WiFi 名称
 * @param pass  WiFi 密码（空字符串表示开放网络）
 */
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);

/** @brief 清除 NVS 里存的凭据并断开连接。 */
void wifi_clear_credentials(void);

/** @brief 当前是否已经连上路由器并拿到 IP。 */
bool wifi_is_connected(void);

/** @brief 已保存的 SSID；没配置过时返回空字符串。 */
const char *wifi_get_ssid(void);

/** @brief 当前 IP 地址字符串；没连上时返回 "0.0.0.0"。 */
const char *wifi_get_ip(void);

/**
 * @brief 彻底关闭 WiFi，把 2.4GHz 射频完全让给蓝牙。
 *
 * 蓝牙 A2DP 播放期间调用（手机一连上就调）。WiFi+BT 共用射频，
 * WiFi 的任何流量都会通过共存仲裁扰乱蓝牙包的到达节奏，导致
 * 播放全程喀哒声。挂起期间不会自动重连。
 */
void wifi_suspend(void);

/** @brief 恢复 WiFi 并重新连接（手机断开蓝牙后调用）。 */
void wifi_resume(void);

/** @brief WiFi 当前是否处于挂起（射频让给蓝牙）状态。 */
bool wifi_is_suspended(void);
