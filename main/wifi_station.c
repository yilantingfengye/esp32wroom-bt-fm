/*
* ============================================================================
*  wifi_station.c —— WiFi 客户端（STA）
* ============================================================================
*
*  【职责】
*  连接家里的路由器，SSID/密码存 NVS（断电重启不用重新配网），
*  断线自动重连（带退避），并把连接状态给上层查询。
*
*  【踩过的坑】
*  1. 路由器上残留了本机的 STA 条目时，会一直报
*     "Association refused temporarily" —— 靠偶尔 stop/start 一下
*     把这个状态清掉（见 reconnect_timer_cb）。
*  2. WiFi 默认的 min-modem 省电模式在信标之间休眠，每个包都带
*     10ms+ 的延迟和抖动，连续流听起来就是一顿一顿的 —— 直接关掉。
*  3. esp_wifi_start() 必须在配置都设好之后再调用，否则上面那次
*     esp_wifi_connect() 会静默失败（ESP_ERR_WIFI_NOT_START）。
*/

#include <stdint.h>     // 整数类型
#include <string.h>     // 字符串函数

#include "freertos/FreeRTOS.h"        // FreeRTOS 内核
#include "freertos/event_groups.h"    // 事件组（连接状态位）

#include "esp_check.h"   // ESP_ERROR_CHECK
#include "esp_event.h"   // 事件循环
#include "esp_log.h"     // 日志
#include "esp_netif.h"   // 网络接口
#include "esp_timer.h"   // 定时器（重连退避）
#include "esp_wifi.h"    // WiFi 驱动
#include "nvs.h"         // NVS 键值 API
#include "nvs_flash.h"   // NVS 分区

#include "wifi_station.h"

static const char *TAG = "wifi";

/* ---- NVS 里存凭据的命名空间（和电台表分开） ---- */
#define NVS_NAMESPACE        "wifi_cfg"
/* ---- 事件组里的"已连接"标志位 ---- */
#define WIFI_CONNECTED_BIT   BIT0

static EventGroupHandle_t s_events;            // 事件组：连接状态
static esp_timer_handle_t s_reconnect_timer;   // 定时器：退避重连
static bool               s_inited;            // WiFi 是否已经初始化过
static volatile bool      s_connected;         // 当前是否连上
static int                s_disc_count;        // 连续断开次数（用于退避）
static int64_t            s_last_disc_log_us;  // 上次打印断开日志的时刻
static char               s_ssid[33];          // 当前 SSID
static char               s_ip[16] = "0.0.0.0"; // 当前 IP
static volatile bool      s_suspended;         /* 蓝牙播放期间 WiFi 被挂起（射频让给蓝牙） */

/**
* @brief 重连定时器的回调（跑在 esp_timer 任务里，不是事件回调上下文）。
*
* 这里可以安全地调用 esp_wifi_connect()。
*/
static void reconnect_timer_cb(void *arg)
{
/* 每次 AP 都回 "Association refused temporarily" 说明 AP 那边留着
* 一条过期的 STA 记录，deauth 能清掉它，esp_wifi_stop() 会发一个。
* 所以每几次尝试就"弹"一下 STA，跳出这个死循环。 */
    if (s_disc_count % 8 == 0) {
        ESP_LOGW(TAG, "bouncing wifi sta to clear stale ap state");
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_start(); /* STA_START 事件会触发 esp_wifi_connect() */
        return;
    }
    esp_wifi_disconnect(); /* 先丢掉本地残留的关联状态 */
    esp_wifi_connect();
}

/**
* @brief 安排一次重连。
*
* 头几次立刻重试（网络可能只是刚恢复），之后指数退避
* （1s、2s…最多 10s）—— 死命敲一个刚拒绝过我们的 AP 只会让自己被拉黑。
*/
static void schedule_reconnect(void)
{
    if (s_suspended) {
        return;              /* 蓝牙占着射频，挂起期间绝不重连 */
    }
    s_disc_count++;                                   // 断开次数加一
    if (s_disc_count <= 3 || !s_reconnect_timer) {     // 头几次立刻重试
        esp_wifi_disconnect();
        esp_wifi_connect();
        return;
    }
    int64_t delay_us = (int64_t)(s_disc_count - 2) * 1000000;   // 1s、2s、3s…
    if (delay_us > 10000000) {                        // 上限 10 秒
        delay_us = 10000000;
    }
    esp_timer_stop(s_reconnect_timer);                // 重新计时前先停掉旧的
    esp_timer_start_once(s_reconnect_timer, delay_us);
    ESP_LOGI(TAG, "reconnect attempt %d, backing off %d s", s_disc_count, (int)(delay_us / 1000000));
}

/**
* @brief WiFi / IP 事件的处理函数。
*
* @param arg        注册时传的参数（没用）
* @param event_base 事件来源（WIFI_EVENT 或 IP_EVENT）
* @param event_id   具体事件
* @param event_data 事件数据
*/
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();                           // STA 启动了：开始连
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;                          // 标记为未连接
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));      // IP 失效
/* 一定要打印断开原因：第一次连不上（密码错、找不到 AP）如果静默失败，根本没法排查。限流：前几次都打，之后最多 5 秒一条，免得疯狂重连把控制台刷爆。 */
        s_disc_count++;                               // 断开次数加一
        int64_t now = esp_timer_get_time();           // 当前时刻（微秒）
        if (s_disc_count <= 3 || now - s_last_disc_log_us >= 5000000) {
            s_last_disc_log_us = now;                 // 记住打印时刻
            const char *hint = "";                    // 给人看的提示
            if (disc->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                disc->reason == WIFI_REASON_AUTH_FAIL ||
                disc->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
                hint = " (wrong password?)";           // 认证失败 = 密码错了
            } else if (disc->reason == WIFI_REASON_NO_AP_FOUND) {
                hint = " (ap not found, esp32 supports 2.4GHz only)";
            } else if (disc->reason == WIFI_REASON_ASSOC_FAIL ||
                       disc->reason == WIFI_REASON_ASSOC_TOOMANY) {
                hint = " (ap refused association - check hotspot max connections / restart it)";
            }
            ESP_LOGW(TAG, "disconnected from \"%s\" (reason %d)%s", s_ssid, disc->reason, hint);
        }
        schedule_reconnect();                         // 安排重连
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));  // 记下 IP
        s_connected = true;                           // 标记为已连接
        s_disc_count = 0;                             // 断开计数清零
        ESP_LOGI(TAG, "connected to \"%s\", ip %s", s_ssid, s_ip);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);   // 置"已连接"标志
    }
}

/**
* @brief 把 SSID/密码写进 NVS。
*/
static esp_err_t nvs_store_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;                              // NVS 句柄
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;                                    // 打不开就报错
    }
    err = nvs_set_str(handle, "ssid", ssid);           // 写 SSID
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "pass", pass);       // 写密码
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);                      // 提交（否则不落盘）
    }
    nvs_close(handle);
    return err;
}

/**
* @brief 从 NVS 读出 SSID/密码。
*
* @return 两个都读到返回 true，没有凭据返回 false
*/
static bool nvs_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;                                  // 命名空间都打不开
    }
    size_t ssid_size = ssid_len;                      // 输入=缓冲区大小，输出=实际长度
    size_t pass_size = pass_len;
    esp_err_t err_ssid = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    esp_err_t err_pass = nvs_get_str(handle, "pass", pass, &pass_size);
    nvs_close(handle);
    return err_ssid == ESP_OK && err_pass == ESP_OK;
}

/**
* @brief 把凭据写进 WiFi 驱动并发起连接。
*/
static void wifi_apply_and_connect(const char *ssid, const char *pass)
{
    strlcpy(s_ssid, ssid, sizeof(s_ssid));            // 留一份给状态查询用

    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // 默认按 WPA2 处理
        },
    };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    if (strlen(pass) == 0) {                          // 空密码 = 开放网络
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGI(TAG, "connecting to \"%s\" with %s auth (pass len %d)", ssid,
             strlen(pass) == 0 ? "OPEN" : "WPA2", (int)strlen(pass));

    esp_wifi_disconnect();                            // 先断开旧的
    esp_wifi_set_config(WIFI_IF_STA, &cfg);           // 换配置
    esp_wifi_connect();                               // 再连
}

/**
* @brief 用 NVS 里的凭据启动 WiFi STA。
*
* 不阻塞等待 IP：连上与否由事件回调更新状态。
*/
esp_err_t wifi_init_sta(void)
{
    char ssid[33];                                   // SSID 缓冲区
    char pass[65];                                   // 密码缓冲区
    if (!nvs_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return ESP_ERR_NOT_FOUND;                    // 没配过网
    }

/* esp_netif_init() 和默认事件循环必须先建好（见 app_main）。 */
    s_events = xEventGroupCreate();                  // 连接状态事件组

    const esp_timer_create_args_t timer_args = {
        .callback = &reconnect_timer_cb,             // 重连定时器的回调
        .name = "wifi_reconnect",                    // 定时器名字（调试用）
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    esp_netif_create_default_wifi_sta();              // 创建默认 STA 网络接口

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));       // 初始化 WiFi 驱动

    esp_event_handler_instance_t instance_any_id;    // 事件处理实例
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));   // 只做客户端
    s_inited = true;
    wifi_apply_and_connect(ssid, pass);              // 写配置并连接

/* 必须在配置就位之后才启动 WiFi：STA_START 事件会触发事件处理里的 esp_wifi_connect()（在启动之前调 connect() 会静默失败，报 ESP_ERR_WIFI_NOT_START）。 */
    ESP_ERROR_CHECK(esp_wifi_start());

/* 关掉省电模式。默认的 min-modem 会在信标之间休眠，给每个包加 10ms+ 的延迟和抖动，连续流听起来就是一顿一顿的。板子走 USB 供电，省电在这里没有任何好处。 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);
    return ESP_OK;
}

/**
* @brief 保存新的凭据并（重新）连接。
*/
esp_err_t wifi_set_credentials(const char *ssid, const char *pass)
{
    size_t ssid_len = strlen(ssid);                  // SSID 长度
    size_t pass_len = strlen(pass);                  // 密码长度
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 64) {   // 长度检查
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_store_credentials(ssid, pass);   // 先写进 NVS
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to store credentials: %s", esp_err_to_name(err));
        return err;
    }

/* 只用于诊断 —— 绝不会打印密码本身。'I' 是 0x49，所以末尾如果混进了杂字符，从十六进制尾巴能看出来。 */
    size_t plen = strlen(pass);                      // 密码长度
    uint8_t b0 = plen >= 2 ? (uint8_t)pass[plen - 2] : 0;   // 倒数第二个字符
    uint8_t b1 = plen >= 1 ? (uint8_t)pass[plen - 1] : 0;   // 最后一个字符
    ESP_LOGI(TAG, "stored ssid=\"%s\" (%u chars), pass %u chars, tail hex %02x %02x",
             ssid, (unsigned)strlen(ssid), (unsigned)plen, b0, b1);

    if (s_inited) {                                  // WiFi 已经启动过：直接换配置重连
        wifi_apply_and_connect(ssid, pass);
        ESP_LOGI(TAG, "reconnecting to \"%s\" ...", ssid);
        return ESP_OK;
    }
    return wifi_init_sta();                          // 还没启动：走完整的初始化流程
}

/**
* @brief 清除 NVS 里存的凭据并断开连接。
*/
void wifi_clear_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "ssid");               // 删 SSID
        nvs_erase_key(handle, "pass");               // 删密码
        nvs_commit(handle);
        nvs_close(handle);
    }
    s_ssid[0] = '\0';                                // 内存里的副本也清掉
    if (s_inited) {
        esp_wifi_disconnect();                       // 断开当前连接
    }
    ESP_LOGI(TAG, "credentials cleared");
}

/**
* @brief 当前是否已经连上路由器并拿到 IP。
*/
bool wifi_is_connected(void)
{
    return s_connected;
}

/**
* @brief 已保存的 SSID；没配置过时返回空字符串。
*/
const char *wifi_get_ssid(void)
{
    return s_ssid;
}

/**
* @brief 当前 IP 地址字符串；没连上时返回 "0.0.0.0"。
*/
const char *wifi_get_ip(void)
{
    return s_ip;
}

/**
* @brief 把 WiFi 彻底关掉，2.4GHz 射频完全让给蓝牙（用户要求）。
*
* WiFi 和蓝牙共用同一条射频，WiFi 的重连尝试/信标流量会通过共存
* 仲裁把蓝牙 A2DP 数据包的到达节奏打乱（实测秒级 ±2% 波动），
* 接收端的环形缓冲兜不住就全程喀哒声。蓝牙一连上就调用本函数，
* 手机断开后用 wifi_resume() 恢复。顺带释放几十 KB 堆给蓝牙栈。
*/
void wifi_suspend(void)
{
    if (!s_inited || s_suspended) {
        return;                                  /* 还没初始化 / 已经挂起 */
    }
    s_suspended = true;
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);       /* 别再安排重连 */
    }
    s_connected = false;                         // 标记为未连接
    strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));      // IP 失效
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();                       /* 丢弃关联状态 */
    esp_wifi_stop();                             /* 射频完全关闭 */
    ESP_LOGW(TAG, "wifi suspended: bluetooth owns the radio now");
}

/**
* @brief 恢复 WiFi（手机断开蓝牙后调用）。
*
* esp_wifi_start() 会触发 STA_START 事件，事件处理器里会自动
* esp_wifi_connect()，所以这里不需要再显式连接。
*/
void wifi_resume(void)
{
    if (!s_inited || !s_suspended) {
        return;                                  /* 没挂起过，无事可做 */
    }
    s_suspended = false;
    s_disc_count = 0;                            // 退避计数清零
    ESP_LOGI(TAG, "wifi resumed, reconnecting ...");
    esp_wifi_start();                            /* STA_START 事件触发连接 */
}

/**
* @brief 当前 WiFi 是否处于挂起（射频让给蓝牙）状态。
*/
bool wifi_is_suspended(void)
{
    return s_suspended;
}
