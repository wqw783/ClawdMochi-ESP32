/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  interaction.cpp — 用户互动系统实现
 *
 *  流程：
 *    两个输入源（BOOT 按键 / 网页戳一下）
 *        ↓  interactionPress / Release / Click
 *    去抖 + 长按判定 + 连击 burst 计数 + 冷却
 *        ↓  递减奖励表
 *    EmotionManager::addEmotion()
 *        ↓  （第二阶段的衰减 / 判定 / 稳定确认照常跑）
 *    ExpressionManager
 *
 *  全程 millis() 驱动，没有一处 delay()。
 *  本文件除最后那段「设备端调试屏」外，不出现任何 tft 调用。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "interaction.h"

// ═══════════════════════════════════════════════════════════════════════════
//  名字表
// ═══════════════════════════════════════════════════════════════════════════
static const char* const IA_KEYS[IA_COUNT] = {
  "none", "single", "multi", "rapid", "long"
};
static const char* const IA_LABELS[IA_COUNT] = {
  "无", "单击", "连击", "过度连击", "长按"
};
static const char* const IA_SHORT[IA_COUNT] = {
  "NONE", "SINGLE", "MULTI", "RAPID", "LONG"
};

// ═══════════════════════════════════════════════════════════════════════════
//  运行时状态（全部静态，约 40 字节）
// ═══════════════════════════════════════════════════════════════════════════

// 按键去抖
static bool     gRaw        = false;
static bool     gStable     = false;
static uint32_t gRawChangeMs = 0;

// 按住状态
static bool     gPressed    = false;
static bool     gLongFired  = false;
static uint32_t gPressMs    = 0;

// 连击 burst
static uint8_t  gClickCount   = 0;
static uint32_t gBurstStartMs = 0;
static uint32_t gLastClickMs  = 0;

// 互动记录
static InteractionEvent gLastEvent = IA_NONE;
static uint32_t gLastInteractionMs = 0;
static bool     gEverInteracted    = false;
static uint16_t gTotalClicks       = 0;

// 反馈 / idle
static uint32_t gFeedbackUntil = 0;
static uint32_t gLastIdleTick  = 0;
static uint32_t gLastScreenMs  = 0;

// ═══════════════════════════════════════════════════════════════════════════
//  内部：互动时间记录
// ═══════════════════════════════════════════════════════════════════════════
static void touchNow(uint32_t now) {
  gLastInteractionMs = now;
  gEverInteracted    = true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部：把「脸」推到桌宠屏幕上（本次修复 · 互动无反应）
//
//  Bug：以前的 giveFeedback() 只调用 setExpressionAnimation(ANIM_BOUNCE)，
//       而表情渲染在 expression.cpp 里有一道门：
//           if (currentView != VIEW_EXPRESSION) { gDirty = true; return; }
//       也就是说 —— 只有设备停在「表情视图」时才会真的画脸。
//       手机端进入「互动」次界面时，autoEnter 会把设备切到互动调试屏
//       （VIEW_INTERACTION），于是点了按钮只会改情绪/动画状态，
//       屏幕上的脸一动不动，表现就是「互动完全失效」。
//
//  修复：互动发生时，如果设备正停在某个「功能调试屏」上，就自动把屏幕
//        切回表情视图，让情绪系统刚刚算出来的表情真正显示出来。
//        （表情本身仍由 emotionApply() 决定，本模块不越权指定表情。）
//
//  刻意不动的情况：
//    · 终端 / 画板 / 时钟 —— 用户正在用这些视图，不能被互动抢走画面；
//    · 表情锁 —— 与 giveFeedback() 的既有语义保持一致；
//    · 官方眼睛视图 —— 不去打断用户主动选的常驻视图。
// ═══════════════════════════════════════════════════════════════════════════
// 这些视图号在主固件里是 #define，对 .cpp 不可见，按项目既有惯例用 #ifndef 兜底。
// 只声明真正用到的三个（终端 / 画板 / 时钟）——它们是"不要抢画面"的视图。
#ifndef VIEW_CODE
#define VIEW_CODE 2
#endif
#ifndef VIEW_DRAW
#define VIEW_DRAW 3
#endif
#ifndef VIEW_CLOCK
#define VIEW_CLOCK 4
#endif

// 按事件挑一个「当场就该看到」的表情
// （不依赖情绪系统：情绪有 1.5 秒确认延迟，点一下当场看不到变化）
static ExpressionId reactionFor() {
  if (gClickCount >= 6) return EXPR_ANGRY;        // 点太多次 → 会烦
  switch (gLastEvent) {
    case IA_MULTI_CLICK: return EXPR_LAUGH;       // 连击 → 大笑
    case IA_RAPID_CLICK: return EXPR_PLAYFUL;     // 过度连击 → 调皮
    case IA_LONG_PRESS:  return EXPR_LOVE;        // 长按 = 抚摸 → 爱心
    default:             return EXPR_HAPPY;       // 单击 → 开心
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部：即时视觉反馈（本次修复的核心）
//
//  以前这里只调用 setExpressionAnimation(ANIM_BOUNCE)：
//    · 不改表情（只换个动画），
//    · 而且表情渲染在 expression.cpp 里有一道门：
//        if (currentView != VIEW_EXPRESSION) { gDirty = true; return; }
//      → 只有设备停在表情视图时才会真的画脸。
//  手机端进「互动」次界面时，autoEnter 把设备切到了互动调试屏
//  （VIEW_INTERACTION），所以点了按钮屏幕上的脸一动不动，
//  表现就是「互动完全失效」。
//
//  现在：① 立刻按事件换一个表情（不等情绪确认，保证当场有反应）；
//        ② 叠一个跳动动画，持续 IA_FEEDBACK_MS，之后自动回到该表情的默认动作；
//        ③ 若设备还停在功能调试屏上，顺手切回表情视图，把脸推到桌宠屏幕上。
//  IA_FEEDBACK_MS 后由 interactionTick() 复原动画。
// ═══════════════════════════════════════════════════════════════════════════
static void giveFeedback() {
  if (isExpressionLocked()) return;      // 锁定时不抢画面（既有语义，保持不变）

  // 终端 / 画板 / 时钟：用户正在用这些视图，不抢画面
  if (currentView != VIEW_CODE && currentView != VIEW_DRAW && currentView != VIEW_CLOCK) {
    const ExpressionId want = reactionFor();
    // 只有表情真的变了才请求：requestExpression() 内部会整屏重绘一次，
    // 连续快按时表情往往相同，跳过可以省下 ~40ms 的 SPI 时间。
    if (getCurrentExpression() != want) requestExpression(want, false);
    setExpressionAnimation(ANIM_BOUNCE);                   // 已是 BOUNCE 时内部会直接返回
    if (currentView != VIEW_EXPRESSION) exprEnterView();   // 关键：把脸推到桌宠屏幕上
  }
  gFeedbackUntil = millis() + IA_FEEDBACK_MS;
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部：奖励表
//  递减式：第 1 次给最多，越往后越少；第 6 次起翻脸 → anger 上升。
//  这样既能越过情绪阈值，又不会几下就把值打满。
// ═══════════════════════════════════════════════════════════════════════════
static void applyClickReward(uint8_t n) {
  switch (n) {
    case 1:  addEmotion(EV_HAPPINESS, 30); break;
    case 2:  addEmotion(EV_HAPPINESS, 18); break;
    case 3:  addEmotion(EV_HAPPINESS, 12); addEmotion(EV_ENERGY, 10); break;
    case 4:  addEmotion(EV_HAPPINESS,  8); addEmotion(EV_ENERGY, 12); break;
    case 5:  addEmotion(EV_HAPPINESS,  6); addEmotion(EV_ENERGY, 12); break;
    default: addEmotion(EV_ANGER, 12);     addEmotion(EV_HAPPINESS, -6); break;
  }
  addEmotion(EV_SADNESS, -3);            // 有人理它 → 难过被冲淡
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部：事件触发
// ═══════════════════════════════════════════════════════════════════════════
static void registerClick(uint32_t now, bool bypassGap) {
  // 冷却：间隔太短的重复点击不计入 burst、不加情绪，但反馈照给，
  // 所以不会出现「点了完全没反应」的感觉。
  if (!bypassGap && gLastClickMs != 0 && (now - gLastClickMs) < IA_MIN_GAP_MS) {
    giveFeedback();
    return;
  }
  gLastClickMs = now;

  // burst 窗口：超过 IA_BURST_MS 没有新点击就重新计数
  if (gClickCount == 0 || (now - gBurstStartMs) > IA_BURST_MS) {
    gClickCount   = 0;
    gBurstStartMs = now;
  }
  gClickCount++;
  if (gTotalClicks < 65535) gTotalClicks++;

  applyClickReward(gClickCount);

  gLastEvent = (gClickCount >= IA_RAPID_MIN) ? IA_RAPID_CLICK
             : (gClickCount >= IA_MULTI_MIN) ? IA_MULTI_CLICK
                                             : IA_SINGLE_CLICK;

  giveFeedback();
  touchNow(now);
  if (currentView == VIEW_INTERACTION) interactionRedraw();
}

static void fireLongPress(uint32_t now) {
  gLastEvent = IA_LONG_PRESS;
  addEmotion(EV_CURIOSITY, 32);          // 长按 → 好奇
  addEmotion(EV_HAPPINESS,  8);
  addEmotion(EV_SADNESS,   -3);
  giveFeedback();
  touchNow(now);
  if (currentView == VIEW_INTERACTION) interactionRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  输入源接口
// ═══════════════════════════════════════════════════════════════════════════
void interactionPress() {
  if (gPressed) return;
  const uint32_t now = millis();
  gPressed   = true;
  gLongFired = false;
  gPressMs   = now;
  touchNow(now);                         // 按下就算互动过
  if (currentView == VIEW_INTERACTION) interactionRedraw();
}

void interactionRelease() {
  if (!gPressed) return;
  const uint32_t now = millis();
  gPressed = false;
  if (gLongFired) { gLongFired = false; return; }   // 长按已经处理过，不再算单击
  registerClick(now, false);
}

// 网页那种只有 click、没有按下/松开的上游用这个
void interactionClick() {
  registerClick(millis(), false);
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部：按键轮询（非阻塞去抖）
// ═══════════════════════════════════════════════════════════════════════════
static void pollButton(uint32_t now) {
  const bool raw = (digitalRead(IA_PIN) == LOW);     // 板载 BOOT 按下为 LOW

  if (raw != gRaw) { gRaw = raw; gRawChangeMs = now; }

  if (gRaw != gStable && (now - gRawChangeMs) >= IA_DEBOUNCE_MS) {
    gStable = gRaw;
    if (gStable) interactionPress();
    else         interactionRelease();
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部：长按判定 + 丢失 release 的兜底
// ═══════════════════════════════════════════════════════════════════════════
static void pollHold(uint32_t now) {
  if (!gPressed) return;

  if (!gLongFired && (now - gPressMs) >= IA_LONG_MS) {
    gLongFired = true;
    fireLongPress(now);
    return;
  }
  // 网页端如果中途关掉页面，release 就丢了 —— 超过 8 秒强制复位
  if ((now - gPressMs) >= IA_PRESS_LOST_MS) {
    gPressed   = false;
    gLongFired = false;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  内部：idle 检测 → 轻微低落
//  只有「互动过之后再冷落」才会低落，刚上电没人碰不会自己难过。
// ═══════════════════════════════════════════════════════════════════════════
static void pollIdle(uint32_t now) {
  if (now - gLastIdleTick < IA_IDLE_TICK_MS) return;
  gLastIdleTick = now;

  if (!gEverInteracted) return;
  if ((now - gLastInteractionMs) < IA_IDLE_SAD_AFTER) return;

  // 封顶，防止无限增长
  if (getEmotionValue(EV_SADNESS) < IA_IDLE_SAD_CAP) {
    addEmotion(EV_SADNESS, 2);
  }
  if (currentView == VIEW_INTERACTION) interactionRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void interactionBegin() {
  pinMode(IA_PIN, INPUT_PULLUP);        // 板载 BOOT 按键
  gRaw = gStable = (digitalRead(IA_PIN) == LOW);
  gRawChangeMs = millis();

  gPressed       = false;
  gLongFired     = false;
  gPressMs       = 0;
  gClickCount    = 0;
  gBurstStartMs  = 0;
  gLastClickMs   = 0;
  gLastEvent     = IA_NONE;
  gEverInteracted = false;
  gLastInteractionMs = millis();
  gTotalClicks   = 0;
  gFeedbackUntil = 0;
  gLastIdleTick  = millis();
  gLastScreenMs  = millis();
}

void interactionTick() {
  const uint32_t now = millis();

  pollButton(now);
  pollHold(now);

  // 跳动反馈结束 → 回到当前表情的默认动作
  if (gFeedbackUntil != 0 && (int32_t)(now - gFeedbackUntil) >= 0) {
    gFeedbackUntil = 0;
    setExpressionAnimation((AnimId)getExpressionDef(getCurrentExpression()).anim);
  }

  pollIdle(now);

  // 调试屏每秒刷一次（idle 是活的）
  if (currentView == VIEW_INTERACTION && (now - gLastScreenMs) >= 1000) {
    gLastScreenMs = now;
    interactionRedraw();
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  状态查询
// ═══════════════════════════════════════════════════════════════════════════
InteractionEvent getLastInteraction() { return gLastEvent; }
uint8_t  getClickCount()      { return gClickCount; }
uint16_t getTotalClicks()     { return gTotalClicks; }
uint32_t getLastInteractionTime() { return gLastInteractionMs; }
bool     hasUserInteracted()  { return gEverInteracted; }
bool     isPressed()          { return gPressed; }

uint32_t getIdleTime() {
  const uint32_t now = millis();
  if (!gEverInteracted) return now;               // 还没互动过 → 从开机算起
  return now - gLastInteractionMs;
}

uint8_t getFeedbackLeft() {
  if (gFeedbackUntil == 0) return 0;
  const uint32_t now = millis();
  if ((int32_t)(now - gFeedbackUntil) >= 0) return 0;
  return (uint8_t)((gFeedbackUntil - now) * 100UL / IA_FEEDBACK_MS);
}

// ═══════════════════════════════════════════════════════════════════════════
//  调试
// ═══════════════════════════════════════════════════════════════════════════
void interactionReset() {
  gClickCount    = 0;
  gBurstStartMs  = 0;
  gLastClickMs   = 0;
  gLastEvent     = IA_NONE;
  gTotalClicks   = 0;
  gPressed       = false;
  gLongFired     = false;
  gFeedbackUntil = 0;
  gEverInteracted = false;
  gLastInteractionMs = millis();
  if (currentView == VIEW_INTERACTION) interactionRedraw();
}

// 网页测试按钮用：直接注入一个事件（绕过冷却，好让一次调用就能凑够连击数）
void interactionInject(InteractionEvent e) {
  const uint32_t now = millis();
  switch (e) {
    case IA_SINGLE_CLICK:
      registerClick(now, true);
      break;
    case IA_MULTI_CLICK:
      for (uint8_t i = 0; i < 3; i++) registerClick(now, true);
      break;
    case IA_RAPID_CLICK:
      for (uint8_t i = 0; i < IA_RAPID_MIN; i++) registerClick(now, true);
      break;
    case IA_LONG_PRESS:
      gLastEvent = IA_LONG_PRESS;
      fireLongPress(now);
      break;
    default:
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  设备端互动调试屏（纯观察窗口，不参与任何互动逻辑）
// ═══════════════════════════════════════════════════════════════════════════
static void printRight(const char* s, int16_t xRight, int16_t y, uint8_t size, uint16_t col) {
  int len = 0;
  while (s[len]) len++;
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor(xRight - len * 6 * size, y);
  tft.print(s);
}

static void printRow(const char* label, const char* value, int16_t y) {
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(10, y); tft.print(label);
  printRight(value, 230, y, 1, C_WHITE);
}

static void upperCopy(char* dst, const char* src, uint8_t maxLen) {
  uint8_t i = 0;
  while (src[i] && i + 1 < maxLen) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    dst[i] = c;
    i++;
  }
  dst[i] = 0;
}

void interactionRedraw() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,   240, 4, C_ORANGE);
  tft.fillRect(0, 236, 240, 4, C_ORANGE);

  tft.setTextSize(1);
  tft.setTextColor(C_ORANGE);
  tft.setCursor(8, 12);
  tft.print("INTERACTION DEBUG");
  printRight("GPIO9", 232, 12, 1, C_MUTED);

  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(10, 34);
  tft.print("LAST EVENT");

  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(10, 46);
  tft.print(IA_SHORT[gLastEvent]);

  tft.drawFastHLine(0, 82, 240, C_GRID);

  char b[16];
  int16_t y = 94;
  const int16_t ROW = 26;

  snprintf(b, sizeof b, "%u", (unsigned)gClickCount);
  printRow("CLICKS", b, y); y += ROW;

  const uint32_t idle = getIdleTime();
  snprintf(b, sizeof b, "%lu.%lus",
           (unsigned long)(idle / 1000UL), (unsigned long)((idle % 1000UL) / 100UL));
  printRow("IDLE", b, y); y += ROW;

  snprintf(b, sizeof b, "%u", (unsigned)gTotalClicks);
  printRow("TOTAL", b, y); y += ROW;

  char emo[12];
  upperCopy(emo, emotionKey(getCurrentEmotion()), sizeof emo);
  printRow("EMOTION", emo, y); y += ROW;

  printRow("PIN", gPressed ? "PRESSED" : "RELEASED", y);
}

void interactionEnterView() {
  currentView = VIEW_INTERACTION;
  gLastScreenMs = millis();
  interactionRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  名字
// ═══════════════════════════════════════════════════════════════════════════
const char* interactionKey(InteractionEvent e) {
  return IA_KEYS[e < IA_COUNT ? e : 0];
}
const char* interactionLabel(InteractionEvent e) {
  return IA_LABELS[e < IA_COUNT ? e : 0];
}
const char* interactionShort(InteractionEvent e) {
  return IA_SHORT[e < IA_COUNT ? e : 0];
}
