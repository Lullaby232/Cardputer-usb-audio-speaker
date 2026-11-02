# Cardputer Speaker · M5 Cardputer ADV 免驱 USB 声卡

把 M5Stack Cardputer ADV 插到电脑 USB 口，它就变成一只 **免驱的 USB 扬声器**：
声音从板载 ES8311 + 功放 + 扬声器出来，Windows / macOS / Linux 全部免装驱动。

> Turn an M5Stack Cardputer ADV into a driver-free USB Audio Class 2.0 speaker.
> Plug in, pick "Cardputer Speaker" in the system sound settings, done.

---

## 特性

- **真免驱**：标准 USB Audio Class 2.0，Windows 10 1703+ / macOS 10.11+ / Linux 原生识别
- **48 kHz / 16 bit / 立体声**，整片固件 8 MB，烧一次即可
- **屏幕实时监视**：连接状态、左右声道电平表（对数刻度）、本机音量、电脑侧音量、DAC 增益、缓冲水位
- **本机可调音量**：`-` / `=` 调 32 级软件音量，`[` / `]` 调 ES8311 模拟增益（−6 ~ +16 dB）
- **免按键进下载模式**：长按 G0 三秒即可重新烧录，不必拔线按住 BOOT
- **抗时钟漂移**：ADAPTIVE 播放端点 + 64 KB 环形缓冲 + 采样级丢补

## 烧录

整片固件在 `固件/usbspk-v1.5-8MB.bin`，烧到 **0x0**（不是 0x1000）。

**图形工具**：ESP32 Flash Download Tool → ESP32-S3 → Develop
→ 文件选 bin，地址 `0x0`，SPI 80MHz / DIO，FLASH SIZE 8MB → START

**命令行**：

```bash
esptool.exe --chip esp32s3 -p COM3 -b 921600 write_flash 0x0 usbspk-v1.5-8MB.bin
```

整片固件包含 bootloader（0x0）、分区表（0x8000）、otadata（0xe000）、
应用（0x10000，Huge APP 分区），所以一条命令烧 `0x0` 就够。

烧完**必须拔插一次 USB 线**才会启动（原因见「踩坑记录」第 1 条）。

## 使用

1. 插上 USB，等 2 秒，屏幕显示 `已连接 · 待机`
2. 电脑右下角喇叭图标 → 选择输出设备 **Cardputer Speaker**
3. 系统音量拉到 100%，播放任何声音

| 按键 | 作用 |
|---|---|
| `-` / `=` | 本机软件音量 −/+（32 级） |
| `[` / `]` | ES8311 模拟增益 −/+ 2 dB（−6 … +16 dB） |
| `m` | 静音开关 |
| `G0` 短按 | 静音开关 |
| `G0` 长按 3 秒 | 进入 USB 下载模式（重新烧录用） |
| `Fn` + 方向键 | 同上，兼容方向键习惯 |

> Cardputer 的 ←/→/↑/↓ 本身是 **Fn 组合键**，直接按只会输入字符。

### 屏幕布局

```
┌────────────────────────────────┐
│ Cardputer USB 免驱扬声器  v1.5 │  ← 标题栏
│ 已连接 · 待机        D:136 ok  │  ← 状态 + 描述符自检
│ L ████████████░░░░░░░░  □      │  ← 左声道电平（-48…0 dBFS）
│ R ██████████░░░░░░░░░░  □      │  ← 右声道电平 + CLIP 指示
│ 本机音量 100%                  │
│ ████████████████████████       │
│ 电脑 -23.0dB  DAC +6dB         │
│ 缓冲  35ms                     │
└────────────────────────────────┘
```

## 编译

源码在 `源码/usbspk/usbspk.ino`，用 Arduino IDE 或 arduino-cli 均可。

| 依赖 | 版本 |
|---|---|
| esp32 core | 3.3.12 |
| M5Cardputer | 1.1.1 |
| M5Unified | 0.2.29 |
| M5GFX | 0.2.36 |

```bash
arduino-cli lib install "M5Cardputer@1.1.1"
arduino-cli core install esp32:esp32@3.3.12
```

**开发板设置（关键）**：`ESP32S3 Dev Module`、Flash Size `8MB (64Mb)`、
分区 `Huge APP (3MB No OTA/1MB SPIFFS)`、
**USB Mode = `Hardware CDC and JTAG` 之外的 TinyUSB 模式（arduino-cli 里写 `USBMode=default`）**、
**USB CDC On Boot = `Default`（不自动初始化 CDC）**。

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=huge_app,USBMode=default,CDCOnBoot=default" \
  --libraries "<你的 libraries 目录>" --build-path build firmware/usbspk
```

生成的 `build/usbspk.ino.merged.bin` 就是 8 MB 整片镜像。

## 技术要点

| 环节 | 做法 |
|---|---|
| USB 协议栈 | 预编译的 TinyUSB（`libarduino_tinyusb.a`），Audio 类符号已包含 |
| 接口注册 | `tinyusb_enable_interface(USB_INTERFACE_AUDIO, len, cb)`，必须在 `tinyusb_init()` **之前** |
| 描述符 | 全部用 tinyusb 官方 `TUD_AUDIO20_DESC_*` 宏拼装，接口 0 = AudioControl，接口 1 = AudioStreaming |
| 接口绑定 | AC + AS 用 **IAD** 绑成一个功能，否则 Windows 会当成两个独立设备 |
| 播放端点 | `ISOCHRONOUS + ADAPTIVE + DATA`，无反馈端点 |
| 时钟漂移 | 环形缓冲水位 >75% 时周期性丢一对样本，<12.5% 时补一对，听感无察觉 |
| 音频链路 | EP OUT FIFO → 环形缓冲（64 KB ≈ 340 ms）→ 3 个 480 帧缓冲轮转 `playRaw()` → I2S1 → ES8311 → 功放 |
| 音量链 | 数字增益（电脑侧音量，上限 0 dB，不放大以避削波）→ M5Unified 主音量 → ES8311 DAC 模拟增益 |

## 踩坑记录：开发过程中遇到的问题与修复

这一节是整份文档最值钱的部分。UAC2 + ESP32-S3 + M5Unified 的组合坑很多，
以下每一条都是实测踩出来的，按遇到顺序排列。

### 1. 烧完不启动，必须拔插 USB

**现象**：`esptool` 报 `Hash of data verified` 成功，但设备停在 ROM 下载模式不跑应用。

**原因**：Cardputer ADV 用 ESP32-S3 **内置的 USB-Serial/JTAG**，没有把 RTS 接到 EN 脚，
所以 esptool 最后那句 `Hard resetting via RTS pin` 实际不起作用。

**结论**：这是硬件限制，**只能物理拔插一次**。为了让"重新烧录"这一步尽量省事，
固件里加了长按 G0 三秒写 `RTC_CNTL_FORCE_DOWNLOAD_BOOT` 再 `esp_restart()` 的入口，
这样不必拔线按住 BOOT 才能进下载模式（但烧完仍需拔插一次启动）。

### 2. 编译出来根本没有 USB 音频功能

**现象**：编译通过、烧录成功，但电脑里完全看不到音频设备。

**原因**：FQBN 缺 `USBMode`，走的是硬件 CDC，**TinyUSB 根本没被编进固件**。

**修复**：FQBN 必须显式带上 `USBMode=default,CDCOnBoot=default`。

### 3. `audio_device.h` 找不到

`libarduino_tinyusb.a` 配套头文件的相对路径是 `class/audio/audio_device.h`，
不是 `audio_device.h`。

### 4. 描述符长度自己就对不上

输入终端（Input Terminal）是 **17 字节**，不是 18；
AudioControl 接口的 `bInterfaceProtocol` 应为 **0x20**（IP 协议版本 2.0），写 0x00 会被主机忽略。
用 `AUDIO_DESC_LEN` 宏算出的长度与 `audio_desc_cb` 实际写入的字节数做自检，屏幕上那个
`D:136 ok` 就是这个自检结果显示的（长度不符会变红显示 `BAD`）。

### 5. Windows 报"设备无法启动（代码 10）" ← 最坑的一条

**现象**：设备管理器里出现黄色感叹号，属性里错误代码 10，音频端点不注册。

**排查过程**：先补 IAD（无效）→ 再换成 tinyusb 官方 `uac2_headset` 的宏重写一整份描述符
（无效）→ 最后逐个字段二分，发现是 **`bcdUSB = 0x0210`**。

**根因**：`bcdUSB ≥ 0x0210` 会让主机去读 **BOS 描述符**，而 arduino-esp32 核心内置的 BOS
描述符是给 CDC/WebUSB 用的，对纯音频设备不适用，主机解析失败后直接判定设备无法启动。

**修复**：`ucfg.usb_version = 0x0200;` —— 改回 USB 2.00 后**一次枚举成功**，
Windows 自动挂上 "USB Audio 2.0" 系统驱动，完全免驱。

### 6. 声音小到几乎听不见（−42 dB）

**现象**：驱动正常、电平表在动，但扬声器几乎没声音。

**根因**：M5Unified 的软件主音量是**平方曲线**：

```cpp
float volume = magnification * (self->_master_volume * self->_master_volume);
```

初版用了 `setVolume(32)` 并把 `magnification` 从默认的 16 降到 8，
相当于满配置的 `8 × (32/255)² ≈ 1/120` ≈ **−42 dB**。

**修复**：主音量恢复满量程 255、`magnification` 恢复 16；再把 ES8311 的 DAC 音量
（寄存器 `0x32`，`0xBF` = 0 dB）做成可调，默认 +6 dB，最高 +16 dB。
单这一项就把响度拉回约 120 倍。

### 7. 屏幕一直在闪

**现象**：音量那一行每 80 ms 闪一下。

**原因**：每帧都 `fillRect` 整块区域再重画文字，SPI 写入不是原子的，能看出"先黑一下再亮"。

**修复**：所有数值区都记住"上次画的值"，**只在数值变化时才重画**；
一直在变的缓冲水位改成"前景色 + 背景色"定宽覆盖写，不整行清屏。

### 8. 键盘只有 G0 有反应

**现象**：短按 G0 能静音（走 `M5.BtnA` 的 GPIO），但 `-` `=` `[` `]` `m` 全部无效。

**根因**：`M5Cardputer.Keyboard.updateKeysState()` **只解析已有的键列表，不会去硬件取事件**。
ADV 的键盘是 **TCA8418**（I2C 扫描芯片，中断驱动），必须先 `updateKeyList()` 读它的事件 FIFO。

**修复**：改调 `M5Cardputer.update()` —— 它内部等于
`M5.update() + Keyboard.updateKeyList() + Keyboard.updateKeysState()`。

### 9. 长按 G0 三秒没反应

**原因**：靠 `++g0_hold > 150` 数 loop 次数计时，但 loop 里刷屏会让周期远大于 20 ms，
实际要七八秒才触发。

**修复**：改用 `millis()` 差值判断，与循环速度解耦。

### 10. 电平条有规律的闪烁

**原因**：和屏闪同源 —— 每 80 ms 把 200 px 宽的条先刷成深灰底、再覆盖前景色、再重画边框，
三趟 SPI 写入之间能被看出明暗跳变。

**修复**：改成**增量绘制** —— 记住上次的宽度和颜色，变长只补新增的一小段、变短只擦多出的一小段、
宽度没变就一个像素都不写，边框只画一次。另外给颜色加迟滞（红 0.98/0.94、黄 0.90/0.86），
避免电平在阈值附近抖动时绿黄来回翻转。

### 11. 状态行的文字盖住了 L 电平条

**现象**：屏幕上"已连接 · 待机 / D:136 ok"压住了 L 条，还叠着模糊的旧文字。

**根因**：不是清屏范围太大，而是**上一屏的残留没清**。开机时 `usb_ok == false` 那一支会在
y=44 / y=60 写两行诊断文字（"等待主机枚举…"、"desc 136/136"），连上后只清了状态行
（y 20–37），那两行就永远留在屏上，正好被电平条压着。

**修复**：加一个"脏标记"。未连接分支画完把标记置位，已连接分支第一次进来先
`fillRect(0, 20, 240, 115)` 整块清空，并把所有"记住上次画了什么"的状态
（电平条宽度/颜色、音量、dB、剪波灯）一起复位强制重画，之后才恢复"只清状态行"的轻量模式。

**经验**：局部刷新时，"上一屏残留"比"清屏范围过大"更容易被忽略 ——
任何切入另一套布局的分支，离开时都要把整块区域标记为脏。

## 常见问题

<details>
<summary>声音设置里找不到 Cardputer Speaker</summary>

先确认设备管理器里没有黄色感叹号。若是 "USB Audio 2.0" 且状态正常，展开"音频输入和输出"，
右键"显示已断开的设备"看看；实在不行拔插一次。极少数情况是 USB 线只能充电、没有数据线芯。
</details>

<details>
<summary>声音偏小</summary>

系统音量拉到 100%，再按 `]` 把 DAC 增益加到 +10 ~ +16 dB。再大就会开始失真。
</details>

<details>
<summary>有轻微咔哒声 / 爆音</summary>

把系统音量降到 80–90%。数字域没有余量时（输入就是 0 dBFS 满幅），任何额外的模拟增益都会削波。
</details>

<details>
<summary>想改回普通 Cardputer 用途</summary>

烧回官方固件或你自己的 sketch 即可，本固件不写 NVS、不改任何持久设置。
</details>

<details>
<summary>怎么重新烧录</summary>

长按 G0 三秒进入下载模式（屏幕会显示"进入 USB 下载模式..."），此时电脑出现 COM 口，
直接烧 `0x0`。烧完拔插一次 USB 启动。
</details>

## 许可

| 组件 | 许可 | 来源 |
|---|---|---|
| TinyUSB | MIT | https://github.com/hathach/tinyusb |
| M5Cardputer / M5Unified / M5GFX | MIT | https://github.com/m5stack |
| arduino-esp32 | LGPL-2.1 | https://github.com/espressif/arduino-esp32 |

源码内保留各方原始版权声明。本固件源码可自由使用、修改、再分发。
