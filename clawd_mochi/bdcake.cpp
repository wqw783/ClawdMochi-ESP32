/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  bdcake.cpp — 生日动画（桌宠屏幕 240×240 像素场景）
 *
 *  画面与「生日动画/index.html」1:1 对齐（同一套坐标、同一套像素精灵）：
 *    ① 顶部横幅  x 52..228, y 4..50（奶油底 + 粉色描边 + 燕尾飘带）
 *    ② 螃蟹      Clawd 56×36，左上 (4, 8)
 *    ③ 三层蛋糕  中心线 x=120；数字蜡烛「20」+ 火焰；奶油盘 + 蕾丝边
 *    ④ 玉玉兽    42×62，左上 (4, 126)
 *    ⑤ 王飚飚    67×87，左上 (168, 100)
 *    ⑥ 对话框    x 12..228, y 194..232
 *    ⑦ 粒子      气球 / 彩带 / 爱心 / 青烟（吹蜡烛时飞出）
 *
 *  ── 为什么是「分带渲染」而不是整屏画布（R8 修复）────────────────────
 *  R7 用了一块 240×240×2 = 115,200 B 的整屏离屏画布，真机上出现「手机有画面、
 *  桌宠屏全白」。排障结论与对策：
 *    · 115 KB 一次性 malloc 在「WiFi + WebServer 都起来之后」很容易失败或把堆榨干，
 *      失败/崩溃的观感就是屏幕停在初始化后的白屏；
 *    · 整屏一次推 115 KB 也是 SPI 上最重的一笔。
 *  现在改成 **3 条 240×80 的横带**（单块 38,400 B）：
 *    每条带只申请一次、失败会退避重试；一帧 = 画 3 遍（每遍只落到该带内），
 *    每带推 240×80。内存压力降到原来的 1/3，画面仍是逐帧整屏刷新、不闪。
 *
 *  ── 其它稳健性设计 ────────────────────────────────────────────────
 *    · 进视图先重初始化面板（tft.init + setRotation + 40MHz）：面板被意外复位/
 *      进入休眠/上电顺序抖动导致的白屏，这一步能直接救回来；
 *    · 动画期间每 5 秒补一条 DISPON（幂等），防止面板被误关显示；
 *    · 画布申请失败会在屏上写原因（不是白屏），并每 3 秒重试；
 *    · 复位原因 / 启动次数 / 剩余堆 / 帧率 全部暴露给 /bc/state，
 *      网页上一眼能看出「桌宠是不是重启过、堆还够不够」；
 *    · 点许愿 / 吹蜡烛 / 重新点亮时，若桌宠没停在生日动画视图会自动切过去。
 *
 *  ── 中文对话框（R9）──────────────────────────────────────────────
 *  GFX 自带的 5×7 字库没有中文字形，所以对话框改用 bdcake_cjk.h 里的
 *  **16×16 点阵**：只做用得到的 29 个字（928 B），宋体自带点阵、笔画不断线。
 *     生日快乐！ / 闭上眼睛许愿吧 / 许愿成功，心想事成 /
 *     祝你生日快乐！ / 蜡烛点好啦，再来一次吧
 *  UTF-8 解码 → 码点查表 → 16 行位图绘制，见下面「中文点阵」一节。
 *
 *  帧率 20fps（BC_FRAME_MS = 50ms），非阻塞；除画布外零堆分配。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "bdcake.h"
#include "bdcake_sprites.h"
#include "bdcake_cjk.h"       // 对话框中文 16×16 点阵（自动生成，29 字 / 928 B）

#include <math.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_attr.h>

// ── 帧率 / 交互时长 / 分带 ─────────────────────────────────────
#define BC_FRAME_MS      50          // 20fps：留足时间给 WebServer 处理请求
#define BC_WISH_MS     3000          // 「闭上眼睛许愿吧」→「许愿成功」的间隔
#define BC_FLASH_MS     500          // 吹蜡烛后的彩闪时长
#define BC_DLG_SLIDE    160          // 对话框上滑动画时长
#define BC_BAND_H        80          // 每条横带高度（240/80 = 3 条）
#define BC_BANDS          3
#define BC_POKE_MS     5000          // 每 5 秒补一次 DISPON（面板自愈）
#define BC_RETRY_MS    3000          // 画布申请失败后的重试间隔
#define BC_MIN_HEAP   60000UL        // 申请画布前要求的最小剩余堆（留安全余量）

// ── 调色板（RGB565，编译期常量，运行时拷进 RAM 数组加速）─────────
#define BC_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

enum {
  BC_BG, BC_FLOOR, BC_FLOOR_ALT, BC_FLOOR_LINE, BC_INK, BC_CREAM, BC_CREAM_DK,
  BC_CREAM_LT, BC_PINK, BC_PINK_LT, BC_PINK_DK, BC_RED, BC_MINT, BC_YELLOW,
  BC_LAV, BC_SKY, BC_WHITE, BC_PLATE, BC_LACE, BC_TIER_BOT, BC_BERRY_HI,
  BC_BERRY_LEAF, BC_BOW, BC_BOW2, BC_HEART, BC_GLOW, BC_SHADOW, BC_WICK,
  BC_FLAME1, BC_FLAME2, BC_FLAME3, BC_FLAME4, BC_DEADWICK, BC_SMOKE,
  BC_BAL0, BC_BAL1, BC_BAL2, BC_BAL3, BC_BAL4, BC_BAL5, BC_BAL6,
  BC_COUNT
};

static const uint16_t BC_PROG[BC_COUNT] PROGMEM = {
  BC_RGB(255, 182, 193),  // bg
  BC_RGB(255, 163, 181),  // floor
  BC_RGB(255, 171, 187),  // floorAlt
  BC_RGB(255, 144, 164),  // floorLine
  BC_RGB(142,  43,  66),  // ink
  BC_RGB(255, 243, 226),  // cream
  BC_RGB(245, 221, 194),  // creamDark
  BC_RGB(255, 250, 242),  // creamLite
  BC_RGB(255, 134, 168),  // pink
  BC_RGB(255, 194, 211),  // pinkLite
  BC_RGB(227,  93, 130),  // pinkDark
  BC_RGB(228,  45,  69),  // red
  BC_RGB( 95, 216, 200),  // mint
  BC_RGB(255, 211,  77),  // yellow
  BC_RGB(185, 140, 240),  // lav
  BC_RGB(127, 194, 255),  // sky
  BC_RGB(255, 253, 249),  // white
  BC_RGB(255, 246, 234),  // plate #fff6ea
  BC_RGB(255, 224, 233),  // lace  #ffe0e9
  BC_RGB(246, 224, 201),  // tier bottom #f6e0c9
  BC_RGB(255, 125, 140),  // berry highlight #ff7d8c
  BC_RGB( 79, 191, 106),  // berry leaf #4fbf6a
  BC_RGB(255,  95, 138),  // bow #ff5f8a
  BC_RGB(255, 127, 162),  // bow2 #ff7fa2
  BC_RGB(255,  95, 138),  // heart #ff5f8a
  BC_RGB(255, 186, 196),  // glow ≈ rgba(255,255,255,.05) over bg
  BC_RGB(237, 144, 163),  // cake shadow over floor
  BC_RGB(138,  74,  44),  // wick #8a4a2c
  BC_RGB(240,  99,  15),  // flame outer #f0630f
  BC_RGB(255, 154,  46),  // flame mid #ff9a2e
  BC_RGB(255, 211,  77),  // flame inner #ffd34d
  BC_RGB(255, 246, 208),  // flame core #fff6d0
  BC_RGB(107,  59,  42),  // dead wick #6b3b2a
  BC_RGB(214, 204, 216),  // smoke #d6ccd8
  BC_RGB(255, 107, 138),  // balloon 0
  BC_RGB(255, 211,  77),  // 1
  BC_RGB( 95, 216, 200),  // 2
  BC_RGB(185, 140, 240),  // 3
  BC_RGB(127, 194, 255),  // 4
  BC_RGB(255, 158, 196),  // 5
  BC_RGB(255, 176, 196)   // 6
};

static uint16_t bcC[BC_COUNT];          // 调色板 RAM 副本
#define BC(cl) (bcC[(cl)])

// ── 画面状态 ────────────────────────────────────────────────────
static GFXcanvas16* gCv = nullptr;      // 240×80 横带画布（只申请一次）
static int16_t  gBandTop = 0;           // 当前这条带在屏幕上的 y 起点
static uint32_t gStartMs     = 0;       // 动画时间原点
static uint32_t gLastFrameMs = 0;
static bool     gFlames      = true;    // 蜡烛是否点着
static bool     gWishing     = false;   // 正在许愿（飘爱心）
static uint32_t gWishAt      = 0;
static uint32_t gFlashAt     = 0;       // 彩闪起点（0 = 没有）
static uint32_t gDialogAt    = 0;       // 对话框弹出时间
static const char* gDialog   = "生日快乐！";
static uint32_t gFrameNow    = 0;       // 本帧统一时间戳（三条带用同一个，避免接缝）
static float    gHeartAcc    = 0;

// ── 健康状态 / 自诊断 ──────────────────────────────────────────
static uint32_t gLastPokeMs  = 0;
static uint32_t gLastRetryMs = 0;
static uint32_t gFrameCount  = 0;       // 已推帧数（算 fps）
static uint32_t gFpsMarkMs   = 0;
static uint8_t  gFps         = 0;
static uint16_t gFrameMs     = 0;       // 最近一帧耗时（含 SPI）
static bool     gErrShown    = false;
static uint8_t  gResetReason = 0;
static uint32_t gBootCount   = 0;

// 跨复位保留的启动计数（RTC 不掉电内存；掉电归零，用 magic 校验）
RTC_NOINIT_ATTR static uint32_t bcRtcMagic;
RTC_NOINIT_ATTR static uint32_t bcRtcBoot;
#define BC_RTC_MAGIC 0x4244434BUL      // 'BDCK'

// ── 粒子（固定数组，零堆）───────────────────────────────────────
#define BC_BALLOON_MAX  28
#define BC_CONFETTI_MAX 44
#define BC_HEART_MAX    24
#define BC_SMOKE_MAX    12

struct BcBalloon { float x, y, v, r, ph; uint8_t col; };
struct BcConfetti { float x, y, v, ph; uint8_t col, w, h; };
struct BcHeart { float x, y, v, ph, life; };
struct BcSmoke { float x, y, v, ph, life; };

static BcBalloon  gBal[BC_BALLOON_MAX];
static uint8_t    gBalN = 0;
static BcConfetti gCon[BC_CONFETTI_MAX];
static uint8_t    gConN = 0;
static BcHeart    gHrt[BC_HEART_MAX];
static uint8_t    gHrtN = 0;
static BcSmoke    gSmk[BC_SMOKE_MAX];
static uint8_t    gSmkN = 0;

// ── 小工具 ──────────────────────────────────────────────────────
static inline int16_t bcRound(float v) { return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f); }

static float bcRand(float a, float b) {
  return a + (float)(random(0, 10001)) * 0.0001f * (b - a);
}

/** 这条带是否与场景纵坐标 [y0, y1) 相交 */
static inline bool bcBandHit(int16_t y0, int16_t y1) {
  return !(y1 <= gBandTop || y0 >= (int16_t)(gBandTop + BC_BAND_H));
}

/** 像素矩形（场景坐标；只画落在当前横带里的部分） */
static void bcPx(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t cl) {
  if (!gCv || w <= 0 || h <= 0) return;
  const int16_t yy = y - gBandTop;
  if (yy >= BC_BAND_H || yy + h <= 0) return;
  if (x >= 240 || x + w <= 0) return;
  gCv->fillRect(x, yy, w, h, BC(cl));       // 画布自身会裁掉越界部分
}

/** 逐行扫描的像素圆角矩形（保留锯齿感，不抗锯齿） */
static void bcRR(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t cl) {
  if (!gCv || w <= 0 || h <= 0) return;
  if (!bcBandHit(y, y + h)) return;
  const int16_t maxR = (w < h ? w : h) / 2;
  if (r > maxR) r = maxR;
  if (r <= 0) { bcPx(x, y, w, h, cl); return; }
  for (int16_t i = 0; i < r; i++) {
    const float dy = (float)r - 0.5f - (float)i;
    float v = (float)r * (float)r - dy * dy;
    if (v < 0) v = 0;
    const int16_t dx = bcRound((float)r - sqrtf(v));
    bcPx(x + dx, y + i, w - 2 * dx, 1, cl);
    bcPx(x + dx, y + h - 1 - i, w - 2 * dx, 1, cl);
  }
  if (h - 2 * r > 0) bcPx(x, y + r, w, h - 2 * r, cl);
}

/** 像素椭圆（蛋糕投影用） */
static void bcEll(int16_t cx, int16_t cy, int16_t rx, int16_t ry, uint8_t cl) {
  if (!gCv || rx <= 0 || ry <= 0) return;
  if (!bcBandHit(cy - ry, cy + ry)) return;
  for (int16_t dy = -ry; dy < ry; dy++) {
    const float t = (float)dy / (float)ry;
    float v = 1.0f - t * t;
    if (v < 0) v = 0;
    const int16_t w = bcRound((float)rx * sqrtf(v));
    if (w > 0) bcPx(cx - w, cy + dy, w * 2, 1, cl);
  }
}

static int16_t bcTextW(const char* s, uint8_t size) {
  return (int16_t)strlen(s) * 6 * size - size;
}

/** 5×7 内置字体的像素文字；outline=true 时先描一圈深色边 */
static void bcTextAt(const char* s, int16_t x, int16_t y, uint8_t size, uint8_t cl, bool outline) {
  if (!gCv) return;
  const int16_t textH = 8 * size;
  if (!bcBandHit(y, y + textH)) return;       // 整段文字不在本带 → 跳过
  const int16_t yy = y - gBandTop;
  gCv->setTextSize(size);
  if (outline) {
    gCv->setTextColor(BC(BC_INK));
    const int8_t ox[4] = {-1, 1, 0, 0};
    const int8_t oy[4] = {0, 0, -1, 1};
    for (uint8_t k = 0; k < 4; k++) {
      gCv->setCursor(x + ox[k], yy + oy[k]);
      gCv->print(s);
    }
  }
  gCv->setTextColor(BC(cl));
  gCv->setCursor(x, yy);
  gCv->print(s);
}

/** 像素精灵（PROGMEM 行优先 RLE + RGB565 调色板，value 15 = 透明） */
static void bcSprite(const uint8_t* rle, const uint16_t* pal,
                     int16_t w, int16_t h, int16_t dx, int16_t dy) {
  if (!gCv) return;
  if (!bcBandHit(dy, dy + h)) return;
  uint16_t p = 0;
  for (int16_t y = 0; y < h; y++) {
    const int16_t py = dy + y - gBandTop;
    const bool yOk = (py >= 0 && py < BC_BAND_H);
    int16_t x = 0;
    while (x < w) {
      uint8_t n = pgm_read_byte(rle + p++);
      const uint8_t v = pgm_read_byte(rle + p++);
      if (n == 0) break;                      // 数据异常保护：不推进就退出，避免死循环
      if (x + n > w) n = w - x;
      if (v != 15 && yOk) {
        const uint16_t c = (uint16_t)pgm_read_word(pal + v);
        for (int16_t k = 0; k < n; k++) {
          const int16_t px = dx + x + k;
          if (px >= 0 && px < 240) gCv->drawPixel(px, py, c);
        }
      }
      x += n;
    }
  }
}

// ════════════════════════════════════════════════════════════════
//  场景图层
// ════════════════════════════════════════════════════════════════

static void bcBackground() {
  if (!gCv) return;
  gCv->fillScreen(BC(BC_BG));            // 画布只有一条带高 → 整块清就是这一带的背景
  // 蛋糕后方极浅的粉色光晕（近似 rgba(255,255,255,.05)，上下左右对称）
  static const int16_t GLOW[3][3] = {{120, 122, 60}, {58, 96, 24}, {198, 122, 20}};
  for (uint8_t i = 0; i < 3; i++) {
    const int16_t cx = GLOW[i][0], cy = GLOW[i][1], r = GLOW[i][2];
    if (!bcBandHit(cy - r, cy + r)) continue;
    for (int16_t dy = -r; dy < r; dy += 2) {
      const float k = (float)dy / (float)r;
      float v = 1.0f - k * k;
      if (v < 0) v = 0;
      const int16_t w = bcRound((float)r * sqrtf(v));
      if (w > 0) bcPx(cx - w, cy + dy, w * 2, 2, BC_GLOW);
    }
  }
}

static void bcFloor() {
  if (!bcBandHit(188, 240)) return;
  bcPx(0, 188, 240, 240 - 188, BC_FLOOR);
  bcPx(0, 188, 240, 1, BC_FLOOR_LINE);
  for (int16_t y = 189; y < 240; y += 8) {
    for (int16_t x = 0; x < 240; x += 8) {
      if (((x / 8) + (y / 8)) % 2 == 0) {
        bcPx(x, y, 8, (y + 8 > 240) ? (240 - y) : 8, BC_FLOOR_ALT);
      }
    }
  }
}

static void bcStars(float t) {
  static const int16_t ST[5][3] = {{26, 92, 2}, {210, 94, 2}, {62, 122, 1}, {176, 66, 1}, {150, 56, 2}};
  for (uint8_t i = 0; i < 5; i++) {
    const int16_t x = ST[i][0], y = ST[i][1], s = ST[i][2];
    if (!bcBandHit(y - s, y + s + 1)) continue;
    const float a = 0.35f + 0.5f * fabsf(sinf(t * 1.6f + (float)x * 0.1f));
    if (a < 0.45f) continue;                     // 屏上没有 alpha，用「亮 / 不亮」模拟闪烁
    bcPx(x - s, y, s * 2 + 1, 1, BC_WHITE);
    bcPx(x, y - s, 1, s * 2 + 1, BC_WHITE);
  }
}

/** ① 顶部横幅（横幅本体 x 52..228, y 4..50） */
static void bcBanner() {
  if (!bcBandHit(4, 66)) return;
  // 左右燕尾飘带
  static const int16_t TX[2] = {58, 202};
  for (uint8_t k = 0; k < 2; k++) {
    const int16_t tx = TX[k];
    bcRR(tx, 36, 24, 24, 3, BC_INK);
    bcRR(tx + 2, 36, 20, 22, 2, BC_PINK);
    bcRR(tx + 4, 38, 3, 8, 1, BC_PINK_LT);
    for (int16_t i = 0; i < 8; i++) {
      const int16_t w = i * 2 + 1;
      bcPx(tx + 12 - i, 52 + i, w, 1, BC_BG);     // 燕尾缺口：露出背景
    }
  }
  // 底板
  bcRR(52, 4, 176, 46, 9, BC_INK);
  bcRR(54, 6, 172, 42, 8, BC_PINK);
  bcRR(57, 9, 166, 36, 6, BC_CREAM);
  bcRR(60, 11, 160, 5, 2, BC_CREAM_LT);
  // 文字（居中，带 1px 深色描边）
  const int16_t cx = 54 + 172 / 2;
  bcTextAt("HAPPY",     cx - bcTextW("HAPPY", 2) / 2,     11, 2, BC_PINK_DK, true);
  bcTextAt("BIRTHDAY!", cx - bcTextW("BIRTHDAY!", 2) / 2, 28, 2, BC_PINK_DK, true);
  // 两侧小爱心
  static const int16_t HX[2] = {67, 205};
  for (uint8_t k = 0; k < 2; k++) {
    const int16_t hx = HX[k], hy = 24;
    bcPx(hx, hy, 2, 2, BC_BOW);
    bcPx(hx + 3, hy, 2, 2, BC_BOW);
    bcPx(hx, hy + 1, 5, 3, BC_BOW);
    bcPx(hx + 1, hy + 4, 3, 1, BC_BOW);
    bcPx(hx + 2, hy + 5, 1, 1, BC_BOW);
  }
}

/** ③ 蛋糕装饰件 */
static void bcFrosting(int16_t x, int16_t y, int16_t w, int16_t f) {
  static const uint8_t DRIPS[14] = {6, 9, 4, 7, 5, 9, 4, 7, 6, 8, 4, 7, 5, 8};
  bcRR(x, y, w, f, 2, BC_PINK);
  bcRR(x + 2, y + 1, w - 4, 1, 0, BC_PINK_LT);
  uint8_t i = 0;
  for (int16_t sx = x + 1; sx < x + w - 2; sx += 7) {
    const uint8_t d = DRIPS[i++ % 14];
    int16_t ww = x + w - 1 - sx;
    if (ww > 6) ww = 6;
    if (ww > 0) bcRR(sx, y + f - 2, ww, d, 2, BC_PINK);
  }
}

static void bcBerry(int16_t x, int16_t y) {
  bcRR(x, y, 7, 8, 3, BC_INK);
  bcRR(x + 1, y + 1, 5, 6, 2, BC_RED);
  bcRR(x + 2, y + 2, 2, 2, 1, BC_BERRY_HI);
  bcRR(x + 2, y - 1, 3, 2, 1, BC_BERRY_LEAF);
}

static void bcDollop(int16_t x, int16_t y) {
  bcRR(x, y, 5, 5, 2, BC_INK);
  bcRR(x + 1, y + 1, 3, 3, 1, BC_WHITE);
  bcPx(x + 2, y - 1, 1, 1, BC_WHITE);
}

static void bcCakeTier(int16_t x, int16_t y, int16_t w, int16_t h, int16_t f) {
  if (!bcBandHit(y - 1, y + h + 4)) return;
  bcRR(x - 1, y - 1, w + 2, h + 4, 5, BC_INK);
  bcRR(x, y, w, h + 1, 3, BC_CREAM);
  bcRR(x + w - 7, y + 4, 7, h - 2, 3, BC_CREAM_DK);
  bcRR(x, y + h - 4, w, 4, 2, BC_TIER_BOT);
  bcRR(x + 3, y + 2, w - 6, 2, 1, BC_CREAM_LT);
  bcFrosting(x, y, w, f);
}

static void bcBow(int16_t cx, int16_t y) {
  if (!bcBandHit(y, y + 20)) return;
  bcRR(cx - 18, y, 15, 11, 4, BC_INK);
  bcRR(cx - 17, y + 1, 13, 9, 3, BC_BOW);
  bcRR(cx + 3, y, 15, 11, 4, BC_INK);
  bcRR(cx + 4, y + 1, 13, 9, 3, BC_BOW);
  bcRR(cx - 6, y + 3, 12, 11, 3, BC_INK);
  bcRR(cx - 5, y + 4, 10, 9, 2, BC_BOW2);
  bcRR(cx - 10, y + 10, 8, 9, 2, BC_INK);
  bcRR(cx - 9, y + 11, 6, 7, 1, BC_BOW);
  bcRR(cx + 2, y + 10, 8, 9, 2, BC_INK);
  bcRR(cx + 3, y + 11, 6, 7, 1, BC_BOW);
}

/** 像素火苗：上窄下宽，逐行改变宽度 */
static void bcFlameShape(int16_t cx, int16_t baseY, int16_t h, int16_t wMax, float sway, uint8_t cl) {
  if (h < 2) return;
  for (int16_t i = 0; i < h; i++) {
    const float p = (float)i / (float)(h - 1);
    int16_t w = bcRound(1.0f + p * p * (float)(wMax - 1));
    if (w < 1) w = 1;
    const int16_t x = cx - bcRound((float)w / 2.0f) + bcRound(sway * (1.0f - p));
    bcPx(x, baseY - h + i, w, 1, cl);
  }
}

static void bcFlame(int16_t cx, int16_t baseY, float t, float seed) {
  if (!bcBandHit(baseY - 30, baseY + 2)) return;
  const float sway = sinf(t * 6.3f + seed * 1.7f) * 1.8f;
  const int16_t h = 11 + bcRound(sinf(t * 8.7f + seed) * 2.0f);
  bcPx(cx - 1, baseY - 2, 2, 3, BC_WICK);
  bcFlameShape(cx, baseY, h + 4, 8, sway, BC_FLAME1);
  bcFlameShape(cx, baseY, h + 1, 6, sway * 0.9f, BC_FLAME2);
  bcFlameShape(cx, baseY, h * 2 / 3, 4, sway * 0.7f, BC_FLAME3);
  bcFlameShape(cx, baseY, h * 3 / 10, 2, sway * 0.5f, BC_FLAME4);
}

static void bcCake(float t) {
  if (!bcBandHit(56, 191)) return;
  // 地面投影
  bcEll(120, 184, 56, 6, BC_SHADOW);
  // 奶油盘 + 蕾丝边
  bcRR(58, 176, 124, 12, 6, BC_INK);
  bcRR(60, 178, 120, 9, 5, BC_PLATE);
  for (uint8_t i = 0; i < 11; i++) bcRR(64 + i * 10, 184, 7, 3, 2, BC_LACE);

  // 三层（底层 → 顶层）
  bcCakeTier(70, 148, 100, 30, 11);
  bcCakeTier(82, 124, 76, 24, 10);
  bcCakeTier(94, 102, 52, 22, 10);

  // 草莓 + 奶油小尖
  bcBerry(97, 106);  bcBerry(133, 106);
  bcDollop(114, 107);
  bcBerry(85, 128);  bcBerry(149, 128);
  bcBerry(116, 133);
  bcDollop(101, 129); bcDollop(137, 130);
  bcBerry(73, 152);  bcBerry(160, 152);
  bcBerry(96, 158);
  bcDollop(84, 154); bcDollop(141, 156);

  // 糖针
  static const int16_t SPR[12][3] = {
    {104, 152, BC_YELLOW}, {116, 162, BC_MINT}, {132, 151, BC_LAV},
    {142, 163, BC_YELLOW}, {94, 166, BC_SKY},   {124, 170, BC_MINT},
    {90, 138, BC_WHITE},   {148, 140, BC_YELLOW}, {120, 136, BC_LAV},
    {104, 116, BC_WHITE},  {134, 114, BC_YELLOW}, {126, 150, BC_SKY}
  };
  for (uint8_t i = 0; i < 12; i++) bcPx(SPR[i][0], SPR[i][1], 2, 2, (uint8_t)SPR[i][2]);

  bcBow(120, 156);

  // 数字蜡烛「20」：内置 5×7 字体放大 4 倍，位置与网页一致（图层 96,72）
  bcTextAt("2", 96 + 2,  74, 4, BC_PINK, true);
  bcTextAt("0", 96 + 26, 74, 4, BC_PINK, true);
  bcRR(96 + 5,  102, 14, 5, 2, BC_INK);
  bcRR(96 + 6,  103, 12, 3, 1, BC_CREAM);
  bcRR(96 + 29, 102, 14, 5, 2, BC_INK);
  bcRR(96 + 30, 103, 12, 3, 1, BC_CREAM);

  if (gFlames) {
    bcFlame(108, 74, t, 0.0f);
    bcFlame(132, 74, t, 2.1f);
  } else {
    bcPx(107, 71, 2, 3, BC_DEADWICK);
    bcPx(131, 71, 2, 3, BC_DEADWICK);
  }
}

// ════════════════════════════════════════════════════════════════
//  中文点阵（R9）：UTF-8 解码 → 码点查表 → 16 行位图绘制
//  字模来自 bdcake_cjk.h（只有对话框用到的 29 个字，928 B）
// ════════════════════════════════════════════════════════════════
static uint32_t bcUtf8Next(const uint8_t** p) {
  const uint8_t c = **p;
  if (c < 0x80) { (*p)++; return (uint32_t)c; }
  uint32_t v;
  uint8_t n;
  if ((c & 0xE0) == 0xC0)      { v = (uint32_t)(c & 0x1F); n = 1; }
  else if ((c & 0xF0) == 0xE0) { v = (uint32_t)(c & 0x0F); n = 2; }
  else if ((c & 0xF8) == 0xF0) { v = (uint32_t)(c & 0x07); n = 3; }
  else { (*p)++; return (uint32_t)'?'; }
  (*p)++;
  while (n--) { v = (v << 6) | (uint32_t)(**p & 0x3F); (*p)++; }
  return v;
}

static int8_t bcCjkIndex(uint32_t cp) {
  for (uint8_t i = 0; i < BC_CJK_COUNT; i++) {
    if (pgm_read_dword(&BC_CJK_CP[i]) == cp) return (int8_t)i;
  }
  return -1;
}

static uint8_t bcUtf8Count(const char* s) {
  uint8_t n = 0;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) { bcUtf8Next(&p); n++; }
  return n;
}

/** 一个 16×16 汉字：逐行取位，连续 1 的位合并成一次 fillRect */
static void bcCjkGlyph(uint32_t cp, int16_t x, int16_t y, uint8_t cl) {
  const int8_t gi = bcCjkIndex(cp);
  if (gi < 0) {                              // 字模没收录：画个小方块占位，绝不刷成乱码
    bcPx(x + 5, y + 5, 6, 6, BC_PINK_LT);
    return;
  }
  const uint16_t base = (uint16_t)((uint8_t)gi) * BC_CJK_H;
  for (int16_t r = 0; r < BC_CJK_H; r++) {
    uint16_t bits = (uint16_t)pgm_read_word(&BC_CJK_ROWS[base + r]);
    if (!bits) continue;
    int16_t c = 0;
    while (c < BC_CJK_W) {
      if (bits & (uint16_t)(0x8000u >> c)) {
        const int16_t s0 = c;
        while (c < BC_CJK_W && (bits & (uint16_t)(0x8000u >> c))) c++;
        bcPx(x + s0, y + r, c - s0, 1, cl);
      } else {
        c++;
      }
    }
  }
}

/** 一行中文的像素宽度（字距 1px） */
static int16_t bcCjkWidth(const char* s) {
  const uint8_t n = bcUtf8Count(s);
  return (int16_t)n * BC_CJK_W + (int16_t)(n > 0 ? (n - 1) : 0);
}

static void bcCjkText(const char* s, int16_t x, int16_t y, uint8_t cl) {
  const uint8_t* p = (const uint8_t*)s;
  int16_t cx = x;
  while (*p) {
    bcCjkGlyph(bcUtf8Next(&p), cx, y, cl);
    cx += BC_CJK_W + 1;
  }
}

// ── ⑥ 底部像素对话框（中文点阵，与网页同一句文案）───────────────
static void bcDialogBox(uint32_t now) {
  if (!bcBandHit(194, 240)) return;
  const int16_t boxW = 216, boxH = 38;
  const int16_t x = (240 - boxW) / 2, y = 194;
  float k = (float)(now - gDialogAt) / (float)BC_DLG_SLIDE;
  if (k < 0) k = 0;
  if (k > 1) k = 1;
  const int16_t oy = bcRound((1.0f - k) * 8.0f);
  bcRR(x - 3, y - 3 + oy, boxW + 6, boxH + 6, 10, BC_INK);
  bcRR(x - 1, y - 1 + oy, boxW + 2, boxH + 2, 8, BC_PINK_LT);
  bcRR(x + 1, y + 1 + oy, boxW - 2, boxH - 2, 7, BC_CREAM_LT);
  const int16_t tw = bcCjkWidth(gDialog);
  bcCjkText(gDialog, 120 - tw / 2, y + oy + 11, BC_INK);
}

/** 换一句对话框文案并重新开始上滑动画 */
static void bcShowDialog(const char* text) {
  gDialog   = text;
  gDialogAt = millis();
}

// ── ⑦ 粒子 ──────────────────────────────────────────────────────
static void bcSpawnParty() {
  gBalN = 0; gConN = 0;
  for (uint8_t i = 0; i < BC_BALLOON_MAX; i++) {
    BcBalloon &b = gBal[gBalN++];
    b.x  = bcRand(10, 230);
    b.y  = (i < 16) ? bcRand(100, 240) : bcRand(250, 360);
    b.v  = bcRand(22, 52);
    b.r  = bcRand(5, 9);
    b.ph = bcRand(0, 6.28f);
    b.col = (uint8_t)(BC_BAL0 + random(0, 7));
  }
  for (uint8_t i = 0; i < BC_CONFETTI_MAX; i++) {
    BcConfetti &f = gCon[gConN++];
    f.x  = bcRand(0, 240);
    f.y  = (i < 24) ? bcRand(110, 230) : bcRand(240, 330);
    f.v  = bcRand(34, 88);
    f.ph = bcRand(0, 6.28f);
    f.col = (uint8_t)(BC_BAL0 + random(0, 7));
    f.w = 2 + (uint8_t)random(0, 2);
    f.h = 3 + (uint8_t)random(0, 3);
  }
}

static void bcSpawnHearts(uint8_t n) {
  for (uint8_t i = 0; i < n && gHrtN < BC_HEART_MAX; i++) {
    BcHeart &h = gHrt[gHrtN++];
    h.x = bcRand(96, 148);
    h.y = bcRand(152, 164);
    h.v = bcRand(16, 34);
    h.ph = bcRand(0, 6.28f);
    h.life = 1.0f;
  }
}

static void bcSpawnSmoke() {
  gSmkN = 0;
  for (uint8_t i = 0; i < BC_SMOKE_MAX; i++) {
    BcSmoke &s = gSmk[gSmkN++];
    s.x = (float)((i % 2) ? 108 : 132) + bcRand(-3, 3);
    s.y = 72;
    s.v = bcRand(14, 26);
    s.ph = bcRand(0, 6.28f);
    s.life = 1.0f;
  }
}

static void bcUpdateParticles(float dt, float t) {
  for (uint8_t i = 0; i < gBalN; i++) {
    gBal[i].y -= gBal[i].v * dt;
    gBal[i].x += sinf(t * 1.5f + gBal[i].ph) * 14 * dt;
  }
  for (uint8_t i = 0; i < gBalN; i++) {
    if (gBal[i].y <= -26) { gBal[i] = gBal[--gBalN]; i--; }
  }
  for (uint8_t i = 0; i < gConN; i++) {
    gCon[i].y -= gCon[i].v * dt;
    gCon[i].x += sinf(t * 2.4f + gCon[i].ph) * 18 * dt;
  }
  for (uint8_t i = 0; i < gConN; i++) {
    if (gCon[i].y <= -14) { gCon[i] = gCon[--gConN]; i--; }
  }
  for (uint8_t i = 0; i < gHrtN; i++) {
    gHrt[i].y -= gHrt[i].v * dt;
    gHrt[i].x += sinf(t * 2.2f + gHrt[i].ph) * 10 * dt;
    gHrt[i].life -= dt * 0.42f;
  }
  for (uint8_t i = 0; i < gHrtN; i++) {
    if (gHrt[i].life <= 0) { gHrt[i] = gHrt[--gHrtN]; i--; }
  }
  for (uint8_t i = 0; i < gSmkN; i++) {
    gSmk[i].y -= gSmk[i].v * dt;
    gSmk[i].x += sinf(t * 3.0f + gSmk[i].ph) * 13 * dt;
    gSmk[i].life -= dt * 0.5f;
  }
  for (uint8_t i = 0; i < gSmkN; i++) {
    if (gSmk[i].life <= 0) { gSmk[i] = gSmk[--gSmkN]; i--; }
  }
  if (gWishing) {
    gHeartAcc -= dt;
    if (gHeartAcc <= 0) { bcSpawnHearts(1); gHeartAcc = 0.26f; }
  }
}

static void bcHeartPx(int16_t x, int16_t y, uint8_t s, uint8_t cl) {
  static const uint8_t HEART[6] = {0x36, 0x7F, 0x7F, 0x3E, 0x1C, 0x08};   // 7 宽心形位图
  for (uint8_t r = 0; r < 6; r++) {
    for (uint8_t c = 0; c < 7; c++) {
      if (HEART[r] & (0x40 >> c)) bcPx(x + c * s, y + r * s, s, s, cl);
    }
  }
}

static void bcParticles(float t) {
  if (!gCv) return;
  for (uint8_t i = 0; i < gBalN; i++) {
    BcBalloon &b = gBal[i];
    const int16_t cx = bcRound(b.x), cy = bcRound(b.y), r = bcRound(b.r);
    if (!bcBandHit(cy - r - 20, cy + r + 20)) continue;
    for (int16_t dy = -r; dy <= r; dy++) {
      const float k = (float)dy / b.r;
      float v = 1.0f - k * k;
      if (v < 0) v = 0;
      const int16_t w = bcRound(b.r * sqrtf(v));
      if (w > 0) bcPx(cx - w, cy + dy, w * 2, 1, b.col);
    }
    bcPx(cx - bcRound(b.r * 0.5f), cy - bcRound(b.r * 0.55f), 2, 2, BC_WHITE);
    bcPx(cx - 1, cy + r, 2, 2, b.col);
    for (uint8_t k = 0; k < 8; k++) {
      bcPx(cx + bcRound(sinf(t * 2 + k * 0.6f) * 2),
           cy + r + 2 + k * 2, 1, 2, BC_WHITE);
    }
  }
  for (uint8_t i = 0; i < gConN; i++) {
    bcPx(bcRound(gCon[i].x), bcRound(gCon[i].y), gCon[i].w, gCon[i].h, gCon[i].col);
  }
  for (uint8_t i = 0; i < gSmkN; i++) {
    if (gSmk[i].life <= 0.25f) continue;          // 快消失的烟不再画（无 alpha）
    bcPx(bcRound(gSmk[i].x), bcRound(gSmk[i].y), 3, 3, BC_SMOKE);
  }
  for (uint8_t i = 0; i < gHrtN; i++) {
    if (gHrt[i].life <= 0.08f) continue;
    bcHeartPx(bcRound(gHrt[i].x), bcRound(gHrt[i].y), 1,
              (gHrt[i].life > 0.5f) ? BC_HEART : BC_PINK_LT);
  }
}

/** 吹蜡烛后的彩色闪烁：只叠彩条 + 细白框（不做整屏白闪，避免瞬间大电流） */
static void bcFlashFx(uint32_t now) {
  if (gFlashAt == 0) return;
  const uint32_t e = now - gFlashAt;
  if (e >= BC_FLASH_MS) return;
  const int16_t bands = 6, bh = 40;
  for (int16_t i = 0; i < bands; i++) {
    const uint8_t cl = (uint8_t)(BC_BAL0 + ((i * 3 + (int)(e / 90)) % 7));
    for (int16_t y = i * bh; y < (i + 1) * bh; y += 2) {
      bcPx(0, y, 240, 1, cl);
    }
  }
  if (e < 140) {                                  // 细白框代替整屏白闪
    bcPx(0, 0, 240, 2, BC_WHITE);
    bcPx(0, 238, 240, 2, BC_WHITE);
    bcPx(0, 0, 2, 240, BC_WHITE);
    bcPx(238, 0, 2, 240, BC_WHITE);
  }
}

// ════════════════════════════════════════════════════════════════
//  合成 + 分带上屏
// ════════════════════════════════════════════════════════════════
static void bcDrawScene(float t) {
  bcBackground();
  bcFloor();
  bcStars(t);
  bcBanner();
  if (bcBandHit(8, 45)) {
    bcSprite(BC_CRAB_RLE, BC_CRAB_PAL, BC_CRAB_W, BC_CRAB_H,
             4, 8 + bcRound(sinf(t * 2.2f)));
  }
  bcCake(t);
  bcSprite(BC_YUYU_RLE, BC_YUYU_PAL, BC_YUYU_W, BC_YUYU_H,
           4, 126 + bcRound(sinf(t * 1.6f)));
  bcSprite(BC_WANG_RLE, BC_WANG_PAL, BC_WANG_W, BC_WANG_H,
           168, 100 + bcRound(sinf(t * 1.3f + 0.7f)));
  bcParticles(t);
  bcDialogBox(gFrameNow);          // 用本帧统一时间戳：三条带的对话框位置完全一致
}

static bool bcRenderFrame() {
  if (!gCv) return false;
  const uint32_t t0 = millis();
  gFrameNow = t0;
  const float t = (float)(t0 - gStartMs) / 1000.0f;
  for (int8_t b = 0; b < BC_BANDS; b++) {
    gBandTop = (int16_t)(b * BC_BAND_H);
    bcDrawScene(t);                  // 整场重画一遍，只有落在本带的像素会落进画布
    bcFlashFx(t0);                   // 彩闪也在合成之后、按带叠加
    tft.drawRGBBitmap(0, gBandTop, gCv->getBuffer(), 240, BC_BAND_H);
  }
  gBandTop = 0;
  gFrameMs = (uint16_t)(millis() - t0);
  gFrameCount++;
  if (gFpsMarkMs == 0) gFpsMarkMs = t0;
  if ((uint32_t)(t0 - gFpsMarkMs) >= 1000) {
    gFps = (uint8_t)(gFrameCount > 255 ? 255 : gFrameCount);
    gFrameCount = 0;
    gFpsMarkMs = t0;
  }
  return true;
}

// ════════════════════════════════════════════════════════════════
//  面板健康 / 画布兜底 / 诊断
// ════════════════════════════════════════════════════════════════

/** 重新初始化面板：面板被意外复位 / 休眠 / 上电顺序抖动导致白屏时，这一步能救回来 */
static void bcDisplayInit() {
  tft.init(240, 240);
  tft.setRotation(1);
  tft.setSPISpeed(40000000);
  tft.writeCommand(ST77XX_DISPON);      // 幂等：确保显示打开
  tft.writeCommand(ST77XX_NORON);
  Serial.println("[BDCAKE] 面板已重新初始化（240x240 / rot1 / 40MHz）");
}

/** 动画期间每 5 秒补一条 DISPON，防止面板被误关显示后一直白屏 */
static void bcDisplayPoke(uint32_t now) {
  if ((uint32_t)(now - gLastPokeMs) < BC_POKE_MS) return;
  gLastPokeMs = now;
  tft.writeCommand(ST77XX_DISPON);
}

static bool bcEnsureCanvas() {
  if (gCv) return true;
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < BC_MIN_HEAP) {
    Serial.printf("[BDCAKE] 画布暂不申请：剩余堆 %u < %u\n", (unsigned)heap, (unsigned)BC_MIN_HEAP);
    return false;
  }
  gCv = new GFXcanvas16(240, BC_BAND_H);
  if (gCv && gCv->getBuffer() == nullptr) { delete gCv; gCv = nullptr; }
  if (gCv) {
    Serial.printf("[BDCAKE] 横带画布就绪 240x%d x2 = %u B，剩余堆 %u B\n",
                  BC_BAND_H, (unsigned)(240 * BC_BAND_H * 2), (unsigned)ESP.getFreeHeap());
  } else {
    Serial.printf("[BDCAKE] 横带画布申请失败，剩余堆 %u B\n", (unsigned)ESP.getFreeHeap());
  }
  return gCv != nullptr;
}

/** 画布申请不到时：在屏上写清原因（绝不是白屏），并给出堆数值 */
static void bcDrawFailScreen(const char* why) {
  const uint16_t bg = bcC[BC_BG], ink = bcC[BC_INK];
  tft.fillScreen(bg);
  tft.fillRect(0, 0, 240, 6, ink);
  tft.setTextColor(ink);
  tft.setTextSize(2);
  tft.setCursor(12, 40);  tft.print("BIRTHDAY ANIM");
  tft.setTextSize(1);
  tft.setCursor(12, 80);  tft.print("screen buffer failed");
  tft.setCursor(12, 96);  tft.print(why);
  tft.setCursor(12, 118); tft.printf("heap=%lu  boot=%lu",
                                     (unsigned long)ESP.getFreeHeap(),
                                     (unsigned long)gBootCount);
  tft.setCursor(12, 134); tft.print("retry every 3s ...");
  gErrShown = true;
}

static const char* bcResetReasonName(uint8_t r) {
  switch (r) {
    case 1:  return "POWERON";
    case 2:  return "EXT";
    case 3:  return "SW";
    case 4:  return "PANIC";
    case 5:  return "INT_WDT";
    case 6:  return "TASK_WDT";
    case 7:  return "WDT";
    case 8:  return "DEEPSLEEP";
    case 9:  return "BROWNOUT";
    case 10: return "SDIO";
    default: return "UNKNOWN";
  }
}

// ════════════════════════════════════════════════════════════════
//  对外接口
// ════════════════════════════════════════════════════════════════
void bdcakeBegin() {
  memcpy_P(bcC, BC_PROG, sizeof(bcC));
  randomSeed(esp_random());
  gStartMs     = millis();
  gLastFrameMs = 0;
  gDialogAt    = 0;          // 0 = 立即显示，无上滑
  gFlashAt     = 0;
  gLastPokeMs  = gStartMs;

  // 启动诊断：复位原因 + 启动次数（跨软件复位保留；掉电归零）
  gResetReason = (uint8_t)esp_reset_reason();
  if (bcRtcMagic != BC_RTC_MAGIC) { bcRtcMagic = BC_RTC_MAGIC; bcRtcBoot = 0; }
  gBootCount = ++bcRtcBoot;
  Serial.printf("[BDCAKE] boot #%u  reset=%u(%s)  heap=%u\n",
                (unsigned)gBootCount, (unsigned)gResetReason,
                bcResetReasonName(gResetReason), (unsigned)ESP.getFreeHeap());
}

void bdcakeEnterView() {
  currentView = VIEW_BDCAKE;
  bcDisplayInit();                       // 先确保面板是醒着的（白屏自愈）
  if (!bcEnsureCanvas()) {
    bcDrawFailScreen("low memory");
    return;
  }
  if (gStartMs == 0) gStartMs = millis();
  gErrShown = false;
  const uint32_t t0 = millis();
  bcRenderFrame();
  gLastFrameMs = millis();
  Serial.printf("[BDCAKE] enter ok：首帧 %u ms，整帧 %u ms，堆 %u B\n",
                (unsigned)(millis() - t0), (unsigned)gFrameMs,
                (unsigned)ESP.getFreeHeap());
}

void bdcakeRedraw() {
  // 只重画，不重初始化面板：/redraw 会被背景色、速度滑杆频繁触发，
  // 每次都 tft.init() 会闪屏。真正的「救屏」在 bdcakeEnterView() / /bc/show。
  if (currentView != VIEW_BDCAKE) return;
  if (!bcEnsureCanvas()) { bcDrawFailScreen("low memory"); return; }
  bcRenderFrame();
  gLastFrameMs = millis();
}

void bdcakeTick() {
  if (currentView != VIEW_BDCAKE) return;
  const uint32_t now = millis();

  // 画布没了（首次失败）→ 每 3 秒重试一次；仍失败就维持「原因屏」
  if (!gCv) {
    if ((uint32_t)(now - gLastRetryMs) >= BC_RETRY_MS) {
      gLastRetryMs = now;
      if (bcEnsureCanvas()) {
        gErrShown = false;
        bcRenderFrame();
        gLastFrameMs = now;
      } else if (!gErrShown) {
        bcDrawFailScreen("low memory");
      }
    }
    return;
  }

  // 许愿 → 3 秒后给结果
  if (gWishing && (uint32_t)(now - gWishAt) >= BC_WISH_MS) {
    gWishing = false;
    bcShowDialog("许愿成功，心想事成");
    bcSpawnHearts(4);
  }

  bcDisplayPoke(now);

  if ((uint32_t)(now - gLastFrameMs) < BC_FRAME_MS) return;
  gLastFrameMs = now;
  bcUpdateParticles((float)BC_FRAME_MS / 1000.0f, (float)(now - gStartMs) / 1000.0f);
  bcRenderFrame();
}

/** 若桌宠没停在生日动画视图，先切过去（保证「点按钮 = 屏幕上有反应」） */
static void bcEnsureInView() {
  if (currentView != VIEW_BDCAKE || !gCv) bdcakeEnterView();
}

void bdcakeWish() {
  bcEnsureInView();
  gWishing  = true;
  gWishAt   = millis();
  gHeartAcc = 0;
  bcShowDialog("闭上眼睛许愿吧");
  bcSpawnHearts(3);
  if (currentView == VIEW_BDCAKE && gCv) { bcRenderFrame(); gLastFrameMs = millis(); }
}

void bdcakeBlow() {
  bcEnsureInView();
  gWishing = false;
  gFlames  = false;
  gFlashAt = millis();
  if (gFlashAt == 0) gFlashAt = 1;
  bcSpawnParty();
  bcSpawnSmoke();
  bcShowDialog("祝你生日快乐！");
  if (currentView == VIEW_BDCAKE && gCv) { bcRenderFrame(); gLastFrameMs = millis(); }
}

void bdcakeReset() {
  bcEnsureInView();
  gWishing = false;
  gFlames  = true;
  gFlashAt = 0;
  gBalN = 0; gConN = 0; gHrtN = 0; gSmkN = 0;
  gHeartAcc = 0;
  bcShowDialog("蜡烛点好啦，再来一次吧");
  if (currentView == VIEW_BDCAKE && gCv) { bcRenderFrame(); gLastFrameMs = millis(); }
}

bool bdcakeFlamesOn()     { return gFlames; }
bool bdcakeIsWishing()    { return gWishing; }
bool bdcakeCanvasReady()  { return gCv != nullptr; }
uint32_t bdcakeHeap()     { return ESP.getFreeHeap(); }
uint32_t bdcakeHeapMin()  { return ESP.getMinFreeHeap(); }
uint32_t bdcakeUptimeSec(){ return (millis() - gStartMs) / 1000; }
uint32_t bdcakeBootCount(){ return gBootCount; }
uint8_t  bdcakeResetReason(){ return gResetReason; }
const char* bdcakeResetReasonName() { return bcResetReasonName(gResetReason); }
uint8_t  bdcakeFps()      { return gFps; }
uint16_t bdcakeFrameMs()  { return gFrameMs; }
