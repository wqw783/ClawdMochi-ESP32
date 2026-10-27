/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  sleep.h — Clawd Mochi 睡眠 / 唤醒系统（第五阶段）
 *
 *  职责边界（和前面四个阶段一样，只管一件事）：
 *      SleepManager  只回答「我现在醒着、困了，还是睡着了？」
 *
 *  所以本模块：
 *    · 不直接操作 LCD —— 睡颜一律经由 ExpressionManager 走
 *        SleepManager → SleepState → ExpressionManager → Animation → Display
 *    · 不新建时间系统 —— 复用主固件的 epochToLocalDT（通过 localTimeHM）
 *    · 不重新实现 Idle —— 复用 interaction.h 的 getIdleTime()
 *    · 不用 delay() —— 全部 millis() 驱动，非阻塞
 *    · 无堆分配、无 Flash 写入
 *
 *  两条铁律：
 *    ① 用户互动永远优先 —— 一有操作立刻醒，醒后还有一段冷却期不再睡
 *    ② 睡着时主动互动必须停 —— 通过 proactiveSetSleeping() 闸门
 *
 *  优先级链：用户互动 > 睡眠/唤醒 > 主动互动 > 普通动画
 *
 *  资源：约 50 字节静态变量。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "expression.h"    // requestExpression / setExpressionAnimation / isExpressionLocked
#include "emotion.h"       // emotionApply / emotionSetApplyPaused
#include "interaction.h"   // getIdleTime       ← 直接复用，不重新实现 Idle 检测
#include "proactive.h"     // proactiveSetSleeping ← 睡眠闸门

// ═══════════════════════════════════════════════════════════════════════════
//  来自主固件 clawd_mochi.ino
// ═══════════════════════════════════════════════════════════════════════════
extern Adafruit_ST7789 tft;
extern uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
extern uint16_t C_CLKBG, C_CREAM, C_CLKLINE, C_CLKDEEP;
extern uint8_t  currentView;

// 主固件已有的本地时间（网页校时 + millis() 走时），不是第二套时间系统。
// 返回 false = 网页还没校时，此时一律按「白天」处理。
bool localTimeHM(uint8_t &hour, uint8_t &minute);

// ═══════════════════════════════════════════════════════════════════════════
//  视图编号（第五阶段新增：设备端睡眠调试屏）
// ═══════════════════════════════════════════════════════════════════════════
#define VIEW_SLEEP 9

// ═══════════════════════════════════════════════════════════════════════════
//  睡眠状态 —— 4 个
// ═══════════════════════════════════════════════════════════════════════════
enum SleepState : uint8_t {
  SLEEP_AWAKE = 0,   // 正常活动
  SLEEP_DROWSY,      // 困倦（睡眠前的过渡）
  SLEEP_SLEEPING,    // 正式睡眠
  SLEEP_WAKING,      // 刚被唤醒
  SLEEP_STATE_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  睡眠原因 —— 便于以后扩展（比如「白天太累」「被无视太久」）
// ═══════════════════════════════════════════════════════════════════════════
enum SleepReason : uint8_t {
  SLEEP_REASON_NONE = 0,   // 没在睡
  SLEEP_REASON_IDLE,       // 白天长时间没互动
  SLEEP_REASON_NIGHT,      // 夜间时段
  SLEEP_REASON_MANUAL,     // 调试按钮强制
  SLEEP_REASON_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  夜间时段 —— 默认 23:00 ~ 07:00（跨午夜）
//  别把 23 / 7 散落在代码各处，以后做设置页面直接改这几个变量
// ═══════════════════════════════════════════════════════════════════════════
#define SLEEP_NIGHT_START_H   23
#define SLEEP_NIGHT_START_M    0
#define SLEEP_NIGHT_END_H      7
#define SLEEP_NIGHT_END_M      0

// ═══════════════════════════════════════════════════════════════════════════
//  阈值（全部毫秒）
//
//  睡眠不是只看时间，也不是只看 Idle，而是「当前时间 + Idle」一起看：
//      白天：idle ≥ 5 分钟 → DROWSY；idle ≥ 10 分钟 → SLEEPING
//      夜间：idle ≥ 45 秒  → DROWSY；idle ≥ 2 分钟  → SLEEPING
//  也就是说 DROWSY 会自然持续 (SLEEP_IDLE - DROWSY_IDLE) 那么久。
// ═══════════════════════════════════════════════════════════════════════════
#define SLEEP_DAY_DROWSY_IDLE_MS     300000UL   // 白天：5 分钟
#define SLEEP_DAY_SLEEP_IDLE_MS      600000UL   // 白天：10 分钟
#define SLEEP_NIGHT_DROWSY_IDLE_MS    45000UL   // 夜间：45 秒
#define SLEEP_NIGHT_SLEEP_IDLE_MS    120000UL   // 夜间：2 分钟
#define SLEEP_NIGHT_GRACE_MS          20000UL   // 刚入夜先醒 20 秒，不瞬间睡着

// 开发测试档（网页一键切换，默认关）
#define SLEEP_TEST_DROWSY_IDLE_MS     60000UL   // 60 秒
#define SLEEP_TEST_SLEEP_IDLE_MS      80000UL   // 80 秒

// 其它节拍
#define SLEEP_WAKE_IDLE_MS              400UL   // 睡着时 idle 掉到这么低 = 用户点了
#define SLEEP_WAKING_PHASE_MS           400UL   // WAKING 第一段（睁眼）时长
#define SLEEP_WAKING_MS                1200UL   // WAKING 总时长，之后回 AWAKE
#define SLEEP_WAKE_COOLDOWN_MS        45000UL   // 唤醒后 45 秒内不再睡
#define SLEEP_EVAL_MS                   500UL   // 判定节拍
#define SLEEP_BOOT_GRACE_MS            3000UL   // 开机后先醒 3 秒

// ═══════════════════════════════════════════════════════════════════════════
//  对外接口
// ═══════════════════════════════════════════════════════════════════════════

// ── 生命周期 ──
void sleepBegin();              // setup() 中调用一次
void sleepTick();               // loop() 中每轮调用，非阻塞

// ── 状态查询 ──
SleepState  getSleepState();
SleepReason getSleepReason();
bool        isSleeping();       // SLEEPING
bool        isDrowsy();         // DROWSY
bool        isWaking();         // WAKING
bool        isSleepAwake();     // AWAKE（不叫 isAwake，避免和别的库撞名）
bool        isNightTimeNow();   // 当前是否处于夜间时段
bool        isTimeSynced();     // 时间是否已同步（未同步则按白天处理）

// ── 唤醒 / 强制 ──
bool wakeUp();                  // 唤醒（SLEEPING → WAKING；DROWSY → AWAKE）
void forceSleep();              // 直接睡着（调试）
void forceDrowsy();             // 直接犯困（调试）

// ── 开关 ──
void enableSleep();
void disableSleep();
bool isSleepEnabled();

// ── 开发测试 ──
void sleepSetTestMode(bool on);     // 切到 60s / 80s 的短阈值
bool isSleepTestMode();
void sleepSetTestTime(int8_t hour, int8_t minute);  // 覆盖当前时间（-1 = 关闭）
void sleepClearTestTime();
bool getSleepTestTime(uint8_t &hour, uint8_t &minute);  // false = 没有覆盖，用真实时间
void sleepResetTimer();             // 清冷却 + 重新开始计时

// ── 夜间时段配置 ──
void sleepSetNightWindow(uint8_t startH, uint8_t startM, uint8_t endH, uint8_t endM);
uint8_t getSleepNightStartH();
uint8_t getSleepNightStartM();
uint8_t getSleepNightEndH();
uint8_t getSleepNightEndM();

// ── 调试读数 ──
uint32_t getSleepIdleTime();        // 当前参与判定的 Idle
uint32_t getDrowsyIn();             // 距犯困还差多少毫秒（0 = 已经到了）
uint32_t getSleepIn();              // 距睡着还差多少毫秒
uint32_t getWakeCooldownLeft();     // 唤醒冷却剩余
uint8_t  getSleepHour();            // 参与判定的当前小时
uint8_t  getSleepMinute();

// ── 设备端调试屏 ──
void sleepEnterView();
void sleepRedraw();

// ── 名字（供网页 / 调试屏）──
const char* sleepStateKey(SleepState s);      // "awake"
const char* sleepStateLabel(SleepState s);    // "醒着"
const char* sleepStateShort(SleepState s);    // "AWAKE"（给调试屏用）
const char* sleepReasonKey(SleepReason r);    // "night"
const char* sleepReasonLabel(SleepReason r);  // "夜间"
