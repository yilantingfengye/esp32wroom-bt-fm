# ESP32 网络收音机（esp32_max98357_fm）

基于 ESP32 + MAX98357 I2S 数字功放的 WiFi 网络收音机，使用 ESP-IDF v6.1 开发，
无需 ESP-ADF。通过串口命令配置 WiFi 和电台，所有配置持久化到 NVS。

## 功能特性

- **WiFi STA 连接**：串口命令配置（SSID/密码可含空格），NVS 持久化，断线自动重连（指数退避）
- **网络音频流**：`esp_http_client` 流式拉取，支持 HTTP/HTTPS（mbedTLS 证书包校验服务器证书）
- **ICY 元数据**：解析 `StreamTitle`，实时显示正在播放的曲目
- **MP3 解码**：minimp3（单头文件，vendored 在 `components/minimp3`），流式帧同步 + 坏帧跳过
- **I2S 输出**：标准 I2S 16bit 立体声直推 MAX98357，自动跟随流的采样率（32k/44.1k/48k）
- **蓝牙 A2DP 接收（手机 → ESP32 → 功放）**：可像手机 App 一样**开启 / 关闭 / 释放**（见"工作模式"）
- **两种工作模式，串口切换**：上电**不自动进电台**，默认蓝牙待命；`play <n>` 进电台模式（自动关蓝牙），`bt open` 切回蓝牙模式
- **电台槽位可增删**：`url`/`name`/`add`/`del` 命令运行时修改，存 NVS，立即生效
- **健壮性**：断流/断网自动重连（3s 退避）、缓冲满丢弃旧数据、日志节流（同类警告最多几秒一条）
- **串口控制台**：`help` 查看全部命令

> **电台与蓝牙互斥**：两者共用同一个 MAX98357A 功放和同一条 2.4GHz 射频，
> 不会同时出声。切换由串口命令控制，射频与内存都会完整交接（见"工作模式与射频让渡"）。

## 硬件接线

### ESP32 ↔ MAX98357（默认引脚，可在 menuconfig 修改）

| ESP32 | MAX98357 | 说明 |
|---|---|---|
| GPIO27 | BCLK | 位时钟（`PIN_I2S_BCK`） |
| GPIO25 | LRC/WS | 左右声道时钟（`PIN_I2S_WS`） |
| GPIO33 | DIN | 音频数据（`PIN_I2S_DATA`） |
| 5V | VCC/Vin | 功放电源（2.5–5.5V，**建议 5V**，功率更大） |
| GND | GND | 必须共地 |
| GPIO26 | — | 状态 LED（播放常亮 / 重连闪烁，可改） |

扬声器接 `OUT+/OUT−`（模块丝印可能是 SPK+/SPK−），**阻抗 ≥ 4Ω**，不用接地。
MAX98357 无需 MCLK（自动从 BCLK/LRC 恢复时钟）。

### GAIN 增益设置

| GAIN 接法 | 增益 |
|---|---|
| 悬空 | **9 dB（默认，模块一般就是这样）** |
| 直接接 GND | 15 dB |
| 经 100kΩ 接 GND | 12 dB |
| 经 100kΩ 接 VDD | 6 dB |
| 直接接 VDD | 3 dB |

改增益后需重新上电生效。`MAX98357A BGA 2.8` 这类 mini 模块的 GAIN 排针**保持悬空**即可（9dB）。

### SD 引脚

| SD 电压 | 功能 |
|---|---|
| < 0.16V | 关断静音 |
| 0.16–0.77V | **(L+R)/2 立体声混合（正常播放）** |
| > 1.4V | 仅左声道 |

- 成品模块（板载 1MΩ 上拉）：SD 不用管
- 裸芯片/无上拉模块：SD 悬空会被内部 100kΩ 下拉 → **无声**，需经 **1MΩ 接 VDD**

### 电源注意

- 5V 供电时峰值电流可达 650mA，电源按 ≥1A 预留
- **不要从 ESP32 开发板 5V 脚给功放供电**：功放峰值与 WiFi 峰值叠加会导致掉压复位，功放应独立供电、只共地
- 建议在 ESP32 模块 3V3–GND 间并 **100µF + 0.1µF 电容**（靠近模块）

## 功放无声的分步体检法

ESP32 侧已经用直驱模式（PWM/DAC）验证过：解码 → 重采样 → 波形全部正确。
所以**功放无声时，故障只可能在功放模块、接线、喇叭三者之一**，按下面的顺序排除，
每一步都能得到一个二值结论，不用猜。

### 软件侧已与"能正常出声"的参考例程逐项对齐

参考工程 `F:\各种参考例程\esp32s3_max98357a`（ESP32-S3 + MAX98357A，**实测能正常播放**）
与本工程的 I2S 配置对比：

| 项 | 参考例程（可出声） | 本工程 | 结论 |
|---|---|---|---|
| 数据格式 | Philips 标准 I2S | Philips 标准 I2S | ✅ 一致 |
| 位宽 | 16 bit | 16 bit | ✅ 一致 |
| MCLK | `I2S_GPIO_UNUSED` | `I2S_GPIO_UNUSED` | ✅ 一致（**不需要 MCLK**） |
| 槽位 | mono + LEFT 掩码 | stereo（BOTH） | ✅ 都合法（SD=0.33V 时功放做 (L+R)/2 混合） |
| 角色 | master | master | ✅ 一致 |
| BCLK/WS/DIN | GPIO 15/16/7 | GPIO 18/19/22 | 仅引脚不同 |

**结论：软件侧无差异，故障必然在硬件。**（`tone` 命令现在会打印实际写进 I2S DMA 的字节数，
如果这个数不为 0，就证明数据确实送到了 I2S 外设，软件彻底排除。）

### 第 0 步：用 SD 电压反推模块供电（30 秒，已做）

模块上 SD 引脚有 1MΩ 上拉到 Vin、芯片内部 100kΩ 下拉，所以

```
V_SD = Vin × 100k / (1M + 100k) = Vin / 11
```

实测 **SD = 0.33V ⇒ 模块端 Vin ≈ 3.6V** ⇒ **模块供电正常、芯片处于工作态
（0.16–0.77V = (L+R)/2 混合模式）**。这一步直接排除了"供电没到"和"SD 被拉死"两个原因。

> 顺带：用 5V 供电时 SD = 0.45V，仍在混合模式区间，**升压测试不需要动 SD 电阻**。

### 第 0.5 步：SD 脚开关测试（不用示波器，判定芯片死活）

固件新增 `sd` 命令，用 GPIO23 接管模块的 SD 脚（**接 22Ω 电阻**，模块自带的 1MΩ 上拉
会被拉到 ~0.07V，远低于 0.16V 关断门限）：

```
GPIO23 ──[22Ω]── SD 脚
```

| 命令 | SD 脚 | 功放状态 | **万用表看 OUT+ 对 GND** |
|---|---|---|---|
| `sd z` | 悬空 | 模块默认（1M 上拉 → 0.33V，(L+R)/2 混合，9dB） | 应 ≈1.65V |
| `sd 0` | 拉到 GND | **强制关断** | **应掉到 ≈0V** |
| `sd 1` | 拉到 3.3V | 左声道模式，15dB | 应 ≈1.65V，且能听到声音 |

判定：

- **`sd 0` 时 OUT+ 掉到 0V** → **芯片是活的**，问题在 I2S 数据通路或喇叭 → 查接线/换喇叭
- **`sd 0` 时 OUT+ 仍是 1.65V** → **芯片没响应 SD 脚 = 芯片死了或焊接不良** → **换模块**
- **`sd 1` 时能出声** → 之前是模式问题（模块的 SD 上拉可能虚焊），保持 `sd 1` 也能用

> 这个 gpio 上电后是**高阻输入**状态，不影响正常播放；只有执行 `sd` 命令才会接管。

### 第 1 步：5V 芯片体检（最决定性）

把模块换成 **5V 供电**（共地照旧，ESP32 仍用 3.3V 驱动 I2S，模块支持 3.3–5V 逻辑），
万用表直流档量 **OUT+ 对 GND** 和 **OUT− 对 GND**：

| 读数 | 结论 | 动作 |
|---|---|---|
| 两边都 **≈2.5V**（= VDD/2） | 芯片**活着**，问题在 I2S 数据通路或喇叭 | 继续第 2、3 步 |
| 两边仍 ≈1.2V / 0V / 悬空 | 芯片**没工作或已损坏** | **换模块**（见第 4 步） |

> 3.3V 供电时正常空闲电压应为 1.65V。实测 1.2V 明显偏低，说明芯片没有正常偏置。

### 第 2 步：无示波器时，用万用表交流档验输出

万用表拨 **交流电压档（2V / 20V 量程）**，黑笔接 GND，红笔接 **OUT+**，
然后执行 `tone`（1kHz 方波）：

| 读数 | 结论 |
|---|---|
| **0.5–2V 左右跳动** | 功放在正常放大 → **问题在喇叭或喇叭接线**，换喇叭/换线试 |
| **≈0V** | 功放没有输出 → 回第 1 步（芯片/焊接） |

### 第 3 步：抓 I2S 波形（手上有 Saleae Logic 8 时最直接）

接法（Saleae 的 GND 必须和 ESP32 共地）：

```
Saleae CH0 ── BCLK（当前映射 0 是 GPIO27）
Saleae CH1 ── LRC/WS（当前映射 0 是 GPIO25）
Saleae CH2 ── DIN（当前映射 0 是 GPIO33）
Saleae GND ── ESP32 GND
```

在 Saleae Logic 2 里加一个 **I2S 协议解码器**：BCLK=CH0、WS=CH1、DATA=CH2，
触发器设在 CH0 上升沿，然后执行 `tone`（1kHz 方波，2 秒）。

| 抓到的波形 | 结论 |
|---|---|
| CH0 有方波、CH1 在 1kHz 翻转、CH2 有数据 | **ESP32 侧 100% 正常** → 故障必在模块/喇叭/接线 |
| CH0 没方波 | ESP32 的 I2S 没在跑（`tone` 日志里的字节数会是 0）→ 固件问题 |
| CH0 有、CH1 没有 | WS 接线错（模块丝印可能是 WS/MODE） |
| 三路都有，但模块 OUT 无信号 | **模块坏了** → 换模块 |

没有 Saleae 时，用有源探针/示波器看模块排针上的 BCLK/LRC/DIN，判据相同。

### 第 4 步：BGA 封装模块的特殊风险（重点）

`MAX98357A BGA 2.8` 这类 mini 模块是 **BGA 封装 + 2 层板 + 热回流装配**：

- **所有焊点在芯片正下方，肉眼完全看不到**，万用表探针也够不到
- 冷焊/虚焊/空焊是这类模块的头号故障，表现就是"供电正常、SD 正常、就是没声音"
- 处理顺序：
  1. **换模块**（最省事，便宜模块批次问题很常见）
  2. 加热重-flow：热风枪 250℃ 左右轻吹芯片区域，或烙铁轻压芯片并同时给模块供电
     （压紧后有声音 = 焊点问题；压紧也没声音 = 芯片本身坏了）
  3. 模块的 GAIN 引脚**必须悬空或按丝印接**，别把它当 SD 用

## 直驱输出模式（PWM / DAC，诊断用）

当功放或接线存疑、无法判断"是 ESP32 没出声还是功放坏了"时，可以让音频**不经过 MAX98357**，
直接从 GPIO 输出到耳机/喇叭，单独验证 ESP32 侧的音频链路。

`menuconfig → Audio output interface` 三个选项：

| 选项 | 说明 | 输出引脚 |
|---|---|---|
| `I2S digital amplifier (MAX98357)` | **正常模式**，数字功放 | BCK/WS/DATA |
| `PWM direct drive` | LEDC PWM 直驱，100kHz 载波 + 8bit 占空比 | `PIN_PWM`（32） |
| `DAC direct drive` | ESP32 内置 8bit DAC 真模拟输出 | GPIO25（通道 0）/ GPIO26（通道 1） |

PWM 接线（4 节 TRRS 耳机：L、R 并联，Mic 悬空）：

```
GPIO32 ──[22Ω]──┬── 耳机 L
                └── 耳机 R
GND ────────────────── 耳机 GND
```

DAC 接线（**必须串联隔直电容**，否则 1.65V 直流会把 DAC 直接拉垮）：

```
GPIO25 ──[1~47µF]──┬── 耳机 L
                  └── 耳机 R
GND ────────────────── 耳机 GND
```

两种直驱模式的实现要点（16kHz 采样，每个中断写 1 个采样）：

- **硬件定时器 ISR 采样**（gptimer，16.000kHz）：不用 esp_timer 任务，WiFi 抢占不会影响时基
- **直接写寄存器**：绕开驱动。`ledc_update_duty()` 会在**关中断的临界区里忙等** `duty_start` 位
  （`ledc_ll.h:571`），16kHz 下直接把 CPU 拖死 → interrupt watchdog panic。直接写
  `LEDC.channel_group[0].channel[0].duty.duty` 和 `conf1.duty_start` 即可
  （载波周期 10µs ≪ 采样周期 62.5µs，更新永不重叠，忙等本来就是多余的）
- **ISR 放 IRAM**（`CONFIG_GPTIMER_ISR_CACHE_SAFE=y`）：NVS / WiFi 校准写 flash 期间 ISR 也能执行
- **背压**：环形缓冲满时生产者阻塞等待（I2S 模式靠阻塞式 DMA 写天然具备），否则溢出丢样本 = 卡顿
- **峰值归一化**：广播 MP3 峰值常在 -12dBFS，8bit 占空比只用 1/4 摆幅 → 自动增益到满量程
- **GPIO 驱动能力拉满**（`GPIO_DRIVE_CAP_3`）：GPIO 输出阻抗（几十欧）会吃掉大半摆幅

**物理限制**（为什么直驱模式只适合验证、不适合长期使用）：

| 输出方式 | 驱动能力 | 32Ω 耳机（L+R 并联 = 16Ω） |
|---|---|---|
| DAC | 几 mA | **基本听不见**（且没隔直电容时直流会把 DAC 拉塌） |
| GPIO PWM | 20~40mA | 安静环境可闻，音量约功放的 1/10 |

结论：ESP32 侧音频链路验证无误后，**换掉 MAX98357 模块**才是正解。

## 快速开始

### 1. 配置 Python 环境（复用本机已有 venv）

```powershell
. .\activate.ps1        # 设置 IDF_PATH / IDF_PYTHON_ENV_PATH / PYTHONUTF8
```

VS Code ESP-IDF 插件已通过 `.vscode/settings.json` 的 `idf.customExtraVars`
指向同一环境（改 settings 后需 Reload Window）。

### 2. 编译烧录

```powershell
idf.py build
idf.py -p COM3 flash monitor
```

### 3. 配置 WiFi（首次启动会提示）

串口发送（`|` 分隔，SSID/密码可含空格；开放网络密码留空）：

```
wifi MyHome|12345678
wifi ChinaNet-abc|            # 开放网络
wifi                          # 查看当前状态
wifi clear                    # 清除已存凭据
```

凭据存 NVS，之后开机自动连 WiFi。但**上电不会自动播放电台** ——
开机后停在蓝牙待命状态，等你发指令（`play <n>` 进电台，`bt open` 回蓝牙）。
断开原因会打印 reason 码：

| reason | 含义 | 处理 |
|---|---|---|
| 15 | 握手超时 | 密码错误，重发 `wifi` 命令 |
| 202 | 认证失败 | 密码错误或加密不兼容（WPA3-only 部分手机热点） |
| 201 | 找不到 AP | SSID 错误，或连了 5GHz（**ESP32 只支持 2.4GHz**） |
| 203/205 | 关联被拒 | 热点连接数已满，重启热点 |

### 4. 换电台

```
url 0 https://lhttp.qingting.fm/live/339/64k.mp3
name 0 北京新闻广播
```

---

## 工作模式与射频让渡（电台 ↔ 蓝牙）

两者共用**同一个 MAX98357A 功放**，也共用**同一条 2.4GHz 射频**，
所以是**互斥**的：同一时刻只有一路出声。模式由**串口命令**切换（上电不会自动进电台）。

### 两种模式

| 模式 | 怎么进 | 射频归谁 | 另一路的状态 |
|---|---|---|---|
| **蓝牙模式** | 上电默认；或 `bt open` / `bt mode` | 蓝牙独占 | WiFi 在手机连上时彻底关闭（`esp_wifi_stop`） |
| **电台模式** | `play <n>` / `next` / `prev` | WiFi/电台独占 | 蓝牙**彻底释放**（协议栈卸载，约 40KB 内存归还堆） |

退出电台模式：`stop` → 会话结束，自动恢复蓝牙（`bt_player_open` 冷/热启动）。

### 蓝牙的生命周期

```
bt open   开启：没开过/被释放过 → 完整冷启动；close 过 → 秒级热启动
bt close  关闭：断开手机 + 关控制器（协议栈和内存都保留，可秒级再开）
bt free   释放：整个协议栈拆除，内存（约 40KB）全部归还堆
```

`bt` 命令里多了一行电源状态：`bluetooth : idle (power: on / closed / released)`。

> **为什么电台模式要"释放"而不是只"关闭"蓝牙**：只关控制器时，
> 蓝牙协议栈仍占着约 40KB 堆内存，电台随后要申请自己那 18KB 大缓冲就会跟它抢堆 ——
> 抢不过就会让蓝牙栈申请不到入站缓冲而崩溃重启（之前已经发生过）。
> 释放后堆上只剩电台一家，干净且安全。

### FreeRTOS 调度约定

- **蓝牙协议栈的回调里只做轻活**：A2DP 连接回调只置一个标志 + `xTaskNotifyGive` 唤醒音频任务；
  停电台、锁 I2S、关 WiFi 这些**阻塞操作全部挪到音频任务里做**。
  （曾踩坑：在回调里同步 `radio_stop_and_release()` 阻塞 ~1 秒 → AVDTP 信令被卡死 → 手机等超时主动断链，
  症状是"上电第一次自动重连必掉、第二次才正常"。）
- **蓝牙栈（BTC/HCI/控制器）全在 core 0**，音频消费者任务绑 **core 1**（应用核）——
  这样它就算优先级提到 21 也绝不会饿死 BTC(19)。（也踩过坑：绑 core 0 + prio 21 会把 BTC 饿死。）
- 播放统计打印间隔 **30 秒**（`BT_STAT_INTERVAL_US`），每包/波形调试打印由 `BT_VERBOSE` 控制（默认关）。

---

## 串口命令

| 命令 | 说明 |
|---|---|
| `help` | 显示帮助 |
| `list` | 列出所有电台 |
| `play <0-3>` | 播放指定电台（**进电台模式**：自动关闭蓝牙） |
| `next` / `prev` | 上/下一个电台 |
| `stop` | 停止播放（电台模式结束 → 自动重新开启蓝牙） |
| `vol <0-100>` | 设置音量（`vol` 单独发显示当前值） |
| `tone [f] [ms]` | 用 I2S 直接发方波测试音（1kHz/2s），日志会打印**实际写出字节数** |
| `sd <0\|1\|z>` | 接管功放 SD 脚：`0`=强制关断，`1`=左声道/15dB，`z`=悬空（模块默认） |
| `pins` | 列出所有 I2S 引脚映射（当前这套打 `*`） |
| `pins <n>` | 切到第 n 套引脚（存 NVS，重启保持） |
| `norm <0\|1>` | 自动响度增益：`1`=开（默认，音量恒定）`0`=关（原始电平） |
| `bt` | 蓝牙（A2DP sink）状态：状态/设备地址/设备名/采样率/电源 |
| `bt open`（同 `bt mode` / `bt on`） | **开启蓝牙**：停掉电台 + 冷/热启动协议栈，等手机连 |
| `bt close`（同 `bt off`） | **关闭蓝牙**：断开手机 + 关控制器（协议栈和内存保留，秒级可再开） |
| `bt free` | **释放蓝牙**：整个协议栈拆除，内存（约 40KB）全部归还堆 |
| `bt disc` | 主动断开蓝牙设备 |
| `url <n> <url>` | 修改第 n 个电台地址（存 NVS，当前台立即重连） |
| `name <n> <名字>` | 修改第 n 个电台名（存 NVS） |
| `add <url> [名字]` | **新增**一个电台（存 NVS，名字不给就从 URL 末段自动取） |
| `del <n>` | **删除**第 n 个电台（存 NVS，后面的自动前移） |
| `pl` | 显示当前电台解析出来的播放列表（`.m3u`） |
| `wifi` | 显示 WiFi 状态（SSID/连接状态/IP） |
| `wifi <ssid>\|<pass>` | 设置 WiFi 并连接（`|` 允许空格） |
| `wifi clear` | 清除已存 WiFi 凭据 |
| `status` | 显示收音机 + WiFi 完整状态 |

## 电台地址

默认 4 个 SomaFM 电台（**国内基本无法解析 DNS**，需换成国内电台）。

## 电台列表（存 NVS，最多 16 个）

电台表是**动态**的：固件里预置 4 个（Kconfig 里的默认值），运行时用 `add` 随时加、
用 `del` 随时删，**重启后依然在**（NVS 里按 `url0/name0 … url15/name15` 逐个存，
外加一个 `count` 键记录当前数量）。

```c
#define MAX_STATIONS 16   /* main/radio_player.c */
```

| 操作 | 命令 | 说明 |
|---|---|---|
| 新增 | `add https://lhttp.qingting.fm/live/1271/64k.mp3 蜻蜓FM` | 追加到末尾，返回新下标 |
| 改名 | `name 4 蜻蜓FM` | |
| 改地址 | `url 4 https://lhttp-hw.qtfm.cn/live/1271/64k.mp3` | 当前台会立即重连 |
| 删除 | `del 4` | 后面的电台自动前移，序号重排 |
| 查看 | `list` | 列出全部 |
| 播放 | `play 4` / `next` / `prev` | 下标范围随列表长度变 |

**开机逻辑**：先装入 Kconfig 的 4 个默认值，再逐个用 NVS 里的值覆盖；
如果 NVS 里有 `count`（说明用户增删过），**以 NVS 的数量为准**——多的从 NVS 读，
少的直接截断。

> 想再放宽就改 `main/radio_player.c` 的 `MAX_STATIONS`（名字 48 字节 + URL 160 字节/个，
> 32 个也才 6KB RAM）。

## 音量与响度（为什么以前声音会忽大忽小）

### 症状

音量明明调好了，播起来还是"**时大时小**"——鼓点一响音量掉一截，安静段落又慢慢爬回来。

### 根因：老版本用的是**峰值归一化 + 快起慢落**

```c
int32_t target = (32000 << 8) / peak;          // ← 每帧的「瞬时峰值」当响度
if (target > s_audio_gain) s_audio_gain += (target - s_audio_gain) / 4;   // 快起
else                        s_audio_gain += (target - s_audio_gain) / 32;  // 慢落
```

四个问题叠在一起：

| # | 问题 | 后果 |
|---|---|---|
| 1 | **按每帧（26ms）的瞬时峰值算** | 一帧里一个鼓点，峰值就飙高 → 增益被砸掉 25%（-2.6dB） |
| 2 | **快起慢落**（1/4 vs 1/32） | 砸下去要 32 帧 ≈ **0.85 秒**才爬回来 → 听感是"呼吸" |
| 3 | **峰值恒定 ≠ 响度恒定** | 响度取决于 RMS：鼓点多的段落小声、持续弦乐大声 |
| 4 | **`vol` 只是静态乘法器** | `g = gain * vol / 100`，**完全不参与动态过程** → 调音量对"忽大忽小"无效 |

### 现在的做法：基于**平滑 RMS**（≈听感响度）+ 双向限速

```c
#define AGC_TARGET_RMS   6000      /* 目标 -15 dBFS，留出峰值余量 */
#define AGC_MAX_STEP     32        /* 每帧最多变化 ~1 dB（38 dB/s） */
#define AGC_RMS_SMOOTH   4         /* 一阶平滑，时间常数 ~100ms */

/* 每帧：算 RMS → 平滑 → 求目标增益 → 限速逼近 */
int32_t diff = target - s_audio_gain;
if (diff >  AGC_MAX_STEP) diff =  AGC_MAX_STEP;
if (diff < -AGC_MAX_STEP) diff = -AGC_MAX_STEP;
s_audio_gain += diff;
```

- **双向同速** → 无论变响还是变轻都是 38dB/s，**没有呼吸感**
- **看 RMS 不看峰值** → 鼓点不再把音量砸下去
- **每条流开始时重置** → 换台不会继承上一台的电平
- **首帧直接跳到目标**（不是淡入）→ 换台瞬间就是对的响度

### 开关

```
norm        查看状态
norm 0      关掉自动增益 = 原始电平（广播流本身 -12dBFS，会比较轻）
norm 1      打开（默认）
```

想听"原汁原味"的动态（不被压缩的起伏）就用 `norm 0`，然后用 `vol` 调整体大小。

### 默认音量

开机音量由 Kconfig 的 `CONFIG_RADIO_INITIAL_VOLUME` 决定，当前出厂值为 **40**。

```
I radio: initial volume: 40% (default, not saved)
```

**默认不记忆音量**：`vol 60` 只改内存里的值，重启后回到默认 40；想要多大声音自己调。

需要"记住上次音量"的话，打开持久化开关（**默认关闭**）：

```
CONFIG_RADIO_VOLUME_PERSIST=y
```

可以在 `idf.py menuconfig` 里勾选，或直接写进 `sdkconfig` / `sdkconfig.defaults`。
打开后开机日志会变成：`initial volume: xx% (persistent)`。

**出厂值的持久化**：为避免清空 `sdkconfig` 后退回 1，
`sdkconfig.defaults` 里已固化：

```
CONFIG_RADIO_INITIAL_VOLUME=40
# CONFIG_RADIO_VOLUME_PERSIST is not set
```

**Kconfig 的 `default` 只是"出厂值"**：`sdkconfig` 里已经有这个键时，它永远赢。

---

## 蓝牙播放（手机 → ESP32 → 功放）

ESP32 作为 **A2DP Sink**，手机把音乐通过蓝牙推过来，**从同一个 MAX98357A 功放出来**。

```
手机 --BT/SBC--> Bluedroid --SBC 解码--> PCM --环形缓冲--> I2S --> MAX98357A
```

### 用法

1. 手机「设置 → 蓝牙 → 搜索设备」→ 找到 **ESP32 Radio** → 配对
2. 手机里选一个歌/视频/音乐 App → 输出设备选 **ESP32 Radio**
3. 串口里可以看到：

```
I bt: a2dp sink ready (sbc 44100 hz stereo), name 'ESP32 Radio'
I bt: connected: A4:83:E7:xx:xx:xx
I bt: device name: 某某的手机
I bt: audio codec configured
```

**手机连上时电台会自动让位**（同一个功放不能同时放两路），
**手机断开后电台自动恢复** ✓

```
bt            蓝牙状态（状态/设备地址/设备名/采样率/溢出次数/电源）
bt open       开启蓝牙（同 bt mode / bt on）
bt close      关闭蓝牙（同 bt off）
bt free       释放蓝牙：协议栈拆除、内存归还堆
bt disc       主动断开手机
```

> **注意**：电台模式期间蓝牙是关闭的，手机此时**搜不到也连不上**（预期行为）。
> 想用蓝牙先 `stop` 停掉电台（会自动恢复蓝牙），或直接用 `bt open`（同样会先停电台）。

### 关键实现细节

| 项 | 说明 |
|---|---|
| **SBC 解码** | **不用自己写解码器**：Bluedroid 栈内置 Open Interface SBC 解码器（`CONFIG_BT_A2DP_USE_EXTERNAL_CODEC` 保持关闭），应用层直接拿 **16bit PCM** |
| **能力协商** | **不自己注册流端点**：内部解码器模式下栈在 `esp_a2d_sink_init()` 时就注册好了默认 SBC 端点（官方示例同样不注册） |
| **AVRCP** | 目标端（`esp_avrc_tg_*`）：**没有它很多手机（尤其 iOS）根本不会把 ESP32 列成音频输出设备**；顺带支持手机音量条控制本机音量 |
| **BT+WiFi 共存** | `CONFIG_SW_COEXIST_ENABLE=y`，经典蓝牙和 WiFi 同时工作 |
| **音源仲裁** | `radio_i2s_lock()`：手机连上 → 停电台、**同步等电台释放堆缓冲**、锁住 I2S；断开 → 解锁、恢复电台。电台的输出也会检查这把锁，两个写者绝不能同时往 I2S 里写 |
| **环形缓冲** | 16KB（186ms，远大于栈一次回调的 2560 字节），单生产者单消费者无锁；满了丢最旧的并计数（`bt` 命令里的 overruns） |
| **采样率/声道** | 按 `ESP_A2D_AUDIO_CFG_EVT` 里**协商的结果**走（16/32/44.1/48k、立体声/单声道），写死 44.1k 一定会破音 |

### ⚠️ 三个"手机搜不到"的坑（IDF 6.x 专属）

第一次上机时手机**完全搜不到** ESP32，查出来是三个坑叠在一起：

| # | 坑 | 现象 | 正确做法 |
|---|---|---|---|
| 1 | **漏了 `esp_bt_controller_enable()`** | 串口报 `Controller not initialised`，**整个蓝牙栈根本没起来** | IDF 6.x 里 `esp_bt_controller_init()` 只把状态置成 `INITED`，还要 `esp_bt_controller_enable(模式)` 才会变 `ENABLED`，而 `esp_bluedroid_init()` 要求已经是 `ENABLED` |
| 2 | **enable 的模式必须和 init 时一致** | `esp_bt_controller_enable()` 返回 `ESP_ERR_INVALID_ARG` | 模式要和 `BT_CONTROLLER_INIT_CONFIG_DEFAULT()` 里的 `.mode` **完全一样**。我们把 Kconfig 改成 `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y`（官方示例的做法），顺带**省下 600KB flash + 7KB DRAM**（不用 BLE） |
| 3 | **漏了 `esp_bt_gap_set_scan_mode()`** | 栈起来了、手机还是搜不到 | 经典蓝牙（BR/EDR）**没有 BLE 那种"广播"**，必须显式设成"可连接 + 可被发现"：`esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE)` |

**正确的初始化顺序**（官方 `a2dp_sink_stream` 示例）：

```c
esp_bt_controller_mem_release(ESP_BT_MODE_BLE);   // 不用 BLE 就把它的内存释放掉
esp_bt_controller_init(&bt_cfg);                   // -> INITED
esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT); // -> ENABLED  ★ 少这行就全废
esp_bluedroid_init_with_cfg(&bluedroid_cfg);
esp_bluedroid_enable();
esp_avrc_tg_init();                               // AVRCP 必须在 A2DP 之前
esp_a2d_sink_init();
esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);  // ★ 手机靠这个才搜得到
```

正常启动应该是这样：

```
I (849) BTDM_INIT: Bluetooth MAC: 30:ae:a4:89:87:d2
I (1569) bt: discoverable, waiting for a phone ...
I (1589) bt: a2dp sink ready (sbc 44100 hz stereo), name 'ESP32 Radio'
```

**还是搜不到的话**，按这个顺序排查：
1. **天线**：WROOM 模块的 PCB 天线不能被金属/导线/手挡住（BT 的射频比 WiFi 弱很多）
2. 手机里**取消配对**旧的记录再扫
3. 有些手机只在"媒体输出 → 选设备"里才列蓝牙音频设备，主蓝牙列表里看不到

### ⚠️ 五个"连上了却没声音 / 破音"的坑（IDF 6.x 专属）

手机连上了、码流也起来了（`audio stream started`），**但一个采样都收不到**，
或者听到了却是破音 —— 这一节把这五个坑都列出来，每个都有对应的日志特征。

| # | 坑 | 现象 / 日志特征 | 正确做法 |
|---|---|---|---|
| 1 | **回调 API 用错** | `pcm in : 0 cb`（回调一次都没被调用） | 内部解码器要用 `esp_a2d_sink_register_data_callback()`（交来**已解码的 PCM**）；`esp_a2d_sink_register_audio_data_callback()` 是**外部解码器**专用（交来未解码的 SBC 帧），用它回调**永远不会被调用** |
| 2 | **自己注册了流端点** | 同上，PCM 不来 | 内部解码器模式下**不要**调 `esp_a2d_sink_register_stream_endpoint()`：栈在 `esp_a2d_sink_init()` 时就注册好了默认 SBC 端点，自己再注册一个会**顶掉**它 |
| 3 | **内存被电台占着** | `E BT_OSI: malloc failed (size=4112)` + `heap info: free=4340` | 蓝牙栈连接后**立刻**要 4KB（媒体线程 + SBC 解码器），而电台还占着 23KB → 申请失败 → **媒体线程根本没起来** → 一个采样都收不到。用 `radio_stop_and_release()` **同步等电台把堆缓冲还回来**再让蓝牙往下走 |
| 4 | **环形缓冲不够大** | `overruns: 932`（一直在丢音频）→ 声音断断续续 | 蓝牙栈**不是匀速喂数据**的：WiFi/BT 调度一抖动就一次吐好几包。实测丢音频比例：4KB→9.3%、8KB→2.2%、16KB→0.5%，所以要用 **32KB（186ms）**。而且写 I2S 要**小块写**（256 帧/次），大块写会阻塞在 DMA 上、期间新数据全堆在环里 |
| 5 | **采样率/声道写死了** | 声音"破"（音调不对、左右错位） | 从 `ESP_A2D_AUDIO_CFG_EVT` 里读**协商结果**（采样率 16/32/44.1/48k、声道模式），I2S 时钟跟着走；手机协商到单声道时要复制成左右声道 |

**判定口诀**（`bt` 命令里的计数器）：

```
pcm in  : 0 cb            → 坑 1/2/3：协议栈根本没把 PCM 交过来
pcm in  有数 / i2s out = 0 → 音频任务没在写（检查任务和 I2S 锁）
overruns > 0              → 坑 4：环太小 / 写 I2S 的块太大，音频在被丢
pcm in ≈ i2s out           → 通路正常，剩下的就是音量/功放/接线
```

### ⚠️ 内存：为什么大缓冲都放在堆里

蓝牙栈要吃掉约 40KB 的**静态** RAM，而 ESP32 的链接器只给应用分 **124KB 静态 DRAM**
（堆还有 ~96KB）。直接开蓝牙会**链接失败**（DRAM 溢出 40KB）。

解决方式：**把大缓冲改成堆分配，用完就还**：

| 缓冲 | 大小 | 分配时机 | 释放时机 |
|---|---|---|---|
| 电台流缓冲 + 解码缓冲 | 15KB | 开始播放时 `malloc` | 停止时 `free` |
| 蓝牙 PCM 环形缓冲 | 32KB | 手机连上时 `malloc` | 断开时 `free` |

电台和蓝牙本来就**互斥**，所以内存可以干净地交接 ✓
（代价：切换时会有一两次 malloc/free，堆碎片可接受）。

⚠️ 交接必须是**同步**的：蓝牙栈在连接回调返回后**立刻**就要 4KB，
电台的缓冲是电台任务异步释放的（它要先退出收流循环）。
两者错开 ~170ms 就正好撞上 → `BT_OSI: malloc failed` → 蓝牙没声音。
所以蓝牙连接时调 `radio_stop_and_release(1000)`：**等电台真的把内存还回来**再继续。

其它省内存的手段：
- WiFi 静态接收缓冲 10 → 2（省 IRAM）
- 关闭 `ESP_WIFI_RX_IRAM_OPT`（接收缓冲放 DRAM 而不是 IRAM）
- 流缓冲 32KB → 16KB（TCP 窗口 28KB 才是真正的抖动缓冲）

### 分区表

蓝牙让固件从 1.05MB 涨到 **1.78MB**，默认的 "single app large" 分区（1.5MB）装不下，
所以改用自定义 `partitions.csv`：

```
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1F0000,   ← 1.9MB
```

按 **2MB flash** 设计（IDF 默认值），所以 2MB/4MB 的板子都能烧 ✓
（4MB 的板子只用了前 2MB）。

---

## 播放在线音乐（.m3u 播放列表 + 断点续传）

### ① 播放列表：把电台地址指向一个 `.m3u` 文件

**不需要新命令**——`url` 命令填一个 `.m3u` 地址就行，固件会自动识别并逐首播放：

```
url 0 http://192.168.1.100:8000/list.m3u
play 0
pl            ← 看解析出来的歌单
```

日志：

```
I radio: playlist: 3 entries from http://192.168.1.100:8000/list.m3u
I radio: playlist 1/3: 01 晴天
I radio: stream: 01 晴天 (http 200, metaint=-1, type=audio/mpeg)
I radio:   mp3: 44100 Hz, 2 ch, 128 kbps
I radio: playlist 2/3: 02 稻香
...
```

**支持的标准 m3u 格式**：

```
#EXTM3U
#EXTINF:-1,歌名
http://host/song1.mp3        ← 绝对地址
song2.mp3                    ← 相对地址（相对播放列表所在目录解析）
```

- `#EXTINF:` 的歌名会显示为曲目名
- **相对地址**按播放列表的目录解析 → 同一个歌单换台机器也能用
- 相对地址里的**空格/中文会自动百分号编码**（`01 晴天.mp3` → `01%20%E6%99%B4%E5%A4%A9.mp3`），
  已经编码过的 `%xx` 不会二次编码
- 最多 16 首（`PL_MAX_ENTRIES`），整个列表播完自动从头再来

### ② 断点续传：大文件断了不用从头下

- 服务端给了 `Content-Length`（= 这是个**文件**而不是直播流）→ 记下已收字节数
- 连接断了/重连时带 `Range: bytes=<已收>-` → **服务端回 206，从断点继续**
- 服务端**忽略** Range（回 200）→ 自动从头开始，不会重复播放
- **直播流没有 Content-Length → 永远从当前流的开头读**（不会错误地发 Range）

日志：

```
I radio: resuming at byte 1048576/5242880
I radio:   resumed: 4194304 bytes left
```

### ③ 电脑上的音乐怎么放上来（一条命令）

仓库里带了一个零依赖的小服务器（**支持 Range**，所以断点续传也能测）：

```powershell
python tools\serve_music.py D:\我的音乐 --port 8000
```

它会打印出可以直接粘进串口的地址：

```
  playlist  http://192.168.1.100:8000/list.m3u      (relative entries)
  playlist  http://192.168.1.100:8000/list_abs.m3u  (absolute urls)
```

| 地址 | 用途 |
|---|---|
| `/list.m3u` | 相对地址的歌单（推荐，换 IP 也能用） |
| `/list_abs.m3u` | 绝对地址的歌单（用来对比测试） |
| `/任意文件.mp3` | 直接访问单曲也行（播完自动从头再来） |

自测脚本（不需要 esp32，验证歌单生成和 Range）：

```powershell
python tools\test_serve_music.py
```

### ⚠️ 播不了的东西（说清楚免得白折腾）

| 形态 | 原因 |
|---|---|
| **`.m3u8`（HLS）** | 它的分片是 TS/fMP4 容器（多半还是 AAC），本工程只解 MP3。**注意：本工程把 `.m3u8` 当成普通文本歌单处理**，如果分片不是 MP3 会解不出来 |
| **QQ音乐/网易云直链** | 播放地址带签名且时效很短，返回的是 **AES-128 加密的 HLS**，且多为 AAC → 需要额外的解密库和 AAC 解码器 |
| **网盘直链** | 多数是网页或带 token 的临时地址 |

**可行的路子**：局域网自建歌单（上面的脚本）、公开播客的 MP3 直链、各种开放电台目录。

国内可直接使用蜻蜓 FM 直链，格式：

```
https://lhttp.qingting.fm/live/<电台ID>/64k.mp3
```

常用电台 ID（多来源交叉验证，2025-10 整理）：

| 电台 | ID | 电台 | ID |
|---|---|---|---|
| 中国之声 | 386 | 北京新闻广播 | 339 |
| 经济之声 | 387 | 北京交通广播 | 336 |
| 音乐之声 | 388 | 北京文艺广播 | 333 |
| 经典音乐广播 | 389 | 北京城市广播 | 345 |
| 文艺之声 | 395 | 深圳飞扬971 | 1271 |
| 香港之声 | 4986 | 西安音乐广播 | 1612 |
| 华夏之声 | 392 | 武汉经典音乐 | 1297 |
| 老年之声 | 396 | 江苏交通广播 | 4054 |
| 中国乡村之声 | 5039 | 苏州音乐广播 | 2803 |

备用域名：`https://lhttp-hw.qtfm.cn/live/<ID>/64k.mp3`

## 网络流畅度优化

卡顿的根因通常不是 CPU，而是**缓冲链路上最窄的那一环**。本工程从下到上做了 4 处改动：

### 1. WiFi 省电模式关掉（收益最大）

```c
esp_wifi_set_ps(WIFI_PS_NONE);
```

ESP32 默认是 **min-modem** 省电：每个信标周期（DTIM，通常 100ms）之间休眠，
**每个包都带 10ms+ 的延迟和抖动**。对连续流就是持续的小卡顿。
板上走 USB 供电，省电毫无意义 → 直接关掉。

### 2. TCP 接收窗口 5760 → 28680（**决定 CDN 能推多远**）

| | 原值 | 现值 | 意义 |
|---|---|---|---|
| `LWIP_TCP_WND_DEFAULT` | 5760（4×MSS） | **28680**（20×MSS） | TCP 窗口 = 0.7s → **3.6s** 音频 |
| `LWIP_TCP_RECVMBOX_SIZE` | 6 | **22** | 必须按 IDF 公式 `WND/MSS+2` 同步调大，否则 lwip 丢包 |
| `LWIP_TCP_SND_BUF_DEFAULT` | 5760 | 28680 | 发送侧 |

**关键点：应用层那个 32KB 缓冲其实用不满**——TCP 窗口才决定上游能推多远。
窗口只有 5760 时，CDN 最多领先 0.7 秒，一抖动就断粮。

### 3. 应用层流缓冲 16KB → 32KB（未解码 MP3）

64kbps 下 32KB = **4 秒**音频，吸收 CDN 抖动。
代价：直播延迟从 2 秒变成 4 秒。

### 4. 背压代替"丢最旧字节"（消除解码错帧）

原来缓冲满时会 `memmove` 丢掉最旧的字节 → **破坏 MP3 帧边界** → 解码器报
`mp3 decode error, skipping frame` →  audible 咔哒声。
现在改成**缓冲满就不从 socket 读**：TCP 窗口自己收窄、服务器等着，**一个字节都不丢**。

```c
if (s_fill + HTTP_RD_SIZE > STREAM_BUF_SIZE) {
    decode_loop();                      /* 由阻塞式 i2s 写按实时速度腾位置 */
    if (s_fill + HTTP_RD_SIZE > STREAM_BUF_SIZE) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;                        /* 重新检查停止标志再读 */
    }
}
```

### 还能做的（固件之外）

| 手段 | 说明 |
|---|---|
| **换 CDN 节点** | `lhttp.qingting.fm`（默认）↔ `lhttp-hw.qtfm.cn`（华为云边缘），哪个快用哪个 |
| **换码率** | 64kbps → 128kbps：每秒钟的数据量翻倍，对抖动的容忍度更好（CPU 压力也翻倍） |
| **换电台** | 不同电台的 CDN 节点和限流策略不同 |
| **供电** | WiFi TX 峰值 300–500mA，**3.3V 轨塌了会直接表现为卡顿**（见"电源注意"） |
| **看日志定位** | `stream buffer full, dropped N bytes`（旧版）→ 网络跟不上；现在改成背压后这个日志不该再出现 |

找新 ID（qingting.fm 网站改版后不好翻）：
- 搜索引擎搜 `lhttp.qingting.fm/live <电台名>`
- GitHub/Gitee 上的 `广播电台.m3u` 列表（含大量直链）

## 工程结构

```
esp32_max98357_fm/
├── activate.ps1              # 一键激活 IDF 环境（复用已有 venv）
├── sdkconfig.defaults        # 工程级默认配置（BOD 等级等）
├── components/
│   └── minimp3/              # minimp3 单头文件解码器（lieff/minimp3 master）
├── main/
│   ├── Kconfig.projbuild     # menuconfig 配置项（WiFi 默认值/电台/输出方式）
│   ├── board_pins.h          # ★ 引脚定义（唯一来源，见"配置项"一节）
│   ├── main.c                # 入口 + 串口命令
│   ├── wifi_station.c/.h     # WiFi STA（NVS 凭据、退避重连、事件处理）
│   ├── radio_player.c/.h     # 核心：HTTP 流 + ICY 解析 + 解码 + I2S 输出
│   ├── bt_player.c/.h        # 蓝牙 A2DP sink（手机推流 → 同一个功放）
│   ├── CMakeLists.txt
│   └── ...
└── tools/
    ├── serve_music.py         # 本地音乐服务器（生成 .m3u，支持 Range 续传）
    ├── test_serve_music.py    # 它的自测脚本（python tools\test_serve_music.py）
    └── translate_comments.py  # 把源码里残留的英文注释批量换成中文
└── .vscode/                  # ESP-IDF 插件设置（IDF_PATH、Python 环境）
```

---

## 源码编码格式（重要，改代码前先看这段）

### 一句话版本

> **所有源码、脚本、文档都是 UTF-8（无 BOM）+ CRLF 换行；注释一律中文。**

`.gitattributes` 已经把这套规则锁死了，正常用 git 拉下来/提交都不会乱。

### 1. 文件编码：UTF-8 **无 BOM**

| 项 | 值 | 为什么 |
|---|---|---|
| 字符编码 | **UTF-8** | 中文注释、README 中文都需要 |
| BOM | **不要**（no BOM） | 带 BOM 的 `.c/.h` 会被工具链当成文件头字符，轻则编译报错、重则注释乱码 |
| 换行符 | **CRLF**（Windows） | 和仓库里其它文件保持一致，避免整个文件在 diff 里全变 |

验证方法（PowerShell）：

```powershell
# 应该输出 "no-BOM" 和 CRLF 数量
$b = [System.IO.File]::ReadAllBytes("main\radio_player.c")
"$(if($b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF){'BOM'}else{'no-BOM'})"
```

VS Code 里设一次就一劳永逸：

```jsonc
// .vscode/settings.json
{
  "files.encoding": "utf8",          // 注意：不是 utf8bom
  "files.eol": "\r\n",
  "files.insertFinalNewline": true
}
```

### 2. 注释：全中文

自研代码（`main/`、`tools/`、构建脚本、Kconfig）里的**注释全部是中文**，
`components/minimp3` 是第三方库（lieff/minimp3，MIT），**保持原样不动**。

改动注释时注意：

- 注释里如果出现了 `*/`，一定要写成 `*\/` 或者换行，否则会提前结束注释块
- 注释里出现中文全角标点没问题，**但不要在注释里出现 `??` 这样的三字符序列**（GCC 会警告）
- 行尾注释（`// ...`）里也别写 `\` 续行

批量把英文注释换成中文可以用仓库里的脚本（映射表在文件顶部的 `MAP`）：

```powershell
python tools\translate_comments.py
```

它只替换"整块都是英文"的注释，**已经写成中文的不会动**，
并且保持 UTF-8 无 BOM + CRLF 不变。

### 3. 串口终端里的中文显示

固件的**日志文本全是英文**（`I radio: ...`），所以终端代码页不影响日志。
但**电台曲名**（ICY 元数据）会原样透传，绝大多数国内电台发的是 **GBK**，
所以在默认的 GBK 代码页终端里会显示成乱码 —— **这不是 bug**。

三个办法任选：

```powershell
# 1. 切到 UTF-8 代码页（Windows 10 1903+ 自带）
chcp 65001
idf.py -p COM4 monitor

# 2. 用支持 UTF-8 的终端（Windows Terminal / VS Code 终端 / MobaXterm）

# 3. 串口工具里把"接收编码"设成 GBK（如果你的终端不支持 UTF-8）
```

IDF monitor 本身按 UTF-8 输出，Windows Terminal / VS Code 终端下曲名是正常的。

### 4. 固件里的字符串常量

固件源码里的**中文字符串常量很少**（日志一律用英文），
万一要加中文常量，注意：

- 源文件是 UTF-8，但 ESP32 的 flash 里存的是 UTF-8 字节，
  打印时终端要按 UTF-8 解码才对得上（`chcp 65001`）
- 中文常量会占 3 字节/字符，尽量别往日志里塞中文（省 flash，也省串口带宽）

---

## 配置项（idf.py menuconfig）

- `Internet Radio Stations`：4 个电台默认名/URL、初始音量、流缓冲大小（4–32KB）
- `Audio output interface`：音频输出方式（I2S 功放 / PWM 直驱 / DAC 直驱，详见"直驱输出模式"）
- WiFi 凭据**不在** menuconfig 里，一律走串口 `wifi` 命令

### 引脚不在 menuconfig 里 → `main/board_pins.h`

引脚定义是**普通 `#define`，只在一个文件里**：

```c
#define PIN_I2S_BCK    27   /* -> module BCLK */
#define PIN_I2S_WS     25   /* -> module LRC/WS */
#define PIN_I2S_DATA   33   /* -> module DIN   */
#define PIN_STATUS_LED 26
#define PIN_AMP_SD     23   /* -1 = 关闭 SD 控制 */
#define PIN_PWM        32
#define PIN_DAC_CHANNEL 0
```

### 运行时换引脚：`pins` 命令（**功放已经能出声之后的保险措施**）

硬件接线改过好几次，所以把 I2S 引脚做成**两套映射，运行时可切换，选中的那套记在 NVS 里**：

| 映射 | BCLK | LRC/WS | DIN | 说明 |
|---|---|---|---|---|
| 0 | **GPIO27** | **GPIO25** | **GPIO33** | **默认（当前这套接线，已验证出声）** |
| 1 | GPIO18 | GPIO19 | GPIO22 | 早期那套接线（要改回老接线时用） |

```
pins          查看所有映射和当前用的那套
pins 1        切到映射 1（会先停播，切换结果存进 NVS）
```

**为什么值得做**：固件里的默认引脚一旦被改动，而硬件还是老接线，烧录后功放就哑了——
而且很难第一时间联想到"引脚变了"。现在选择被记住，**以后任何一次烧录都不会再把好状态改坏**；
想试另一套接线，**一条命令 5 秒切过去，不用重新编译**。

（实现：`i2s_channel_disable()` → `i2s_channel_reconfig_std_gpio()` → `i2s_channel_enable()`，
NVS 键 `pin_bck` / `pin_ws` / `pin_data`——**存的是引脚本身而不是表序号**，
所以以后增删或重排映射表，也不会让已存的选项含义漂移。）

**为什么不用 Kconfig 选项**：Kconfig 方案下改一个引脚要同步三个地方
（`Kconfig.projbuild` 的 default + `sdkconfig` + `sdkconfig.defaults`），
一旦 sdkconfig 被重新生成，引脚会**悄悄退回旧值**——这个坑本工程已经踩过一次。
两种方式都必须"改 → 重新编译 → 烧录 → 重启"，Kconfig 唯一多出来的好处只是一个
配置界面，为此付三处同步的代价不划算。

改引脚后：改 `board_pins.h` → `idf.py build` → `idf.py -p COM4 flash` → 重新上电，
启动日志会打印实际用的脚：`i2s ready (bck=27 ws=25 dout=33)`。

## 已知问题与注意事项

1. **MAX98357 模块无声 / 喀哒声 → ✅ 已解决（本机实测出声）**：**固件侧从一开始就是对的**——
   `tone` 日志显示 352800 字节在 1970ms 内写进 I2S DMA（= 精确 44.1kHz 实时），
   说明 BCLK 在 GPIO 上以 1.41MHz 正常翻转；I2S 配置与一份**实测能出声**的
   参考例程逐项一致（Philips/16bit/无 MCLK）。
   真正的坑在硬件侧，两个都跟"电"有关：
   - **功放从 3.3V 轨取电**（峰值 400–800mA，和 WiFi 峰值叠加把轨拉塌 → 载波被压扁
     → "嗤嗤"声 + 只在开关瞬间有声）→ **功放独立供电（建议 5V）**；
   - **共地没接稳**（模块 GND 与 ESP32/电源 GND 接触不良/虚接）→ 音频回流路径阻抗
     随电流变化，叠加出宽带脉冲杂音，**空闲时和放歌时都有喀哒声**。
     **接牢共地后喀哒声消失，播放正常。**（排查软件前务必先确认共地可靠。）
   **当前能出声的接线：BCLK=GPIO27、LRC=GPIO25、DIN=GPIO33**（映射 0，已实测）。
   为防止以后再被引脚改动坑到，引脚映射做成了运行时可切换（`pins` 命令），
   选中的那套记在 NVS 里 → **任何一次烧录都不会再把功放改哑**。
2. **I2S 引脚换过位置**：从最早的 25/26（内部 DAC 焊盘，IDF 会提示 not usable）
   → 18/19/22 → 现在的 **27(BCK) / 25(WS) / 33(DIN)**，见 `main/board_pins.h`。
   每次换脚都要同步改接线，**并用启动日志 `i2s ready (bck=.. ws=.. dout=..)` 确认**。
3. **Brownout 检测**：已启用且保持最低阈值（`CONFIG_ESP_BROWNOUT_DET_LVL=0`）。
   若烧录后出现 `E BOD` 复位循环，说明供电扛不住 WiFi 峰值，先加电容（见上文电源注意），
   应急可 `menuconfig → ESP32-Specific → Brownout detector level` 调低/关闭。
4. **radio_task 栈必须 ≥ 32KB**：minimp3 的 `mp3dec_decode_frame` 内部有约 16KB 栈上
   scratch 缓冲（`mp3dec_scratch_t`），栈不足会踩坏堆里的 I2S 对象，表现为 DMA 中断里
   野指针 panic（`LoadStoreAlignment`/`LoadProhibited`）。**不要调小这个值。**
5. **I2S 时钟按"每条新流重新锁定"**：换台/重连时在新流的第一帧重配采样率（此刻没有音频在飞，
   无 DMA 竞态风险）；同一流中途的采样率变化只记日志（节流 5s 一条）。
6. **IDF 6.1 移除了 `skip_verify`**：HTTPS 电台必须验证证书，工程已启用
   `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`（full bundle）并挂 `crt_bundle_attach`。
7. **FreeRTOS tick 只有 100Hz**：`pdMS_TO_TICKS(x)` 在 x<10 时会得到 **0 tick**，
   `vTaskDelay(0)` 只是 yield 不阻塞 → 任务空转饿死 IDLE → task watchdog panic。
   写延时前先算一遍。

## 调试排障记录（踩坑合集）

按本次开发中实际遇到的问题整理：

| 现象 | 原因 | 解决 |
|---|---|---|
| 开机即 abort，循环重启 | `esp_netif_init()` 在 HTTP 请求之后才调用，lwIP 未初始化 | 网络栈初始化移到 `app_main` 最前 |
| WiFi 完全静默、无任何事件 | `wifi_init_sta()` 漏了 `esp_wifi_start()` | 补上；`esp_wifi_connect()` 在未 start 时静默失败 |
| `getaddrinfo() returns 202` | WiFi 未连上（无 IP），或域名无法解析（SomaFM 国内） | 先确认 `connected + ip`；换国内电台 |
| `auth -> assoc` 后被拒 + comeback | AP 侧 STA 条目残留 / 连接数满 | 重启热点；代码加退避 + 定期 STA bounce |
| 手机热点连不上（reason 15/202） | 密码错误，或热点只有 WPA3-only | ESP32 已开 WPA3-SAE，确认密码；或换 WPA2 热点 |
| `E BOD: Brownout detector` | WiFi TX 峰值电流拉低 VDD（微秒级，万用表抓不到） | 加电容/独立供电；调试期可关 BOD |
| `No server verification option set` | IDF 6.1 https 必须验证证书 | 启用证书包 + `crt_bundle_attach` |
| I2S DMA 中断野指针 panic | radio_task 栈溢出（minimp3 scratch ~16KB） | 栈 8KB → 32KB |
| 串口回显末尾多个 `I` | 日志前缀 `I (xxx)` 粘在命令回显后 | 收到回车先输出换行 |
| `wifi` 命令 SSID 带空格被截断 | 按空格分词 | 改用 `|` 分隔符 |
| SomaFM 显示 `12345678I (xxx)` 之类“密码被加字符” | 同上，日志前缀视觉粘连 | 回车先换行修复 |
| `ledc: requested frequency 100000 and duty resolution 10 can not be achieved` | LEDC 时钟源只有 80MHz，100kHz×1024 超出上限；且旧代码没检查返回值，通道绑到"死"定时器上 | 改 8bit 分辨率；**所有驱动调用都要检查返回值**，失败就不启动输出通道 |
| `Interrupt wdt timeout` + backtrace 停在 `ledc_ll_set_duty_start` | `ledc_update_duty()` 在**关中断临界区里忙等** `duty_start` 位，定时器失效时死等 | 不用驱动调用，ISR 里直接写 LEDC 寄存器（见"直驱输出模式"） |
| `task_wdt: IDLE0 ... esp_timer` | esp_timer 周期 62µs < 任务唤醒延迟，定速重 Arm 永远追不上 → 忙循环 → 饿死 idle | 改用硬件定时器 ISR 采样；块输出改成每中断 1 个采样 |
| 声音"特别慢、卡" | 1kHz 定时器 + 每次突发写 16 个采样 = 音频被压成小段爆音 + 长静音 | 每中断写 1 个采样，时基精确到 µs |
| 声音"卡"（有回声感的断续） | 生产端无背压：解码器把网络缓冲一次灌进音频环形缓冲，满了**静默丢样本** | 环形缓冲满时生产者阻塞等待（`audio_out_wait_space`） |
| `task_wdt: IDLE0 ... radio` | `vTaskDelay(pdMS_TO_TICKS(2))`，而 tick 频率是 100Hz → **0 tick** → `vTaskDelay(0)` 只 yield 不阻塞 → 任务空转饿死 idle | 小于 10ms 的延时改用 `vTaskDelay(1)`；或直接用 1 tick |
| 突然**完全没声音** | 环形缓冲空闲数算反（把"已存样本数"当成了"空闲数"），缓冲一空就判定"满"→ 每个样本都等满 1 秒超时后被丢弃 | 空闲数 = `MASK - 已存数` |
| 声音小（但波形正确） | 广播 MP3 峰值多在 -12dBFS，8bit 占空比只用 1/4 摆幅 | 峰值归一化（快起慢落，最高 12 倍），再叠加 GPIO 最大驱动能力 |
| DAC 直驱耳机听不到 | DAC 只能提供几 mA，16Ω 负载需要 100mA；没有隔直电容时 1.65V 直流直接把它拉塌 | DAC 只作测量工具；直驱听感用 PWM + 缓冲级，或换功放 |
| 播放中出现 `mp3 decode error, skipping frame` + 咔哒声 | 流缓冲满时"丢最旧字节"，破坏了 MP3 帧边界，解码器错帧 | 改成**背压**：缓冲满就不从 socket 读，让 TCP 窗口自己收窄，一个字节都不丢 |
| 播放中持续小卡顿（不是断续，是"抖"） | WiFi 默认 min-modem 省电，每个包带 10ms+ 抖动；且 TCP 窗口只有 5760（0.7s），上游推不远 | `esp_wifi_set_ps(WIFI_PS_NONE)` + TCP 窗口/接收邮箱放大到 28680/22 + 流缓冲 32KB |
| **喀哒声**（空闲时也有、放歌时一个字都听不清） | **硬件问题：功放 GND 与 ESP32/电源 GND 没接稳** —— 地线松动/虚接时，音频回流路径阻抗随电流变化，叠加出宽带脉冲杂音；软件缓冲再怎么调都只是治标 | **把共地接牢**（单点粗线、焊牢/压接可靠）；软件侧的缓冲/水位/射频隔离保留作加固 |
| 上电第一次自动重连必掉、第二次才正常 | A2DP 连接回调里**同步**执行了阻塞操作（`radio_stop_and_release()` 最长 1 秒 + `esp_wifi_stop()` 几百毫秒），AVDTP 信令被卡死 → 手机等超时主动断链 | 回调里只置标志 + `xTaskNotifyGive`，重活全部挪到音频任务 |
| 蓝牙连不上（连上码流起了但没声音/栈崩） | 消费者任务绑 **core 0** 且优先级 21 > BTC(19)，把蓝牙栈饿死 | 消费者任务改绑 **core 1**（应用核，没有蓝牙栈任务） |
| 环形缓冲开 32KB 后整机崩溃重启 | 32KB 环 + 蓝牙栈的入站缓冲一起把堆吃光 → `host_recv_pkt_cb couldn't acquire memory` + 断言 | 环保持 **16KB（8192 采样）**；同时让大缓冲按需分配、用完即还 |

## 许可

- 项目代码：MIT
- minimp3：CC0/public domain（lieff/minimp3）
