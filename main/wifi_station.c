#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_station.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE        "wifi_cfg"
#define WIFI_CONNECTED_BIT   BIT0

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_reconnect_timer;
static bool               s_inited;
static volatile bool      s_connected;
static int                s_disc_count;
static int64_t            s_last_disc_log_us;
static char               s_ssid[33];
static char               s_ip[16] = "0.0.0.0";

/* Runs outside the event handler context - safe to call esp_wifi_connect(). */
static void reconnect_timer_cb(void *arg)
{
    /* "Association refused temporarily" on every ap means a stale sta entry
     * is left on the ap side. A deauth clears it, and esp_wifi_stop() sends
     * one. Bounce the sta every few attempts to get out of that loop. */
    if (s_disc_count % 8 == 0) {
        ESP_LOGW(TAG, "bouncing wifi sta to clear stale ap state");
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_start(); /* STA_START event triggers esp_wifi_connect() */
        return;
    }
    esp_wifi_disconnect(); /* drop any local association state first */
    esp_wifi_connect();
}

/* Retry immediately for the first few attempts, then back off exponentially
 * (1s, 2s, ... 10s). Hammering an ap that just refused association only keeps
 * us locked out. */
static void schedule_reconnect(void)
{
    s_disc_count++;
    if (s_disc_count <= 3 || !s_reconnect_timer) {
        esp_wifi_disconnect();
        esp_wifi_connect();
        return;
    }
    int64_t delay_us = (int64_t)(s_disc_count - 2) * 1000000;
    if (delay_us > 10000000) {
        delay_us = 10000000;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, delay_us);
    ESP_LOGI(TAG, "reconnect attempt %d, backing off %d s", s_disc_count, (int)(delay_us / 1000000));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        /* Always log the reason: silent failures on first connect (wrong
         * password, ap not found) are impossible to diagnose otherwise.
         * Rate limited: first few always, then at most one per 5 s so a
         * fast reconnect loop cannot flood the console. */
        s_disc_count++;
        int64_t now = esp_timer_get_time();
        if (s_disc_count <= 3 || now - s_last_disc_log_us >= 5000000) {
            s_last_disc_log_us = now;
            const char *hint = "";
            if (disc->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                disc->reason == WIFI_REASON_AUTH_FAIL ||
                disc->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
                hint = " (wrong password?)";
            } else if (disc->reason == WIFI_REASON_NO_AP_FOUND) {
                hint = " (ap not found, esp32 supports 2.4GHz only)";
            } else if (disc->reason == WIFI_REASON_ASSOC_FAIL ||
                       disc->reason == WIFI_REASON_ASSOC_TOOMANY) {
                hint = " (ap refused association - check hotspot max connections / restart it)";
            }
            ESP_LOGW(TAG, "disconnected from \"%s\" (reason %d)%s", s_ssid, disc->reason, hint);
        }
        schedule_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_disc_count = 0;
        ESP_LOGI(TAG, "connected to \"%s\", ip %s", s_ssid, s_ip);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t nvs_store_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool nvs_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;
    esp_err_t err_ssid = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    esp_err_t err_pass = nvs_get_str(handle, "pass", pass, &pass_size);
    nvs_close(handle);
    return err_ssid == ESP_OK && err_pass == ESP_OK;
}

static void wifi_apply_and_connect(const char *ssid, const char *pass)
{
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    if (strlen(pass) == 0) {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGI(TAG, "connecting to \"%s\" with %s auth (pass len %d)", ssid,
             strlen(pass) == 0 ? "OPEN" : "WPA2", (int)strlen(pass));

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}

esp_err_t wifi_init_sta(void)
{
    char ssid[33];
    char pass[65];
    if (!nvs_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return ESP_ERR_NOT_FOUND;
    }

    /* esp_netif_init() and the default event loop must be created before any
     * network activity - see app_main. */
    s_events = xEventGroupCreate();

    const esp_timer_create_args_t timer_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_inited = true;
    wifi_apply_and_connect(ssid, pass);

    /* Start wifi AFTER the config is in place. STA_START then triggers
     * esp_wifi_connect() from the event handler (the connect() call above
     * fails silently with ESP_ERR_WIFI_NOT_START before this point). */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Power save off. The default min-modem mode sleeps between beacons,
     * which adds 10+ ms of latency and jitter to every packet - audible as
     * stuttering on a continuous stream. The board runs on USB power, so
     * saving power buys nothing here. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);
    return ESP_OK;
}

esp_err_t wifi_set_credentials(const char *ssid, const char *pass)
{
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_store_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to store credentials: %s", esp_err_to_name(err));
        return err;
    }

    /* Diagnostics only - never prints the password itself. 'I' is 0x49, so a
     * stray character at the end is visible in the hex tail. */
    size_t plen = strlen(pass);
    uint8_t b0 = plen >= 2 ? (uint8_t)pass[plen - 2] : 0;
    uint8_t b1 = plen >= 1 ? (uint8_t)pass[plen - 1] : 0;
    ESP_LOGI(TAG, "stored ssid=\"%s\" (%u chars), pass %u chars, tail hex %02x %02x",
             ssid, (unsigned)strlen(ssid), (unsigned)plen, b0, b1);

    if (s_inited) {
        wifi_apply_and_connect(ssid, pass);
        ESP_LOGI(TAG, "reconnecting to \"%s\" ...", ssid);
        return ESP_OK;
    }
    return wifi_init_sta();
}

void wifi_clear_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "ssid");
        nvs_erase_key(handle, "pass");
        nvs_commit(handle);
        nvs_close(handle);
    }
    s_ssid[0] = '\0';
    if (s_inited) {
        esp_wifi_disconnect();
    }
    ESP_LOGI(TAG, "credentials cleared");
}

bool wifi_is_connected(void)
{
    return s_connected;
}

const char *wifi_get_ssid(void)
{
    return s_ssid;
}

const char *wifi_get_ip(void)
{
    return s_ip;
}
