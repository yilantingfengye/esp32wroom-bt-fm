# ============================================================================
#  main/CMakeLists.txt —— 本组件（main）的编译配置
# ============================================================================

idf_component_register(
    # ---- 本组件的源文件 ----
    SRCS "main.c"              # 程序入口 + 串口控制台（命令解析）
         "wifi_station.c"      # WiFi 客户端：连接路由器、凭据存 NVS
         "radio_player.c"      # 核心：收流 + ICY 解析 + MP3 解码 + I2S 输出
         "bt_player.c"         # 蓝牙 A2DP 接收（手机推流到同一个功放）

    # ---- 本组件的头文件搜索路径 ----
    INCLUDE_DIRS "."

    # ---- 依赖的其它组件 ----
    REQUIRES
        nvs_flash         # NVS 键值存储：存电台表、WiFi 凭据、音量、引脚映射
        esp_wifi          # WiFi 驱动
        esp_netif         # 网络接口抽象（lwIP 之上）
        esp_event         # 事件循环（WiFi 事件等）
        esp_http_client   # HTTP/HTTPS 客户端（拉流）
        esp_timer         # 定时器
        esp_driver_i2s    # I2S 驱动（MAX98357A 的数字输入）
        esp_driver_gpio   # GPIO 驱动（状态灯、功放 SD 脚）
        esp_driver_dac    # DAC 驱动（仅诊断模式）
        esp_driver_gptimer# 通用定时器（仅诊断模式）
        esp_driver_ledc   # LEDC（PWM 直驱模式）
        esp_driver_uart   # UART 驱动（串口控制台）
        bt                # 蓝牙：控制器 + Bluedroid 协议栈 + A2DP

    # ---- 只对本组件内部可见的依赖 ----
    PRIV_REQUIRES minimp3  # MP3 解码库（第三方，头文件直接包含，不对外暴露）
)

# ---- 固件版本（同步 CHANGELOG.md 和 git tag）----
# 用法：在代码里写 FW_VERSION，固件启动时会打印出来。
# 发布流程：改这里的版本号 -> 更新 CHANGELOG.md -> 提交 -> git tag vX.Y.Z
target_compile_definitions(${COMPONENT_TARGET} PRIVATE
    FW_VERSION="v${PROJECT_VER}"
)
