#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
translate_comments.py —— 把源码里残留的英文注释换成中文

只替换"整块都是英文"的注释；已经是中文的注释不动。
保持文件的 UTF-8 无 BOM 和 CRLF 换行不变。
"""

import os
import re
import sys

MAIN = r"f:\ESP32-WROOM\esp32-fm\esp32_max98357_fm\main"

# 英文注释块 -> 中文注释块（见后面的 MAP_UPDATE）
MAP = {
    "/* ---------------- radio task ---------------- */":
        "/* ================= 电台任务 ================= */",
    "/* let the queue drain so the tone is fully heard */":
        "/* 让队列放完，测试音才完整 */",
    "/* near full scale: the amp adds 9-15 dB on top of this */":
        "/* 接近满幅：功放还会再叠加 9~15dB 增益 */",
    "/* near full scale: the pwm gpio can source ~40mA, so keep the volume * moderate when driving a low impedance headphone */":
        "/* 接近满幅：PWM 的 GPIO 最多只能出 ~40mA，驱动低阻耳机时幅度要收敛一点 */",
    "/* the track ended normally -> straight to the next one */":
        "/* 这首正常播完了 -> 直接下一首 */",
    "/* the whole list is done -> the radio task reconnects and * plays it again from the top */":
        "/* 整个歌单播完 -> 电台任务会重连，从头再来一遍 */",
    "/* A volume set at runtime (\"vol\" command) is remembered, so it survives * a reboot. The kconfig value is only the fallback. */":
        "/* 运行时用 \"vol\" 命令设的音量会被记住，重启后仍然有效；Kconfig 的值只是兜底。 */",
    "/* A stored count means the user added or deleted stations - it wins * over the compiled-in default list. */":
        "/* NVS 里存过个数 = 用户加过/删过电台，它优先于编译进固件的默认表。 */",
    "/* Wait for the network instead of hammering DNS without an ip. */":
        "/* 没 IP 就等着，别去空敲 DNS。 */",
    "/* Give the audio buffers back to the heap - the bluetooth stack * may need that memory now. */":
        "/* 把音频缓冲还给堆 —— 蓝牙栈现在可能需要这块内存。 */",
    "/* interrupted by the user or a hard error */":
        "/* 被用户打断，或者出了硬错误 */",
    "/* the i2s consumer is too slow - drop the oldest samples */":
        "/* I2S 消费得太慢 —— 丢掉最老的采样 */",
    "/* ---------------- i2s pin maps ---------------- */":
        "/* ================= I2S 引脚映射表 ================= */",
    "/* shift the following stations down so the list stays packed */":
        "/* 后面的电台往前挪，保证表是紧凑的 */",
    "/* Stop the current stream first so the tone runs immediately. */":
        "/* 先停掉正在播的流，测试音才能立刻响。 */",
    "/* no name given: take the last path element of the url */":
        "/* 没给名字：取 URL 末段当名字 */",
    "/* first free slot */":
        "/* 找第一个空位 */",
    "/* ask for the name (the result arrives in the gap callback) */":
        "/* 问设备名（结果在 gap 回调里送到） */",
    "/* our sbc capability, built as raw bytes (see the comment above) */":
        "/* 我们的 SBC 能力，按原始字节拼（见上面的说明） */",
    "/* ---------------- the stream endpoint we offer ---------------- */":
        "/* ================= 我们对外提供的流端点 ================= */",
    "/* ---------------- status LED ---------------- */":
        "/* ================= 状态指示灯 ================= */",
    "/* ---------------- shared i2s output ----------------":
        "/* ================= 共用的 I2S 输出 =================",
    "/* How many stations the list can hold. The compiled-in defaults fill the * first few slots, the rest are added at runtime with the \"add\" command. */":
        "/* 电台表最多能放几个。编译进来的默认电台占前几个位置，其余的用 \"add\" 命令在运行时加。 */",
    "/* Single producer (the bluetooth task) / single consumer (the audio task). * The indices are only written by their owner, so no lock is needed. */":
        "/* 单生产者（蓝牙任务）/ 单消费者（音频任务）。读写指针各自只由自己的主人写，所以不需要加锁。 */",
    "/* The ring buffer lives in the heap: it is only needed while a phone is * connected, and the radio player may want that memory. */":
        "/* 环形缓冲放在堆上：只有手机连着时才需要，电台播放器可能想要这块内存。 */",
    "/* The bluetooth task hands us the decoded pcm. It must return fast, so * the samples only go into the ring buffer here. */":
        "/* 蓝牙任务把解码好的 PCM 交给我们。这个回调必须很快返回，所以这里只把采样放进环形缓冲。 */",
    "/* Always log the reason: silent failures on first connect (wrong * password, ap not found) are impossible to diagnose otherwise. * Rate limited: first few always, then at most one per 5 s so a * fast reconnect loop cannot flood the console. */":
        "/* 一定要打印断开原因：第一次连不上（密码错、找不到 AP）如果静默失败，根本没法排查。限流：前几次都打，之后最多 5 秒一条，免得疯狂重连把控制台刷爆。 */",
    "/* Diagnostics only - never prints the password itself. 'I' is 0x49, so a * stray character at the end is visible in the hex tail. */":
        "/* 只用于诊断 —— 绝不会打印密码本身。'I' 是 0x49，所以末尾如果混进了杂字符，从十六进制尾巴能看出来。 */",
    "/* Start wifi AFTER the config is in place. STA_START then triggers * esp_wifi_connect() from the event handler (the connect() call above * fails silently with ESP_ERR_WIFI_NOT_START before this point). */":
        "/* 必须在配置就位之后才启动 WiFi：STA_START 事件会触发事件处理里的 esp_wifi_connect()（在启动之前调 connect() 会静默失败，报 ESP_ERR_WIFI_NOT_START）。 */",
    "/* Power save off. The default min-modem mode sleeps between beacons, * which adds 10+ ms of latency and jitter to every packet - audible as * stuttering on a continuous stream. The board runs on USB power, so * saving power buys nothing here. */":
        "/* 关掉省电模式。默认的 min-modem 会在信标之间休眠，给每个包加 10ms+ 的延迟和抖动，连续流听起来就是一顿一顿的。板子走 USB 供电，省电在这里没有任何好处。 */",
    "/* The gap (classic bluetooth) callback - used for the remote device name */":
        "/* GAP（经典蓝牙）回调 —— 用来读对端设备名 */",
    "/* Re-point the i2s peripheral at another pin triple without rebooting. * Used to find the wiring that actually works, and to survive a firmware * update that moves the pins. */":
        "/* 不重启就把 I2S 换到另一组引脚。用来找出真正能出声的接线，也用来应对\"固件升级把引脚改了\"。 */",
    "/* Each new stream (station switch / reconnect) may re-lock the I2S clock to * the stream's sample rate - the reconfiguration happens on the very first * frame, before any audio is flowing, so no DMA interrupt race is possible. * Mid-stream rate changes keep the clock and only log (rate limited). */":
        "/* 每条新流（换台/重连）都可能把 I2S 时钟重新锁到这条流的采样率 —— 重新配置发生在第一帧、还没有数据流动的时候，所以不会出现和 DMA 中断抢资源的问题。一条流播到一半再变采样率就只打日志、不动时钟（限流）。 */",
    "/* A 404 does not always mean a dead url: qingting's cdn * throttles quick reconnects (the first attempt of this very * url succeeded). Back off long and only give up after * several consecutive 404s. */":
        "/* 404 不一定代表地址废了：蜻蜓的 CDN 会限制快速重连（这个地址第一次是能连上的）。所以退避久一点，连着几次 404 才放弃。 */",
    "/* The server ignored the Range header (or we did not send one): * the body starts at the beginning, so the offset must be reset. */":
        "/* 服务器忽略了 Range 头（或者我们本来就没发）：正文从头开始，所以偏移必须清零。 */",
    "/* New stream: the loudness estimator starts from scratch, otherwise the * level of the previous station would decide how the new one starts. */":
        "/* 新的流：响度估计器从头开始，否则新电台的起始响度会由上一个电台决定。 */",
    "/* mp3dec_decode_frame() keeps a ~16KB scratch buffer on the stack * (mp3dec_scratch_t), plus the http client call chain. 32KB is safe. */":
        "/* mp3dec_decode_frame() 会在栈上留 ~16KB 的暂存空间（mp3dec_scratch_t），再加上 HTTP 客户端的调用链。32KB 栈是安全的。 */",
    "/* The sd control pin idles as a high-z input, so the amplifier stays in * its default mode (module pull-up wins) until the user overrides it * with the \"sd\" command. */":
        "/* SD 控制脚默认是高阻输入，所以功放保持模块自己的默认模式（模块内部上拉优先），直到用户用 \"sd\" 命令去覆盖。 */",
    "/* A pin map that was chosen at runtime (\"pins\" command) wins over the * compiled-in default, so a firmware update cannot silence a working * amplifier. The stored pins are matched against the table, so the * selection keeps its meaning even if the table is reordered. */":
        "/* 运行时用 \"pins\" 命令选过的引脚优先于编译进固件的默认值，这样升级固件也不会把能出声的功放改哑。存的是引脚本身，去表里比对，所以以后重排映射表也不会让选择含义漂移。 */",
    "/* The radio task must not write to the i2s channel while it is being * reconfigured - stop the stream first and give it a moment to leave * the write call. */":
        "/* 重新配置 I2S 的时候电台任务不能往里写 —— 先停播，等它退出写操作。 */",
    "/* Resolved to wifi_station.c at link time (same component). If absent, * the network is simply considered down. */":
        "/* 链接时解析到同一组件里的 wifi_station.c。如果那里没有这个符号，就认为网络没连上。 */",
    "/* The a2dp sbc codec capability (a2dp spec 4.3.2) is a raw 4 byte * structure. It is built as bytes so that the bit layout is unambiguous: */":
        "/* A2DP 的 SBC 能力（A2DP 规范 4.3.2）是一个 4 字节的原始结构，按字节拼可以保证位序没有歧义： */",
    " *   octet 0: bits 0-3 sample rate  (16k/32k/44.1k/48k) *            bits 4-7 channel mode  (mono/dual/stereo/joint stereo) *   octet 1: bits 0-3 block length (4/8/12/16) *            bits 4-5 subbands      (4/8/12) *            bits 6-7 allocation    (snr/loudness) *   octet 2: minimum bitpool *   octet 3: maximum bitpool":
        " *   第 0 字节: bit 0-3 采样率 (16k/32k/44.1k/48k) *             bit 4-7 声道模式 (单/双/立体声/联合立体声) *   第 1 字节: bit 0-3 块长 (4/8/12/16) *             bit 4-5 子带数 (4/8/12) *             bit 6-7 分配方式 (信噪比/响度) *   第 2 字节: 最小 bitpool *   第 3 字节: 最大 bitpool",
    " * Reason: the bluetooth stack needs about 40 kB of static ram and the * linker only hands out ~124 kB of static dram on the esp32. Moving these * buffers to the heap (they are only needed while a stream is playing) * solves the link error and gives the memory back to the bluetooth stack * whenever the radio is not playing. *":
        " * 原因：蓝牙栈要吃掉约 40KB 静态 RAM，而 ESP32 的链接器只给应用分约 124KB 静态 DRAM。把这些缓冲挪到堆上（它们只在播放时需要），既解决了链接错误，又能在电台不播的时候把内存还给蓝牙栈。 *",
    " * All three buffers are carved out of one allocation so that the heap * stays tidy (one block in, one block out). */":
        " * 三块缓冲从同一次申请里切出来，堆才干净（一次进、一次出）。 */",
    "/* Diagnostic: square wave straight through the audio path - no network, no * decoder. Used to isolate \"silent speaker\" hardware problems. * duration_ms == 0 means \"continuous until stop\" - needed for multimeter * measurements, a 2 s burst is too short to read a meter. */":
        "/* 诊断用：方波直接走音频通路，不联网、不解码，用来区分\"喇叭不响\"这类硬件问题。duration_ms == 0 表示一直响到 stop —— 万用表测量需要，2 秒太短来不及读数。 */",
    "/* Proof that the i2s dma really moved the samples out. If this is 0 the * fault is on the esp32 side, not on the amplifier. */":
        "/* 证明 I2S 的 DMA 真的把采样搬走了：如果这里是 0，问题在 ESP32 这边，不是功放。 */",
    "/* Backpressure: never read from the socket while the stream buffer is * (nearly) full. The tcp receive window closes by itself, the server * waits, and not a single mp3 byte is thrown away. *":
        "/* 反压：流缓冲快满时绝不去读 socket。TCP 接收窗口会自己关掉，* 服务器会等，一个 MP3 字节都不会被丢掉。 *",
    " * Throwing bytes away would break the frame alignment - the decoder * then reports \"mp3 decode error, skipping frame\" and the glitch is * clearly audible. decode_loop() is paced by the blocking i2s write, * so it frees the buffer exactly at the speed the audio is played. */":
        " * 扔掉字节会破坏帧对齐 —— 解码器会报\"mp3 decode error, skipping frame\"，* 杂音非常明显。decode_loop() 由阻塞的 I2S 写操作控制节奏，* 腾缓冲的速度正好等于音频播放的速度。 */",
    "/* Backpressure: the ring must not overflow. In the i2s build the blocking * dma write provides this for free; with a register based output the * producer would run ahead, the ring would overflow and samples would be * dropped silently - heard as stuttering. */":
        "/* 反压：环形缓冲不能溢出。I2S 版本靠阻塞的 DMA 写天然就有这个保证；* 寄存器直驱的话生产者会跑在前面，环溢出会静默丢采样 —— 听感就是一顿一顿。 */",
    "/* NOTE: the tick rate is 100Hz, so pdMS_TO_TICKS(2) would be 0 ticks * and vTaskDelay(0) only yields - the task would spin, never block * and starve the idle task (task watchdog panic). One tick = 10ms. */":
        "/* 注意：tick 频率是 100Hz，所以 pdMS_TO_TICKS(2) 会得到 0 tick，* 而 vTaskDelay(0) 只是让出 CPU —— 任务会空转、永不阻塞，* 把 idle 任务饿死（触发任务看门狗 panic）。1 个 tick = 10ms。 */",
    "/* Strongest drive: a headphone is a low impedance load and the gpio * output impedance (tens of ohms) would eat most of the swing. */":
        "/* 最强驱动能力：耳机是低阻负载，GPIO 自身的输出阻抗（几十欧）会吃掉大部分摆幅。 */",
    "/* Without a valid timer the channel is dead and a duty update would * hang inside the driver's critical section (interrupt wdt panic). */":
        "/* 定时器没配好，通道就是死的，更新占空比会卡在驱动的临界区里（触发中断看门狗 panic）。 */",
    "/* sets the pad enable and clears the continuous wave mode */":
        "/* 打开 pad 输出并清掉连续波模式 */",
    "/* exactly the two writes the ledc driver does, minus the busy-wait */":
        "/* 就是 LEDC 驱动的那两行寄存器写，只是不带忙等 */",
    "/* Resample interleaved stereo pcm down to AUDIO_SAMPLE_RATE mono and queue. */":
        "/* 把交错的立体声 PCM 重采样成 AUDIO_SAMPLE_RATE 单声道并入队。 */",
    "/* One sample per hardware timer interrupt. */":
        "/* 每次硬件定时器中断取一个采样。 */",
    "/* Free space in the ring buffer. (head - tail) is the number of *stored* * samples; one slot is always kept empty, so the usable free space is * MASK - stored. */":
        "/* 环形缓冲的剩余空间。(head - tail) 是已存放的采样数；* 有一个槽位永远留空，所以可用空间 = MASK - 已存数。 */",
    "/* Block until the ring has at least \"need\" free samples. The isr drains the * ring at the sample rate, so this paces the producer to the audio clock. * The network jitter is then absorbed by the mp3 stream buffer instead of * showing up as dropouts in the audio. */":
        "/* 等到环形缓冲至少有 \"need\" 个空位。中断按采样率取数据，所以这里把* 生产者压到音频时钟的节奏上。网络抖动则由 MP3 流缓冲吸收，不会变成声音里的断点。 */",
    "/* If this is 0 the * fault is on the esp32 side, not on the amplifier. */":
        "/* 如果这里是 0，问题在 ESP32 这边，不是功放。 */",
    "/* The user volume is stored in the nvs so that \"vol 40\" survives a reboot. * The kconfig value (CONFIG_RADIO_INITIAL_VOLUME) is only the fallback. */":
        "/* 用户音量存在 NVS 里，\"vol 40\" 重启后仍然有效；Kconfig 的值只是兜底。 */",
    "/* The number of stations is stored as well, so that stations added at * runtime survive a reboot and deleted ones stay deleted. */":
        "/* 电台个数也存起来：运行时加的电台重启后还在，删掉的也保持删掉。 */",
    "/* The i2s pin map is remembered so that a firmware update which changes the * default pins cannot silence an amplifier that is already working. *":
        "/* I2S 引脚映射会被记住，这样改掉默认引脚的固件升级也不会把* 已经能出声的功放改哑。 *",
    " * The pins themselves are stored (not the table index) - so reordering or * extending the table later can never silently change the meaning of an * already stored selection. */":
        " * 存的是引脚本身（不是表里的序号）—— 以后重排或扩充映射表，* 也不会悄悄改变已经保存的选择的含义。 */",
    "/* What the server says it is actually serving - this is what explains * \"same url, different content\": the cdn may answer with a different * source, a different bitrate or a redirect to another edge node. */":
        "/* 服务端自己说它在放什么 —— 这正是\"同一个地址内容不一样\"的线索：* CDN 可能给了另一个源、另一个码率，或者重定向到别的边缘节点。 */",
    "/* Fetches a text playlist and parses it into s_pl_urls / s_pl_titles. * Returns the number of entries (0 = not a playlist / failed). */":
        "/* 抓取文本歌单并解析进 s_pl_urls / s_pl_titles。返回条目数（0 = 不是歌单或失败）。 */",
    "/* Percent-encodes the characters that must not appear literally in a url * path (space, non-ascii, control chars, ...). Sequences that are already * encoded (%xx) are left alone, so a playlist that contains encoded names * does not get double encoded. */":
        "/* 把 URL 路径里不能直接出现的字符做百分号编码（空格、非 ASCII、控制符…）。* 已经是 %xx 的序列原样保留，所以不会二次编码。 */",
    "/* Resolves a playlist entry against the playlist url: absolute urls are taken * as they are, relative ones are appended to the directory of the playlist * (that is what a local music folder needs). */":
        "/* 把歌单里的一条目解析成完整地址：绝对地址原样使用，相对地址按歌单所在目录拼接（本地音乐文件夹就靠这个）。 */",
    "/* Byte offset of the current file plus its total length (-1 = unknown, i.e. * a live stream). Only files with a known length are resumed with a Range * request - a live stream must always be read from the start. */":
        "/* 当前文件的字节偏移 + 文件总长（-1 = 未知，也就是直播流）。* 只有长度已知的文件才发 Range 续传请求 —— 直播流必须从头读。 */",
    "/* The url itself is dead (qingting rotates channel ids): retrying * the same address forever is pointless. */":
        "/* 地址本身废了（蜻蜓会轮换频道 id）：一直重试同一个地址没有意义。 */",
    "/* Partial content: the server honoured our Range request. */":
        "/* 206 Partial Content：服务器接受了我们的 Range 请求。 */",
    "/* only files are resumed, a live stream has no length */":
        "/* 只有文件能续传，直播流没有长度 */",
    "/* decoder cannot keep up: drop oldest bytes */":
        "/* 解码跟不上：丢掉最老的字节 */",
    "/* plain audio payload */":
        "/* 这里是纯音频数据 */",
    "/* Plays a station: either a single stream or, if the url points to a .m3u * playlist, every entry of that playlist one after another. */":
        "/* 播放一个电台：普通流就播一条；地址是 .m3u 歌单就按顺序逐首播。 */",
    "/* The bluetooth player owns the amplifier while a phone is connected. */":
        "/* 手机连着的时候，蓝牙播放器占着功放。 */",
    "/* The audio buffers are taken from the heap while a stream is playing * (the bluetooth stack needs the same memory). */":
        "/* 播放期间音频缓冲从堆上取（蓝牙栈需要同一块内存）。 */",
    "/* Snapshot name/url under the mutex: the console task may rewrite them * (url/name commands) while we are connecting. Reading a half-written * string can crash the http client url parser. */":
        "/* 在互斥锁下取名字/地址的快照：控制台任务可能正在改它们（url/name 命令），* 读到写了一半的字符串会让 HTTP 客户端的 URL 解析器崩溃。 */",
    "/* A different station means a different file: forget the resume point. */":
        "/* 换台 = 换文件，断点作废。 */",
    "/* the track ended normally -> straight to the next one */":
        "/* 这首正常播完了 -> 直接下一首 */",
    "/* the whole list is done -> the radio task reconnects and * plays it again from the top */":
        "/* 整个歌单播完 -> 电台任务会重连，从头再来一遍 */",
    "/* playlist empty or unreachable, treating as a plain stream */":
        "/* 歌单是空的或抓不到，当成普通流处理 */",
    "/* interrupted by the user or a hard error */":
        "/* 被用户打断，或者出了硬错误 */",
    "/* Diagnostic: square wave straight through the audio path - no network, no * decoder. Used to isolate \"silent speaker\" hardware problems. * duration_ms == 0 means \"continuous until stop\" - needed for multimeter * measurements, a 2 s burst is too short to read a meter. */":
        "/* 诊断用：方波直接走音频通路，不联网、不解码，用来区分\"喇叭不响\"这类硬件问题。duration_ms == 0 表示一直响到 stop —— 万用表测量需要，2 秒太短来不及读数。 */",
    "/* Proof that the i2s dma really moved the samples out. If this is 0 the * fault is on the esp32 side, not on the amplifier. */":
        "/* 证明 I2S 的 DMA 真的把采样搬走了：如果这里是 0，问题在 ESP32 这边，不是功放。 */",
    "/* near full scale: the amp adds 9-15 dB on top of this */":
        "/* 接近满幅：功放还会再叠加 9~15dB 增益 */",
    "/* near full scale: the pwm gpio can source ~40mA, so keep the volume * moderate when driving a low impedance headphone */":
        "/* 接近满幅：PWM 的 GPIO 最多只能出 ~40mA，驱动低阻耳机时幅度要收敛一点 */",
    "/* let the queue drain so the tone is fully heard */":
        "/* 让队列放完，测试音才完整 */",
    "/* ---- 6. WiFi ---- */":
        "/* ---- 6. WiFi ---- */",

    # ---- 整块的多行注释 ----
    "/* The big buffers live in the heap, not in .bss. * Reason: the bluetooth stack needs about 40 kB of static ram and the * linker only hands out ~124 kB of static dram on the esp32. Moving these * buffers to the heap (they are only needed while a stream is playing) * solves the link error and gives the memory back to the bluetooth stack * whenever the radio is not playing. * All three buffers are carved out of one allocation so that the heap * stays tidy (one block in, one block out). */":
        "/* 大缓冲放在堆上，而不是 .bss。 * 原因：蓝牙栈要吃掉约 40KB 静态 RAM，而 ESP32 的链接器只给应用分约 124KB 静态 DRAM。 * 把这些缓冲挪到堆上（它们只在播放时需要），既解决了链接错误，又能在电台不播的时候 * 把内存还给蓝牙栈。 * 三块缓冲从同一次申请里切出来，堆才干净（一次进、一次出）。 */",

    "/* I2S pin maps. The default one is the wiring that is known to work; the * alternative is for a rewired module. The selection is stored in nvs so a * reflash can never silence the amplifier again. */":
        "/* I2S 引脚映射表。默认那套是【已实测能出声】的接线，另一套给重新接过线的人用。 * 选择结果存进 NVS，所以任何一次重烧都不会再把功放改哑。 */",

    "/* ================= direct audio output (pwm or dac) ================= */ /* Two backends, one engine: *   CONFIG_RADIO_AUDIO_PWM : ledc pwm on a gpio, 100 kHz carrier, 8 bit duty *   CONFIG_RADIO_AUDIO_DAC : esp32 internal 8 bit dac (gpio25 / gpio26) * Both are sampled by a hardware timer isr (gptimer, 16 kHz): one sample per * interrupt, one register write per sample. No esp_timer task is involved, * so the wifi stack cannot distort the audio timing and nothing can starve * the idle task (that was the cause of the task/interrupt watchdog panics). * The ledc driver is deliberately NOT called from the isr: ledc_update_duty() * busy-waits for the hardware to clear the duty_start bit inside an * interrupt-disabled critical section (ledc_ll.h:571). At 16 kHz that hung * the cpu and triggered the watchdog. Writing the register directly is safe * here: the pwm period is 10 us while the sample period is 62 us, so a new * duty update can never overlap the previous one and the wait is never * needed. * Wiring (pwm): gpio -> 22R -> headphone L+R in parallel, gnd -> gnd. * Wiring (dac): gpio -> series cap (1..100uF) -> headphone L+R, gnd -> gnd. */":
        "/* ================= 直驱输出模式（PWM / DAC，仅诊断用） ================= */ /* 两个后端，一套引擎： *   CONFIG_RADIO_AUDIO_PWM : LEDC PWM 输出到 GPIO，100kHz 载波，8 位占空比 *   CONFIG_RADIO_AUDIO_DAC : ESP32 内部 8 位 DAC（GPIO25 / GPIO26） * 两者都由硬件定时器中断（gptimer，16kHz）采样：每次中断取一个采样、写一次寄存器。 * 完全不经过 esp_timer 任务，所以 WiFi 协议栈不会影响音频时序，也不会把 idle 任务饿死 * （那正是任务/中断看门狗 panic 的原因）。 * 特意【不】在中断里调 ledc_update_duty()：它会在关中断的临界区里忙等硬件的 duty_start 标志 * （ledc_ll.h:571），16kHz 下会把 CPU 卡死并触发看门狗。直接写寄存器是安全的： * PWM 周期是 10us 而采样周期是 62us，新的占空比更新不可能和上一次重叠，永远不需要等。 * 接线（PWM）：gpio -> 22R -> 耳机 L+R 并联，gnd -> gnd。 * 接线（DAC）：gpio -> 隔直电容（1~100uF）-> 耳机 L+R，gnd -> gnd。 */",

    "/* ---------------- backend: one sample -> one register write ---------------- */":
        "/* ================= 后端：一个采样 -> 一次寄存器写 ================= */",

    "/* ---------------- engine ---------------- */":
        "/* ================= 引擎 ================= */",

    "/* ---------------- shared i2s output ---------------- * The bluetooth player writes into the same i2s channel as the radio, so * only one of them may own it at a time. The bluetooth player takes the * channel when a phone connects (the radio is stopped then) and gives it * back when the phone disconnects. */":
        "/* ================= 共用的 I2S 输出 ================= * 蓝牙和电台写的是同一个 I2S 通道，同一时刻只能有一个占着： * 手机连上时蓝牙接管（电台停），手机断开时电台拿回来。 */",

    "/* Writes raw pcm to the i2s. Returns false on error. */":
        "/* 往 I2S 写原始 PCM。出错返回 false。 */",

    "/* Forces the i2s clock to a given rate (the bluetooth stream is always * 44100 Hz). The radio unlocks the rate again for its next stream. */":
        "/* 强制把 I2S 时钟设成指定采样率（蓝牙流固定 44.1kHz）。 * 电台在下一条流开始时会重新解锁这个速率。 */",

    "/* ---------------- amplifier SD pin control (diagnostic) ---------------- */":
        "/* ================= 功放 SD 脚控制（诊断用） ================= */",

    "/* ---------------- PCM output ---------------- */":
        "/* ================= PCM 输出 ================= */",

    "/* ---------------- loudness (auto gain) ---------------- * Why this is not a simple peak normalizer: * A peak based normalizer reacts to every single transient - one drum hit * in a 26 ms frame drops the gain for that whole frame, and with a slow * release it takes almost a second to come back. That is exactly the * \"the volume swings with the music\" effect. * The ear judges loudness by the short term rms, so that is what the gain * follows now, and the gain is allowed to move slowly in BOTH directions * (max 1 dB per frame = 38 dB/s). The target rms (-15 dBFS) leaves * enough head room for the peaks, the clamp at the end is the last resort * only. */":
        "/* ================= 响度（自动增益） ================= * 为什么不用简单的峰值归一化： * 峰值归一化会对每一个瞬态（鼓点）做出反应 —— 一帧 26ms 里有个鼓点，整帧的增益就被砸下去， * 再慢慢爬回来，听感就是\"音量跟着音乐呼吸\"。 * 现在改成跟【平滑后的短时 RMS】（≈听感响度）走，而且变快变慢都是 38dB/s（双向限速）， * 所以没有呼吸感。目标 -15dBFS 留足了峰值余量，最后的限幅只是最后一道保险。 */",

    "/* ---------------- station list (runtime editable, persisted in NVS) ---------------- */":
        "/* ================= 电台表（运行时可改，存 NVS） ================= */",

    "/* ---------------- HTTP / ICY ---------------- */":
        "/* ================= HTTP / ICY ================= */",

    "/* ---------------- http client helper ---------------- */":
        "/* ================= HTTP 客户端辅助函数 ================= */",

    "/* ---------------- playlists (.m3u) ---------------- */":
        "/* ================= 播放列表（.m3u） ================= */",

    "/* ---------------- resume state ---------------- */":
        "/* ================= 断点续传状态 ================= */",

    "/* Streams a single url. * Returns ESP_OK when the user interrupted (stop/switch), ESP_FAIL on a * hard error, ESP_ERR_NOT_FINISHED when the server closed the stream. */":
        "/* 播一条流。 * 返回 ESP_OK = 被用户打断（停止/换台），ESP_FAIL = 硬错误， * ESP_ERR_NOT_FINISHED = 服务器把流关了（一首播完/直播断了）。 */",

    "/* The file is already complete -> start over from the top. */":
        "/* 文件已经下完了 -> 从头再播一遍。 */",

    "/* Resume a partially downloaded file instead of starting over. Only * possible when the server told us the total length (a file, not a live * stream - a live stream has no length and must be read from the start). */":
        "/* 续传下了一半的文件，而不是从头再来。 * 只有服务器给了总长度才能这么做（是文件而不是直播流 —— * 直播流没有长度，必须从头读）。 */",

    # ---- bt_player.c ----
    "/* The a2dp sbc codec capability (a2dp spec 4.3.2) is a raw 4 byte * structure. It is built as bytes so that the bit layout is unambiguous: *   octet 0: bits 0-3 sample rate  (16k/32k/44.1k/48k) *            bits 4-7 channel mode  (mono/dual/stereo/joint stereo) *   octet 1: bits 0-3 block length (4/8/12/16) *            bits 4-5 subbands      (4/8/12) *            bits 6-7 allocation    (snr/loudness) *   octet 2: minimum bitpool *   octet 3: maximum bitpool */":
        "/* A2DP 的 SBC 能力（A2DP 规范 4.3.2）是一个 4 字节的原始结构， * 按字节拼可以保证位序没有歧义： *   第 0 字节: bit 0-3 采样率 (16k/32k/44.1k/48k) *             bit 4-7 声道模式 (单/双/立体声/联合立体声) *   第 1 字节: bit 0-3 块长 (4/8/12/16) *             bit 4-5 子带数 (4/8/12) *             bit 6-7 分配方式 (信噪比/响度) *   第 2 字节: 最小 bitpool *   第 3 字节: 最大 bitpool */",

    "/* The bluetooth task hands us the decoded pcm. It must return fast, so * the samples only go into the ring buffer here. */":
        "/* 蓝牙任务把解码好的 PCM 交给我们。这个回调必须很快返回，所以这里只把采样放进环形缓冲。 */",

    "/* wake up the audio task */":
        "/* 唤醒音频任务 */",
    "/* wait for the first samples */":
        "/* 等第一批采样 */",
    "/* the radio may still be writing to the i2s - wait for it */":
        "/* 电台可能还在往 I2S 写 —— 等它停下 */",
    "/* the bt stream runs at a fixed rate, the i2s clock follows */":
        "/* 蓝牙流的速率是固定的，I2S 时钟跟着它走 */",
    "/* the phone takes over the amplifier */":
        "/* 手机接管功放 */",
    "/* The pcm ring buffer comes from the heap - the radio may have * been using that memory until a moment ago. */":
        "/* PCM 环形缓冲从堆上申请 —— 就在刚才电台可能还占着那块内存。 */",
    "/* ---- 3. A2DP sink ---- */":
        "/* ---- 3. A2DP sink（接收端） ---- */",
    "/* The audio task moves the pcm from the ring buffer into the i2s. */":
        "/* 音频任务：把环形缓冲里的 PCM 搬进 I2S。 */",
    "/* The a2dp state callback */":
        "/* A2DP 状态回调 */",
    "/* ---------------- amplifier SD pin control (diagnostic) ---------------- */":
        "/* ================= 功放 SD 脚控制（诊断用） ================= */",
    "/* ---------------- PCM output ---------------- */":
        "/* ================= PCM 输出 ================= */",
    "/* ---------------- station list (runtime editable, persisted in NVS) ---------------- */":
        "/* ================= 电台表（运行时可改，存 NVS） ================= */",
    "/* ---------------- HTTP / ICY ---------------- */":
        "/* ================= HTTP / ICY ================= */",
    "/* ---------------- http client helper ---------------- */":
        "/* ================= HTTP 客户端辅助函数 ================= */",
    "/* ---------------- playlists (.m3u) ---------------- */":
        "/* ================= 播放列表（.m3u） ================= */",
    "/* ---------------- resume state ---------------- */":
        "/* ================= 断点续传状态 ================= */",
    "/* ---------------- backend: one sample -> one register write ---------------- */":
        "/* ================= 后端：一个采样 -> 一次寄存器写 ================= */",
    "/* ---------------- engine ---------------- */":
        "/* ================= 引擎 ================= */",

    # ---- 第二轮：按脚本打印出来的准确 key ----
    "/* ================= direct audio output (pwm or dac) ================= */":
        "/* ================= 直驱输出模式（PWM / DAC，仅诊断用） ================= */",
    "/* start over with a clean state so the change is heard immediately */":
        "/* 状态清零重来，让改动立刻能听出来 */",
    "/* Short term rms (the sum is exact, the mean is taken in float). */":
        "/* 短时 RMS（平方和用整数精确累加，均值用浮点算） */",
    "/* the first frame of a stream: jump straight to the right level * instead of fading in over a second */":
        "/* 一条流的第一帧：直接跳到目标电平，而不是用一秒淡入 */",
    "/* Rate limited in both directions: no pumping, no breathing. */":
        "/* 双向都限速：不会有\"抽吸感\"，也不会有\"呼吸感\" */",
    "/* Loudness normalization, then the user volume. */":
        "/* 先做响度归一化，再乘用户音量 */",
    "/* Decode as many MP3 frames as the stream buffer holds. */":
        "/* 流缓冲里有多少 MP3 帧就解多少帧 */",
    "/* Skip an ID3v2 tag at the start of a stream. */":
        "/* 跳过流开头的 ID3v2 标签 */",
    "/* returns offset of the frame inside the buffer, frame_bytes = frame size + padding */":
        "/* 返回这一帧在缓冲里的偏移，frame_bytes = 帧长 + 填充 */",
    "/* The i2s pin map is remembered so that a firmware update which changes the * default pins cannot silence an amplifier that is already working. * The pins themselves are stored (not the table index) - so reordering or * extending the table later can never silently change the meaning of an * already stored selection. */":
        "/* I2S 引脚映射会被记住，这样改掉默认引脚的固件升级也不会把已经能出声的功放改哑。存的是引脚本身（不是表里的序号）—— 以后重排或扩充映射表，也不会悄悄改变已经保存的选择的含义。 */",
    "/* ================= HTTP / ICY ================= */":
        "/* ================= HTTP / ICY（ICY 元数据） ================= */",
    "/* IDF 6.x removed esp_http_client's skip_verify: https streams must * verify the server. The mbedTLS certificate bundle (enabled in * sdkconfig, CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) provides the root CAs. */":
        "/* IDF 6.x 去掉了 esp_http_client 的 skip_verify：https 流必须校验服务器证书。* 根证书由 mbedTLS 证书包提供（sdkconfig 里 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 已打开）。 */",
    "/* absolute path on the same host */":
        "/* 同一主机上的绝对路径 */",
    "/* relative: use the directory of the playlist */":
        "/* 相对路径：按歌单所在目录拼接 */",
    "/* Parse: \"#EXTINF:<seconds>,<title>\" describes the next url. */":
        "/* 解析：\"#EXTINF:<秒数>,<标题>\" 描述紧随其后的那个地址 */",
    "/* the last line was cut off by the buffer limit -> drop it */":
        "/* 最后一行被缓冲区截断了（半行）-> 丢弃 */",
    "/* Backpressure: never read from the socket while the stream buffer is * (nearly) full. The tcp receive window closes by itself, the server * waits, and not a single mp3 byte is thrown away. * Throwing bytes away would break the frame alignment - the decoder * then reports \"mp3 decode error, skipping frame\" and the glitch is * clearly audible. decode_loop() is paced by the blocking i2s write, * so it frees the buffer exactly at the speed the audio is played. */":
        "/* 反压：流缓冲快满时绝不去读 socket。TCP 接收窗口会自己关掉，服务器会等，一个 MP3 字节都不会被丢掉。扔掉字节会破坏帧对齐 —— 解码器会报\"mp3 decode error, skipping frame\"，杂音非常明显。decode_loop() 由阻塞的 I2S 写操作控制节奏，腾缓冲的速度正好等于音频播放的速度。 */",
    "/* ------------------------------------------------------------------ * The bluetooth task hands us the decoded pcm. It must return fast, so * the samples only go into the ring buffer here. * ------------------------------------------------------------------ */":
        "/* ------------------------------------------------------------------ * 蓝牙任务把解码好的 PCM 交给我们。这个回调必须很快返回，* 所以这里只把采样放进环形缓冲。 * ------------------------------------------------------------------ */",
    "/* ------------------------------------------------------------------ * The audio task moves the pcm from the ring buffer into the i2s. * ------------------------------------------------------------------ */":
        "/* ------------------------------------------------------------------ * 音频任务：把环形缓冲里的 PCM 搬进 I2S。 * ------------------------------------------------------------------ */",
    "/* ------------------------------------------------------------------ * The a2dp state callback * ------------------------------------------------------------------ */":
        "/* ------------------------------------------------------------------ * A2DP 状态回调 * ------------------------------------------------------------------ */",
    "/* ------------------------------------------------------------------ * The gap (classic bluetooth) callback - used for the remote device name * ------------------------------------------------------------------ */":
        "/* ------------------------------------------------------------------ * GAP（经典蓝牙）回调 —— 用来读对端设备名 * ------------------------------------------------------------------ */",
}


def translate(path):
    """把一个文件里残留的英文注释块换成中文，返回替换了几处。"""
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")

    lines = text.split("\r\n")
    out = []
    i = 0
    count = 0
    while i < len(lines):
        s = lines[i].strip()
        # 只有以 /* 或 // 开头的行才是注释（行内的 *bck = ... 是代码，不是注释）
        if s.startswith("/*") or s.startswith("//"):
            # 收集整个注释块
            block = []
            j = i
            while j < len(lines):
                t = lines[j].strip()
                if t.startswith("/*") or t.startswith("*") or t.startswith("//"):
                    block.append(t)
                    if t.endswith("*/"):
                        j += 1
                        break
                    j += 1
                else:
                    break
            # 注释里的独立 "*" 行只是分隔线，不算进 key
            key = " ".join([b for b in block if b and b != "*"])
            en = re.search(r"[A-Za-z]{4,}", key) is not None
            cn = re.search(r"[\u4e00-\u9fff]", key) is not None
            if key in MAP:
                out.append(MAP[key])       # 换成中文（整块变成一行）
                count += 1
                i = j
                continue
            out.extend(block)              # 没在表里：原样保留
            i = j
            continue
        out.append(lines[i])
        i += 1

    new = "\r\n".join(out)
    if new != text:
        with open(path, "wb") as f:
            f.write(new.encode("utf-8"))  # UTF-8 无 BOM
    return count


def main():
    total = 0
    for name in ("radio_player.c", "bt_player.c", "wifi_station.c", "main.c"):
        p = os.path.join(MAIN, name)
        n = translate(p)
        print("%-18s 替换 %d 处" % (name, n))
        total += n
    print("合计 %d 处" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
