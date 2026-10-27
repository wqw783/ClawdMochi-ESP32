/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  interaction.h — Clawd Mochi 用户互动系统（第三阶段）
 *
 *  职责边界：
 *      InteractionManager  只回答「用户做了什么？」
 *      EmotionManager      只回答「这让我有什么感觉？」
 *      ExpressionManager   只回答「应该表现出什么表情？」
 *
 *  所以本模块：
 *    · 不直接改 EmotionManager 内部变量 —— 一律走 addEmotion()
 *    · 不直接操作 LCD —— 唯一的例外是那个纯观察用的设备端调试屏
 *    · 不用 delay() —— 全部 millis() 驱动，非阻塞
 *
 *  输入源有两个，汇进同一套状态机：
 *    ① 板载 BOOT 按键（GPIO9，按下为 LOW）—— 真正的物理点击
 *    ② 网页「戳一下」的 pointerdown / pointerup —— 走 HTTP
 *
 *  资源：约 40 字节静态变量，无堆分配、无 Flash 写入。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "emotion.h"      // EmotionValue / addEmotion / getEmotionValue
#include "expression.h"   // getCurrentExpression / getExpressionDef / setExpressionAnimation

// ═══════════════════════════════════════════════════════════════════════════
//  来自主固件 clawd_mochi.ino
// ═══════════════════════════════════════════════════════════════════════════
extern Adafruit_ST7789 tft;
extern uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
extern uint16_t C_CLKBG, C_CREAM, C_CLKLINE, C_CLKDEEP;
extern uint8_t  currentView;

// ═══════════════════════════════════════════════════════════════════════════
//  视图编号（第三阶段新增：设备端互动调试屏）
// ═══════════════════════════════════════════════════════════════════════════
#define VIEW_INTERACTION 7

// ═══════════════════════════════════════════════════════════════════════════
//  互动事件
// ═══════════════════════════════════════════════════════════════════════════
enum InteractionEvent : uint8_t {
  IA_NONE = 0,      // 还没有任何互动
  IA_SINGLE_CLICK,  // 单击（burst 内第 1~2 次）
  IA_MULTI_CLICK,   // 连击（burst 内第 3~7 次）
  IA_RAPID_CLICK,   // 过度连击（burst 内第 8 次起）→ 会烦
  IA_LONG_PRESS,    // 长按 ≥1 秒
  IA_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  节拍常量（全部毫秒）
// ═══════════════════════════════════════════════════════════════════════════
#define IA_PIN             9      // 板载 BOOT 按键（按下 = LOW）
#define IA_DEBOUNCE_MS     25     // 按键去抖
#define IA_LONG_MS         1000   // 按住多久算长按
#define IA_PRESS_LOST_MS   8000   // 按住超过这么久视为丢了 release，强制复位
#define IA_BURST_MS        3000   // 连击统计窗口
#define IA_MIN_GAP_MS      120    // 冷却：间隔更短的重复点击不计入 burst
#define IA_MULTI_MIN       3      // 窗口内第几次开始算连击
#define IA_RAPID_MIN       8      // 窗口内第几次开始算「过度」
#define IA_FEEDBACK_MS     400    // 戳一下的跳动反馈时长
#define IA_IDLE_TICK_MS    10000  // idle 检查节拍
#define IA_IDLE_SAD_AFTER  30000  // 多久没互动才开始低落
#define IA_IDLE_SAD_CAP    45     // 低落值的上限（防止无限增长）

// ═══════════════════════════════════════════════════════════════════════════
//  对外接口
// ═══════════════════════════════════════════════════════════════════════════

// ── 生命周期 ──
void interactionBegin();        // setup() 中调用一次（含 pinMode）
void interactionTick();         // loop() 中每轮调用，非阻塞

// ── 输入源 → 事件（两个来源都调这几个）──
void interactionPress();        // 按下
void interactionRelease();      // 松开：内部判定是单击还是长按
void interactionClick();        // 原子点击（没有按下/松开的上游，如网页 click）

// ── 状态查询 ──
InteractionEvent getLastInteraction();
uint8_t   getClickCount();            // 当前 burst 窗口内的点击次数
uint16_t  getTotalClicks();           // 上电以来的总点击次数
uint32_t  getLastInteractionTime();   // 最后一次互动的 millis() 时间戳
uint32_t  getIdleTime();              // 距上次互动过了多少毫秒
bool      hasUserInteracted();        // 有没有互动过
bool      isPressed();                // 当前是否按住
uint8_t   getFeedbackLeft();          // 跳动反馈剩余百分比（调试用）

// ── 调试 ──
void interactionReset();                    // 清空所有计数
void interactionInject(InteractionEvent e); // 网页测试按钮：直接注入一个事件
void interactionEnterView();
void interactionRedraw();

// ── 名字（供网页 / 调试屏）──
const char* interactionKey(InteractionEvent e);    // "single"
const char* interactionLabel(InteractionEvent e);  // "单击"
const char* interactionShort(InteractionEvent e);  // "SINGLE"（给 5×7 点阵字用）
