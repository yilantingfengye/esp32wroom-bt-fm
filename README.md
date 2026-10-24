# ESP32 网络收音机（esp32_max98357_fm）

基于 ESP32 + MAX98357 I2S 数字功放的 WiFi 网络收音机，使用 ESP-IDF v6.1 开发，
无需 ESP-ADF。通过串口命令配置 WiFi 和电台，所有配置持久化到 NVS。

## 功能特性

- **WiFi STA 连接**：串口命令配置（SSID/密码可含空格），NVS 持久化，断线自动重连（指数退避）
- **网络音频流**：`esp_http_client` 流式拉取，支持 HTTP/HTTPS（mbedTLS 证书包校验服务器证书）
- **ICY 元数据**：解析 `StreamTitle`，实时显示正在播放的曲目
- **MP3 解码**：minimp3（单头文件，vendored 在 `components/minimp3`），流式帧同步 + 坏帧跳过
- **I2S 输出**：标准 I2S 16bit 立体声直推 MAX98357，自动跟随流的采样率（32k/44.1k/48k）
- **4 个电台槽位**：`url`/`name` 命令运行时修改，存 NVS，立即重连生效
- **健壮性**：断流/断网自动重连（3s 退避）、缓冲满丢弃旧数据、日志节流（同类警告最多几秒一条）
- **串口控制台**：`help` 查看全部命令

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

凭据存 NVS，之后开机自动连接。断开原因会打印 reason 码：

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

## 串口命令

| 命令 | 说明 |
|---|---|
| `help` | 显示帮助 |
| `list` | 列出所有电台 |
| `play <0-3>` | 播放指定电台 |
| `next` / `prev` | 上/下一个电台 |
| `stop` | 停止播放 |
| `vol <0-100>` | 设置音量（`vol` 单独发显示当前值） |
| `tone [f] [ms]` | 用 I2S 直接发方波测试音（1kHz/2s），日志会打印**实际写出字节数** |
| `sd <0\|1\|z>` | 接管功放 SD 脚：`0`=强制关断，`1`=左声道/15dB，`z`=悬空（模块默认） |
| `pins` | 列出所有 I2S 引脚映射（当前这套打 `*`） |
| `pins <n>` | 切到第 n 套引脚（存 NVS，重启保持） |
| `url <n> <url>` | 修改第 n 个电台地址（存 NVS，当前台立即重连） |
| `name <n> <名字>` | 修改第 n 个电台名（存 NVS） |
| `add <url> [名字]` | **新增**一个电台（存 NVS，名字不给就从 URL 末段自动取） |
| `del <n>` | **删除**第 n 个电台（存 NVS，后面的自动前移） |
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
│   └── CMakeLists.txt
└── .vscode/                  # ESP-IDF 插件设置（IDF_PATH、Python 环境）
```

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

1. **MAX98357 模块无声 → ✅ 已解决（本机实测出声）**：**固件侧从一开始就是对的**——
   `tone` 日志显示 352800 字节在 1970ms 内写进 I2S DMA（= 精确 44.1kHz 实时），
   说明 BCLK 在 GPIO 上以 1.41MHz 正常翻转；I2S 配置与一份**实测能出声**的
   参考例程逐项一致（Philips/16bit/无 MCLK）。
   真正的坑在硬件侧：**功放从 3.3V 轨取电**（峰值 400–800mA，和 WiFi 峰值叠加
   把轨拉塌 → 载波被压扁 → "嗤嗤"声 + 只在开关瞬间有声），**以及模块 GND 与
   ESP32 GND 的共地/测量参考点**。
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

## 许可

- 项目代码：MIT
- minimp3：CC0/public domain（lieff/minimp3）
