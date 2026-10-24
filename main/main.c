#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "radio_player.h"
#include "wifi_station.h"
#include "sdkconfig.h"

static const char *TAG = "app";

#define STATION_COUNT 4

static const radio_station_t s_stations[STATION_COUNT] = {
    { CONFIG_RADIO_STATION0_NAME, CONFIG_RADIO_STATION0_URL },
    { CONFIG_RADIO_STATION1_NAME, CONFIG_RADIO_STATION1_URL },
    { CONFIG_RADIO_STATION2_NAME, CONFIG_RADIO_STATION2_URL },
    { CONFIG_RADIO_STATION3_NAME, CONFIG_RADIO_STATION3_URL },
};

static void radio_event_cb(radio_event_t event, const char *msg, void *ctx)
{
    switch (event) {
    case RADIO_EVENT_CONNECTING: ESP_LOGI(TAG, "connecting: %s", msg); break;
    case RADIO_EVENT_STREAMING:  ESP_LOGI(TAG, "streaming: %s", msg);  break;
    case RADIO_EVENT_TRACK:      ESP_LOGI(TAG, ">> %s", msg);         break;
    case RADIO_EVENT_ERROR:
        ESP_LOGW(TAG, "%s (wifi: %s, ip: %s)", msg,
                 wifi_is_connected() ? "connected" : "down", wifi_get_ip());
        break;
    case RADIO_EVENT_STOPPED:    ESP_LOGI(TAG, "stopped");            break;
    }
}

static void print_help(void)
{
    printf("\n==================== commands ====================\n"
           "  help                 show this help\n"
           "  list                 list all stations\n"
           "  play <0-%d>            play station by index\n"
           "  next | prev          next / previous station\n"
           "  stop                 stop playback\n"
           "  tone [f] [ms]        test tone via i2s (1000Hz 2s; ms=0 = continuous)\n"
           "  sd <0|1|z>          amp SD pin: 0=shutdown 1=left ch/15dB z=float\n"
           "  pins                show the i2s pin maps (current one marked)\n"
           "  pins <n>            switch i2s pins to map n (saved to nvs)\n"
           "  vol <0-100>          set volume ('vol' shows current)\n"
           "  add <url> [name]     add a station (saved to nvs)\n"
           "  del <n>              delete station n (saved to nvs)\n"
           "  url <n> <url>        change station n stream url (saved)\n"
           "  name <n> <name>      rename station n (saved)\n"
           "  wifi                 show wifi status\n"
           "  wifi <ssid>|<pass>   set wifi and connect (| allows spaces,\n"
           "                       pass can be empty for open ap)\n"
           "  wifi clear           erase stored wifi credentials\n"
           "  track                show current track title\n"
           "  status               show radio and wifi status\n"
           "==================================================\n",
           radio_station_count() - 1);
}

static void trim_str(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void cmd_wifi(const char *arg)
{
    if (strcmp(arg, "clear") == 0) {
        wifi_clear_credentials();
        printf("wifi credentials erased\n");
        return;
    }

    if (arg[0] == '\0' || strcmp(arg, "status") == 0) {
        printf("ssid : %s\n", wifi_get_ssid()[0] ? wifi_get_ssid() : "(not set)");
        printf("state: %s\n", wifi_is_connected() ? "connected" : "disconnected");
        printf("ip   : %s\n", wifi_get_ip());
        if (wifi_get_ssid()[0] == '\0') {
            printf("set with: wifi <ssid>|<password>\n");
        }
        return;
    }

    /* SSIDs and passwords may contain spaces: split on '|'.
     * Fall back to whitespace splitting when no '|' is given. */
    char ssid[33];
    char pass[65];
    pass[0] = '\0';
    const char *sep = strchr(arg, '|');
    if (sep) {
        size_t n = (size_t)(sep - arg);
        if (n >= sizeof(ssid)) {
            n = sizeof(ssid) - 1;
        }
        memcpy(ssid, arg, n);
        ssid[n] = '\0';
        strlcpy(pass, sep + 1, sizeof(pass));
    } else if (sscanf(arg, "%32s %64s", ssid, pass) < 1) {
        printf("usage: wifi <ssid>|<password>  (| allows spaces)\n");
        return;
    }
    trim_str(ssid);
    trim_str(pass);

    if (ssid[0] == '\0') {
        printf("usage: wifi <ssid>|<password>  (| allows spaces)\n");
        return;
    }

    esp_err_t err = wifi_set_credentials(ssid, pass);
    if (err == ESP_OK) {
        printf("connecting to \"%s\" ...\n", ssid);
        radio_play(0);
    } else {
        printf("failed: %s\n", esp_err_to_name(err));
    }
}

static void print_status(void)
{
    int idx = radio_current_index();
    const radio_station_t *st = idx >= 0 ? radio_get_station(idx) : NULL;
    printf("\nstation : [%d/%d] %s\n", idx + 1, radio_station_count(),
           st ? st->name : "-");
    printf("url     : %s\n", st ? st->url : "-");
    printf("volume  : %d%%\n", radio_get_volume());
    printf("track   : %s\n", radio_current_track()[0] ? radio_current_track() : "-");
    printf("state   : %s\n", radio_is_playing() ? "playing" : "stopped");
    printf("wifi    : %s (%s, ip %s)\n",
           wifi_get_ssid()[0] ? wifi_get_ssid() : "(not set)",
           wifi_is_connected() ? "connected" : "down", wifi_get_ip());
}

static void process_command(const char *line)
{
    int  n;
    char url[160];
    char name[48];

    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        print_help();
    } else if (strcmp(line, "list") == 0) {
        printf("\nstations:\n");
        for (int i = 0; i < radio_station_count(); i++) {
            const radio_station_t *st = radio_get_station(i);
            printf("  [%d] %-22s %s\n", i, st->name, st->url);
        }
    } else if (sscanf(line, "url %d %159[^\n]", &n, url) == 2) {
        if (n < 0 || n >= radio_station_count()) {
            printf("station out of range (0-%d)\n", radio_station_count() - 1);
        } else {
            radio_set_station(n, NULL, url);
            printf("station %d url updated\n", n);
        }
    } else if (sscanf(line, "name %d %47[^\n]", &n, name) == 2) {
        if (n < 0 || n >= radio_station_count()) {
            printf("station out of range (0-%d)\n", radio_station_count() - 1);
        } else {
            trim_str(name);
            radio_set_station(n, name, NULL);
            printf("station %d renamed\n", n);
        }
    } else if (strncmp(line, "add ", 4) == 0) {
        char u[160] = "";
        char nm[48] = "";
        int fields = sscanf(line + 4, "%159s %47[^\n]", u, nm);
        trim_str(nm);
        if (fields < 1 || u[0] == '\0') {
            printf("usage: add <url> [name]\n");
        } else {
            int idx = radio_add_station(nm, u);
            if (idx < 0) {
                printf("add failed (bad url or list full)\n");
            } else {
                const radio_station_t *st = radio_get_station(idx);
                printf("station %d added: %s\n", idx, st ? st->name : "?");
            }
        }
    } else if (sscanf(line, "del %d", &n) == 1) {
        if (radio_del_station(n)) {
            printf("station %d deleted (%d left)\n", n, radio_station_count());
        } else {
            printf("delete failed (index out of range 0-%d)\n", radio_station_count() - 1);
        }
    } else if (sscanf(line, "play %d", &n) == 1) {
        if (n < 0 || n >= radio_station_count()) {
            printf("station out of range (0-%d)\n", radio_station_count() - 1);
        } else {
            radio_play(n);
        }
    } else if (strcmp(line, "next") == 0) {
        radio_next();
    } else if (strcmp(line, "prev") == 0) {
        radio_prev();
    } else if (strcmp(line, "stop") == 0) {
        radio_stop();
    } else if (sscanf(line, "vol %d", &n) == 1) {
        radio_set_volume(n);
        printf("volume: %d%%\n", radio_get_volume());
    } else if (strcmp(line, "vol") == 0) {
        printf("volume: %d%%\n", radio_get_volume());
    } else if (strcmp(line, "wifi") == 0) {
        cmd_wifi("");
    } else if (strncmp(line, "wifi ", 5) == 0) {
        cmd_wifi(line + 5);
    } else if (strncmp(line, "tone", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        int f = 1000, d = 2000;
        sscanf(line, "tone %d %d", &f, &d);
        radio_play_tone(f, d);
        if (d == 0) {
            printf("tone: %d Hz, continuous (stop to end)\n", f);
        } else {
            printf("tone: %d Hz, %d ms (stops current playback)\n", f, d);
        }
    } else if (strncmp(line, "sd", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
        char arg[4] = "z";
        sscanf(line, "sd %3s", arg);
        int mode = -1; /* float: let the module's own pull-up decide */
        if (arg[0] == '0') {
            mode = 0;
        } else if (arg[0] == '1') {
            mode = 1;
        }
        radio_set_amp_sd(mode);
    } else if (strcmp(line, "pins") == 0) {
        printf("\ni2s pin maps (current: %d):\n", radio_i2s_pinmap_current());
        for (int i = 0; i < radio_i2s_pinmap_count(); i++) {
            int bck = 0, ws = 0, data = 0;
            radio_i2s_pinmap_get(i, &bck, &ws, &data);
            printf("  [%d] %s bck=%d ws=%d din=%d\n", i,
                   i == radio_i2s_pinmap_current() ? "*" : " ", bck, ws, data);
        }
    } else if (sscanf(line, "pins %d", &n) == 1) {
        if (radio_set_i2s_pinmap(n)) {
            int bck = 0, ws = 0, data = 0;
            radio_i2s_pinmap_get(n, &bck, &ws, &data);
            printf("i2s pins -> map %d (bck=%d ws=%d din=%d), saved in nvs\n",
                   n, bck, ws, data);
        } else {
            printf("pin map %d not available\n", n);
        }
    } else if (strcmp(line, "track") == 0) {
        const char *t = radio_current_track();
        printf("track: %s\n", t[0] ? t : "-");
    } else if (strcmp(line, "status") == 0) {
        print_status();
    } else if (line[0] != '\0') {
        printf("unknown command, type 'help'\n");
    }
}

static void console_task(void *arg)
{
    char line[192];
    int  len = 0;
    uint8_t ch;

    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) <= 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            printf("\n"); /* leave the echoed command line so log output
                             never glues to it (e.g. "...12345678I (123)") */
            if (len > 0) {
                process_command(line);
            }
            len = 0;
            printf("> ");
            fflush(stdout);
        } else if (ch == '\b' || ch == 0x7F) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout); /* stdout is line buffered: flush the echo */
            }
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)ch;
            printf("%c", ch);
            fflush(stdout); /* stdout is line buffered: without this the echo
                               sits in the buffer and never shows up */
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    setvbuf(stdout, NULL, _IOLBF, 0);

    /* The lwIP stack and the default event loop must exist before any
     * network activity (http client, wifi). */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "ESP32 internet radio starting");

    radio_init(s_stations, STATION_COUNT, radio_event_cb, NULL);

    uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
    xTaskCreate(console_task, "console", 4096, NULL, 4, NULL);
    print_help();

    /* Credentials are configured via console: "wifi <ssid> <password>".
     * Playback only starts once wifi is up (or right after configuring it),
     * otherwise the radio task would hammer the network stack in vain. */
    esp_err_t err = wifi_init_sta();
    if (err == ESP_ERR_NOT_FOUND) {
        printf("\nwifi not configured. set it with: wifi <ssid> <password>\n");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
    } else {
        radio_play(0);
    }
}
