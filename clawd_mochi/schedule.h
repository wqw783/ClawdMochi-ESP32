/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  schedule.h — Clawd Mochi 日程 / 倒计时系统（第六阶段）
 *
 *  职责边界（和前面五个阶段一样，只管一件事）：
 *      ScheduleManager 只回答「距离我记的那件事还有多久？」
 *
 *  所以本模块：
 *    · 不直接操作 LCD 上的**表情** —— 提醒只通过 addEmotion() 这个公开 API
 *        轻推一下情绪，之后 EmotionManager → ExpressionManager 自己走
 *        （所以 Expression Lock 天然生效，本模块完全不知道锁的存在）
 *    · 不新建时间系统 —— 复用主固件的 epoch 走时（通过 localEpochNow）
 *    · 不控制 SleepManager —— 两个系统各自读时间，是兄弟不是父子（无循环依赖）
 *    · 不重新实现主动互动 —— 只**暴露** hasUpcomingEvent() 等查询接口，
 *        留给第四阶段的 ProactiveInteractionManager 在第七阶段调用
 *    · 不用 delay() —— 全部 millis() 驱动，非阻塞
 *    · 不用 String —— 固定长度 char 数组，零堆分配
 *    · 不高频写 Flash —— 只在增 / 删 / 改 / 启停 / 提醒位翻转时写 NVS
 *
 *  ⚠ 一个必须知道的事实：设备端 240×240 用的是 Adafruit_GFX 内置 5×7 点阵字体，
 *    **只能显示 ASCII**。中文标题在屏幕上会变成乱码，所以设备端显示的是
 *    「ASCII 化的标题」，没有 ASCII 内容时退回事件类型标签（[EXAM] 之类）。
 *    完整中文标题在网页面板里看。
 *
 *  资源：10 个事件 × 36 字节 = 360 字节 RAM（静态数组，无堆）
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "emotion.h"      // addEmotion ← 提醒唯一出口（公开 API，不碰内部变量）

// ═══════════════════════════════════════════════════════════════════════════
//  来自主固件 clawd_mochi.ino
// ═══════════════════════════════════════════════════════════════════════════
extern Adafruit_ST7789 tft;
extern uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
extern uint8_t  currentView;

// 主固件的「本地 epoch 秒」（网页校时 + millis() 走时 + 时区偏移），
// 与 epochToLocalDT() 内部用的是同一个量，**不是第二套时间系统**。
// 返回 false = 网页还没校时，此时倒计时一律显示「等待校时」。
bool localEpochNow(int32_t &out);

// 主固件已有的日历工具，直接复用，不在本模块重写
bool leapYear(int y);
int  daysInMonth(int y, int m);

// ═══════════════════════════════════════════════════════════════════════════
//  视图编号（第六阶段新增：设备端日程 / 倒计时视图）
//  0..9 已被前五个阶段占用，这里接 10
// ═══════════════════════════════════════════════════════════════════════════
#define VIEW_SCHEDULE 10

// ═══════════════════════════════════════════════════════════════════════════
//  事件类型 —— 6 种
//
//  ⚠ BIRTHDAY / ANNIVERSARY 本阶段**只做类型预留**：
//     它们能建、能存、能算倒计时、能在网页里显示，
//     但没有任何「生日专属」逻辑（不会自动重复、没有专属动画）。
//     那是第七阶段的事。
//
//  前缀用 SEV_ 而不是 EV_ —— emotion.h 已经占了 EV_HAPPINESS 那一组。
// ═══════════════════════════════════════════════════════════════════════════
enum EventType : uint8_t {
  SEV_GENERAL = 0,   // 通用
  SEV_EXAM,          // 考试
  SEV_COMPETITION,   // 比赛
  SEV_TASK,          // 任务 / 待办
  SEV_BIRTHDAY,      // 生日（第七阶段）
  SEV_ANNIVERSARY,   // 纪念日（第七阶段）
  SEV_TYPE_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  事件状态 —— 4 个
//
//      UPCOMING ──距离 ≤ warningSec──→ DUE_SOON ──到点──→ DUE ──确认/超时──→ COMPLETED
//
//  「距离 ≤ urgentSec」不做成独立状态，而是 DUE_SOON 上的一个 urgent 标志 ——
//  需求第十二条明说「不要为了复杂状态机过度设计」。
// ═══════════════════════════════════════════════════════════════════════════
enum EventState : uint8_t {
  SES_NONE = 0,      // 事件不存在 / 已禁用
  SES_UPCOMING,      // 还早
  SES_DUE_SOON,      // 快到了（≤ warningSec）
  SES_DUE,           // 到点了
  SES_COMPLETED,     // 已完成（一次性事件到此为止）
  SES_STATE_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  容量与结构
// ═══════════════════════════════════════════════════════════════════════════
#define SCH_TITLE_LEN   24    // UTF-8 字节数：8 个汉字，或 23 个 ASCII 字符
#define SCH_MAX_EVENTS  10    // 需求建议 5~10；10 个 × 36B = 360B RAM

//  内存布局（全部 POD，可直接整体 putBytes 进 NVS，无指针无虚表）：
//      id(4) + title(24) + targetEpoch(4) + type(1) + enabled(1) + flags(1) + reserved(1) = 36
struct ScheduleEvent {
  uint32_t id;                     // 自增 ID，永不复用
  char     title[SCH_TITLE_LEN];   // UTF-8，网页看中文，设备端只取 ASCII 部分
  int32_t  targetEpoch;            // 目标时刻，本地 epoch 秒（与 localEpochNow 同基准）
  uint8_t  type;                   // EventType
  uint8_t  enabled;                // 0 / 1
  uint8_t  flags;                  // SCH_FLAG_* 一次性提醒位（防重复 + 重启后不重放）
  uint8_t  reserved;               // 对齐 + 以后扩展
};

// flags 位：某个提醒档位是否已经触发过（存盘，所以重启不会重放）
#define SCH_FLAG_SOON     0x01
#define SCH_FLAG_URGENT   0x02
#define SCH_FLAG_DUE      0x04

// reserved 位：用户手动「标记完成」。用 reserved 而不是新加字段，
// 因为需求第十二条允许 DUE → COMPLETED 简化，但又说了「用户确认」这条路。
#define SCH_MARK_DONE     0x01

// ═══════════════════════════════════════════════════════════════════════════
//  阈值 —— 统一配置，**不要散落在多个地方**
//  （写成字面量而不是算式，方便和验证页 schedule_preview.html 的 CFG 逐项比对）
// ═══════════════════════════════════════════════════════════════════════════
#define SCH_WARN_SEC          86400UL   // 24 小时 → DUE_SOON
#define SCH_URGENT_SEC         3600UL   // 1 小时  → urgent（更高优先级）
#define SCH_DONE_AFTER_SEC    86400UL   // DUE 之后再过 24h 自动 COMPLETED

#define SCH_TICK_MS            1000UL   // 判定节拍：每秒一拍（需求第三十条用秒做基本单位）
#define SCH_ROTATE_MS          5000UL   // 设备端自动轮播间隔（主人选的方案）
#define SCH_LIST_ROWS             3     // 设备端列表每页行数

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void scheduleBegin();     // setup() 中调用一次（含从 NVS 载入）
void scheduleTick();      // loop() 中每轮调用，非阻塞，内部 1 秒一拍

// ═══════════════════════════════════════════════════════════════════════════
//  增 / 删 / 改
// ═══════════════════════════════════════════════════════════════════════════
// 返回新事件的索引；-1 = 失败（满了 / 标题空 / 时间无效）
int  addEvent(const char* title, int32_t targetEpoch, EventType type);
// 同上，但直接给年月日时分（内部用 daysFromCivil 转 epoch）
int  addEventYMD(const char* title, int y, int mo, int d, int h, int mi, EventType type);
// 「N 秒之后」——测试用，不用真的等
int  addEventIn(const char* title, int32_t inSeconds, EventType type);

bool removeEvent(uint32_t id);                 // 删除（后面前移，保持紧凑）
bool removeEventAt(int idx);
void clearAllEvents();                         // 清空全部 + 清 NVS

bool updateEvent(uint32_t id, const char* title, int32_t targetEpoch, EventType type);
bool updateEventTarget(uint32_t id, int32_t targetEpoch);
bool updateEventTargetYMD(uint32_t id, int y, int mo, int d, int h, int mi);

bool setEventEnabled(uint32_t id, bool on);
bool enableEvent(uint32_t id);
bool disableEvent(uint32_t id);

// 用户手动标记完成 / 撤销（需求第十二条的「用户确认」分支）
bool markEventDone(uint32_t id);
bool markEventUndone(uint32_t id);
bool isEventDone(uint32_t id);

// ═══════════════════════════════════════════════════════════════════════════
//  查询
// ═══════════════════════════════════════════════════════════════════════════
int  getEventCount();
int  getEnabledCount();
int  findEventIndex(uint32_t id);
const ScheduleEvent* getEventAt(int idx);
const ScheduleEvent* getEventById(uint32_t id);

// 「最近事件」：距离现在最近的、还没完成的、enabled 的事件
int  getNextEventIndex();
const ScheduleEvent* getNextEvent();

// 倒计时（秒）。正数 = 还剩；≤ 0 = 已经到点或过期。**永远不会返回负数错误**
int32_t getCountdown(uint32_t id);
int32_t getCountdownAt(int idx);

// 拆成 天/时/分/秒（只在 remaining > 0 时有意义）
void getCountdownParts(int32_t remaining, uint16_t &days, uint8_t &hours,
                       uint8_t &mins, uint8_t &secs);

// 状态查询
EventState getEventState(uint32_t id);
EventState getEventStateAt(int idx);
bool isEventDue(uint32_t id);          // SES_DUE
bool isEventDueSoon(uint32_t id);      // SES_DUE_SOON（含 urgent）
bool isEventUrgent(uint32_t id);       // DUE_SOON 且 ≤ 1 小时
bool isEventCompleted(uint32_t id);
bool isEventEnabled(uint32_t id);
bool isTimeReady();                    // 网页是否已校时

// ═══════════════════════════════════════════════════════════════════════════
//  给第四阶段主动互动预留的查询接口
//  ⚠ 本阶段**只留接口**，不实现任何主动提醒行为；ProactiveInteractionManager
//    以后可以直接调这几个函数，不需要改 ScheduleManager。
// ═══════════════════════════════════════════════════════════════════════════
bool hasUpcomingEvent();      // 有没有还没完成的事件
bool isAnyEventDueSoon();     // 有没有事件进入 DUE_SOON
bool isAnyEventUrgent();      // 有没有事件 ≤ 1 小时

// ═══════════════════════════════════════════════════════════════════════════
//  开发测试
// ═══════════════════════════════════════════════════════════════════════════
void scheduleSetTestTime(int32_t localEpoch);   // 覆盖「现在」（不影响真实时间系统）
void scheduleClearTestTime();
bool getScheduleTestTime(int32_t &out);         // false = 没有覆盖，用真实时间
int32_t getScheduleNow();                       // 当前参与计算的「现在」
void scheduleResetAlerts();                     // 清掉所有一次性提醒位，让提醒可以重放

// ═══════════════════════════════════════════════════════════════════════════
//  存储（Preferences / NVS）
// ═══════════════════════════════════════════════════════════════════════════
bool scheduleSave();          // 只在增删改 / 启停 / 提醒位翻转时调用
bool scheduleLoad();
void scheduleClearStorage();
uint32_t getScheduleSaveCount();   // 调试读数：一共写过几次 NVS（验证「不高频写」）
bool isScheduleStorageReady();

// ═══════════════════════════════════════════════════════════════════════════
//  设备端视图（240×240，自动轮播，零按键依赖）
// ═══════════════════════════════════════════════════════════════════════════
void scheduleEnterView();
void scheduleRedraw();
void scheduleForceRedraw();
void scheduleSetPage(int p);
int  getSchedulePage();
int  getSchedulePageCount();
void scheduleSetMode(uint8_t mode);   // 0 = 列表，1 = 倒计时详情
uint8_t getScheduleMode();
void scheduleSelectNext();            // 轮播一格
void scheduleSelectIndex(int idx);    // 详情页跳到第 idx 个事件

// ═══════════════════════════════════════════════════════════════════════════
//  名字 / 格式化（供网页 JSON 与设备端屏幕）
// ═══════════════════════════════════════════════════════════════════════════
const char* eventTypeKey(EventType t);     // "exam"
const char* eventTypeLabel(EventType t);   // "考试"
const char* eventTypeAscii(EventType t);   // "EXAM"（设备端 5×7 字体用）
const char* eventStateKey(EventState s);   // "due_soon"
const char* eventStateLabel(EventState s); // "快到了"
const char* eventStateAscii(EventState s); // "SOON"

// 把标题过滤成可打印 ASCII（跳过 UTF-8 多字节序列）；过滤后为空则返回 false
bool asciiTitle(const char* in, char* out, size_t outLen);

// 年月日时分 → 本地 epoch 秒（内部走 daysFromCivil，纯整数，闰年自动正确）
int32_t scheduleMakeEpoch(int y, int mo, int d, int h, int mi);

// 本地 epoch 秒 → 年月日时分（第七阶段新增）
// 生日 / 纪念日系统靠它拿「今天是几月几号」。
// 这样全项目的日历算法仍然只有 schedule.cpp 里那一份，没有第二个实现。
void scheduleYMDHM(int32_t localEpoch, int &y, int &mo, int &d, int &h, int &mi);

// 倒计时格式化（ASCII，设备端用）："12d 3h" / "3h 8m" / "30m 12s" / "DUE"
void formatCountdownAscii(int32_t remaining, char* buf, size_t n);
