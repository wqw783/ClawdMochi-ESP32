/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  emotion.h — Clawd Mochi 情绪系统（第二阶段）
 *
 *  职责边界（很重要）：
 *      EmotionManager  只回答「我现在是什么情绪？」
 *      ExpressionManager 只回答「该显示什么表情？」
 *
 *  本模块 **绝不** 直接操作 tft。所有画面都经由
 *  ExpressionManager::requestExpression(id, automatic = true) 走一遍，
 *  于是第一阶段的 Expression Lock 天然生效，不需要在本模块里判断锁。
 *
 *  资源：7 个 uint8_t 的值 + 几十字节状态，无堆分配、无 Flash 写入。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "expression.h"   // ExpressionId / requestExpression / isExpressionLocked

// ═══════════════════════════════════════════════════════════════════════════
//  来自主固件 clawd_mochi.ino
// ═══════════════════════════════════════════════════════════════════════════
extern Adafruit_ST7789 tft;
extern uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
extern uint16_t C_CLKBG, C_CREAM, C_CLKLINE, C_CLKDEEP;
extern uint8_t  currentView;

// ═══════════════════════════════════════════════════════════════════════════
//  视图编号（第二阶段新增：设备端情绪调试屏）
// ═══════════════════════════════════════════════════════════════════════════
#define VIEW_EMOTION 6

// ═══════════════════════════════════════════════════════════════════════════
//  情绪枚举 —— 10 种
// ═══════════════════════════════════════════════════════════════════════════
enum EmotionId : uint8_t {
  EMO_CALM = 0,   // 平静（兜底）
  EMO_HAPPY,      // 开心
  EMO_EXCITED,    // 兴奋
  EMO_SHY,        // 害羞
  EMO_ANGRY,      // 生气
  EMO_SAD,        // 难过
  EMO_SLEEPY,     // 困倦
  EMO_SURPRISED,  // 惊讶
  EMO_CURIOUS,    // 好奇
  EMO_SCARED,     // 害怕
  EMO_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  情绪数值 —— 7 个，每个 0..100
//  startle（惊吓）是瞬时值：降得最快，SURPRISED / SCARED 由它的强度派生
// ═══════════════════════════════════════════════════════════════════════════
enum EmotionValue : uint8_t {
  EV_HAPPINESS = 0,   // 开心
  EV_ENERGY,          // 精力
  EV_ANGER,           // 生气
  EV_SADNESS,         // 难过
  EV_CURIOSITY,       // 好奇
  EV_STARTLE,         // 惊吓
  EV_SHYNESS,         // 害羞
  EV_COUNT
};

#define EMO_VALUE_MIN 0
#define EMO_VALUE_MAX 100

// ═══════════════════════════════════════════════════════════════════════════
//  节拍常量
// ═══════════════════════════════════════════════════════════════════════════
#define EMO_DECAY_MS    5000   // 衰减节拍：每 5 秒一次
#define EMO_EVAL_MS      250   // 判定节拍：每 250ms 重算候选
#define EMO_CONFIRM_MS  1500   // 候选必须连续保持这么久才真的切换
#define EMO_SWITCH_GAP     6   // 数值余量：候选要越线 6 点以上才允许切换
#define EMO_MANUAL_HOLD 5000   // setEmotion() 之后压制自动判定的时间

// ═══════════════════════════════════════════════════════════════════════════
//  对外接口
// ═══════════════════════════════════════════════════════════════════════════

// ── 生命周期 ──
void emotionBegin();                 // setup() 中调用一次
void emotionTick();                  // loop() 中每轮调用，非阻塞

// ── 情绪状态 ──
EmotionId getCurrentEmotion();
bool      setEmotion(EmotionId id);              // 直接指定（会压制自动判定 5 秒）
bool      setEmotionByIndex(uint8_t id);         // 给网页用的 uint8_t 版本

// ── 情绪值 ──
bool    addEmotion(EmotionValue v, int8_t delta);   // 增加 / 减少，自动钳位到 0..100
bool    modifyEmotion(EmotionValue v, int8_t delta);// addEmotion 的同义接口
uint8_t getEmotionValue(EmotionValue v);
void    setEmotionValue(EmotionValue v, uint8_t value);
void    resetEmotion();                             // 全部回到基线

// ── 推给表情系统 ──
void emotionApply();                 // 按当前情绪调 requestExpression(map, true)

// ── 睡眠闸门（第五阶段新增）──
// 睡眠期间情绪照常衰减 / 判定（gEmotion 该变还是会变），
// 但**不再自动往屏幕推表情** —— 否则睡颜会被情绪变化盖掉。
// 解除后由 SleepManager 调一次 emotionApply() 立刻恢复。
// 注意：手动 setEmotion() 也走同一条路径，所以睡眠中手动设情绪只改数值不改画面。
void emotionSetApplyPaused(bool paused);
bool isEmotionApplyPaused();

// ── 设备端调试屏 ──
void emotionEnterView();
void emotionRedraw();

// ── 给网页 / 调试用 ──
const char* emotionKey(EmotionId id);            // "happy"
const char* emotionLabel(EmotionId id);          // "开心"
const char* emotionValueKey(EmotionValue v);     // "happiness"
const char* emotionValueLabel(EmotionValue v);   // "开心"
uint8_t     emotionValueBaseline(EmotionValue v);
uint8_t     emotionValueStep(EmotionValue v);
