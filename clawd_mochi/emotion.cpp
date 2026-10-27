/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  emotion.cpp — 情绪系统实现
 *
 *  四件事，按顺序：
 *    ① 衰减   decayStep()   每 5 秒把所有值往基线拉一格
 *    ② 判定   judgeCandidate()  按优先级规则表选出候选情绪
 *    ③ 稳定   evaluate()    候选要连续保持 1.5 秒才真的切换（防抖动）
 *    ④ 映射   emotionApply() 调 ExpressionManager，自动请求会被 Expression Lock 拦下
 *
 *  本文件不出现任何 tft.fillScreen / drawXXX —— 除了最后那段「设备端调试屏」。
 *  调试屏是纯观察窗口，不参与情绪逻辑，所以单独隔在文件末尾。
 *
 *  内存：7 个 uint8_t 的值 + 约 20 字节状态。无 new、无 String、无 Flash 写入。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "emotion.h"

// ═══════════════════════════════════════════════════════════════════════════
//  静态数据表
// ═══════════════════════════════════════════════════════════════════════════

// 每个情绪值的基线（衰减目标）与每拍衰减步长
static const uint8_t EV_BASE[EV_COUNT] = { 0, 80,  0,  0,  0, 0, 0 };
static const uint8_t EV_STEP[EV_COUNT] = { 1,  1,  1,  1,  1, 8, 1 };
//                                          ↑   ↑   ↑   ↑   ↑  ↑  ↑
//                                          开  精  生  难  好  惊 害
//                                          心  力  气  过  奇  吓 羞
//                                                          快降

// 情绪判定规则表 —— 从上往下第一个条件成立的即为候选
//   dir: 0 = 值 >= 阈值   1 = 值 <= 阈值
//   urgent: 是否绕过稳定确认立即生效
//
//   ⚠ 阈值在第三阶段为「用户互动」重新标定过一次：
//     原本 HAPPY 50 / EXCITED 75 / ANGRY 55 是为调试按钮定的，
//     单击那点情绪量根本到不了。现在改成
//     HAPPY 25 / EXCITED 60 / ANGRY 30 / SAD 30 / CURIOUS 25 / SHY 25，
//     于是「点一下→开心、连点三下→兴奋、猛点八下→生气」才成立。
struct EmotionRule {
  uint8_t emotion;
  uint8_t valueId;
  uint8_t dir;
  uint8_t thr;
  uint8_t auxId;    // 255 = 无辅助条件
  uint8_t auxMin;   // 辅助值下限
  uint8_t urgent;
};

static const EmotionRule RULES[] = {
  { EMO_SCARED,    EV_STARTLE,   0, 70, 255,       0, 1 },   // 惊吓很重
  { EMO_SURPRISED, EV_STARTLE,   0, 45, 255,       0, 1 },   // 受惊
  { EMO_ANGRY,     EV_ANGER,     0, 30, 255,       0, 0 },
  { EMO_SLEEPY,    EV_ENERGY,    1, 20, 255,       0, 0 },   // 精力见底
  { EMO_SAD,       EV_SADNESS,   0, 30, 255,       0, 0 },
  { EMO_EXCITED,   EV_HAPPINESS, 0, 60, EV_ENERGY, 60, 0 },   // 又开心又有劲
  { EMO_HAPPY,     EV_HAPPINESS, 0, 25, 255,       0, 0 },
  { EMO_CURIOUS,   EV_CURIOSITY, 0, 25, 255,       0, 0 },
  { EMO_SHY,       EV_SHYNESS,   0, 25, 255,       0, 0 },
};
#define RULE_COUNT ((uint8_t)(sizeof(RULES) / sizeof(RULES[0])))

// Emotion → Expression 映射表（唯一一处「情绪长什么样」的定义）
//   SHY 用 embarrassed(害羞) 而不是 shy(撒娇)：
//   撒娇是主动讨好，害羞是被动脸红，语义上更贴 SHY
static const uint8_t EMO_TO_EXPR[EMO_COUNT] = {
  EXPR_NEUTRAL,      // CALM      平静
  EXPR_HAPPY,        // HAPPY     开心
  EXPR_LAUGH,        // EXCITED   大笑
  EXPR_EMBARRASSED,  // SHY       害羞
  EXPR_ANGRY,        // ANGRY     生气
  EXPR_SAD,          // SAD       悲伤
  EXPR_SLEEPY,       // SLEEPY    困
  EXPR_SURPRISED,    // SURPRISED 惊讶
  EXPR_THINKING,     // CURIOUS   思考
  EXPR_SCARED,       // SCARED    害怕
};

// 名字表
static const char* const EMO_KEYS[EMO_COUNT] = {
  "calm", "happy", "excited", "shy", "angry",
  "sad", "sleepy", "surprised", "curious", "scared"
};
static const char* const EMO_LABELS[EMO_COUNT] = {
  "平静", "开心", "兴奋", "害羞", "生气",
  "难过", "困倦", "惊讶", "好奇", "害怕"
};
static const char* const EMO_UPPER[EMO_COUNT] = {
  "CALM", "HAPPY", "EXCITED", "SHY", "ANGRY",
  "SAD", "SLEEPY", "SURPRISED", "CURIOUS", "SCARED"
};
static const char* const EV_KEYS[EV_COUNT] = {
  "happiness", "energy", "anger", "sadness", "curiosity", "startle", "shyness"
};
static const char* const EV_LABELS[EV_COUNT] = {
  "开心", "精力", "生气", "难过", "好奇", "惊吓", "害羞"
};
static const char* const EV_SHORT[EV_COUNT] = {
  "HAPPY", "ENERGY", "ANGER", "SAD", "CURIO", "STARTLE", "SHY"
};

// ═══════════════════════════════════════════════════════════════════════════
//  运行时状态
// ═══════════════════════════════════════════════════════════════════════════
static uint8_t   gVal[EV_COUNT];
static EmotionId gEmotion      = EMO_CALM;
static int8_t    gPending      = -1;     // 正在等待确认的候选情绪
static uint32_t  gPendingSince = 0;
static uint32_t  gHoldUntil    = 0;      // setEmotion() 之后的压制截止时刻
static uint32_t  gLastDecayMs  = 0;
static uint32_t  gLastEvalMs   = 0;
static bool      gApplyPaused  = false;   // 第五阶段：睡眠期间停推表情

// ═══════════════════════════════════════════════════════════════════════════
//  ① 衰减
// ═══════════════════════════════════════════════════════════════════════════
static void decayStep() {
  for (uint8_t i = 0; i < EV_COUNT; i++) {
    const uint8_t b = EV_BASE[i];
    const uint8_t s = EV_STEP[i];
    if (gVal[i] > b)      gVal[i] -= (uint8_t)((gVal[i] - b > s) ? s : (gVal[i] - b));
    else if (gVal[i] < b) gVal[i] += (uint8_t)((b - gVal[i] > s) ? s : (b - gVal[i]));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  ② 判定
// ═══════════════════════════════════════════════════════════════════════════
static bool ruleHolds(const EmotionRule& r) {
  const uint8_t v = gVal[r.valueId];
  const bool ok = (r.dir == 0) ? (v >= r.thr) : (v <= r.thr);
  if (!ok) return false;
  if (r.auxId != 255 && gVal[r.auxId] < r.auxMin) return false;
  return true;
}

static int8_t judgeCandidate() {
  for (uint8_t i = 0; i < RULE_COUNT; i++)
    if (ruleHolds(RULES[i])) return (int8_t)RULES[i].emotion;
  return (int8_t)EMO_CALM;                       // 兜底
}

static bool isUrgent(int8_t emo) {
  for (uint8_t i = 0; i < RULE_COUNT; i++)
    if ((int8_t)RULES[i].emotion == emo) return RULES[i].urgent != 0;
  return false;
}

// 该情绪当前是否还站得住（CALM 是兜底，视为「不成立」，好让位给别的情绪）
static bool emotionStillValid(int8_t emo) {
  if (emo == (int8_t)EMO_CALM) return false;
  for (uint8_t i = 0; i < RULE_COUNT; i++)
    if ((int8_t)RULES[i].emotion == emo) return ruleHolds(RULES[i]);
  return false;
}

// 超出触发线多少（数值余量用）
static int8_t excessOf(int8_t emo) {
  if (emo == (int8_t)EMO_CALM) return 0;
  for (uint8_t i = 0; i < RULE_COUNT; i++) {
    if ((int8_t)RULES[i].emotion != emo) continue;
    const uint8_t v = gVal[RULES[i].valueId];
    return (RULES[i].dir == 0) ? (int8_t)(v - RULES[i].thr)
                               : (int8_t)(RULES[i].thr - v);
  }
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ③ 稳定 —— 三层防抖
//     a) 紧急情绪（惊吓 / 惊讶）立即生效
//     b) 当前情绪仍站得住时，候选必须越线 EMO_SWITCH_GAP 点以上
//     c) 候选必须连续保持 EMO_CONFIRM_MS 才真的切换
// ═══════════════════════════════════════════════════════════════════════════
static void switchTo(int8_t emo) {
  gEmotion = (EmotionId)emo;
  gPending = -1;
  emotionApply();
  if (currentView == VIEW_EMOTION) emotionRedraw();
}

static void evaluate(uint32_t now) {
  const int8_t cand = judgeCandidate();

  if (cand == (int8_t)gEmotion) { gPending = -1; return; }   // 没变

  if (isUrgent(cand)) { switchTo(cand); return; }            // (a)

  if (emotionStillValid((int8_t)gEmotion) &&
      excessOf(cand) < EMO_SWITCH_GAP) { gPending = -1; return; }   // (b)

  if (gPending != cand) { gPending = cand; gPendingSince = now; return; }  // (c)
  if (now - gPendingSince < EMO_CONFIRM_MS) return;

  switchTo(cand);
}

// 手动压制期内不做自动判定
static void maybeEvaluate() {
  const uint32_t now = millis();
  if ((int32_t)(now - gHoldUntil) < 0) return;
  if (now - gLastEvalMs < EMO_EVAL_MS) return;
  gLastEvalMs = now;
  evaluate(now);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ④ 映射 —— 唯一一条通往屏幕的路径
// ═══════════════════════════════════════════════════════════════════════════
void emotionApply() {
  // 第五阶段：睡眠期间屏幕由 SleepManager 掌管，这里直接让路。
  // 只拦「往屏幕推」，不拦情绪本身的衰减与判定。
  if (gApplyPaused) return;

  // automatic = true：ExpressionManager 内部会检查锁定，
  // 锁着就直接返回 false，本模块无需知道锁的存在。
  requestExpression((ExpressionId)EMO_TO_EXPR[gEmotion], true);
}

// ── 睡眠闸门（第五阶段新增）──
void emotionSetApplyPaused(bool paused) { gApplyPaused = paused; }
bool isEmotionApplyPaused()             { return gApplyPaused; }

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void emotionBegin() {
  for (uint8_t i = 0; i < EV_COUNT; i++) gVal[i] = EV_BASE[i];
  gEmotion      = EMO_CALM;
  gPending      = -1;
  gPendingSince = 0;
  gHoldUntil    = 0;
  gLastDecayMs  = millis();
  gLastEvalMs   = millis();
  gApplyPaused  = false;
  emotionApply();                 // 开机即 CALM → 平静表情
}

void emotionTick() {
  const uint32_t now = millis();

  if (now - gLastDecayMs >= EMO_DECAY_MS) {     // 低频衰减，不每帧改
    gLastDecayMs = now;
    decayStep();
    if (currentView == VIEW_EMOTION) emotionRedraw();
  }

  maybeEvaluate();
}

// ═══════════════════════════════════════════════════════════════════════════
//  情绪状态
// ═══════════════════════════════════════════════════════════════════════════
EmotionId getCurrentEmotion() { return gEmotion; }

bool setEmotion(EmotionId id) {
  if (id >= EMO_COUNT) return false;
  gEmotion      = id;
  gPending      = -1;
  gHoldUntil    = millis() + EMO_MANUAL_HOLD;   // 压制自动判定，否则 5 秒内就被改回去
  emotionApply();
  if (currentView == VIEW_EMOTION) emotionRedraw();
  return true;
}

bool setEmotionByIndex(uint8_t id) {
  if (id >= EMO_COUNT) return false;
  return setEmotion((EmotionId)id);
}

// ═══════════════════════════════════════════════════════════════════════════
//  情绪值
// ═══════════════════════════════════════════════════════════════════════════
bool addEmotion(EmotionValue v, int8_t delta) {
  if (v >= EV_COUNT) return false;
  int16_t nv = (int16_t)gVal[v] + (int16_t)delta;
  if (nv < EMO_VALUE_MIN) nv = EMO_VALUE_MIN;
  if (nv > EMO_VALUE_MAX) nv = EMO_VALUE_MAX;
  if ((uint8_t)nv == gVal[v]) return false;

  gVal[v] = (uint8_t)nv;
  maybeEvaluate();
  if (currentView == VIEW_EMOTION) emotionRedraw();
  return true;
}

bool modifyEmotion(EmotionValue v, int8_t delta) {
  return addEmotion(v, delta);
}

uint8_t getEmotionValue(EmotionValue v) {
  return (v < EV_COUNT) ? gVal[v] : 0;
}

void setEmotionValue(EmotionValue v, uint8_t value) {
  if (v >= EV_COUNT) return;
  if (value > EMO_VALUE_MAX) value = EMO_VALUE_MAX;
  if (gVal[v] == value) return;
  gVal[v] = value;
  maybeEvaluate();
  if (currentView == VIEW_EMOTION) emotionRedraw();
}

void resetEmotion() {
  for (uint8_t i = 0; i < EV_COUNT; i++) gVal[i] = EV_BASE[i];
  gEmotion      = EMO_CALM;
  gPending      = -1;
  gPendingSince = 0;
  gHoldUntil    = 0;
  emotionApply();
  if (currentView == VIEW_EMOTION) emotionRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  设备端情绪调试屏（纯观察窗口，不参与任何情绪逻辑）
// ═══════════════════════════════════════════════════════════════════════════
void emotionRedraw() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,   240, 4, C_ORANGE);
  tft.fillRect(0, 236, 240, 4, C_ORANGE);

  tft.setTextSize(1);
  tft.setTextColor(C_ORANGE);
  tft.setCursor(8, 10);   tft.print("EMOTION DEBUG");
  tft.setTextColor(C_MUTED);
  tft.setCursor(150, 10); tft.print("DECAY 5s");

  // 当前情绪（大字）
  tft.setTextColor(C_WHITE);
  tft.setTextSize(3);
  tft.setCursor(8, 24);
  tft.print(EMO_UPPER[gEmotion]);

  tft.drawFastHLine(0, 50, 240, C_GRID);

  // 七个值：名字 + 条 + 数字
  for (uint8_t i = 0; i < EV_COUNT; i++) {
    const int16_t y = 56 + (int16_t)i * 23;

    tft.setTextColor(C_MUTED);
    tft.setTextSize(1);
    tft.setCursor(8, y + 5);
    tft.print(EV_SHORT[i]);

    tft.fillRect(54, y + 3, 140, 10, C_GRID);
    const int16_t w = (int16_t)(140L * gVal[i] / 100);
    if (w > 0) tft.fillRect(54, y + 3, w, 10, C_GREEN);

    char b[6];
    snprintf(b, sizeof b, "%3d", gVal[i]);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(1);
    tft.setCursor(200, y + 5);
    tft.print(b);
  }
}

void emotionEnterView() {
  currentView = VIEW_EMOTION;
  emotionRedraw();
}

// ═══════════════════════════════════════════════════════════════════════════
//  给网页 / 调试用
// ═══════════════════════════════════════════════════════════════════════════
const char* emotionKey(EmotionId id) {
  return EMO_KEYS[id < EMO_COUNT ? id : 0];
}
const char* emotionLabel(EmotionId id) {
  return EMO_LABELS[id < EMO_COUNT ? id : 0];
}
const char* emotionValueKey(EmotionValue v) {
  return EV_KEYS[v < EV_COUNT ? v : 0];
}
const char* emotionValueLabel(EmotionValue v) {
  return EV_LABELS[v < EV_COUNT ? v : 0];
}
uint8_t emotionValueBaseline(EmotionValue v) {
  return EV_BASE[v < EV_COUNT ? v : 0];
}
uint8_t emotionValueStep(EmotionValue v) {
  return EV_STEP[v < EV_COUNT ? v : 0];
}
