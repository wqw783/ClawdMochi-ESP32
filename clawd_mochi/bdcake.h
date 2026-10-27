/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  bdcake.h — 生日动画（桌宠屏幕上的 240×240 像素场景）
 *
 *  和「生日 / 纪念日」模块（birthday.h，VIEW_BIRTHDAY = 11）是两件事：
 *    · birthday.*：日程式的提醒 + 纪念日庆祝（数据在 NVS 里）
 *    · bdcake.*  ：主人点「生日动画」进来时播放的一段画面（蛋糕 / 蜡烛 /
 *                  玉玉兽 / 王飚飚 / 气球彩带 / 底部像素对话框）
 *
 *  设计边界：
 *    · 不新建时间系统：只读 millis()（动画节奏用）
 *    · 不用阻塞延时：全部 millis() 驱动的非阻塞帧循环
 *    · 不用 String：固定数组，零堆分配（除了一块横带画布）
 *    · 不碰情绪 / 表情 / 睡眠 / 日程：它是一个「展示型」视图，
 *      和时钟、画板同级 —— 谁也别抢它，它也不抢别人
 *    · 画面全部自己画：像素精灵来自生日动画 index.html（bdcake_sprites.h）
 *
 *  内存（R8 改动）：不再一次性申请整屏 115,200 B，而是
 *  240×80 的一条横带画布 = 38,400 B；一帧分 3 带画/推，内存压力降到 1/3。
 *
 *  自诊断（网页面板 /bc/state、/bc/show 会带这些值）：
 *    bdcakeHeap / bdcakeHeapMin / bdcakeUptimeSec / bdcakeBootCount /
 *    bdcakeResetReason(Name) / bdcakeFps / bdcakeFrameMs / bdcakeCanvasReady
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// 来自主固件 clawd_mochi.ino
extern Adafruit_ST7789 tft;
extern uint8_t currentView;

// 视图编号：0..11 已被前七个阶段 + 官方 5 视图占用（10/11 = 日程 / 纪念日）
#ifndef VIEW_BDCAKE
  #define VIEW_BDCAKE 12
#endif

void bdcakeBegin();        // setup() 里调用一次：调色板 / 随机种子 / 复位诊断
void bdcakeTick();         // loop() 里每轮调用：非阻塞（内部 50ms 一帧）
void bdcakeEnterView();    // /enter?v=12：切到生日动画视图并画第一帧
void bdcakeRedraw();       // 按当前状态重画一帧（供 redrawCurrentView 用）

// ── 交互（网页两个按钮 + /bc/* 路由都落到这三个函数）──────────────
// 三者都会先确保桌宠停在生日动画视图（手机点了必须看得见）
void bdcakeWish();         // 🙏 许愿：闭上眼睛许愿吧 → 3 秒后 许愿成功，心想事成
void bdcakeBlow();         // 💨 吹蜡烛：熄灭烛火 + 气球彩带 + 祝你生日快乐
void bdcakeReset();        // 🔄 重新点亮蜡烛

// ── 状态查询（给 /bc/state、/bc/show 用）─────────────────────────
bool        bdcakeFlamesOn();
bool        bdcakeIsWishing();
bool        bdcakeCanvasReady();
uint32_t    bdcakeHeap();
uint32_t    bdcakeHeapMin();
uint32_t    bdcakeUptimeSec();
uint32_t    bdcakeBootCount();       // 跨软件复位保留（掉电归零）
uint8_t     bdcakeResetReason();     // esp_reset_reason() 原始值
const char* bdcakeResetReasonName(); // POWERON / BROWNOUT / TASK_WDT / PANIC …
uint8_t     bdcakeFps();
uint16_t    bdcakeFrameMs();
