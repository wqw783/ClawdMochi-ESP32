/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  expression.cpp — 表情管理系统实现
 *
 *  三层结构：
 *    ① ExpressionManager  表情状态 + 锁定 + 切换          ← 本文件上半部
 *    ② Renderer           把「表情定义 + 一帧形变」画出来  ← 本文件中部
 *    ③ Animation          把「时间」变成「一帧形变」      ← 本文件下半部
 *
 *  渲染与动画完全解耦：动画只产出 FaceFrame{dx,dy,eyeOpen}，
 *  渲染器只消费 FaceFrame，双方互不知道对方存在。
 *  因此 HAPPY + BLINK / HAPPY + BOUNCE / ANGRY + SHAKE 可以任意组合。
 *
 *  内存：全部为静态变量（约 40 字节），渲染路径无 new / 无 String / 无帧缓冲。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "expression.h"
#include "expression_data.h"
#include <math.h>

// ═══════════════════════════════════════════════════════════════════════════
//  ① 内部状态
// ═══════════════════════════════════════════════════════════════════════════

static ExpressionId gExpr       = EXPR_HAPPY;
static AnimId       gAnim       = ANIM_BLINK;
static bool         gLocked     = false;
static bool         gDirty      = true;                       // 需要强制重绘
static uint32_t     gLastTickMs = 0;
static FaceFrame    gLastFrame  = { (int16_t)-32768, (int16_t)-32768, 255, 255 };

// 眨眼状态机：0 = 睁眼等待，1 = 闭合中
static uint8_t  gBlinkStep   = 0;
static uint32_t gBlinkStepMs = 0;
static uint32_t gBlinkNextMs = 0;

// 表情专用强调色（只在本模块使用，不动主固件的全局调色板）
static uint16_t EX_BLUSH, EX_TEAR, EX_SWEAT, EX_HEART, EX_SPARK;

// 动画帧间隔基准（会再过一遍 speedMs()，跟随主固件的速度滑块）
#define EXPR_FRAME_BASE 45

// ═══════════════════════════════════════════════════════════════════════════
//  通用绘制小工具
// ═══════════════════════════════════════════════════════════════════════════

// 加粗直线：垂直方向平行复制若干条
static void thickLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint8_t thk, uint16_t col) {
  const int8_t h = (int8_t)(thk / 2);
  for (int8_t t = -h; t <= h; t++)
    tft.drawLine(x1, y1 + t, x2, y2 + t, col);
}

// 加粗折线：pts 为 {x0,y0, x1,y1, ...}，n = 点数
// 注意用 int 而非 int16_t：初始化列表里的 x+2 之类是 int，
// 塞进 int16_t 数组会触发 C++11 的窄化转换硬错误。
static void thickPoly(const int* pts, uint8_t n, uint8_t thk, uint16_t col) {
  for (uint8_t i = 0; i + 1 < n; i++)
    thickLine((int16_t)pts[i * 2], (int16_t)pts[i * 2 + 1],
              (int16_t)pts[i * 2 + 2], (int16_t)pts[i * 2 + 3], thk, col);
}

// 水滴 / 泪滴
static void drawDrop(int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  tft.fillCircle(cx, cy, r, col);
  tft.fillTriangle(cx - r + 1, cy - 1, cx + r - 1, cy - 1, cx, cy - r * 2, col);
}

// 爱心
static void drawHeart(int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  const int16_t b = r / 2 + 1;
  const int16_t ny = cy - r / 4;
  tft.fillCircle(cx - r / 2, ny, b, col);
  tft.fillCircle(cx + r / 2, ny, b, col);
  tft.fillTriangle(cx - r + 1, ny + 1, cx + r - 1, ny + 1, cx, cy + r, col);
}

// 四角星（闪光）
static void drawSparkle(int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  const int16_t w = r / 3 + 1;
  tft.fillTriangle(cx, cy - r, cx - w, cy, cx + w, cy, col);
  tft.fillTriangle(cx, cy + r, cx - w, cy, cx + w, cy, col);
  tft.fillTriangle(cx - r, cy, cx, cy - w, cx, cy + w, col);
  tft.fillTriangle(cx + r, cy, cx, cy - w, cx, cy + w, col);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ② 渲染器 —— 只消费 FaceFrame，不知道动画的存在
// ═══════════════════════════════════════════════════════════════════════════

// ── 单只眼睛 ────────────────────────────────────────────────────────────
// x  = 30×60 眼睛框的左边缘；cy = 垂直中心；left = 是否左眼
static void drawEyeShape(uint8_t style, int16_t x, int16_t cy, bool left,
                         uint8_t op, uint16_t bg) {
  const uint16_t K = ST77XX_BLACK;
  const uint16_t W = ST77XX_WHITE;
  const int16_t  cx = x + EYE_W / 2;

  // ── 闭眼（张开度不足 25%）统一降级 ──
  if (op < 25) {
    switch (style) {
      case EYE_ARC_UP: case EYE_SPARKLE: case EYE_WINK: case EYE_HEART: {
        const int p[6] = { x + 2, cy + 14, cx, cy - 14, x + EYE_W - 2, cy + 14 };
        thickPoly(p, 3, 7, K);
        return;
      }
      case EYE_CRY_CLOSED: {
        const int p[6] = { x + 2, cy - 14, cx, cy + 14, x + EYE_W - 2, cy - 14 };
        thickPoly(p, 3, 7, K);
        return;
      }
      default:
        tft.fillRect(x, cy - 4, EYE_W, 8, K);
        return;
    }
  }

  switch (style) {
    case EYE_SOLID: {
      const int16_t h = (int16_t)(EYE_H * op / 100);
      if (h < 8) { tft.fillRect(x, cy - 4, EYE_W, 8, K); break; }
      tft.fillRect(x, cy - h / 2, EYE_W, h, K);
      break;
    }
    case EYE_ARC_UP: {
      const int p[6] = { x + 2, cy + 15, cx, cy - 15, x + EYE_W - 2, cy + 15 };
      thickPoly(p, 3, 7, K);
      break;
    }
    case EYE_ARC_DOWN: {
      const int p[6] = { x + 2, cy - 15, cx, cy + 15, x + EYE_W - 2, cy - 15 };
      thickPoly(p, 3, 7, K);
      break;
    }
    case EYE_CHEVRON:
      // 与原 Squish eyes 完全一致的画法
      drawChevron(cx, cy, EYE_H / 2, EYE_W / 2, 10, left, K);
      break;
    case EYE_ROUND:
      tft.fillCircle(cx, cy, 15, K);
      tft.fillCircle(cx - 5, cy - 6, 4, W);
      break;
    case EYE_ROUND_SMALL:
      tft.fillCircle(cx, cy, 9, K);
      tft.fillCircle(cx - 3, cy - 3, 3, W);
      break;
    case EYE_SPARKLE:
      tft.fillCircle(cx, cy, 15, K);
      drawSparkle(cx - 3, cy - 4, 9, W);
      tft.fillCircle(cx + 7, cy + 8, 2, W);
      break;
    case EYE_HEART:
      drawHeart(cx, cy, 15, EX_HEART);
      break;
    case EYE_FLAT:
      tft.fillRect(x, cy - 4, EYE_W, 8, K);
      break;
    case EYE_FLAT_SIDE:
      tft.fillRect(x + 8, cy - 4, EYE_W - 12, 8, K);
      break;
    case EYE_TEARY:
      tft.fillCircle(cx, cy, 14, K);
      tft.fillCircle(cx - 4, cy - 5, 4, W);
      drawDrop(cx, cy + 24, 5, EX_TEAR);
      break;
    case EYE_SAD: {
      tft.fillCircle(cx, cy + 4, 13, K);
      tft.fillCircle(cx - 4, cy - 1, 4, W);
      // 用底色切掉上外眼角，做出「下垂」的感觉
      if (left) tft.fillTriangle(cx - 15, cy - 18, cx + 15, cy - 18, cx - 15, cy + 2, bg);
      else      tft.fillTriangle(cx - 15, cy - 18, cx + 15, cy - 18, cx + 15, cy + 2, bg);
      break;
    }
    case EYE_CRY_CLOSED: {
      const int p[6] = { x + 2, cy - 14, cx, cy + 14, x + EYE_W - 2, cy - 14 };
      thickPoly(p, 3, 7, K);
      break;
    }
    case EYE_WINK:
      if (left) {
        tft.fillRect(x, cy - EYE_H / 2, EYE_W, EYE_H, K);
      } else {
        const int p[6] = { x + 2, cy + 15, cx, cy - 15, x + EYE_W - 2, cy + 15 };
        thickPoly(p, 3, 7, K);
      }
      break;
    case EYE_UNEVEN:
      if (left) {
        tft.fillCircle(cx, cy, 15, K);
        tft.fillCircle(cx - 5, cy - 6, 4, W);
      } else {
        tft.fillCircle(cx, cy, 8, K);
        tft.fillCircle(cx - 3, cy - 3, 3, W);
      }
      break;
    case EYE_CLOSED:
    default:
      tft.fillRect(x, cy - 4, EYE_W, 8, K);
      break;
  }
}

// ── 墨镜（横跨双眼，单独处理）──
static void drawShades(int16_t dx, int16_t dy) {
  const int16_t cy = EXPR_EYE_CY + dy;
  const int16_t y  = cy - 15;
  const int16_t L0 = 20 + dx,  L1 = 112 + dx;
  const int16_t R0 = 128 + dx, R1 = 220 + dx;
  tft.fillRoundRect(L0, y, L1 - L0, 30, 9, ST77XX_BLACK);
  tft.fillRoundRect(R0, y, R1 - R0, 30, 9, ST77XX_BLACK);
  tft.fillRect(L1, cy - 4, R0 - L1, 8, ST77XX_BLACK);      // 鼻梁
  tft.fillRect(L0 + 10, y + 7, 18, 4, EX_SPARK);           // 镜面反光
  tft.fillRect(R0 + 10, y + 7, 18, 4, EX_SPARK);
}

static void drawEyes(uint8_t style, int16_t dx, int16_t dy, uint8_t op, uint16_t bg) {
  if (style == EYE_SHADES) { drawShades(dx, dy); return; }
  const int16_t lx = EXPR_EYE_LX + dx;
  const int16_t rx = EXPR_EYE_RX + dx;
  const int16_t cy = EXPR_EYE_CY + dy;
  drawEyeShape(style, lx, cy, true,  op, bg);
  drawEyeShape(style, rx, cy, false, op, bg);
}

// ── 嘴 ──────────────────────────────────────────────────────────────────
static void drawMouth(uint8_t style, int16_t dx, int16_t dy) {
  const uint16_t K  = ST77XX_BLACK;
  const uint16_t bg = animBgColor;
  const int16_t  cx = EXPR_MOUTH_X + dx;
  const int16_t  cy = EXPR_MOUTH_Y + dy;

  switch (style) {
    case MOUTH_SMILE: {
      const int p[10] = { cx - 28, cy - 8, cx - 14, cy + 2, cx, cy + 5,
                              cx + 14, cy + 2, cx + 28, cy - 8 };
      thickPoly(p, 5, 5, K);
      break;
    }
    case MOUTH_OPEN: {
      tft.fillCircle(cx, cy - 8, 18, K);
      tft.fillRect(cx - 19, cy - 40, 38, 24, bg);          // 削平上沿 → D 形
      tft.fillCircle(cx, cy + 4, 7, EX_HEART);             // 舌头
      break;
    }
    case MOUTH_OPEN_BIG: {
      tft.fillCircle(cx, cy - 6, 26, K);
      tft.fillRect(cx - 27, cy - 40, 54, 22, bg);
      tft.fillCircle(cx, cy + 8, 10, EX_HEART);
      break;
    }
    case MOUTH_SMALL_O:
      tft.fillCircle(cx, cy - 6, 9, K);
      break;
    case MOUTH_FROWN: {
      const int p[10] = { cx - 28, cy + 6, cx - 14, cy - 4, cx, cy - 7,
                              cx + 14, cy - 4, cx + 28, cy + 6 };
      thickPoly(p, 5, 5, K);
      break;
    }
    case MOUTH_FROWN_OPEN:
      tft.fillRoundRect(cx - 16, cy - 12, 32, 20, 7, K);
      break;
    case MOUTH_FLAT:
      tft.fillRect(cx - 24, cy - 4, 48, 7, K);
      break;
    case MOUTH_FLAT_SIDE:
      tft.fillRect(cx - 6, cy - 4, 40, 7, K);
      break;
    case MOUTH_WAVY: {
      const int p[12] = { cx - 26, cy - 2, cx - 16, cy - 9, cx - 6, cy + 5,
                              cx + 4, cy - 9, cx + 14, cy + 5, cx + 26, cy - 2 };
      thickPoly(p, 6, 4, K);
      break;
    }
    case MOUTH_GRIN: {
      tft.fillCircle(cx, cy - 10, 28, K);
      tft.fillRect(cx - 29, cy - 44, 58, 24, bg);          // 削平上沿
      tft.fillRect(cx - 26, cy - 10, 52, 5, bg);           // 牙缝
      break;
    }
    case MOUTH_TONGUE: {
      const int p[6] = { cx - 22, cy - 6, cx, cy + 2, cx + 22, cy - 6 };
      thickPoly(p, 3, 5, K);
      tft.fillRoundRect(cx + 4, cy - 2, 18, 22, 8, EX_HEART);
      break;
    }
    case MOUTH_NONE:
    default:
      break;
  }
}

// ── 附加特效 ────────────────────────────────────────────────────────────
// phase = 0 时与第一~四阶段表现完全一致；phase > 0 时 FX_ZZZ 走睡眠动画。
static void drawFx(uint16_t fx, int16_t dx, int16_t dy, uint8_t phase) {
  if (fx == FX_NONE) return;

  const int16_t lx  = EXPR_EYE_LX + dx;
  const int16_t rx  = EXPR_EYE_RX + dx;
  const int16_t ey  = EXPR_EYE_Y  + dy;
  const int16_t cy  = EXPR_EYE_CY + dy;
  const int16_t lcx = lx + EYE_W / 2;
  const int16_t rcx = rx + EYE_W / 2;

  if (fx & FX_BLUSH) {
    tft.fillCircle(lx - 4,          cy + 48, 10, EX_BLUSH);
    tft.fillCircle(rx + EYE_W + 4,  cy + 48, 10, EX_BLUSH);
  }

  if (fx & FX_TEARS) {
    tft.fillRect(lcx - 2, cy + 12, 5, 26, EX_TEAR);
    drawDrop(lcx, cy + 46, 6, EX_TEAR);
    tft.fillRect(rcx - 2, cy + 12, 5, 26, EX_TEAR);
    drawDrop(rcx, cy + 46, 6, EX_TEAR);
  }

  if (fx & FX_SWEAT) {
    drawDrop(rx + EYE_W + 12, ey + 4, 7, EX_SWEAT);
  }

  if (fx & FX_ANGER) {
    const int16_t ax = rx + EYE_W - 8, ay = ey - 14;
    tft.fillRect(ax - 14, ay - 6,  28, 5, C_ORANGE);
    tft.fillRect(ax - 14, ay + 6,  28, 5, C_ORANGE);
    tft.fillRect(ax - 9,  ay - 14,  5, 28, C_ORANGE);
    tft.fillRect(ax + 5,  ay - 14,  5, 28, C_ORANGE);
  }

  if (fx & FX_BROWS_ANGRY) {
    thickLine(lx,           ey - 10, lx + EYE_W, ey + 6, 6, ST77XX_BLACK);
    thickLine(rx + EYE_W,   ey - 10, rx,         ey + 6, 6, ST77XX_BLACK);
  }

  if (fx & FX_BROWS_SAD) {
    thickLine(lx,           ey - 10, lx + EYE_W, ey + 2, 6, ST77XX_BLACK);
    thickLine(rx + EYE_W,   ey - 10, rx,         ey + 2, 6, ST77XX_BLACK);
  }

  if (fx & FX_ZZZ) {
    tft.setTextColor(ST77XX_BLACK);
    if (phase == 0) {
      // 第一~四阶段的静态画法，保持不变
      tft.setTextSize(3);
      tft.setCursor(rx + EYE_W - 4, ey - 30);
      tft.print("z");
      tft.setTextSize(2);
      tft.setCursor(rx + EYE_W + 12, ey - 6);
      tft.print("z");
    } else {
      // 第五阶段：Z → ZZ → ZZZ → 空，缓慢向上飘
      // phase 1..4 → n 0..3（0 = 空一拍，做出「一下一下冒」的节奏）
      const uint8_t k = (uint8_t)((phase - 1) & 3);
      const int16_t zx = rx + EYE_W - 6;
      const int16_t zy = ey - 30 - (int16_t)(k * 6);
      if (k >= 1) { tft.setTextSize(3); tft.setCursor(zx,      zy);      tft.print("z"); }
      if (k >= 2) { tft.setTextSize(2); tft.setCursor(zx + 14, zy + 15); tft.print("z"); }
      if (k >= 3) { tft.setTextSize(1); tft.setCursor(zx + 26, zy + 27); tft.print("z"); }
    }
  }

  if (fx & FX_QUESTION) {
    tft.setTextColor(ST77XX_BLACK);
    tft.setTextSize(3);
    tft.setCursor(rx + EYE_W - 2, ey - 32);
    tft.print("?");
  }

  if (fx & FX_SPARKLE) {
    drawSparkle(lx - 12,          ey - 16, 11, EX_SPARK);
    drawSparkle(rx + EYE_W + 12,  ey - 10,  9, EX_SPARK);
    drawSparkle(rx + EYE_W + 8,   ey + 78,  7, EX_SPARK);
  }

  if (fx & FX_HEARTS) {
    drawHeart(lx - 14,         ey - 18, 9, EX_HEART);
    drawHeart(rx + EYE_W + 14, ey - 8,  7, EX_HEART);
    drawHeart(rx + EYE_W + 16, ey + 66, 6, EX_HEART);
  }
}

// ── 整帧渲染 ────────────────────────────────────────────────────────────
static void renderFrame(const FaceFrame& f) {
  const ExpressionDef& d = EXPR_DEFS[gExpr];
  tft.fillScreen(animBgColor);
  drawEyes(d.eye, f.dx, f.dy, f.eyeOpen, animBgColor);
  drawMouth(d.mouth, f.dx, f.dy);
  drawFx(d.fx, f.dx, f.dy, f.fxPhase);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ③ 动画层 —— 把时间变成一帧形变，不知道画的是什么表情
// ═══════════════════════════════════════════════════════════════════════════

// 眨眼：睁眼等待 → 闭合 130ms → 再等待 2.0~3.8s
// 本次修改：闭合时长固定 130ms（眨眼本来就该"一下就好"，跟着放慢会很诡异），
// 但【眨眼间隔】跟随速度阀钮一起放慢 —— 否则「开心 / 平静」这两个
// 最常用的表情（默认动画就是 ANIM_BLINK）对阀钮完全没反应，
// 用户会直接判定"阀钮没用"。
static uint32_t blinkInterval(uint32_t base) {
  const uint8_t s = animSpeed ? animSpeed : 1;
  uint32_t mul = ((uint32_t)s + 1) / 2;        // 1档→1倍, 3档→2倍, 10档→5倍
  uint32_t v = base * mul;
  if (v > 20000UL) v = 20000UL;
  return v ? v : base;
}

static uint8_t blinkEyeOpen(uint32_t now) {
  if (gBlinkStep == 0) {
    if (gBlinkNextMs == 0) gBlinkNextMs = now + blinkInterval(1200);
    if ((int32_t)(now - gBlinkNextMs) >= 0) { gBlinkStep = 1; gBlinkStepMs = now; return 0; }
    return 100;
  }
  if (now - gBlinkStepMs >= 130) {
    gBlinkStep   = 0;
    gBlinkNextMs = now + blinkInterval(2000 + (now % 1800));
  }
  return 0;
}

// 动画周期：随全局「慢速倍率」放大（本次修改，问题 1）
// animSpeed 由网页「// 速度」滑块设定（1..10，越大越慢）。
// 抖动/晃动/跳动/呼吸这些相位原先直接吃 millis()，完全不受滑块影响，
// 所以之前怎么拖都不变。现在统一经这里放大，
// 一个阀钮即可控制全部表情（表情库/情绪/主动互动/睡眠驱动）的动画节奏。
//
// 入参是【一个完整周期的毫秒数】，不是"每弧度毫秒"——这样才方便设上限。
// 上限分两档（本次调整）：
//   · 抖动类（base < 1500ms，SHAKE / TREMBLE / BOUNCE / SLEEP_BREATH）→ 最多 9 秒
//   · 呼吸类（base >= 1500ms，BREATH 5.3s）→ 最多 8 秒
// 呼吸类若不封顶，10 档会变成 53 秒一个来回，看起来像卡死。
#define ANIM_PERIOD_MAX_SHORT 9000UL
#define ANIM_PERIOD_MAX_LONG  8000UL
static inline uint32_t animPeriod(uint32_t basePeriod) {
  const uint32_t v = basePeriod * (uint32_t)(animSpeed ? animSpeed : 1);
  const uint32_t cap = (basePeriod >= 1500UL) ? ANIM_PERIOD_MAX_LONG : ANIM_PERIOD_MAX_SHORT;
  if (v > cap) return cap;
  return v ? v : basePeriod;
}

static FaceFrame updateFrame(uint32_t now) {
  FaceFrame f = { 0, 0, 100, 0 };

  switch (gAnim) {
    case ANIM_NONE:
      break;

    // 眨眼保持真实时间：眨眼本来就该「一下就好」，跟着放慢会显得诡异。
    case ANIM_BLINK:
      f.eyeOpen = blinkEyeOpen(now);
      break;

    case ANIM_BOUNCE: {
      const uint32_t T = animPeriod(900);
      const float ph = (float)(now % T) / (float)T;
      f.dy = -(int16_t)(fabsf(sinf(ph * 6.2831853f)) * 14.0f);
      break;
    }

    // ── 抖动类：这才是用户说的"抖得太快"，成倍放慢效果最明显 ────────
    case ANIM_SHAKE: {
      // 基础周期 2π×90 ≈ 565ms，幅度 4px
      const uint32_t T = animPeriod(565);
      f.dx = (int16_t)(sinf((float)(now % T) * 6.2831853f / (float)T) * 4.0f);
      break;
    }

    case ANIM_BREATH: {
      // 基础周期 2π×850 ≈ 5341ms（本来就慢，靠 ANIM_PERIOD_MAX 兜住）
      const uint32_t T = animPeriod(5341);
      f.dy = (int16_t)(sinf((float)(now % T) * 6.2831853f / (float)T) * 4.0f);
      break;
    }

    case ANIM_TREMBLE: {
      static const int8_t sx[8] = { 0, 1, -1, 1, 0, -1, 1, -1 };
      static const int8_t sy[8] = { 1, -1, 0, 1, -1, 0, 1, 1 };
      // 整轮 720ms（8 格 × 90ms），按倍率放慢后平均分回每格；幅度 ±1px
      uint32_t step = animPeriod(720) / 8;
      if (step < 1) step = 1;
      const uint8_t i = (uint8_t)((now / step) & 7);
      f.dx = (int16_t)(sx[i] * 1);
      f.dy = (int16_t)(sy[i] * 1);
      break;
    }

    case ANIM_SLEEP_BREATH: {
      // 睡眠深呼吸：基础周期 1.8s，幅度 ±5px
      const uint32_t T = animPeriod(1800);
      const float ph = (float)(now % T) / (float)T;
      f.dy = (int16_t)(sinf(ph * 6.2831853f) * 5.0f);
      // Zzz 相位：基础每 500ms 进一格 → 1..4
      const uint32_t zp = animPeriod(2000) / 4;   // 4 格为一轮
      f.fxPhase = (uint8_t)(((now / (zp ? zp : 1)) & 3) + 1);
      break;
    }

    default:
      break;
  }
  return f;
}

// 帧间隔：固定 45ms（本次修改）
// 这里刻意【不】再乘 animSpeed —— 相位放慢后仍要足够高的刷新率，
// 慢动作才顺滑；而 renderNow/expressionTick 都有"画面没变就跳过"的判重，
// 多算几帧几乎不花 SPI 时间。
static uint32_t frameInterval() {
  uint32_t iv = EXPR_FRAME_BASE;
  if (iv < 34) iv = 34;
  return iv;
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部辅助
// ═══════════════════════════════════════════════════════════════════════════

// 立即重绘一帧（仅当表情视图处于激活状态）
static void renderNow(bool force) {
  if (currentView != VIEW_EXPRESSION) { gDirty = true; return; }
  const FaceFrame f = updateFrame(millis());
  if (!force && !gDirty &&
      f.dx == gLastFrame.dx && f.dy == gLastFrame.dy && f.eyeOpen == gLastFrame.eyeOpen &&
      f.fxPhase == gLastFrame.fxPhase)
    return;
  gLastFrame = f;
  gDirty     = false;
  renderFrame(f);
}

// 状态发生变化时的统一出口
static void markChanged() {
  gDirty = true;
  if (currentView == VIEW_EXPRESSION) renderNow(true);
}

// ═══════════════════════════════════════════════════════════════════════════
//  对外接口
// ═══════════════════════════════════════════════════════════════════════════

void exprBegin() {
  EX_BLUSH = tft.color565(255, 130, 140);
  EX_TEAR  = tft.color565(110, 200, 255);
  EX_SWEAT = tft.color565(150, 220, 255);
  EX_HEART = tft.color565(255,  70, 100);
  EX_SPARK = tft.color565(255, 226, 120);

  gExpr       = EXPR_HAPPY;
  gAnim       = (AnimId)EXPR_DEFS[EXPR_HAPPY].anim;
  gLocked     = false;
  gDirty      = true;
  gLastTickMs = 0;
  gLastFrame  = { (int16_t)-32768, (int16_t)-32768, 255, 255 };
  gBlinkStep  = 0;
  gBlinkStepMs = 0;
  gBlinkNextMs = 0;
}

// ── 核心：设定表情 ──────────────────────────────────────────────────────
void setExpression(ExpressionId id) {
  requestExpression(id, false);
}

// automatic = true 的请求（未来的情绪 / 睡眠 / 随机互动系统）会被锁定拦截
bool requestExpression(ExpressionId id, bool automatic) {
  if (id >= EXPR_COUNT) return false;
  if (automatic && gLocked) return false;

  gExpr        = id;
  gAnim        = (AnimId)EXPR_DEFS[id].anim;   // 跟随该表情的默认动作
  gBlinkStep   = 0;
  gBlinkStepMs = 0;
  gBlinkNextMs = 0;
  gLastTickMs  = 0;
  markChanged();
  return true;
}

void playExpression(ExpressionId id, AnimId anim) {
  if (id >= EXPR_COUNT) return;
  if (anim >= ANIM_COUNT) anim = (AnimId)EXPR_DEFS[id].anim;
  gExpr        = id;
  gAnim        = anim;
  gBlinkStep   = 0;
  gBlinkStepMs = 0;
  gBlinkNextMs = 0;
  gLastTickMs  = 0;
  markChanged();
}

void resetExpression() {
  gExpr        = EXPR_HAPPY;
  gAnim        = (AnimId)EXPR_DEFS[EXPR_HAPPY].anim;
  gBlinkStep   = 0;
  gBlinkStepMs = 0;
  gBlinkNextMs = 0;
  gLastTickMs  = 0;
  markChanged();
}

ExpressionId getCurrentExpression() { return gExpr; }

const ExpressionDef& getExpressionDef(ExpressionId id) {
  return EXPR_DEFS[id < EXPR_COUNT ? id : 0];
}

// ── 锁定 ────────────────────────────────────────────────────────────────
void lockExpression()   { gLocked = true;  }
void unlockExpression() { gLocked = false; }
bool isExpressionLocked(){ return gLocked; }

// ── 动画（与表情正交）──────────────────────────────────────────────────
void setExpressionAnimation(AnimId anim) {
  if (anim >= ANIM_COUNT || anim == gAnim) return;
  gAnim        = anim;
  gBlinkStep   = 0;
  gBlinkStepMs = 0;
  gBlinkNextMs = 0;
  gLastTickMs  = 0;
  markChanged();
}

AnimId getExpressionAnimation() { return gAnim; }

// ── 渲染 / 驱动 ─────────────────────────────────────────────────────────
void exprEnterView() {
  currentView = VIEW_EXPRESSION;
  gLastTickMs = 0;
  gDirty      = true;
  renderNow(true);
}

void exprRedraw() {
  renderNow(true);
}

// loop() 中每轮调用；不在表情视图时直接返回，零开销
void expressionTick() {
  if (currentView != VIEW_EXPRESSION) return;

  const uint32_t now = millis();
  if (gLastTickMs != 0 && (now - gLastTickMs) < frameInterval()) return;
  gLastTickMs = now;

  const FaceFrame f = updateFrame(now);
  if (!gDirty &&
      f.dx == gLastFrame.dx && f.dy == gLastFrame.dy && f.eyeOpen == gLastFrame.eyeOpen &&
      f.fxPhase == gLastFrame.fxPhase)
    return;                                   // 画面没变 → 一次 SPI 都不发

  gLastFrame = f;
  gDirty     = false;
  renderFrame(f);
}

// ── 供 /state 与 /expr/list 使用 ────────────────────────────────────────
const char* exprKeyOf(ExpressionId id) {
  return EXPR_DEFS[id < EXPR_COUNT ? id : 0].key;
}

const char* exprAnimKeyOf(AnimId anim) {
  return ANIM_KEYS[anim < ANIM_COUNT ? anim : 0];
}
