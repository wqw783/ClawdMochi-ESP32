/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  sleep.cpp — 睡眠 / 唤醒系统实现
 *
 *  状态机：
 *      AWAKE ──(idle 够久 / 入夜)──> DROWSY ──(idle 更久)──> SLEEPING
 *        ↑                             │                        │
 *        └────(用户点了一下)───────────┘                        │
 *        └────(WAKING 演完，进 45s 冷却)────────────────────────┘
 *
 *  输入只有两个：
 *      ① 当前时间  → localTimeHM()    （复用主固件的时间系统）
 *      ② 用户 idle → getIdleTime()    （复用 InteractionManager）
 *
 *  输出只有两个：
 *      ① 表情 / 动画 → requestExpression / setExpressionAnimation
 *      ② 闸门        → proactiveSetSleeping / emotionSetApplyPaused
 *
 *  本文件里**没有一处 tft.fillScreen / drawXXX** —— 除了最后那段
 *  「设备端睡眠调试屏」，那是纯观察窗口，不参与任何逻辑。
 *
 *  内存：约 50 字节静态变量。无 new、无 String、无 Flash 写入、无 delay()。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "sleep.h"

// ═══════════════════════════════════════════════════════════════════════════
//  名字表
// ═══════════════════════════════════════════════════════════════════════════
static const char* const SL_KEYS[SLEEP_STATE_COUNT] = {
  "awake", "drowsy", "sleeping", "waking"
};
static const char* const SL_LABELS[SLEEP_STATE_COUNT] = {
  "醒着", "困了", "睡着了", "刚醒"
};
static const char* const SL_SHORT[SLEEP_STATE_COUNT] = {
  "AWAKE", "DROWSY", "SLEEPING", "WAKING"
};

static const char* const SL_RKEYS[SLEEP_REASON_COUNT] = {
  "none", "idle", "night", "manual"
};
static const char* const SL_RLABELS[SLEEP_REASON_COUNT] = {
  "无", "白天闲置", "夜间", "手动"
};
static const char* const SL_RUPPER[SLEEP_REASON_COUNT] = {
  "NONE", "IDLE", "NIGHT", "MANUAL"
};

// ═══════════════════════════════════════════════════════════════════════════
//  运行时状态（约 50 字节）
// ═══════════════════════════════════════════════════════════════════════════
static SleepState  gState         = SLEEP_AWAKE;
static SleepReason gReason        = SLEEP_REASON_NONE;
static bool        gEnabled       = true;
static bool        gTestMode      = false;
static uint32_t    gStateSince    = 0;
static uint32_t    gCooldownUntil = 0;
static uint32_t    gLastEvalMs    = 0;
static uint32_t    gNightSince    = 0;      // 进入夜间窗口的时刻，0 = 不在夜间
static bool        gNightNow      = false;
static bool        gWakePhase2    = false;  // WAKING 第二段（睁完眼 → 开心）
static bool        gLastLocked    = false;  // Expression Lock 下降沿检测
static uint32_t    gLastScreenMs  = 0;

// 时间缓存（每拍最多取一次）
static uint8_t     gHour          = 0;
static uint8_t     gMinute        = 0;
static bool        gTimeOk        = false;

// 测试时间覆盖（-1 = 关闭，用真实时间）
static int8_t      gTestHour      = -1;
static int8_t      gTestMinute    = -1;

// 夜间时段（可配置，默认 23:00 ~ 07:00）
static uint8_t     gNightStartH   = SLEEP_NIGHT_START_H;
static uint8_t     gNightStartM   = SLEEP_NIGHT_START_M;
static uint8_t     gNightEndH     = SLEEP_NIGHT_END_H;
static uint8_t     gNightEndM     = SLEEP_NIGHT_END_M;

// ═══════════════════════════════════════════════════════════════════════════
//  跨午夜判断
//
//  起止时刻先换算成「当天第几分钟」再比区间，绝不能写成 hour >= 23 && hour < 7。
//      23:00~07:00 → s=1380, e=420, s>e → (cur >= 1380 || cur < 420)
//      22:59 → 假 / 23:00 → 真 / 02:00 → 真 / 06:59 → 真 / 07:00 → 假
// ═══════════════════════════════════════════════════════════════════════════
static bool isNightHM(uint8_t h, uint8_t m) {
  const uint16_t cur = (uint16_t)h * 60 + (uint16_t)m;
  const uint16_t s   = (uint16_t)gNightStartH * 60 + (uint16_t)gNightStartM;
  const uint16_t e   = (uint16_t)gNightEndH   * 60 + (uint16_t)gNightEndM;
  if (s == e) return false;                  // 退化配置：视为没有夜间
  if (s < e)  return (cur >= s && cur < e);  // 同一天内，例如 01:00~06:00
  return (cur >= s || cur < e);              // 跨午夜，例如 23:00~07:00
}

// ═══════════════════════════════════════════════════════════════════════════
//  阈值：白天一套、夜间一套、测试档一套
// ═══════════════════════════════════════════════════════════════════════════
static uint32_t drowsyIdleMs() {
  if (gTestMode) return SLEEP_TEST_DROWSY_IDLE_MS;
  return gNightNow ? SLEEP_NIGHT_DROWSY_IDLE_MS : SLEEP_DAY_DROWSY_IDLE_MS;
}
static uint32_t sleepIdleMs() {
  if (gTestMode) return SLEEP_TEST_SLEEP_IDLE_MS;
  return gNightNow ? SLEEP_NIGHT_SLEEP_IDLE_MS : SLEEP_DAY_SLEEP_IDLE_MS;
}

// ═══════════════════════════════════════════════════════════════════════════
//  把状态翻译成表情 —— 本模块唯一的「往屏幕」通道
//
//  全部走 ExpressionManager，所以 Expression Lock 天然生效：
//  锁着的时候 requestExpression 返回 false，屏幕保持用户锁的表情，
//  但 gState 照样变 —— 这正是需求第二十六条要的语义。
// ═══════════════════════════════════════════════════════════════════════════
static void applyFace(SleepState st) {
  switch (st) {
    case SLEEP_AWAKE:
      // 交还给情绪系统：解除闸门 + 立刻按当前情绪恢复表情
      emotionSetApplyPaused(false);
      emotionApply();
      break;

    case SLEEP_DROWSY:
      emotionSetApplyPaused(true);
      // 复用已有的 EXPR_SLEEPY（闭眼 + 小圆嘴 + Zzz），零新增图片资源。
      // 动画用 ANIM_BREATH：轻轻晃，Zzz 是静态的两个 z。
      requestExpression(EXPR_SLEEPY, true);
      setExpressionAnimation(ANIM_BREATH);
      break;

    case SLEEP_SLEEPING:
      emotionSetApplyPaused(true);
      // 同一个 EXPR_SLEEPY，换成 ANIM_SLEEP_BREATH：
      // 呼吸更慢更深，Zzz 变成 Z → ZZ → ZZZ 慢慢往上飘。
      requestExpression(EXPR_SLEEPY, true);
      setExpressionAnimation(ANIM_SLEEP_BREATH);
      break;

    case SLEEP_WAKING:
      emotionSetApplyPaused(true);
      // 第一段：睁大眼睛（EXPR_SURPRISED 就是圆眼）
      requestExpression(EXPR_SURPRISED, true);
      setExpressionAnimation(ANIM_NONE);
      break;

    default: break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  状态迁移 —— 所有闸门与表情都在这里统一出口
// ═══════════════════════════════════════════════════════════════════════════
static void enterState(SleepState st, SleepReason r, uint32_t now) {
  const bool changed = (st != gState);

  gState      = st;
  gReason     = (st == SLEEP_AWAKE) ? SLEEP_REASON_NONE : r;
  gStateSince = now;
  gWakePhase2 = false;

  // 闸门：先停主动互动，再贴睡颜。顺序反了睡颜会被主动行为盖掉。
  if (st == SLEEP_AWAKE) {
    proactiveSetSleeping(false);
    proactiveResetTimer();          // 刚醒别马上又蹦一下
  } else {
    proactiveSetSleeping(true);
  }

  applyFace(st);

  if (changed && currentView == VIEW_SLEEP) sleepRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void sleepBegin() {
  gState         = SLEEP_AWAKE;
  gReason        = SLEEP_REASON_NONE;
  gEnabled       = true;
  gTestMode      = false;
  gStateSince    = millis();
  gCooldownUntil = 0;
  gLastEvalMs    = millis();
  gNightSince    = 0;
  gNightNow      = false;
  gWakePhase2    = false;
  gLastLocked    = isExpressionLocked();
  gLastScreenMs  = millis();
  gHour = gMinute = 0;
  gTimeOk        = false;
  gTestHour      = -1;
  gTestMinute    = -1;
  gNightStartH   = SLEEP_NIGHT_START_H;
  gNightStartM   = SLEEP_NIGHT_START_M;
  gNightEndH     = SLEEP_NIGHT_END_H;
  gNightEndM     = SLEEP_NIGHT_END_M;
  // 不在这里动表情：开机表情由 EmotionManager 决定
}

void sleepTick() {
  const uint32_t now = millis();

  // 调试屏每秒刷一次（倒计时是活的）
  if (currentView == VIEW_SLEEP && (now - gLastScreenMs) >= 1000) {
    gLastScreenMs = now;
    sleepRedraw();
  }

  // ── Expression Lock 解除的下降沿 ──
  // 锁着的时候 requestExpression 被拦下，屏幕保持用户锁的表情；
  // 一旦解锁，这里把「当前睡眠状态该有的表情」补回去。
  const bool locked = isExpressionLocked();
  if (gLastLocked && !locked) applyFace(gState);
  gLastLocked = locked;

  if (now - gLastEvalMs < SLEEP_EVAL_MS) return;
  gLastEvalMs = now;

  // ── 取时间（复用主固件，绝不新建第二套 RTC / NTP）──
  if (gTestHour >= 0) {
    gHour   = (uint8_t)gTestHour;
    gMinute = (uint8_t)gTestMinute;
    gTimeOk = true;
  } else {
    gTimeOk = localTimeHM(gHour, gMinute);
  }
  const bool night = gTimeOk && isNightHM(gHour, gMinute);
  if (night && !gNightNow) gNightSince = now;   // 刚入夜，记一笔
  if (!night)              gNightSince = 0;
  gNightNow = night;

  const uint32_t idle = getIdleTime();

  // ── 总开关 ──
  if (!gEnabled) {
    if (gState != SLEEP_AWAKE) enterState(SLEEP_AWAKE, SLEEP_REASON_NONE, now);
    return;
  }

  switch (gState) {
    // ───────────────────────────────────────────────────────────────
    case SLEEP_AWAKE: {
      if (now < SLEEP_BOOT_GRACE_MS) return;                   // 刚上电先醒一会儿
      if ((int32_t)(now - gCooldownUntil) < 0) return;         // 唤醒冷却中，不睡
      // 刚到夜间先醒 20 秒再考虑犯困 —— 不要求 23:00 一到就瞬间睡着
      if (night && gNightSince != 0 &&
          (now - gNightSince) < SLEEP_NIGHT_GRACE_MS) return;
      // 用户还在连续操作 → idle 很短 → 根本进不了这一支
      if (idle >= drowsyIdleMs())
        enterState(SLEEP_DROWSY,
                   night ? SLEEP_REASON_NIGHT : SLEEP_REASON_IDLE, now);
      break;
    }

    // ───────────────────────────────────────────────────────────────
    case SLEEP_DROWSY: {
      // 用户一有动作就醒，而且不经过完整的 SLEEPING → WAKING
      if (idle < SLEEP_WAKE_IDLE_MS) {
        enterState(SLEEP_AWAKE, SLEEP_REASON_NONE, now);
        break;
      }
      if (idle >= sleepIdleMs()) enterState(SLEEP_SLEEPING, gReason, now);
      break;
    }

    // ───────────────────────────────────────────────────────────────
    case SLEEP_SLEEPING: {
      // 本阶段最重要的交互：睡着时点一下 → 醒。
      // 这里只是「观察到 idle 掉下去了」，并不消费这次点击 ——
      // 所以第一下唤醒、后续点击照常进 InteractionManager。
      if (idle < SLEEP_WAKE_IDLE_MS)
        enterState(SLEEP_WAKING, gReason, now);
      break;
    }

    // ───────────────────────────────────────────────────────────────
    case SLEEP_WAKING: {
      const uint32_t held = now - gStateSince;
      if (!gWakePhase2 && held >= SLEEP_WAKING_PHASE_MS) {
        gWakePhase2 = true;                       // 睁完眼 → 开心一下 / 伸懒腰
        requestExpression(EXPR_HAPPY, true);
        setExpressionAnimation(ANIM_BOUNCE);
      }
      if (held >= SLEEP_WAKING_MS) {
        gCooldownUntil = now + SLEEP_WAKE_COOLDOWN_MS;   // 醒后 45 秒内不再睡
        enterState(SLEEP_AWAKE, SLEEP_REASON_NONE, now);
      }
      break;
    }

    default: break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  状态查询
// ═══════════════════════════════════════════════════════════════════════════
SleepState  getSleepState()  { return gState; }
SleepReason getSleepReason() { return gReason; }
bool isSleeping()            { return gState == SLEEP_SLEEPING; }
bool isDrowsy()              { return gState == SLEEP_DROWSY; }
bool isWaking()              { return gState == SLEEP_WAKING; }
bool isSleepAwake()          { return gState == SLEEP_AWAKE; }
bool isNightTimeNow()        { return gNightNow; }
bool isTimeSynced()          { return gTimeOk; }

// ═══════════════════════════════════════════════════════════════════════════
//  唤醒 / 强制
// ═══════════════════════════════════════════════════════════════════════════
bool wakeUp() {
  const uint32_t now = millis();
  if (gState == SLEEP_AWAKE) return false;

  if (gState == SLEEP_SLEEPING) {
    enterState(SLEEP_WAKING, gReason, now);          // 走完整的醒来流程
  } else {
    enterState(SLEEP_AWAKE, SLEEP_REASON_NONE, now); // DROWSY → 直接醒
  }
  gCooldownUntil = now + SLEEP_WAKE_COOLDOWN_MS;
  return true;
}

void forceSleep() {
  gCooldownUntil = 0;                                // 强制睡着，冷却作废
  enterState(SLEEP_SLEEPING, SLEEP_REASON_MANUAL, millis());
}

void forceDrowsy() {
  gCooldownUntil = 0;
  enterState(SLEEP_DROWSY, SLEEP_REASON_MANUAL, millis());
}

// ═══════════════════════════════════════════════════════════════════════════
//  总开关
// ═══════════════════════════════════════════════════════════════════════════
void enableSleep() {
  gEnabled = true;
  sleepResetTimer();
}
void disableSleep() {
  gEnabled = false;
  if (gState != SLEEP_AWAKE) enterState(SLEEP_AWAKE, SLEEP_REASON_NONE, millis());
}
bool isSleepEnabled() { return gEnabled; }

// ═══════════════════════════════════════════════════════════════════════════
//  开发测试
// ═══════════════════════════════════════════════════════════════════════════
void sleepSetTestMode(bool on) {
  gTestMode = on;
  sleepResetTimer();
}
bool isSleepTestMode() { return gTestMode; }

void sleepClearTestTime() {
  gTestHour   = -1;
  gTestMinute = -1;
  if (currentView == VIEW_SLEEP) sleepRedraw();
}

bool getSleepTestTime(uint8_t &hour, uint8_t &minute) {
  if (gTestHour < 0) return false;
  hour   = (uint8_t)gTestHour;
  minute = (uint8_t)gTestMinute;
  return true;
}

void sleepSetTestTime(int8_t hour, int8_t minute) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    sleepClearTestTime();
    return;
  }
  gTestHour   = hour;
  gTestMinute = minute;

  // 立刻按新时间重算夜间状态，测试时不用等下一拍
  gHour   = (uint8_t)hour;
  gMinute = (uint8_t)minute;
  gTimeOk = true;
  const bool night = isNightHM(gHour, gMinute);
  if (night && !gNightNow) gNightSince = millis();
  if (!night)              gNightSince = 0;
  gNightNow = night;

  if (currentView == VIEW_SLEEP) sleepRedraw();
}

void sleepResetTimer() {
  const uint32_t now = millis();
  gCooldownUntil = 0;
  gLastEvalMs    = now;
  gNightSince    = gNightNow ? now : 0;    // 重新开始算「入夜宽限」
  if (currentView == VIEW_SLEEP) sleepRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  夜间时段配置
// ═══════════════════════════════════════════════════════════════════════════
void sleepSetNightWindow(uint8_t startH, uint8_t startM, uint8_t endH, uint8_t endM) {
  if (startH > 23 || endH > 23 || startM > 59 || endM > 59) return;
  gNightStartH = startH; gNightStartM = startM;
  gNightEndH   = endH;   gNightEndM   = endM;
  gNightSince  = 0;
  gNightNow    = false;
  if (currentView == VIEW_SLEEP) sleepRedraw();
}

uint8_t getSleepNightStartH() { return gNightStartH; }
uint8_t getSleepNightStartM() { return gNightStartM; }
uint8_t getSleepNightEndH()   { return gNightEndH; }
uint8_t getSleepNightEndM()   { return gNightEndM; }

// ═══════════════════════════════════════════════════════════════════════════
//  调试读数
// ═══════════════════════════════════════════════════════════════════════════
uint32_t getSleepIdleTime() { return getIdleTime(); }
uint8_t  getSleepHour()     { return gHour; }
uint8_t  getSleepMinute()   { return gMinute; }

uint32_t getDrowsyIn() {
  const uint32_t idle = getIdleTime();
  const uint32_t at   = drowsyIdleMs();
  return (idle >= at) ? 0 : (at - idle);
}

uint32_t getSleepIn() {
  const uint32_t idle = getIdleTime();
  const uint32_t at   = sleepIdleMs();
  return (idle >= at) ? 0 : (at - idle);
}

uint32_t getWakeCooldownLeft() {
  const int32_t left = (int32_t)(gCooldownUntil - millis());
  return (left > 0) ? (uint32_t)left : 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  设备端睡眠调试屏（纯观察窗口，不参与任何睡眠逻辑）
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

void sleepRedraw() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,   240, 4, C_ORANGE);
  tft.fillRect(0, 236, 240, 4, C_ORANGE);

  tft.setTextSize(1);
  tft.setTextColor(C_ORANGE);
  tft.setCursor(8, 12);
  tft.print("SLEEP DEBUG");
  printRight("VIEW 9", 232, 12, 1, C_MUTED);

  tft.setTextSize(2);
  tft.setTextColor(gState == SLEEP_SLEEPING ? C_GREEN : C_WHITE);
  tft.setCursor(10, 30);
  tft.print(SL_SHORT[gState]);

  tft.drawFastHLine(0, 56, 240, C_GRID);

  char b[24];
  int16_t y = 64;
  const int16_t ROW = 21;

  printRow("ENABLED", gEnabled ? (gTestMode ? "ON  TEST" : "ON") : "OFF", y); y += ROW;
  printRow("REASON",  SL_RUPPER[gReason], y); y += ROW;

  if (!gTimeOk)      snprintf(b, sizeof b, "--:-- NOSYNC");
  else if (gNightNow) snprintf(b, sizeof b, "%02u:%02u NIGHT", (unsigned)gHour, (unsigned)gMinute);
  else                snprintf(b, sizeof b, "%02u:%02u DAY",   (unsigned)gHour, (unsigned)gMinute);
  printRow("CLOCK", b, y); y += ROW;

  const uint32_t idle = getIdleTime();
  snprintf(b, sizeof b, "%lu.%lus",
           (unsigned long)(idle / 1000UL), (unsigned long)((idle % 1000UL) / 100UL));
  printRow("IDLE", b, y); y += ROW;

  const uint32_t dIn = getDrowsyIn();
  if (dIn == 0) snprintf(b, sizeof b, "NOW");
  else          snprintf(b, sizeof b, "%lus", (unsigned long)(dIn / 1000UL));
  printRow("DROWSY IN", b, y); y += ROW;

  const uint32_t sIn = getSleepIn();
  if (sIn == 0) snprintf(b, sizeof b, "NOW");
  else          snprintf(b, sizeof b, "%lus", (unsigned long)(sIn / 1000UL));
  printRow("SLEEP IN", b, y); y += ROW;

  const uint32_t cd = getWakeCooldownLeft();
  if (cd == 0) snprintf(b, sizeof b, "--");
  else         snprintf(b, sizeof b, "%lus", (unsigned long)(cd / 1000UL));
  printRow("COOLDOWN", b, y);

  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(10, 208);
  tft.print("night 23:00-07:00  grace 20s");
  tft.setCursor(10, 222);
  tft.print("proactive paused while asleep");
}

void sleepEnterView() {
  currentView = VIEW_SLEEP;
  gLastScreenMs = millis();
  sleepRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  名字
// ═══════════════════════════════════════════════════════════════════════════
const char* sleepStateKey(SleepState s) {
  return SL_KEYS[s < SLEEP_STATE_COUNT ? s : 0];
}
const char* sleepStateLabel(SleepState s) {
  return SL_LABELS[s < SLEEP_STATE_COUNT ? s : 0];
}
const char* sleepStateShort(SleepState s) {
  return SL_SHORT[s < SLEEP_STATE_COUNT ? s : 0];
}
const char* sleepReasonKey(SleepReason r) {
  return SL_RKEYS[r < SLEEP_REASON_COUNT ? r : 0];
}
const char* sleepReasonLabel(SleepReason r) {
  return SL_RLABELS[r < SLEEP_REASON_COUNT ? r : 0];
}
