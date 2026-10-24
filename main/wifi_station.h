#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Start WiFi STA using credentials stored in NVS.
 *
 * @return ESP_OK when started, ESP_ERR_NOT_FOUND when no credentials are
 *         stored yet (configure with wifi_set_credentials via console).
 *         Never blocks waiting for an IP.
 */
esp_err_t wifi_init_sta(void);

/**
 * @brief Store SSID/password in NVS and connect (initializes WiFi on first use).
 */
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);

/** @brief Erase stored credentials and disconnect. */
void wifi_clear_credentials(void);

bool        wifi_is_connected(void);
const char *wifi_get_ssid(void);   /**< stored SSID, "" when not configured */
const char *wifi_get_ip(void);     /**< "0.0.0.0" when not connected */
