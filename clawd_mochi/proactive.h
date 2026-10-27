/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  proactive.h — Clawd Mochi 随机主动互动系统（第四阶段）
 *
 *  职责：在用户没有操作的时候，偶尔让桌宠自己动一下。
 *
 *  两条铁律：
 *    ① 只动「表情 + 动画」，**绝不碰真实情绪**
 *       —— 全程不出现 addEmotion / setEmotion。SAD → BLINK → 还是 SAD。
 *    ② 用户操作优先
 *       —— 轮询 InteractionManager::getIdleTime()，用户一有动作立刻中止当前行为。
 *
 *  三条复用（不重新实现）：
 *    · Idle 检测  → 直接问 interaction.h 的 getIdleTime()
 *    · 临时表情    → 直接用 expression.h 的 requestExpression(id, automatic=true)
 *    · 锁定兼容    → 交给 ExpressionManager 自己拦，本模块不做判断
 *
 *  资源：约 40 字节静态变量，无堆分配、无 Flash 写入、无 delay()。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "expression.h"    // requestExpression / setExpressionAnimation / isExpressionLocked
#include "emotion.h"       // getCurrentEmotion
#include "interaction.h"   // getIdleTime  ← 直接复用，不重新实现 Idle 检测

// ═══════════════════════════════════════════════════════════════════════════
//  来自主固件 clawd_mochi.ino
// ═══════════════════════════════════════════════════════════════════════════
extern Adafruit_ST7789 tft;
extern uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
extern uint16_t C_CLKBG, C_CREAM, C_CLKLINE, C_CLKDEEP;
extern uint8_t  currentView;

// ═══════════════════════════════════════════════════════════════════════════
//  视图编号（第四阶段新增：设备端主动互动调试屏）
// ═══════════════════════════════════════════════════════════════════════════
#define VIEW_PROACTIVE 8

// ═══════════════════════════════════════════════════════════════════════════
//  主动行为 —— 第一版 6 种（按需求第八条的清单）
//  没有 LOOK_AROUND：当前渲染器只能整体平移，做不到「眼睛独立左右看」，
//  硬做就得给动画引擎加新 AnimId，代价不成比例，按需求第七条跳过。
// ═══════════════════════════════════════════════════════════════════════════
enum ProactiveAction : uint8_t {
  PRO_NONE = 0,   // 没在演
  PRO_BLINK,      // 眨一下眼
  PRO_YAWN,       // 打个哈欠
  PRO_DAYDREAM,   // 发呆走神
  PRO_CURIOUS,    // 好奇张望
  PRO_HAPPY,      // 突然开心一下
  PRO_BOUNCE,     // 轻轻弹一下
  PRO_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  节拍常量（全部毫秒）
// ═══════════════════════════════════════════════════════════════════════════
#define PRO_IDLE_MIN_MS    10000    // 用户 idle 超过这么久，才允许主动互动
#define PRO_ABORT_IDLE_MS   1200    // 用户最近这么久内有操作 → 立即中止当前行为
#define PRO_RAND_MIN_MS    60000    // 随机间隔下界（同时也是「最短间隔」）
#define PRO_RAND_MAX_MS   180000    // 随机间隔上界
#define PRO_FIRST_MIN_MS   20000    // 开机后第一次的最短等待
#define PRO_FIRST_MAX_MS   60000    // 开机后第一次的最长等待
#define PRO_EVAL_MS          250    // 判定节拍

// ═══════════════════════════════════════════════════════════════════════════
//  对外接口
// ═══════════════════════════════════════════════════════════════════════════

// ── 生命周期 ──
void proactiveBegin();          // setup() 中调用一次
void proactiveTick();           // loop() 中每轮调用，非阻塞

// ── 总开关 ──
void enableProactive();
void disableProactive();
bool isProactiveEnabled();

// ── 睡眠闸门（第五阶段新增）──
// 睡眠期间必须彻底停摆：不能出现「睡觉 → 突然蹦起来 → 继续睡」。
// 这里刻意用独立开关，而不是去调 disableProactive()，
// 否则会把用户在网页上手动设置的总开关状态一起冲掉。
void proactiveSetSleeping(bool sleeping);
bool isProactiveSleeping();

// ── 调试 ──
bool proactiveTriggerNow();     // 立刻触发一次（返回 false = 当前不满足条件）
void proactiveResetTimer();     // 重掷下一次触发时间
void proactiveEnterView();
void proactiveRedraw();

// ── 状态查询 ──
ProactiveAction getProactiveAction();       // 当前正在演的行为
ProactiveAction getNextPlannedAction();     // 下一次打算演什么
uint32_t getNextTriggerIn();                // 距下次触发还有多少毫秒
uint32_t getSinceLastProactive();           // 距上次触发过了多少毫秒
bool     hasProactiveEverFired();

// ── 名字（供网页 / 调试屏）──
const char* proactiveKey(ProactiveAction a);    // "yawn"
const char* proactiveLabel(ProactiveAction a);  // "打哈欠"
const char* proactiveShort(ProactiveAction a);  // "YAWN"（给 5×7 点阵字用）
