/*
* ============================================================================
*  main.c —— 程序入口 + 串口控制台
* ============================================================================
*
*  【这个文件负责什么】
*  1. 启动顺序：NVS -> lwIP -> 事件循环 -> 电台模块 -> 蓝牙 -> 串口控制台
*  2. 串口控制台：接收一行命令，解析后调用 radio_player / wifi_station /
*     bt_player 的接口
*
*  【线程】
*  - app_main（主任务）：做完初始化就退出
*  - console（控制台任务）：读串口、解析命令
*  - radio（电台任务，在 radio_player.c 里）：收流、解码、写 I2S
*  - bt_audio（蓝牙音频任务，在 bt_player.c 里）：把手机来的 PCM 写进 I2S
*
*  【编码】
*  本文件和工程里其它源码一样是 UTF-8（无 BOM）。
*  控制台输出用英文，这样在任何代码页的终端下都不会乱码；
*  电台发来的曲名是 GBK，原样打印（见 README 的"编码格式"一节）。
*/

#include <stdio.h>      // printf / sscanf
#include <string.h>     // strcmp / memcpy / strlen

#include "freertos/FreeRTOS.h"   // FreeRTOS 内核
#include "freertos/task.h"       // xTaskCreate / vTaskDelay

#include "esp_err.h"     // esp_err_t
#include "esp_event.h"   // 事件循环
#include "esp_log.h"     // ESP_LOGx
#include "esp_system.h"  // esp_get_free_heap_size（bt free 前后对比堆）
#include "esp_netif.h"   // lwIP 栈
#include "nvs_flash.h"   // NVS 键值存储
#include "driver/uart.h" // 串口驱动（UART0 = 串口控制台）

#include "radio_player.h" // 电台模块
#include "wifi_station.h" // WiFi 模块
#include "bt_player.h"    // 蓝牙模块
#include "sdkconfig.h"    // 编译期配置（Kconfig 里的值）

static const char *TAG = "app";   // 日志前缀

/* ---- 固件内置的默认电台个数（真正的电台表在 NVS 里，可以运行时增删） ---- */
#define STATION_COUNT 4

/* ---- 默认电台表（来自 Kconfig，用户可以在运行时用 url/name/add/del 改） ---- */
static const radio_station_t s_stations[STATION_COUNT] = {
    { CONFIG_RADIO_STATION0_NAME, CONFIG_RADIO_STATION0_URL },   // 电台 0
    { CONFIG_RADIO_STATION1_NAME, CONFIG_RADIO_STATION1_URL },   // 电台 1
    { CONFIG_RADIO_STATION2_NAME, CONFIG_RADIO_STATION2_URL },   // 电台 2
    { CONFIG_RADIO_STATION3_NAME, CONFIG_RADIO_STATION3_URL },   // 电台 3
};

/**
* @brief 电台模块的事件回调：把内部状态变化打印到日志里。
*
* @param event 事件类型（连接中/开始推流/曲名/出错/停止）
* @param msg   附带信息（电台名、错误描述…）
* @param ctx   注册时传进来的上下文（本工程没用）
*/
static void radio_event_cb(radio_event_t event, const char *msg, void *ctx)
{
    switch (event) {
    case RADIO_EVENT_CONNECTING:                       // 正在连接电台
        ESP_LOGI(TAG, "connecting: %s", msg);
        break;
    case RADIO_EVENT_STREAMING:                        // 已经连上、开始推流
        ESP_LOGI(TAG, "streaming: %s", msg);
        break;
    case RADIO_EVENT_TRACK:                            // 曲名更新（ICY 元数据）
        ESP_LOGI(TAG, ">> %s", msg);
        break;
    case RADIO_EVENT_ERROR:                            // 出错了
        ESP_LOGW(TAG, "%s (wifi: %s, ip: %s)", msg,
                 wifi_is_connected() ? "connected" : "down", wifi_get_ip());
        break;
    case RADIO_EVENT_STOPPED:                          // 停止播放
        ESP_LOGI(TAG, "stopped");
        break;
    }
}

/**
* @brief 打印命令帮助。
*
* 电台下标范围是动态的（电台表可以增删），所以用 radio_station_count() 现算。
*/
static void print_help(void)
{
    printf("\n==================== commands ====================\n"
           "  firmware v%s\n"
           "  help                 show this help\n"
           "  list                 list all stations\n"
           "  pl                   show the playlist of the current station\n"
           "  play <0-%d>            play station by index\n"
           "  next | prev          next / previous station\n"
           "  stop                 stop playback\n"
           "  tone [f] [ms]        test tone via i2s (1000Hz 2s; ms=0 = continuous)\n"
           "  sd <0|1|z>          amp SD pin: 0=shutdown 1=left ch/15dB z=float\n"
           "  pins                show the i2s pin maps (current one marked)\n"
           "  pins <n>            switch i2s pins to map n (saved to nvs)\n"
           "  bt                  bluetooth status (a2dp sink)\n"
           "  bt open|mode        open bt: stop radio, start stack (wait for phone)\n"
           "  bt close|off        close bt: disconnect + controller off (stack kept)\n"
           "  bt free             release bt: unload stack, free all bt memory\n"
           "  bt disc             disconnect the bluetooth device\n"
           "                      (play <n> auto-closes bt; stop auto-opens it)\n"
           "  vol <0-100>          set volume ('vol' shows current; not saved\n"
           "                       unless RADIO_VOLUME_PERSIST=y)\n"
           "  norm <0|1>          auto loudness gain: 1=on (default) 0=raw level\n"
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
           FW_VERSION, radio_station_count() - 1);
}

/**
* @brief 去掉字符串首尾的空格和制表符。
*
* 控制台是按行输入的，用户很容易在参数前后带上空格。
*/
static void trim_str(char *s)
{
    char *p = s;                       // p 指向第一个非空白字符
    while (*p == ' ' || *p == '\t') {  // 跳过开头的空白
        p++;
    }
    if (p != s) {                      // 前面有空白：把内容整体前移
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);              // 当前长度
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {  // 删掉结尾的空白
        s[--n] = '\0';
    }
}

/**
* @brief 处理 "wifi" 命令的三种用法。
*
*   wifi            -> 显示状态
*   wifi clear      -> 清除已存凭据
*   wifi <ssid>|<pass> -> 设置并连接（"|" 允许 SSID/密码里有空格）
*
* @param arg 命令后面的参数（"wifi" 本身已经去掉了）
*/
static void cmd_wifi(const char *arg)
{
    if (strcmp(arg, "clear") == 0) {              // wifi clear
        wifi_clear_credentials();
        printf("wifi credentials erased\n");
        return;
    }

    if (arg[0] == '\0' || strcmp(arg, "status") == 0) {   // wifi / wifi status
        printf("ssid : %s\n", wifi_get_ssid()[0] ? wifi_get_ssid() : "(not set)");
        printf("state: %s\n", wifi_is_connected() ? "connected" : "disconnected");
        printf("ip   : %s\n", wifi_get_ip());
        if (wifi_get_ssid()[0] == '\0') {         // 还没配过网
            printf("set with: wifi <ssid>|<password>\n");
        }
        return;
    }

/* SSID 和密码都可能带空格，所以优先按 '|' 切分；
* 没给 '|' 时退回按空白切分。 */
    char ssid[33];                // SSID 最多 32 个字符
    char pass[65];                // 密码最多 64 个字符
    pass[0] = '\0';
    const char *sep = strchr(arg, '|');           // 找分隔符
    if (sep) {
        size_t n = (size_t)(sep - arg);           // '|' 前面的长度
        if (n >= sizeof(ssid)) {                  // 太长就截断
            n = sizeof(ssid) - 1;
        }
        memcpy(ssid, arg, n);                     // 复制 SSID
        ssid[n] = '\0';
        strlcpy(pass, sep + 1, sizeof(pass));     // '|' 后面是密码
    } else if (sscanf(arg, "%32s %64s", ssid, pass) < 1) {   // 没给 '|'
        printf("usage: wifi <ssid>|<password>  (| allows spaces)\n");
        return;
    }
    trim_str(ssid);                               // 去掉首尾空格
    trim_str(pass);

    if (ssid[0] == '\0') {                        // SSID 不能为空
        printf("usage: wifi <ssid>|<password>  (| allows spaces)\n");
        return;
    }

    esp_err_t err = wifi_set_credentials(ssid, pass);   // 存 NVS 并连接
    if (err == ESP_OK) {
        printf("connecting to \"%s\" ...\n", ssid);
        /* 【用户要求】配完 WiFi 不自动进电台，想听台用 play <n> */
    } else {
        printf("failed: %s\n", esp_err_to_name(err));
    }
}

/**
* @brief 判断一个字符串是不是 http(s) 地址。
*/
static bool looks_like_url(const char *s)
{
    return s && (strncasecmp(s, "http://", 7) == 0 || strncasecmp(s, "https://", 8) == 0);
}

/**
* @brief 打印当前状态总览（status 命令）。
*/
static void print_status(void)
{
    int idx = radio_current_index();                              // 当前电台下标
    const radio_station_t *st = idx >= 0 ? radio_get_station(idx) : NULL;
    printf("\nstation : [%d/%d] %s\n", idx + 1, radio_station_count(),
           st ? st->name : "-");                                  // 电台名
    printf("url     : %s\n", st ? st->url : "-");                 // 流地址
    printf("volume  : %d%%\n", radio_get_volume());              // 音量
    printf("track   : %s\n", radio_current_track()[0] ? radio_current_track() : "-");
    printf("state   : %s\n", radio_is_playing() ? "playing" : "stopped");
    printf("wifi    : %s (%s, ip %s)\n",
           wifi_get_ssid()[0] ? wifi_get_ssid() : "(not set)",
           wifi_is_connected() ? "connected" : "down", wifi_get_ip());
    printf("firmware: %s (build %s %s)\n", FW_VERSION, __DATE__, __TIME__);
}

/**
* @brief 解析并执行一条控制台命令。
*
* @param line 已经去掉换行符的命令字符串
*/
static void process_command(const char *line)
{
    int  n;              // 解析出来的整数参数
    char url[160];       // 解析出来的 URL
    char name[48];       // 解析出来的名字

    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        print_help();
    } else if (strcmp(line, "list") == 0) {                      // 列出所有电台
        printf("\nstations:\n");
        for (int i = 0; i < radio_station_count(); i++) {
            const radio_station_t *st = radio_get_station(i);
            printf("  [%d] %-22s %s\n", i, st->name, st->url);
        }
    } else if (strcmp(line, "pl") == 0) {                       // 列出当前歌单
        int cnt = radio_playlist_count();
        if (cnt == 0) {
            printf("no playlist loaded (point a station url to a .m3u file)\n");
        } else {
            printf("\nplaylist (%d entries):\n", cnt);
            for (int i = 0; i < cnt; i++) {
                const char *t = radio_playlist_title(i);
                const char *u = radio_playlist_url(i);
                printf("  [%d] %-28s %s\n", i,
                       (t && t[0]) ? t : "-", u ? u : "-");
            }
        }
    } else if (sscanf(line, "url %d %159[^\n]", &n, url) == 2) {   // url <n> <地址>
        if (n < 0 || n >= radio_station_count()) {
            printf("station out of range (0-%d)\n", radio_station_count() - 1);
        } else if (!looks_like_url(url)) {
/* 少了 http:// 前缀的话，HTTP 客户端会直接报
* "Error parse url"（它按 scheme 解析地址），这里提前拦下来。 */
            printf("not a http(s) url: %s\n", url);
            printf("usage: url <n> <http://...>\n");
        } else {
            radio_set_station(n, NULL, url);     // 只改地址，名字不动
            printf("station %d url updated\n", n);
        }
    } else if (sscanf(line, "name %d %47[^\n]", &n, name) == 2) { // name <n> <名字>
        if (n < 0 || n >= radio_station_count()) {
            printf("station out of range (0-%d)\n", radio_station_count() - 1);
        } else {
            trim_str(name);
            radio_set_station(n, name, NULL);    // 只改名字，地址不动
            printf("station %d renamed\n", n);
        }
    } else if (strncmp(line, "add ", 4) == 0) {                 // add <url> [名字]
        char u[160] = "";                       // 地址
        char nm[48] = "";                       // 名字（可省）
        int fields = sscanf(line + 4, "%159s %47[^\n]", u, nm);
        trim_str(nm);
/* 参数顺序容错：写反了（add <名字> <地址>）就自动交换。
* 不然的话地址会变成 "url" 这种字符串，报错信息还很难懂。 */
        if (fields == 2 && !looks_like_url(u) && looks_like_url(nm)) {
            char tmp[160];
            strlcpy(tmp, u, sizeof(tmp));
            strlcpy(u, nm, sizeof(u));
            strlcpy(nm, tmp, sizeof(nm));
            printf("note: swapped the arguments (address first, name second)\n");
        }
        if (fields < 1 || u[0] == '\0') {        // 至少要有个地址
            printf("usage: add <url> [name]\n");
        } else if (!looks_like_url(u)) {
            printf("not a http(s) url: %s\n", u);
            printf("usage: add <http://...> [name]\n");
        } else {
            int idx = radio_add_station(nm, u); // 加进电台表并存 NVS
            if (idx < 0) {
                printf("add failed (bad url or list full)\n");
            } else {
                const radio_station_t *st = radio_get_station(idx);
                printf("station %d added: %s\n", idx, st ? st->name : "?");
            }
        }
    } else if (sscanf(line, "del %d", &n) == 1) {               // del <n>
        if (radio_del_station(n)) {
            printf("station %d deleted (%d left)\n", n, radio_station_count());
        } else {
            printf("delete failed (index out of range 0-%d)\n", radio_station_count() - 1);
        }
    } else if (sscanf(line, "play %d", &n) == 1) {              // play <n>
        if (n < 0 || n >= radio_station_count()) {
            printf("station out of range (0-%d)\n", radio_station_count() - 1);
        } else {
            radio_play(n);
        }
    } else if (strcmp(line, "next") == 0) {                    // 下一个电台
        radio_next();
    } else if (strcmp(line, "prev") == 0) {                    // 上一个电台
        radio_prev();
    } else if (strcmp(line, "stop") == 0) {                    // 停止播放
        radio_stop();
    } else if (sscanf(line, "vol %d", &n) == 1) {              // vol <n>
        radio_set_volume(n);
        printf("volume: %d%% (saved)\n", radio_get_volume());
    } else if (strcmp(line, "vol") == 0) {                     // vol（只查询）
        printf("volume: %d%% (saved in nvs)\n", radio_get_volume());
    } else if (sscanf(line, "norm %d", &n) == 1) {             // norm <0|1>
        radio_set_normalizer(n != 0);          // 0 = 关，1 = 开
        printf("auto gain: %s\n", radio_normalizer_enabled() ? "on" : "off");
    } else if (strcmp(line, "norm") == 0) {                    // norm（只查询）
        printf("auto gain: %s (norm 0 = raw level, norm 1 = loudness normalized)\n",
               radio_normalizer_enabled() ? "on" : "off");
    } else if (strcmp(line, "wifi") == 0) {                    // wifi（只查询）
        cmd_wifi("");
    } else if (strncmp(line, "wifi ", 5) == 0) {                // wifi <参数>
        cmd_wifi(line + 5);
    } else if (strncmp(line, "tone", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
/* tone [频率] [毫秒]：不联网、不解码，直接在 I2S 上输出方波，
* 用来判断功放和接线是否正常。ms=0 表示一直响到 stop。 */
        int f = 1000, d = 2000;                 // 默认 1000Hz / 2 秒
        sscanf(line, "tone %d %d", &f, &d);
        radio_play_tone(f, d);
        if (d == 0) {
            printf("tone: %d Hz, continuous (stop to end)\n", f);
        } else {
            printf("tone: %d Hz, %d ms (stops current playback)\n", f, d);
        }
    } else if (strncmp(line, "sd", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
/* sd <0|1|z>：用 GPIO 驱动功放的 SD 脚，检测模块死活。 */
        char arg[4] = "z";                     // 默认悬空（模块自己的默认状态）
        sscanf(line, "sd %3s", arg);
        int mode = -1;                         // -1 = 悬空（模块自己决定）
        if (arg[0] == '0') {
            mode = 0;                          // 拉低 = 关断
        } else if (arg[0] == '1') {
            mode = 1;                          // 拉高 = 只放左声道
        }
        radio_set_amp_sd(mode);
    } else if (strcmp(line, "pins") == 0) {                    // pins（只查询）
        printf("\ni2s pin maps (current: %d):\n", radio_i2s_pinmap_current());
        for (int i = 0; i < radio_i2s_pinmap_count(); i++) {
            int bck = 0, ws = 0, data = 0;
            radio_i2s_pinmap_get(i, &bck, &ws, &data);
            printf("  [%d] %s bck=%d ws=%d din=%d\n", i,
                   i == radio_i2s_pinmap_current() ? "*" : " ", bck, ws, data);
        }
    } else if (sscanf(line, "pins %d", &n) == 1) {             // pins <n>
        if (radio_set_i2s_pinmap(n)) {         // 切换引脚映射并存 NVS
            int bck = 0, ws = 0, data = 0;
            radio_i2s_pinmap_get(n, &bck, &ws, &data);
            printf("i2s pins -> map %d (bck=%d ws=%d din=%d), saved in nvs\n",
                   n, bck, ws, data);
        } else {
            printf("pin map %d not available\n", n);
        }
    } else if (strcmp(line, "track") == 0) {                   // track（曲名）
        const char *t = radio_current_track();
        printf("track: %s\n", t[0] ? t : "-");
    } else if (strncmp(line, "bt", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
/* bt        -> 蓝牙状态
* bt disc   -> 主动断开手机 */
        if (strstr(line, "disc") != NULL) {
            printf(bt_player_disconnect() ? "bluetooth device disconnected\n"
                                          : "no bluetooth device to disconnect\n");
        } else if (strstr(line, "free") != NULL) {
/* bt free -> 释放（像卸载 App）：整个协议栈拆掉，内存全部还给堆 */
            size_t before = esp_get_free_heap_size();
            bt_player_release();
            printf("bluetooth released, heap %u -> %u bytes\n",
                   (unsigned)before, (unsigned)esp_get_free_heap_size());
        } else if (strstr(line, "close") != NULL || strstr(line, "off") != NULL) {
/* bt close -> 关闭（像停掉 App）：断手机 + 关控制器，栈和内存保留 */
            printf(bt_player_close() ? "bluetooth closed (open it with: bt open)\n"
                                     : "bluetooth close failed\n");
        } else if (strstr(line, "mode") != NULL || strstr(line, "open") != NULL ||
                   strstr(line, "on") != NULL) {
/* bt open / bt mode -> 开启：停电台 + 冷/热启动蓝牙（等手机连）。
 * （进电台模式 = play <n>/next/prev，电台任务会把蓝牙自动 close。） */
            radio_stop();
            printf(bt_player_open() ? "bluetooth open (radio stopped)\n"
                                    : "bluetooth open failed\n");
        } else {
            printf("\nbluetooth : %s (power: %s)\n", bt_player_state_str(),
                   bt_player_power_str());
            printf("  device  : %s\n", bt_player_peer_addr());
            printf("  name    : %s\n", bt_player_peer_name());
            printf("  rate    : %d Hz\n", bt_player_sample_rate());
            printf("  overruns: %u\n", (unsigned)bt_player_overruns());
            printf("  pcm in  : %u cb / %u bytes\n",
                   (unsigned)bt_player_data_cbs(), (unsigned)bt_player_bytes_in());
            printf("  i2s out : %u samples\n", (unsigned)bt_player_samples_out());
            printf("  volume  : %d%%\n", radio_get_volume());
        }
    } else if (strcmp(line, "status") == 0) {                  // status（总览）
        print_status();
    } else if (line[0] != '\0') {                              // 不认识的命令
        printf("unknown command, type 'help'\n");
    }
}

/**
* @brief 串口控制台任务：逐字节读串口，回显并按行解析命令。
*
* @param arg 未使用
*/
static void console_task(void *arg)
{
    char line[192];         // 正在输入的一行
    int  len = 0;           // 已经输入的字符数
    uint8_t ch;             // 刚读到的字节

    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) <= 0) {
            continue;       // 没读到就继续等
        }
        if (ch == '\r' || ch == '\n') {                 // 回车：执行这一行
            line[len] = '\0';
            printf("\n"); /* 另起一行，这样日志输出不会粘在命令后面
                             （否则会出现 "...12345678I (123)" 这种） */
            if (len > 0) {                              // 空行就忽略
                process_command(line);
            }
            len = 0;
            printf("> ");                             // 新的提示符
            fflush(stdout);
        } else if (ch == '\b' || ch == 0x7F) {         // 退格：删一个字符
            if (len > 0) {
                len--;
                printf("\b \b");                       // 擦掉屏幕上那个字符
                fflush(stdout); /* stdout 是行缓冲的，必须 flush 才会立刻显示 */
            }
        } else if (len < (int)sizeof(line) - 1) {      // 普通字符：收进缓冲区
            line[len++] = (char)ch;
            printf("%c", ch);                          // 回显
            fflush(stdout); /* 行缓冲：不 flush 的话回显会卡在缓冲区里不出来 */
        }
    }
}

/**
* @brief 程序入口（ESP-IDF 里的 app_main）。
*/
void app_main(void)
{
/* ---- 1. NVS 必须最先初始化：电台表、WiFi 凭据、音量都存在里面 ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
/* 分区满了或者版本不对：擦掉重来（会丢凭据，但比起不来强） */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);          // 还是失败就重启

    setvbuf(stdout, NULL, _IOLBF, 0);   // stdout 改成行缓冲，日志能及时刷出来

/* ---- 2. lwIP 协议栈和默认事件循环必须在任何网络操作之前建好 ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "ESP32 internet radio starting");
    ESP_LOGI(TAG, "firmware %s (build %s %s)", FW_VERSION, __DATE__, __TIME__);

/* ---- 3. 电台模块：装入电台表、初始化 I2S、创建电台任务 ---- */
    radio_init(s_stations, STATION_COUNT, radio_event_cb, NULL);

/* ---- 4. 蓝牙 A2DP 接收：手机可以把声音推到同一个功放 ----
*      蓝牙要用 NVS（存配对信息），所以必须放在 NVS 初始化之后。 */
    if (bt_player_init() != ESP_OK) {
        ESP_LOGW(TAG, "bluetooth not available");
    }

/* ---- 5. 串口控制台 ---- */
    uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);   // 装 UART0 驱动
    xTaskCreate(console_task, "console", 4096, NULL, 4, NULL);   // 起控制台任务
    print_help();                                    // 打印命令帮助

/* ---- 6. WiFi 联网 ----
* 凭据是用控制台命令 "wifi <ssid>|<password>" 配的。
* 只有 WiFi 起来了（或者刚配好）才开始播放，
* 否则电台任务只会对着网络栈空转。 */
    esp_err_t err = wifi_init_sta();
    if (err == ESP_ERR_NOT_FOUND) {
        printf("\nwifi not configured. set it with: wifi <ssid> <password>\n");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
    } else {
        /* 【用户要求】上电不自动进电台：起来就是蓝牙待命，等手机连。
         * 要听电台用串口命令（play <n> / next / prev）；
         * 开台后蓝牙会自动挂起（射频让给电台），stop 后自动恢复。 */
        printf("\nready. bluetooth: waiting for a phone. radio: play <n>  ('help' = all commands)\n");
    }
}