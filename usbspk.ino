/*
 *  Cardputer ADV — USB 免驱扬声器  (usbspk v1.5)
 *  ------------------------------------------------
 *  插到电脑 USB 口，Cardputer ADV 立即变成一个标准 USB 声卡：
 *    · USB Audio Class 2.0（Windows 10 1703+ / macOS / Linux 原生免驱）
 *    · 16bit / 48kHz / 立体声，ADAPTIVE 播放端点（主机按标称速率发送，
 *      本机用环形缓冲 + 采样级补偿吸收两侧时钟微差）
 *    · 声音从板载 ES8311 编解码芯片 + 功放 + 扬声器播出（M5Unified 驱动）
 *
 *  实现：
 *    · TinyUSB（esp32-hal-tinyusb）注册 USB_INTERFACE_AUDIO 自定义描述符
 *    · 描述符全部用 tinyusb 官方 TUD_AUDIO20_DESC_* 宏拼装
 *    · 音频任务：EP OUT FIFO → 环形缓冲 → 3 缓冲轮转 playRaw 交给 I2S
 *    · 音量/静音由电脑端混音器控制（UAC Feature Unit），键盘 -/= 调本机音量
 *    · [ ] 调 ES8311 模拟增益（-6 .. +16dB），屏幕显示状态与实时电平
 *
 *  按键：- / = 本机音量     [ / ] 模拟增益     m 或 G0 短按 静音
 *        G0 长按 3 秒 进入 USB 下载模式
 *        （Cardputer 的 ←/→/↑/↓ 是 Fn 组合键，Fn+方向键同样可以调）
 *
 *  关于音量（v1.2 修正）：
 *    M5Unified 的软件主音量是「平方曲线」：
 *        volume = magnification * (master_volume * master_volume)
 *    v1.1 用 setVolume(32) + magnification=8，比满配置低约 42dB，
 *    导致听起来几乎无声。现在 master 默认满量程 255，magnification 恢复 16，
 *    再叠加 ES8311 DAC 模拟增益，响度才正常。
 */

#include <M5Unified.h>
#include <M5Cardputer.h>
#include "esp32-hal-tinyusb.h"
#include "soc/rtc_cntl_reg.h"

#define FW_VERSION      "v1.5"

// ---------------- 音频参数 ----------------
#define SAMPLE_RATE     48000
#define CHUNK_FRAMES    480                     // 10ms
#define CHUNK_SAMPLES   (CHUNK_FRAMES * 2)      // 立体声 int16
#define N_OUTBUF        3                       // playRaw 轮转缓冲
#define RING_BYTES      (64 * 1024)             // 340ms @48k/16bit/2ch
#define RING_MASK       (RING_BYTES - 1)
#define START_CHUNKS    3                       // 缓冲够 30ms 才开播

// ---------------- UAC2 实体 ID（与描述符一致） ----------------
#define ENT_CLOCK       0x04
#define ENT_FU          0x02
#define UAC_REQ_CUR     0x01
#define UAC_REQ_RANGE   0x02
#define CTRL_MUTE       0x00
#define CTRL_VOLUME     0x02
#define CTRL_SAM_FREQ   0x01

// ---------------- USB 状态 ----------------
static uint8_t  g_itf_as   = 1;      // AudioStreaming 接口号（枚举时填充）
static uint8_t  g_ep_out   = 0x01;   // 音频数据 OUT 端点
static bool     g_desc_ok  = false;  // 描述符长度自检（诊断用）
static uint16_t g_desc_len = 0;
static volatile bool g_streaming = false;
static volatile bool g_mute  = false;

// ---------------- 音量 ----------------
// 电脑侧（UAC Feature Unit）音量，1/256 dB。0x0000 = 0dB = 主机满幅输出
static volatile int16_t g_uac_db256 = 0;
// 本机软件主音量 0..32（32 = 满量程）。M5Unified 内部按平方曲线换算
static volatile uint8_t g_master = 32;
// ES8311 DAC 模拟增益（dB，-6..+16），直接写 codec 寄存器 0x32
static volatile int8_t  g_dac_db = 6;
static uint32_t g_gain_q16 = 65536;            // 电脑侧音量换算出的数字增益 Q16

#define ES8311_ADDR    0x18
#define ES8311_REG_DAC_VOL 0x32

// ES8311 DAC 音量：0xBF = 0dB，每步 0.5dB，0xFF = +32dB
static void es8311_set_dac_db(int db) {
  int reg = 0xBF + db * 2;
  if (reg < 0x00) reg = 0x00;
  if (reg > 0xFF) reg = 0xFF;
  M5.In_I2C.writeRegister8(ES8311_ADDR, ES8311_REG_DAC_VOL, (uint8_t)reg, 100000);
}

// 软件主音量：交给 M5Unified（0..255，内部平方曲线）
static void apply_master_volume(void) {
  uint16_t v = ((uint16_t)g_master * 255u + 16u) / 32u;
  M5.Speaker.setVolume((uint8_t)(v > 255 ? 255 : v));
}

static void update_gain(void) {
  float uac = powf(10.0f, g_uac_db256 / (256.0f * 20.0f));
  // 主机最多给到 0dB（满幅），这里不再放大数字信号，避免削波
  float g = (g_mute || g_master == 0) ? 0.0f : uac;
  if (g > 1.0f) g = 1.0f;
  g_gain_q16 = (uint32_t)(g * 65536.0f + 0.5f);
  apply_master_volume();
}

// ============================================================
//  USB 描述符（在 tinyusb_init 前通过 tinyusb_enable_interface 注册）
//  全部用 tinyusb 官方 TUD_AUDIO20_DESC_* 宏拼装：
//    接口 0 = AudioControl，接口 1 = AudioStreaming（alt0 空闲 / alt1 播放）
//    播放端点用 ADAPTIVE 同步方式（与 tinyusb 官方 uac2_headset 示例一致）
// ============================================================
#define AUDIO_DESC_LEN (TUD_AUDIO20_DESC_IAD_LEN                                   \
                      + TUD_AUDIO20_DESC_STD_AC_LEN                                \
                      + TUD_AUDIO20_DESC_CS_AC_LEN                                 \
                      + TUD_AUDIO20_DESC_CLK_SRC_LEN                               \
                      + TUD_AUDIO20_DESC_INPUT_TERM_LEN                            \
                      + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2)                       \
                      + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN                           \
                      + TUD_AUDIO20_DESC_STD_AS_LEN                                \
                      + TUD_AUDIO20_DESC_STD_AS_LEN                                \
                      + TUD_AUDIO20_DESC_CS_AS_INT_LEN                             \
                      + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN                         \
                      + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN                         \
                      + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

static uint16_t audio_desc_cb(uint8_t *dst, uint8_t *itf) {
  uint8_t itf_ac = *itf;
  uint8_t itf_as = *itf + 1;
  *itf += 2;
  g_itf_as = itf_as;

  uint8_t n_out = tinyusb_get_free_out_endpoint();
  if (!n_out) return 0;
  g_ep_out = n_out;

  const uint8_t desc[] = {
    // IAD：把 AC + AS 绑定为一个音频功能
    TUD_AUDIO20_DESC_IAD(itf_ac, 2, 0x00),
    // AudioControl 接口
    TUD_AUDIO20_DESC_STD_AC(itf_ac, 0x00, 0x00),
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_DESKTOP_SPEAKER,
      TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN
        + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN,
      AUDIO20_CTRL_NONE),
    TUD_AUDIO20_DESC_CLK_SRC(ENT_CLOCK, 0x03, 0x07, 0x00, 0x00),
    TUD_AUDIO20_DESC_INPUT_TERM(0x01, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, ENT_CLOCK,
                                0x02, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0x0000, 0x00),
    // 特性单元：静音(mute, pos0) + 音量(volume, pos4)，主/左/右都可读写 → 0x33
    TUD_AUDIO20_DESC_FEATURE_UNIT(ENT_FU, 0x01, 0x00,
      (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS),
      (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS),
      (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)),
    TUD_AUDIO20_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, 0x00,
                                 ENT_FU, ENT_CLOCK, 0x0000, 0x00),
    // AudioStreaming 接口：alt0（零带宽）
    TUD_AUDIO20_DESC_STD_AS_INT(itf_as, 0x00, 0x00, 0x00),
    // alt1（播放）
    TUD_AUDIO20_DESC_STD_AS_INT(itf_as, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(0x01, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, 0x02,
                               AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(2, 16),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(g_ep_out,
      (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA),
      TUD_AUDIO_EP_SIZE(false, SAMPLE_RATE, 2, 2), 0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(0x00, AUDIO20_CTRL_NONE, 0x01, 0x0001),
  };

  g_desc_len = sizeof(desc);
  g_desc_ok  = (sizeof(desc) == AUDIO_DESC_LEN);
  memcpy(dst, desc, sizeof(desc));
  return (uint16_t)sizeof(desc);
}

// ============================================================
//  TinyUSB 音频回调
// ============================================================
extern "C" {

// 接口设置：alt1 = 开始传输
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *req) {
  (void)rhport;
  if ((req->wIndex & 0xFF) == g_itf_as) {
    g_streaming = ((req->wValue & 0xFF) == 1);
  }
  return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *req) {
  (void)rhport; (void)req;
  return true;
}

// 实体 SET 请求
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf) {
  (void)rhport;
  uint8_t ent = req->wIndex >> 8;
  uint8_t cs  = req->wValue >> 8;
  if (ent == ENT_CLOCK && cs == CTRL_SAM_FREQ && req->bRequest == UAC_REQ_CUR) {
    uint32_t f = buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16);
    return f == SAMPLE_RATE;            // 只接受 48k，其余 STALL
  }
  if (ent == ENT_FU) {
    if (cs == CTRL_MUTE)   { g_mute = (buf[0] != 0); update_gain(); return true; }
    if (cs == CTRL_VOLUME) {
      int16_t v = (int16_t)(buf[0] | (buf[1] << 8));
      g_uac_db256 = v;
      update_gain();
      return true;
    }
  }
  return false;
}

// 实体 GET 请求
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req) {
  uint8_t ent = req->wIndex >> 8;
  uint8_t cs  = req->wValue >> 8;
  static uint8_t   b1;
  static uint16_t  b2;
  static uint8_t   b3[3];
  // 音量范围：-60dB .. 0dB，步进 1dB
  static uint8_t vr[8] = {0x01, 0x00, 0x00, 0xC4, 0x00, 0x00, 0x00, 0x01};
  static uint8_t mr[8] = {0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00};
  // 采样率范围：仅 48000
  static uint8_t fr[14] = {0x01, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  if (req->bRequest == UAC_REQ_CUR) {
    if (ent == ENT_CLOCK && cs == CTRL_SAM_FREQ) {
      b3[0] = SAMPLE_RATE & 0xFF; b3[1] = (SAMPLE_RATE >> 8) & 0xFF; b3[2] = (SAMPLE_RATE >> 16) & 0xFF;
      return tud_control_xfer(rhport, req, b3, 3);
    }
    if (ent == ENT_FU && cs == CTRL_MUTE)   { b1 = g_mute ? 1 : 0; return tud_control_xfer(rhport, req, &b1, 1); }
    if (ent == ENT_FU && cs == CTRL_VOLUME) { b2 = (uint16_t)g_uac_db256; return tud_control_xfer(rhport, req, (uint8_t *)&b2, 2); }
  } else if (req->bRequest == UAC_REQ_RANGE) {
    if (ent == ENT_FU && cs == CTRL_VOLUME)   return tud_control_xfer(rhport, req, vr, 8);
    if (ent == ENT_FU && cs == CTRL_MUTE)     return tud_control_xfer(rhport, req, mr, 8);
    if (ent == ENT_CLOCK && cs == CTRL_SAM_FREQ) return tud_control_xfer(rhport, req, fr, 14);
  }
  return false;
}

} // extern "C"

// ============================================================
//  环形缓冲（单生产者单消费者，同一任务，无需锁）
// ============================================================
static uint8_t  s_ring[RING_BYTES];
static uint32_t s_head = 0, s_tail = 0;

static inline uint32_t ring_count(void) { return (s_head - s_tail) & RING_MASK; }
static inline uint32_t ring_free (void) { return (RING_MASK + 1 - ((s_head - s_tail) & RING_MASK)) - 1; }

static inline void ring_push(int16_t s) {
  if (ring_free() < 2) return;          // 满（正常不会发生，反馈端会压速率）
  s_ring[s_head] = (uint8_t)(s & 0xFF);
  s_ring[(s_head + 1) & RING_MASK] = (uint8_t)(s >> 8);
  s_head = (s_head + 2) & RING_MASK;
}

static inline int16_t ring_pop(void) {
  int16_t s = (int16_t)(s_ring[s_tail] | (s_ring[(s_tail + 1) & RING_MASK] << 8));
  s_tail = (s_tail + 2) & RING_MASK;
  return s;
}

// ============================================================
//  电平表（供 UI）
// ============================================================
static volatile int32_t  g_pkL = 0, g_pkR = 0;      // 增益后峰值（0..32768）
static volatile uint32_t g_last_clip_ms = 0;         // 最近一次削波时刻

// ============================================================
//  音频任务：EP OUT → 环形缓冲 → playRaw
// ============================================================
static void audio_task(void *arg) {
  (void)arg;
  static int16_t scratch[512];                              // 每次搬运缓冲
  static int16_t outbuf[N_OUTBUF][CHUNK_SAMPLES];           // 轮转播放缓冲
  uint8_t  ob = 0;
  bool     started = false;
  uint32_t idle_ms = 0;
  uint32_t drift_cnt = 0;                                   // 时钟漂移补偿计数

  for (;;) {
    if (g_streaming && tud_mounted()) {
      // 1) 尽量排空 TinyUSB FIFO
      for (;;) {
        uint16_t avail = tud_audio_available();
        if (!avail) break;
        uint16_t take = avail > sizeof(scratch) ? sizeof(scratch) : (uint16_t)avail;
        take &= ~1;
        if (!take) break;
        int n = tud_audio_read(scratch, take);
        if (n <= 0) break;
        int ns = n >> 1;
        int32_t pkL = 0, pkR = 0;
        uint32_t gq = g_gain_q16;
        // ADAPTIVE 端点：主机按标称 48k 发送，本机 I2S 时钟略有偏差，
        // 用环形缓冲水位做采样级补偿（丢/补一对样本，听感上无察觉）
        uint32_t fill = ring_count();
        bool do_drop = (fill > (RING_BYTES * 3 / 4)) && ((drift_cnt % 4096) == 4095);
        bool do_dup  = (fill < (RING_BYTES / 8))     && ((drift_cnt % 4096) == 4095);
        for (int i = 0; i < ns; i += 2) {
          int32_t l = scratch[i], r = scratch[i + 1];
          int32_t lo = (l * (int32_t)gq) >> 16, ro = (r * (int32_t)gq) >> 16;
          // 削波检测：增益后超出 int16 范围 → clamp，并记录时间供 UI 亮 CLIP
          if (lo >  32767) { lo =  32767; g_last_clip_ms = millis(); }
          if (lo < -32768) { lo = -32768; g_last_clip_ms = millis(); }
          if (ro >  32767) { ro =  32767; g_last_clip_ms = millis(); }
          if (ro < -32768) { ro = -32768; g_last_clip_ms = millis(); }
          // 电平表统计的是「实际送进喇叭」的信号
          int32_t al = lo < 0 ? -lo : lo;
          int32_t ar = ro < 0 ? -ro : ro;
          if (al > pkL) pkL = al;
          if (ar > pkR) pkR = ar;
          drift_cnt++;
          if (do_drop && drift_cnt == 4096) { drift_cnt = 0; continue; }   // 丢弃这一对
          ring_push((int16_t)lo);
          ring_push((int16_t)ro);
          if (do_dup && drift_cnt == 4096) {                               // 补一对
            ring_push((int16_t)lo); ring_push((int16_t)ro); drift_cnt = 0;
          }
        }
        if (pkL > g_pkL) g_pkL = pkL;
        if (pkR > g_pkR) g_pkR = pkR;
      }

      // 2) 喂给扬声器
      uint32_t avail_s = ring_count() >> 1;
      if (!started) {
        if (avail_s >= CHUNK_SAMPLES * START_CHUNKS) {
          for (int i = 0; i < CHUNK_SAMPLES; i++) outbuf[ob][i] = ring_pop();
          M5.Speaker.playRaw(outbuf[ob], CHUNK_SAMPLES, SAMPLE_RATE, true, 1, 0);
          ob = (ob + 1) % N_OUTBUF;
          started = true;
        }
      } else if (M5.Speaker.isPlaying(0) < 2) {
        if (avail_s >= CHUNK_SAMPLES) {
          for (int i = 0; i < CHUNK_SAMPLES; i++) outbuf[ob][i] = ring_pop();
          M5.Speaker.playRaw(outbuf[ob], CHUNK_SAMPLES, SAMPLE_RATE, true, 1, 0);
          ob = (ob + 1) % N_OUTBUF;
        } else if (avail_s == 0) {
          if (++idle_ms > 500) started = false;   // 断流后回到预缓冲状态
        } else {
          idle_ms = 0;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      s_head = s_tail = 0;
      started = false;
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

// ============================================================
//  UI
// ============================================================
static lgfx::v1::LGFX_Device *LCD;
static int32_t s_dL = 0, s_dR = 0;    // 显示电平（衰减）
static uint32_t s_last_draw = 0;
// 上一次画到屏上的数值：只有变化时才重画，避免每帧清屏造成的闪烁
static int  s_draw_master = -1;
static bool s_draw_mute   = false;
static int  s_draw_db     = INT32_MIN;
static int  s_draw_dac    = INT32_MIN;

// 线性幅度 → 对数刻度（-48dB..0dB → 0.0..1.0）
// 线性刻度下，正常音量的音乐也会把条顶满，看不出还有多少余量
static float lvl_ratio(float x) {
  if (x <= 0.004f) return 0.0f;              // 低于约 -48dB 视为静音
  float db = 20.0f * log10f(x);
  if (db <= -48.0f) return 0.0f;
  float r = (db + 48.0f) / 48.0f;
  return r > 1.0f ? 1.0f : r;
}

static void bar(int x, int y, int w, int h, float ratio, uint16_t col) {
  LCD->fillRect(x, y, w, h, 0x2124);
  int fw = (int)(w * ratio + 0.5f);
  if (fw > w) fw = w;
  if (fw > 0) LCD->fillRect(x, y, fw, h, col);
  LCD->drawRect(x, y, w, h, 0x528A);
}

// ------------------------------------------------------------
//  增量电平条
//  电平每 80ms 变一次，若每次都「整条清背景 + 整条画前景」，
//  在 200px 的宽度上能看出一次明显的明暗跳变（和之前的屏闪同源）。
//  这里记住上次的宽度和颜色，只改写真正变化的那一段，边框只画一次。
// ------------------------------------------------------------
struct BarState { int w; uint16_t col; };
static BarState s_bars[2] = { { -1, 0 }, { -1, 0 } };   // 0=L 1=R
static int s_clip_state = -1;      // CLIP 小方块：-1 未知 / 0 灭 / 1 亮

// 颜色加迟滞：避免在阈值附近来回跳绿/黄
static uint16_t level_color(float r, uint16_t prev) {
  if (r >= 0.98f) return 0xF800;                       // 红：≥ -1dB
  if (prev == 0xF800 && r >= 0.94f) return 0xF800;     // 迟滞
  if (r >= 0.90f) return 0xFD20;                       // 黄：≥ -5dB
  if (prev == 0xFD20 && r >= 0.86f) return 0xFD20;     // 迟滞
  return 0x07E0;                                       // 绿
}

static void bar_dyn(int idx, int x, int y, int w, int h, float ratio, uint16_t col) {
  int fw = (int)(w * ratio + 0.5f);
  if (fw < 0) fw = 0;
  if (fw > w) fw = w;
  int  &sw = s_bars[idx].w;
  uint16_t &sc = s_bars[idx].col;

  if (sw < 0) {                         // 首次：铺底 + 边框
    LCD->fillRect(x, y, w, h, 0x2124);
    LCD->drawRect(x, y, w, h, 0x528A);
    sw = 0;
    sc = 0x2124;
  }
  if (fw == sw && col == sc) return;

  if (col != sc) {                      // 颜色变了，前景整段重画
    LCD->fillRect(x, y, fw > sw ? fw : sw, h, 0x2124);
    if (fw > 0) LCD->fillRect(x, y, fw, h, col);
  } else if (fw > sw) {                 // 变长：只补新增的一段
    LCD->fillRect(x + sw, y, fw - sw, h, col);
  } else {                              // 变短：只擦掉多出来的一段
    LCD->fillRect(x + fw, y, sw - fw, h, 0x2124);
  }
  sw = fw;
  sc = col;
}

static void draw_ui(bool usb_ok, bool full) {
  if (!full && millis() - s_last_draw < 80) return;
  s_last_draw = millis();

  static bool last_usb = false, last_stream = false;
  bool state_changed = full || (usb_ok != last_usb) || (g_streaming != last_stream);
  last_usb = usb_ok; last_stream = g_streaming;

  if (full) {
    LCD->fillScreen(0x0000);
    LCD->fillRect(0, 0, 240, 20, 0x0145);
    LCD->setTextColor(0xFFFF);
    LCD->setFont(&fonts::efontCN_12);
    LCD->setTextDatum(lgfx::v1::textdatum_t::middle_center);
    LCD->drawString("Cardputer USB 免驱扬声器  " FW_VERSION, 120, 10);
    LCD->setTextDatum(lgfx::v1::textdatum_t::top_left);
    // 全屏重绘后强制刷新所有数值区
    s_draw_master = -1; s_draw_db = INT32_MIN; s_draw_dac = INT32_MIN;
    s_bars[0].w = -1; s_bars[1].w = -1; s_clip_state = -1;
  }

  char line[64];

  // 状态行（仅状态变化时重画，避免闪烁）
  // 两级重绘策略：
  //   · 「未连接」界面会在 y44 / y60 写两行诊断文字，连上后必须整块清掉，
  //     否则那两行会一直留在屏幕上，被 L/R 电平条压着（v1.4 的现象）
  //   · 平时状态切换（正在传输/待机）只清状态行 y20..37，不影响电平条
  static bool s_need_repaint = true;   // true = 下一帧整块重画内容区
  if (state_changed) {
  if (!usb_ok) {
    LCD->fillRect(0, 20, 240, 115, 0x0000);      // 断开时整块重排
    s_bars[0].w = -1; s_bars[1].w = -1; s_clip_state = -1;
    s_draw_master = -1; s_draw_db = INT32_MIN; s_draw_dac = INT32_MIN;
    s_need_repaint = true;                       // 连上后必须再整块清一次
    LCD->setFont(&fonts::efontCN_12);
    LCD->setTextColor(0xF800);
    LCD->drawString("未连接 — 请插入电脑 USB 口", 8, 22);
    LCD->setTextColor(0x8410);
    LCD->drawString("等待主机枚举 (UAC2 48kHz/16bit)", 8, 44);
    LCD->setFont(&fonts::Font0);
    LCD->setTextColor(g_desc_ok ? 0x07E0 : 0xF800);
    snprintf(line, sizeof(line), "desc %u/%u", (unsigned)g_desc_len, (unsigned)AUDIO_DESC_LEN);
    LCD->drawString(line, 8, 60);
    return;
  }
  if (s_need_repaint) {
    LCD->fillRect(0, 20, 240, 115, 0x0000);      // 清掉「未连接」残留与旧数值
    s_bars[0].w = -1; s_bars[1].w = -1; s_clip_state = -1;
    s_draw_master = -1; s_draw_db = INT32_MIN; s_draw_dac = INT32_MIN;
    s_need_repaint = false;
  } else {
    LCD->fillRect(0, 20, 240, 17, 0x0000);       // 只清状态行本身
  }
  LCD->setFont(&fonts::efontCN_12);
  LCD->setTextColor(0x07FF);
  LCD->drawString(g_streaming ? "已连接 · 正在传输" : "已连接 · 待机", 6, 22);
  // 描述符自检角标（D:长度；异常为红色）
  LCD->setFont(&fonts::Font0);
  LCD->setTextColor(g_desc_ok ? 0x07E0 : 0xF800);
  snprintf(line, sizeof(line), "D:%u %s", (unsigned)g_desc_len, g_desc_ok ? "ok" : "BAD");
  LCD->drawRightString(line, 234, 24);
  LCD->setFont(&fonts::efontCN_12);
  }  // end state_changed

  // 电平表：显示「增益后、送进喇叭」的峰值（0dBFS = 满格）
  int32_t dpkL = g_pkL, dpkR = g_pkR;
  if (dpkL > s_dL) s_dL = dpkL; else s_dL = (s_dL * 15 + dpkL) >> 4;
  if (dpkR > s_dR) s_dR = dpkR; else s_dR = (s_dR * 15 + dpkR) >> 4;
  float fl = s_dL / 32768.0f, fr = s_dR / 32768.0f;
  if (g_mute || g_master == 0) { fl = fr = 0; }
  fl = lvl_ratio(fl); fr = lvl_ratio(fr);
  // 刻度已对数化：红 ≥ -1dB(0.98) ｜ 黄 ≥ -5dB(0.90) ｜ 绿 其余
  uint16_t cl = level_color(fl, s_bars[0].col);
  uint16_t cr = level_color(fr, s_bars[1].col);
  LCD->setTextColor(0x8410); LCD->setFont(&fonts::efontCN_12);
  LCD->drawString("L", 6, 39);
  LCD->drawString("R", 6, 53);
  bar_dyn(0, 22, 41, 194, 9, fl, cl);
  bar_dyn(1, 22, 55, 194, 9, fr, cr);
  // CLIP：真的发生数字削波时亮红（保持 600ms，方便看清）
  int clip_now = ((millis() - g_last_clip_ms) < 600) ? 1 : 0;
  if (clip_now != s_clip_state) {
    s_clip_state = clip_now;
    uint16_t cc = clip_now ? 0xF800 : 0x2124;
    LCD->fillRect(222, 41, 12, 9, cc);
    LCD->fillRect(222, 55, 12, 9, cc);
  }

  // 本机音量：只在数值/静音状态变化时重画（每帧清屏会闪）
  bool mute_now = (g_mute || g_master == 0);
  if (s_draw_master != g_master || s_draw_mute != mute_now) {
    s_draw_master = g_master;
    s_draw_mute   = mute_now;
    LCD->fillRect(0, 66, 240, 30, 0x0000);
    snprintf(line, sizeof(line), "本机音量 %d%%  %s", g_master * 100 / 32,
             mute_now ? "[静音]" : "");
    LCD->setTextColor(mute_now ? 0xFD20 : 0xFFFF);
    LCD->drawString(line, 6, 68);
    bar(6, 84, 228, 8, (float)g_master / 32.0f, mute_now ? 0x8410 : 0x07FF);
  }

  // 电脑侧音量 / 模拟增益：同样只在变化时重画
  if (s_draw_db != g_uac_db256 || s_draw_dac != g_dac_db) {
    s_draw_db  = g_uac_db256;
    s_draw_dac = g_dac_db;
    int db_x10 = g_uac_db256 * 10 / 256;
    LCD->fillRect(0, 96, 240, 14, 0x0000);
    LCD->setTextColor(0x8410);
    snprintf(line, sizeof(line), "电脑 %s%d.%ddB  DAC %+ddB", db_x10 < 0 ? "-" : "",
             abs(db_x10) / 10, abs(db_x10) % 10, (int)g_dac_db);
    LCD->drawString(line, 6, 97);
  }

  // 缓冲水位一直在变：用「前景+背景色」直接覆盖写，定宽，避免整行清屏闪烁
  {
    int buf_ms = (int)((ring_count() >> 1) * 1000 / SAMPLE_RATE / 2);
    if (buf_ms > 999) buf_ms = 999;
    snprintf(line, sizeof(line), "缓冲 %3dms", buf_ms);
    LCD->setTextColor(0x8410, 0x0000);
    LCD->drawString(line, 6, 111);
    LCD->setTextColor(0x8410);
  }

  if (g_pkL) g_pkL = g_pkL / 2;  // 衰减
  if (g_pkR) g_pkR = g_pkR / 2;
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.internal_mic = false;

  // 扬声器配置必须放在 begin 之前：M5.begin 内部会沿用这里的 sample_rate / stereo，
  // 只覆盖 I2S 引脚。magnification 保持默认 16（设小等于直接砍掉数 dB 增益）。
  auto spk = M5.Speaker.config();
  spk.sample_rate  = SAMPLE_RATE;
  spk.stereo       = true;
  spk.magnification = 16;
  M5.Speaker.config(spk);

  M5Cardputer.begin(cfg, true);

  // 主音量必须在 begin 之后设（begin 会重置 _master_volume）
  g_master = 32;
  apply_master_volume();
  es8311_set_dac_db(g_dac_db);
  update_gain();

  LCD = &M5Cardputer.Display;
  draw_ui(false, true);
  LCD->setTextColor(0xFFFF);
  LCD->setFont(&fonts::efontCN_12);
  LCD->drawString("USB 声卡启动中...", 8, 46);

  // 注册 UAC2 接口并启动 TinyUSB（USB OTG）
  tinyusb_enable_interface(USB_INTERFACE_AUDIO, AUDIO_DESC_LEN, audio_desc_cb);
  tinyusb_device_config_t ucfg = {};
  ucfg.vid               = USB_ESPRESSIF_VID;
  ucfg.pid               = 0x4010;
  ucfg.product_name      = "Cardputer Speaker";
  ucfg.manufacturer_name = "M5Stack";
  ucfg.serial_number     = "";      // 空 = 自动用芯片 MAC
  ucfg.fw_version        = 0x0100;
  ucfg.usb_version       = 0x0200;   // 2.00：避免主机来读 BOS（核心自带的 BOS 是给 CDC/WebUSB 用的）
  ucfg.usb_class         = 0x00;     // 每接口各自声明类
  ucfg.usb_attributes    = 0x80;     // 总线供电
  ucfg.usb_power_ma      = 500;      // 功放峰值电流较大，向主机要足 500mA
  if (tinyusb_init(&ucfg) != ESP_OK) {
    LCD->setTextColor(0xF800);
    LCD->drawString("USB 初始化失败!", 8, 66);
    for (;;) vTaskDelay(1000);
  }

  xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 10, NULL, 1);
}

void loop() {
  // M5Cardputer.update() = M5.update() + Keyboard.updateKeyList()（TCA8418 事件读取）
  //                      + Keyboard.updateKeysState()
  // 只调 updateKeysState() 不会去 TCA8418 取事件，键列表永远是空的，字符键全部无效。
  M5Cardputer.update();

  // 键盘：-/= 调音量，[/] 调模拟增益，m 静音
  //      （Cardputer 的 ←/→/↑/↓ 需要 Fn 组合键，直接按只会出字符，所以两种都接受）
  if (M5Cardputer.Keyboard.isPressed()) {
    auto &st = M5Cardputer.Keyboard.keysState();
    bool down = st.left, up = st.right;
    bool dac_down = st.down, dac_up = st.up;
    bool mute_now_key = false;
    for (auto c : st.word) {
      if      (c == '-' || c == ',') down = true;
      else if (c == '=' || c == '/') up   = true;
      else if (c == '[') dac_down = true;
      else if (c == ']') dac_up   = true;
      else if (c == 'm' || c == 'M') mute_now_key = true;
    }
    // m 只认「刚按下」这一下，避免按住时反复翻转
    static bool m_held = false;
    if (mute_now_key && !m_held) { g_mute = !g_mute; update_gain(); }
    m_held = mute_now_key;

    bool act = false, dac = false;
    if (down && g_master > 0)  { g_master--; act = true; }
    if (up   && g_master < 32) { g_master++; act = true; }
    if (dac_down && g_dac_db > -6) { g_dac_db -= 2; dac = true; }
    if (dac_up   && g_dac_db < 16) { g_dac_db += 2; dac = true; }
    if (act) { update_gain(); vTaskDelay(pdMS_TO_TICKS(90)); }
    if (dac) { es8311_set_dac_db(g_dac_db); vTaskDelay(pdMS_TO_TICKS(90)); }
  }
  // G0 键：短按静音，长按 3 秒进入 USB 下载模式（方便以后免按键烧录）
  // 用 millis() 计时：loop 周期受刷屏影响并不固定，按循环计数会拖到七八秒才触发
  static uint32_t g0_down_ms = 0;
  static bool     g0_tripped = false;
  if (M5.BtnA.isPressed()) {
    if (M5.BtnA.wasPressed()) {
      g_mute = !g_mute; update_gain();
      g0_down_ms = millis();
      g0_tripped = false;
    }
    if (!g0_tripped && g0_down_ms && (millis() - g0_down_ms > 3000)) {
      g0_tripped = true;
      LCD->fillScreen(0x0000);
      LCD->setTextColor(0xFFFF);
      LCD->setFont(&fonts::efontCN_14);
      LCD->drawString("进入 USB 下载模式...", 8, 56);
      vTaskDelay(pdMS_TO_TICKS(400));
      REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
      esp_restart();
    }
  } else {
    g0_down_ms = 0;
    g0_tripped = false;
  }

  draw_ui(tud_mounted(), false);
  vTaskDelay(pdMS_TO_TICKS(20));
}
