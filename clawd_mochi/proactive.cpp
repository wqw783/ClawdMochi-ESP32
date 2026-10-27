/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  proactive.cpp — 随机主动互动系统实现
 *
 *  状态机：
 *      WAITING ──(随机时间到 + idle 够久 + 在表情视图)──> RUNNING
 *         ↑                                                 │
 *         └────(行为时长到 / 用户操作 / 用户切走视图)───────┘
 *
 *  本文件里**不会出现 addEmotion / setEmotion 任何一处** —— 主动行为只改
 *  表情和动画，真实情绪一个字节都不动。所以「SAD → 眨个眼 → 还是 SAD」。
 *
 *  除最后那段设备端调试屏外，本文件不出现任何 tft 调用。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "proactive.h"

// ═══════════════════════════════════════════════════════════════════════════
//  行为定义表：临时表情 / 动画 / 时长
//  expr = EXPR_COUNT 表示「不改表情，只在当前表情上播动画」
// ═══════════════════════════════════════════════════════════════════════════
struct ProactiveDef {
  uint8_t  expr;
  uint8_t  anim;
  uint16_t ms;
};

static const ProactiveDef PRO_DEFS[PRO_COUNT] = {
  { EXPR_COUNT,     ANIM_NONE,     0    },   // NONE
  { EXPR_COUNT,     ANIM_BLINK,    900  },   // BLINK      眨眼
  { EXPR_SLEEPY,    ANIM_BREATH,  2600  },   // YAWN       打哈欠
  { EXPR_THINKING,  ANIM_NONE,    3200  },   // DAYDREAM   发呆
  { EXPR_CONFUSED,  ANIM_BLINK,   1800  },   // CURIOUS    好奇张望
  { EXPR_HAPPY,     ANIM_BOUNCE,  2000  },   // HAPPY      突然开心
  { EXPR_COUNT,     ANIM_BOUNCE,  1300  },   // BOUNCE     弹一下
};

// ═══════════════════════════════════════════════════════════════════════════
//  情绪 → 行为池（位掩码）
//  开心时更容易蹦跶，难过时更容易发呆打哈欠，困了就想打哈欠。
//  10 个情绪 × 2 字节 = 20 字节。
// ═══════════════════════════════════════════════════════════════════════════
#define PRO_BIT(a) ((uint16_t)(1u << (a)))

static const uint16_t PRO_POOL[EMO_COUNT] = {
  /* CALM      */ PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_YAWN)|PRO_BIT(PRO_DAYDREAM)|PRO_BIT(PRO_CURIOUS)|PRO_BIT(PRO_BOUNCE),
  /* HAPPY     */ PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_HAPPY)|PRO_BIT(PRO_BOUNCE)|PRO_BIT(PRO_CURIOUS),
  /* EXCITED   */ PRO_BIT(PRO_BOUNCE)|PRO_BIT(PRO_HAPPY)|PRO_BIT(PRO_BLINK),
  /* SHY       */ PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_CURIOUS)|PRO_BIT(PRO_DAYDREAM),
  /* ANGRY     */ PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_BOUNCE)|PRO_BIT(PRO_DAYDREAM),
  /* SAD       */ PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_YAWN)|PRO_BIT(PRO_DAYDREAM),
  /* SLEEPY    */ PRO_BIT(PRO_YAWN)|PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_DAYDREAM),
  /* SURPRISED */ PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_CURIOUS),
  /* CURIOUS   */ PRO_BIT(PRO_CURIOUS)|PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_DAYDREAM)|PRO_BIT(PRO_HAPPY),
  /* SCARED    */ PRO_BIT(PRO_BLINK)|PRO_BIT(PRO_DAYDREAM),
};

// ═══════════════════════════════════════════════════════════════════════════
//  名字表
// ═══════════════════════════════════════════════════════════════════════════
static const char* const PRO_KEYS[PRO_COUNT] = {
  "none", "blink", "yawn", "daydream", "curious", "happy", "bounce"
};
static const char* const PRO_LABELS[PRO_COUNT] = {
  "无", "眨眼", "打哈欠", "发呆", "好奇张望", "开心一下", "弹一下"
};
static const char* const PRO_SHORT[PRO_COUNT] = {
  "NONE", "BLINK", "YAWN", "DAYDREAM", "CURIOUS", "HAPPY", "BOUNCE"
};

// ═══════════════════════════════════════════════════════════════════════════
//  运行时状态（约 40 字节）
// ═══════════════════════════════════════════════════════════════════════════
static bool     gEnabled      = true;
static ProactiveAction gAction = PRO_NONE;
static uint32_t gActionStart  = 0;
static uint16_t gActionMs     = 0;
static uint8_t  gSavedExpr    = 0;
static uint8_t  gTempExpr     = 0;      // EXPR_COUNT = 本次没改表情
static uint32_t gNextAt       = 0;
static ProactiveAction gNextAction = PRO_NONE;
static uint32_t gLastProactiveMs   = 0;
static bool     gEverFired    = false;
static uint32_t gLastEvalMs   = 0;
static uint32_t gLastScreenMs = 0;
static uint32_t gRng          = 1;
static bool     gSleeping     = false;   // 第五阶段：睡眠闸门

// ═══════════════════════════════════════════════════════════════════════════
//  随机数（自带 LCG，不动全局 RNG）
// ═══════════════════════════════════════════════════════════════════════════
static uint16_t rndNext() {
  gRng = gRng * 1103515245UL + 12345UL;
  return (uint16_t)((gRng >> 16) & 0x7FFF);
}

static uint32_t rndRange(uint32_t lo, uint32_t hi) {   // 返回 [lo, hi)
  if (hi <= lo) return lo;
  return lo + (uint32_t)(rndNext() % (hi - lo));
}

// ═══════════════════════════════════════════════════════════════════════════
//  按当前情绪挑一个行为
// ═══════════════════════════════════════════════════════════════════════════
static ProactiveAction pickAction() {
  const uint16_t mask = PRO_POOL[getCurrentEmotion()];

  uint8_t n = 0;
  for (uint8_t i = 1; i < PRO_COUNT; i++) if (mask & PRO_BIT(i)) n++;
  if (n == 0) return PRO_BLINK;

  uint8_t k = (uint8_t)(rndNext() % n);
  for (uint8_t i = 1; i < PRO_COUNT; i++) {
    if (!(mask & PRO_BIT(i))) continue;
    if (k == 0) return (ProactiveAction)i;
    k--;
  }
  return PRO_BLINK;
}

// ═══════════════════════════════════════════════════════════════════════════
//  排下一次：随机时间 + 预先挑好行为（调试屏要显示「Next Action」）
//  注意：行为池依赖情绪，等真到点了还会再校验一次
// ═══════════════════════════════════════════════════════════════════════════
static void scheduleNext(uint32_t now) {
  gNextAt = now + (gEverFired
                   ? rndRange(PRO_RAND_MIN_MS,  PRO_RAND_MAX_MS)
                   : rndRange(PRO_FIRST_MIN_MS, PRO_FIRST_MAX_MS));
  gNextAction = pickAction();
}

// ═══════════════════════════════════════════════════════════════════════════
//  开始一个行为
// ═══════════════════════════════════════════════════════════════════════════
static void startAction(uint32_t now) {
  // 表情被锁定 → 本次跳过（计时照常重排，锁定解除后自动恢复）
  if (isExpressionLocked()) { scheduleNext(now); return; }

  // 不在表情视图 → 演了也看不见，直接跳过
  if (currentView != VIEW_EXPRESSION) { scheduleNext(now); return; }

  ProactiveAction a = gNextAction;
  // 等待期间情绪可能变了，池子里没有就重新挑
  if (a < 1 || a >= PRO_COUNT || !(PRO_POOL[getCurrentEmotion()] & PRO_BIT(a)))
    a = pickAction();

  const ProactiveDef& d = PRO_DEFS[a];

  gSavedExpr = (uint8_t)getCurrentExpression();
  if (d.expr != EXPR_COUNT) {
    gTempExpr = d.expr;
    // automatic = true：ExpressionManager 内部会检查锁定
    requestExpression((ExpressionId)d.expr, true);
    if ((uint8_t)getCurrentExpression() != d.expr) {   // 双保险：被拦下了就放弃
      gTempExpr = gSavedExpr;
      scheduleNext(now);
      return;
    }
  } else {
    gTempExpr = gSavedExpr;                            // 本次不改表情
  }

  setExpressionAnimation((AnimId)d.anim);

  gAction          = a;
  gActionStart     = now;
  gActionMs        = d.ms;
  gLastProactiveMs = now;
  gEverFired       = true;

  if (currentView == VIEW_PROACTIVE) proactiveRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  结束一个行为
//
//  restore 的那个条件判断是本模块最关键的一行：
//  只有当屏幕还停在「我们设的临时表情」上时才还回去。
//  如果行为期间用户点了桌宠、情绪从 SAD 变成了 HAPPY，表情已经换过了，
//  这时候把 SAD 盖回去就是错的 —— 直接跳过恢复，交给情绪系统。
// ═══════════════════════════════════════════════════════════════════════════
static void endAction(uint32_t now, bool restore) {
  if (restore && (uint8_t)getCurrentExpression() == gTempExpr) {
    requestExpression((ExpressionId)gSavedExpr, true);
  }
  gAction  = PRO_NONE;
  gTempExpr = gSavedExpr;
  scheduleNext(now);
  if (currentView == VIEW_PROACTIVE) proactiveRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void proactiveBegin() {
  gEnabled        = true;
  gSleeping       = false;
  gAction         = PRO_NONE;
  gActionStart    = 0;
  gActionMs       = 0;
  gSavedExpr      = (uint8_t)getCurrentExpression();
  gTempExpr       = gSavedExpr;
  gEverFired      = false;
  gLastProactiveMs = millis();
  gLastEvalMs     = millis();
  gLastScreenMs   = millis();
  gRng            = (uint32_t)micros() | 1u;    // 用上电时间做种子
  scheduleNext(millis());
}

void proactiveTick() {
  const uint32_t now = millis();

  // 调试屏每秒刷一次（倒计时是活的）
  if (currentView == VIEW_PROACTIVE && (now - gLastScreenMs) >= 1000) {
    gLastScreenMs = now;
    proactiveRedraw();
  }

  // ── 睡眠闸门：睡着了一律不触发（正在演的立刻收尾）──
  // 用户操作优先 → 睡眠次之 → 主动互动最低，这就是那条优先级链。
  if (gSleeping) {
    if (gAction != PRO_NONE) endAction(now, true);
    return;
  }

  // ── 正在演 ──
  if (gAction != PRO_NONE) {
    // 用户刚操作过 → 立刻中止，用户优先
    if (getIdleTime() < PRO_ABORT_IDLE_MS) { endAction(now, true); return; }
    // 用户切走了视图 → 也结束
    if (currentView != VIEW_EXPRESSION)    { endAction(now, true); return; }
    // 演完了
    if (now - gActionStart >= gActionMs)   { endAction(now, true); return; }
    return;
  }

  if (!gEnabled) return;

  // ── 等待中 ──
  if (now - gLastEvalMs < PRO_EVAL_MS) return;
  gLastEvalMs = now;

  if (getIdleTime() < PRO_IDLE_MIN_MS) return;          // 用户还在玩，别打扰
  if ((int32_t)(now - gNextAt) >= 0) startAction(now);  // 到点了
}

// ═══════════════════════════════════════════════════════════════════════════
//  总开关
// ═══════════════════════════════════════════════════════════════════════════
void enableProactive()  {
  gEnabled = true;
  proactiveResetTimer();
}
void disableProactive() {
  gEnabled = false;
  // 关掉时如果正在演，让它把表情还回去
  if (gAction != PRO_NONE) endAction(millis(), true);
}
bool isProactiveEnabled() { return gEnabled; }

// ═══════════════════════════════════════════════════════════════════════════
//  睡眠闸门（第五阶段新增）
//
//  与 gEnabled 分开：gEnabled 是用户的总开关，这里是睡眠系统的临时闸门。
//  两者互不覆盖 —— 用户关掉了主动互动，睡醒之后依然是关着的。
// ═══════════════════════════════════════════════════════════════════════════
void proactiveSetSleeping(bool sleeping) {
  if (sleeping == gSleeping) return;
  gSleeping = sleeping;
  if (gSleeping) {
    // 正在演就立刻收尾，并把表情还回去；
    // 调用方（SleepManager）随后会贴睡颜，顺序不能反。
    if (gAction != PRO_NONE) endAction(millis(), true);
    gNextAction = PRO_NONE;
    if (currentView == VIEW_PROACTIVE) proactiveRedraw();
  }
}

bool isProactiveSleeping() { return gSleeping; }

// ═══════════════════════════════════════════════════════════════════════════
//  调试
// ═══════════════════════════════════════════════════════════════════════════
bool proactiveTriggerNow() {
  const uint32_t now = millis();
  if (gAction != PRO_NONE) return false;                 // 正在演就不打断
  if (isExpressionLocked()) return false;
  if (currentView != VIEW_EXPRESSION) return false;      // 演了也看不见
  gNextAction = pickAction();
  startAction(now);
  return gAction != PRO_NONE;
}

void proactiveResetTimer() {
  scheduleNext(millis());
  gLastEvalMs = 0;
  if (currentView == VIEW_PROACTIVE) proactiveRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  状态查询
// ═══════════════════════════════════════════════════════════════════════════
ProactiveAction getProactiveAction()    { return gAction; }
ProactiveAction getNextPlannedAction()  { return gNextAction; }
bool hasProactiveEverFired()            { return gEverFired; }

uint32_t getNextTriggerIn() {
  const int32_t left = (int32_t)(gNextAt - millis());
  return (left > 0) ? (uint32_t)left : 0;
}

uint32_t getSinceLastProactive() {
  if (!gEverFired) return millis();
  return millis() - gLastProactiveMs;
}

// ═══════════════════════════════════════════════════════════════════════════
//  设备端主动互动调试屏（纯观察窗口，不参与任何逻辑）
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

void proactiveRedraw() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,   240, 4, C_ORANGE);
  tft.fillRect(0, 236, 240, 4, C_ORANGE);

  tft.setTextSize(1);
  tft.setTextColor(C_ORANGE);
  tft.setCursor(8, 12);
  tft.print("PROACTIVE DEBUG");
  printRight("VIEW 8", 232, 12, 1, C_MUTED);

  tft.setTextSize(2);
  tft.setTextColor((gEnabled && !gSleeping) ? C_GREEN : C_MUTED);
  tft.setCursor(10, 30);
  tft.print(gSleeping ? "PAUSED (SLEEP)"
                      : (gEnabled ? "PROACTIVE ON" : "PROACTIVE OFF"));

  tft.drawFastHLine(0, 56, 240, C_GRID);

  char b[16];
  char emo[12];
  int16_t y = 68;
  const int16_t ROW = 24;

  const uint32_t idle = getIdleTime();
  snprintf(b, sizeof b, "%lu.%lus",
           (unsigned long)(idle / 1000UL), (unsigned long)((idle % 1000UL) / 100UL));
  printRow("IDLE", b, y); y += ROW;

  upperCopy(emo, emotionKey(getCurrentEmotion()), sizeof emo);
  printRow("EMOTION", emo, y); y += ROW;

  printRow("ACTION", proactiveShort(gAction), y); y += ROW;
  printRow("NEXT",   proactiveShort(gNextAction), y); y += ROW;

  if (gAction != PRO_NONE) {
    snprintf(b, sizeof b, "--");
  } else if (gSleeping) {
    snprintf(b, sizeof b, "SLEEP");
  } else if (!gEnabled) {
    snprintf(b, sizeof b, "OFF");
  } else {
    snprintf(b, sizeof b, "%lus", (unsigned long)(getNextTriggerIn() / 1000UL));
  }
  printRow("TRIGGER IN", b, y);

  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(10, 200);
  tft.print("random 60-180s  idle>=10s");
  tft.setCursor(10, 214);
  tft.print("user action aborts it");
}

void proactiveEnterView() {
  currentView = VIEW_PROACTIVE;
  gLastScreenMs = millis();
  proactiveRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  名字
// ═══════════════════════════════════════════════════════════════════════════
const char* proactiveKey(ProactiveAction a) {
  return PRO_KEYS[a < PRO_COUNT ? a : 0];
}
const char* proactiveLabel(ProactiveAction a) {
  return PRO_LABELS[a < PRO_COUNT ? a : 0];
}
const char* proactiveShort(ProactiveAction a) {
  return PRO_SHORT[a < PRO_COUNT ? a : 0];
}
