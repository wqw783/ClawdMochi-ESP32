/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  birthday.h — Clawd Mochi 生日 / 纪念日系统（第七阶段）
 *
 *  职责边界：
 *      BirthdayManager 只回答「今天是不是那个特殊日子？还有几天？」
 *
 *  所以本模块：
 *    · 不新建时间系统 —— 「现在」从 schedule 那边拿（getScheduleNow），
 *        日历换算用 scheduleMakeEpoch() / scheduleYMDHM()
 *        **全项目的日历算法仍然只有 schedule.cpp 里那一份**
 *    · 不新建日历库 —— 不做「365 天」这种近似，闰年交给 daysFromCivil
 *    · 不新建动画系统 —— 表情阶段直接 requestExpression + setExpressionAnimation
 *    · 不新建表情系统 —— 脸上永远是已有的 EXPR_HAPPY，场景里的蛋糕/彩带是自己画的
 *    · 不重新实现情绪 —— 只调 addEmotion()，让它自然衰减回去（临时情绪，不锁死）
 *    · 不控制 SleepManager —— 只**读** isSleeping() / isDrowsy()，睡着就不播庆祝
 *    · 不改 ProactiveInteractionManager —— 只**暴露** isSpecialDayToday() 给它读
 *    · 不用 delay() —— 全部 millis() 驱动，非阻塞
 *    · 不用 String —— 固定长度 char 数组，零堆分配
 *    · 不高频写 Flash —— 只在 增/删/改/启停 + 档位触发 时写 NVS
 *
 *  三个「不打扰用户」的克制：
 *    ① 只在 currentView == VIEW_EXPRESSION 时自动播庆祝 —— 用户在终端敲字 /
 *       看时钟 / 看日程时**不劫持屏幕**，改成挂起，等他回到表情视图再放
 *    ② 睡觉时不播（SLEEPING / DROWSY 都挂起）—— 不强制叫醒
 *    ③ Expression Lock 期间不播 —— 挂起，解锁后再放
 *
 *  2 月 29 日的策略（主人拍板）：平年提前到 2 月 28 日庆祝。
 *
 *  资源：8 个特殊日期 × 40 字节 = 320 字节 RAM（静态数组，零堆）
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "expression.h"   // requestExpression / setExpressionAnimation / isExpressionLocked
#include "emotion.h"      // addEmotion / emotionApply
#include "sleep.h"        // isSleeping / isDrowsy  ← 只读查询，不构成循环依赖
#include "schedule.h"     // getScheduleNow / scheduleMakeEpoch / scheduleYMDHM
                          // / isTimeReady / asciiTitle / leapYear / daysInMonth

// ═══════════════════════════════════════════════════════════════════════════
//  来自主固件 clawd_mochi.ino
// ═══════════════════════════════════════════════════════════════════════════
extern Adafruit_ST7789 tft;
extern uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
extern uint16_t C_CLKBG, C_CREAM, C_CLKLINE, C_CLKDEEP;
extern uint8_t  currentView;

// ═══════════════════════════════════════════════════════════════════════════
//  视图编号（第七阶段新增）
//  0..10 已被前六个阶段占用
//  这个视图有两个模式：庆祝进行中 → 庆祝场景；否则 → 特殊日期列表
// ═══════════════════════════════════════════════════════════════════════════
#define VIEW_BIRTHDAY 11

// 主固件的视图编号 —— 值必须与 clawd_mochi.ino 完全一致。
// .ino 里也会 #define 一次，替换列表相同 → 重复定义合法。
// （本模块只用来判断「用户是不是正在忙」，不去画这些视图）
#ifndef VIEW_EYES_NORMAL
  #define VIEW_EYES_NORMAL 0
#endif
#ifndef VIEW_CODE
  #define VIEW_CODE 2
#endif
#ifndef VIEW_DRAW
  #define VIEW_DRAW 3
#endif

// 主固件：按 currentView 重绘当前画面（定义在 clawd_mochi.ino，
// 与 routeRedraw 共用同一份分派，庆祝收尾时用它把画面还回去）
void redrawCurrentView();

// ═══════════════════════════════════════════════════════════════════════════
//  特殊日期类型
//  前缀用 SDT_ / SDS_，避开 emotion.h 的 EV_* 和 schedule.h 的 SEV_*/SES_*
// ═══════════════════════════════════════════════════════════════════════════
enum SpecialDayType : uint8_t {
  SDT_BIRTHDAY = 0,   // 🎂 生日（每年重复）
  SDT_ANNIVERSARY,    // ❤️ 纪念日（每年重复 + 算「第 N 年」）
  SDT_SPECIAL,        // 🎉 特殊日期（每年重复，不记年）
  SDT_TYPE_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  今日状态 —— 4 个（需求第八条）
//
//  PASSED 不是「永久过去」：生日每年重复，所以 PASSED 只表示
//  「这一次刚过去不久（≤7 天）」，下一年还会重新进入 UPCOMING / TODAY。
// ═══════════════════════════════════════════════════════════════════════════
enum SpecialDayState : uint8_t {
  SDS_NORMAL = 0,   // 还早
  SDS_UPCOMING,     // 快到了（≤ 7 天）
  SDS_TODAY,        // 就是今天
  SDS_PASSED,       // 刚过去（≤ 7 天前）
  SDS_STATE_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  容量与结构
// ═══════════════════════════════════════════════════════════════════════════
#define SD_TITLE_LEN  24    // UTF-8：8 个汉字 / 23 个 ASCII
#define SD_MAX         8    // 主人拍板；8 × 40B = 320B RAM

//  内存布局（纯 POD，可直接整体 putBytes 进 NVS，无指针无虚表）：
//      id(4) + title(24) + lastKey(4) + year(2) + type/month/day/enabled/stageMask/reserved(6) = 40
struct SpecialDay {
  uint32_t id;                     // 自增 ID
  char     title[SD_TITLE_LEN];    // UTF-8；设备端只取 ASCII 部分
  uint32_t lastKey;                // 本次周期标识 = 下一次发生日的 dateKey
                                   //   （变化 = 进入新一轮，档位清零 → 明年能再触发）
  uint16_t year;                   // 起始年：生日=出生年，纪念日=起始年；0 = 不记年
  uint8_t  type;                   // SpecialDayType
  uint8_t  month;                  // 1..12
  uint8_t  day;                    // 1..31（2/29 允许）
  uint8_t  enabled;                // 0 / 1
  uint8_t  stageMask;              // 本周期内已触发的档位（SD_STAGE_*）
  uint8_t  reserved;               // 对齐 + 以后扩展
};

// ═══════════════════════════════════════════════════════════════════════════
//  提前提醒档位（需求第十三条：7天前 / 3天前 / 1天前 / 当天，各最多一次）
// ═══════════════════════════════════════════════════════════════════════════
#define SD_STAGE_D7     0x01
#define SD_STAGE_D3     0x02
#define SD_STAGE_D1     0x04
#define SD_STAGE_TODAY  0x08

#define SD_UPCOMING_DAYS   7    // ≤ 7 天 → UPCOMING
#define SD_PASSED_DAYS     7    // 过去 ≤ 7 天 → PASSED

// ═══════════════════════════════════════════════════════════════════════════
//  节拍与庆祝时长
// ═══════════════════════════════════════════════════════════════════════════
#define BDAY_TICK_MS        1000UL   // 日常判定：1 秒一拍
#define CELE_FRAME_MS        160UL   // 庆祝场景帧率（彩带下落、烛火闪烁）
#define CELE_FACE_MS        2200UL   // 第一阶段：表情（HAPPY + BOUNCE）
#define CELE_SCENE_MS       7300UL   // 第二阶段：场景（蛋糕 → 彩带 → 横幅）
#define CELE_TOTAL_MS      (CELE_FACE_MS + CELE_SCENE_MS)   // 总共 9.5 秒
#define CELE_SUB_CONF_MS    1500UL   // 场景内：多久后开始下彩带
#define CELE_SUB_BANNER_MS  3500UL   // 场景内：多久后出横幅文字

// 庆祝阶段
enum CelePhase : uint8_t {
  CELE_IDLE = 0,
  CELE_FACE,     // 表情阶段（复用 ExpressionManager）
  CELE_SCENE,    // 场景阶段（VIEW_BIRTHDAY）
  CELE_WRAPUP    // 收尾：恢复视图 + 交还情绪系统
};

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void birthdayBegin();     // setup() 中调用一次（含从 NVS 载入）
void birthdayTick();      // loop() 中每轮调用，非阻塞（内部 1 秒判定 + 160ms 场景帧）

// ═══════════════════════════════════════════════════════════════════════════
//  增 / 删 / 改
// ═══════════════════════════════════════════════════════════════════════════
// 返回新索引；-1 = 失败（满了 / 月日非法）
int  addSpecialDay(const char* title, uint8_t month, uint8_t day,
                   uint16_t year, SpecialDayType type);
int  addBirthday(const char* title, uint8_t month, uint8_t day, uint16_t birthYear);
int  addAnniversary(const char* title, uint8_t month, uint8_t day, uint16_t startYear);

bool removeSpecialDay(uint32_t id);
bool removeSpecialDayAt(int idx);
void clearAllSpecialDays();

bool updateSpecialDay(uint32_t id, const char* title, uint8_t month, uint8_t day,
                      uint16_t year, SpecialDayType type);
bool setSpecialDayEnabled(uint32_t id, bool on);

// ═══════════════════════════════════════════════════════════════════════════
//  查询
// ═══════════════════════════════════════════════════════════════════════════
int  getSpecialDayCount();
int  getEnabledSpecialDayCount();
int  findSpecialDayIndex(uint32_t id);
const SpecialDay* getSpecialDayAt(int idx);
const SpecialDay* getSpecialDayById(uint32_t id);

// ═══════════════════════════════════════════════════════════════════════════
//  日期计算（全部复用 schedule.cpp 的日历，本文件不做任何日历运算）
// ═══════════════════════════════════════════════════════════════════════════
int32_t  getNextOccurrence(int idx);      // 下一次发生日 00:00（本地 epoch）
uint16_t getOccurrenceYear(int idx);      // 下一次发生日的年份
int      getDaysUntilNext(int idx);       // 距下一次还有几天（0 = 今天；-1 = 无效）
int      getDaysSinceLast(int idx);       // 距上一次过去几天（≥0；-1 = 无效）
int      getAnniversaryYears(int idx);    // 第 N 年 / 年龄（0 = 不适用）

// ═══════════════════════════════════════════════════════════════════════════
//  今日判断（需求第七条）
// ═══════════════════════════════════════════════════════════════════════════
SpecialDayState getSpecialDayState(int idx);
bool isTodaySpecialDay();
bool isBirthdayToday();
bool isAnniversaryToday();
int  getTodaySpecialDayIndex();            // 今天过节的第一个，-1 = 没有
const SpecialDay* getTodaySpecialDay();

// 给 ProactiveInteractionManager 读的（需求第十五条）—— 本模块不调用主动互动
bool isSpecialDayToday();                  // = isTodaySpecialDay()
bool isSpecialDayUpcoming();               // 有任何一个进入 UPCOMING
int  getUpcomingSpecialDayIndex();         // 最近的一个 UPCOMING / TODAY

// 今天的年月日（未校时返回 false）—— 网页面板显示用
bool getTodayDate(int &y, int &mo, int &d);

int  getDaysUntilNextBirthday();           // 最近一个生日的天数；-1 = 没有
int  getDaysUntilNextAnniversary();

// ═══════════════════════════════════════════════════════════════════════════
//  庆祝
// ═══════════════════════════════════════════════════════════════════════════
void triggerCelebration(SpecialDayType type, const char* title, uint8_t years);
bool isCelebrating();
uint8_t getCelePhase();
void birthdayEnterView();
void birthdayRedraw();
void birthdayForceRedraw();
int  getBirthdayPage();
int  getBirthdayPageCount();
void birthdaySetPage(int p);
void birthdaySelectNext();

// ═══════════════════════════════════════════════════════════════════════════
//  开发测试（需求第二十二条）
// ═══════════════════════════════════════════════════════════════════════════
int  triggerBirthdayTest();                // 立刻播一次生日庆祝
int  triggerAnniversaryTest();             // 立刻播一次纪念日庆祝
int  triggerUpcomingTest(uint8_t daysBefore);  // 模拟「N 天前」的提醒
void clearSpecialDayTriggers();            // 清掉所有档位 + 周期标识（让提醒可以重放）
bool setTestDate(int y, int mo, int d);    // 覆盖「今天」（不影响真实时间系统）
void clearTestDate();
bool getTestDate(int &y, int &mo, int &d); // false = 没覆盖

// ═══════════════════════════════════════════════════════════════════════════
//  存储（Preferences / NVS，命名空间 "bday"，与日程的 "sched" 分开）
// ═══════════════════════════════════════════════════════════════════════════
bool     birthdaySave();
bool     birthdayLoad();
void     birthdayClearStorage();
uint32_t getBirthdaySaveCount();           // 调试读数：一共写过几次 NVS
bool     isBirthdayStorageReady();

// ═══════════════════════════════════════════════════════════════════════════
//  名字（供网页 JSON 与设备端屏幕）
// ═══════════════════════════════════════════════════════════════════════════
const char* sdTypeKey(SpecialDayType t);     // "birthday"
const char* sdTypeLabel(SpecialDayType t);   // "生日"
const char* sdTypeAscii(SpecialDayType t);   // "BDAY"（设备端 5×7 字体用）
const char* sdTypeGlyph(SpecialDayType t);   // 网页图标
const char* sdStateKey(SpecialDayState s);   // "today"
const char* sdStateLabel(SpecialDayState s); // "就是今天"
const char* sdStateAscii(SpecialDayState s); // "TODAY"
const char* celePhaseKey(CelePhase p);       // "scene"
