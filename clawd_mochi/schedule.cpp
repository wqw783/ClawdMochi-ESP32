/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  schedule.cpp — Clawd Mochi 日程 / 倒计时系统（第六阶段）
 *
 *  一句话：这是一个「适合 240×240 ESP32-C3 桌宠的轻量级重要事情倒计时器」，
 *         不是手机日历 App。
 *
 *  ── 倒计时怎么算 ─────────────────────────────────────────────────────────
 *  全部走「本地 epoch 秒」这一个单位：
 *
 *      remaining = targetEpoch - localEpochNow()
 *
 *  因为 epoch 本身就是单调递增的秒数，所以
 *      · 跨天    23:50 → 00:10  = 1200 秒   ✓
 *      · 跨月    9/30 → 10/1                ✓
 *      · 跨年    2026-12-31 → 2027-01-01    ✓
 *      · 闰年    2028-02-28 → 2028-02-29    ✓
 *  全部自动正确，**一次月份天数 / 闰年判断都不需要做**。
 *  本文件里没有任何「这个月有几天」的减法逻辑。
 *
 *  反向（用户填年月日 → epoch）才需要 daysFromCivil()，是 Howard Hinnant 的
 *  纯整数算法。主固件的 epochToLocalDT() 用的是同一个算法的正向版本 ——
 *  这里独立实现一份，是为了**一行都不动主固件的时间系统**。
 *
 *  ── 提醒怎么触发 ─────────────────────────────────────────────────────────
 *  需求第十八条的红线：**不能每秒 +1 情绪**。
 *  做法：每个提醒档位在 ScheduleEvent.flags 里占一个 bit，
 *       跨过阈值的那一刻置位并推一次情绪，之后永远不再触发。
 *       flags 会一起存进 NVS，所以重启也不会重放。
 *
 *  ── 存储 ────────────────────────────────────────────────────────────────
 *  Preferences（NVS，ESP32 Arduino 核心自带，不需要装任何库）。
 *  只在 增 / 删 / 改 / 启停 / 提醒位翻转 时写，**查看倒计时绝不写 Flash**。
 *  写了多少次由 getScheduleSaveCount() 记录，方便验证这一点。
 *
 *  ── 设备端为什么只显示 ASCII ─────────────────────────────────────────────
 *  240×240 用的是 Adafruit_GFX 内置 5×7 点阵字体，只有 ASCII 字形。
 *  中文标题会变乱码，所以设备端显示 asciiTitle() 过滤后的结果；
 *  过滤完是空的（标题全是中文）就退回事件类型标签 [EXAM] / [BIRTHDAY]。
 *  完整中文标题在网页面板里看。
 *
 *  资源：静态数组 360B（10×36）+ 缓存 50B + 标量 ~40B ≈ 450B RAM，零堆分配。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#include "schedule.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 主固件的居中打印（定义在 clawd_mochi.ino，这里只声明不重写）
void printCentered(const char* s, uint8_t size, int16_t y, uint16_t col);

// ═══════════════════════════════════════════════════════════════════════════
//  常量
// ═══════════════════════════════════════════════════════════════════════════
#define SCH_W           240
#define SCH_H           240
#define SCH_NVS_NS      "sched"
#define SCH_STORE_VER   1

// 设备端版式
#define SCH_HDR_H        28
#define SCH_ROW_H        62
#define SCH_ROWS_Y       30
#define SCH_DOTS_Y      228

// ═══════════════════════════════════════════════════════════════════════════
//  静态状态（全部固定大小，无 String、无 new）
// ═══════════════════════════════════════════════════════════════════════════
static ScheduleEvent gEvents[SCH_MAX_EVENTS];
static int      gCount   = 0;
static uint32_t gNextId  = 1;

// 每秒重算一次的缓存，让查询函数 O(1)
static int32_t  gRemain[SCH_MAX_EVENTS];
static uint8_t  gStateCache[SCH_MAX_EVENTS];

// 存储
static Preferences gPrefs;
static bool        gStorageReady = false;
static uint32_t    gSaveCount    = 0;

// 时间
static int32_t gNowEpoch  = 0;
static bool    gTimeReady = false;
static int32_t gTestEpoch = 0;
static bool    gTestMode  = false;

// 节拍 / 设备端
static uint32_t gLastTickMs  = 0;
static uint32_t gRotateMs    = 0;
static uint8_t  gMode        = 0;      // 0 = 列表页，1 = 倒计时详情页
static uint8_t  gPage        = 0;
static uint8_t  gDetail      = 0;
static uint32_t gLastSig     = 0;
static uint8_t  gLastMode    = 0xFF;
static uint8_t  gLastPage    = 0xFF;
static uint8_t  gLastDetail  = 0xFF;

// ═══════════════════════════════════════════════════════════════════════════
//  日历：本地 epoch 秒 ⇄ 年月日时分
//
//  daysFromCivil / civilFromDays 是 Howard Hinnant 的 proleptic Gregorian
//  纯整数算法，主固件 epochToLocalDT() 里用的是同一套（正向版本）。
// ═══════════════════════════════════════════════════════════════════════════

// 1970-01-01 = 第 0 天
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int32_t  era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned mp  = (m > 2u) ? (m - 3u) : (m + 9u);   // 3 月当一年之首
  const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static void civilFromDays(int32_t z, int &y, int &mo, int &d) {
  z += 719468;
  const int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const int32_t  yy  = (int32_t)yoe + era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp  = (5u * doy + 2u) / 153u;
  d  = (int)(doy - (153u * mp + 2u) / 5u + 1u);
  mo = (int)(mp < 10u ? mp + 3u : mp - 9u);
  y  = (int)yy + (mo <= 2 ? 1 : 0);
}

static int32_t ymdhmToLocalEpoch(int y, int mo, int d, int h, int mi) {
  return daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400
       + (int32_t)h * 3600 + (int32_t)mi * 60;
}

static void localEpochToYMDHM(int32_t e, int &y, int &mo, int &d, int &h, int &mi) {
  int32_t days = e / 86400;
  int32_t rem  = e % 86400;
  if (rem < 0) { rem += 86400; days--; }
  civilFromDays(days, y, mo, d);
  h  = (int)(rem / 3600);
  mi = (int)((rem % 3600) / 60);
}

// ═══════════════════════════════════════════════════════════════════════════
//  存储（Preferences / NVS）
//
//  布局：
//      ver  UChar   结构版本（对不上就不读，避免固件升级后读到垃圾）
//      n    UChar   事件个数
//      nid  UInt    下一个 ID
//      e0..e9 Bytes 每个事件 36 字节整体存取（POD，无指针）
// ═══════════════════════════════════════════════════════════════════════════
static void nvsKey(int i, char* k, size_t n) {
  snprintf(k, n, "e%d", i);
}

bool scheduleSave() {
  if (!gStorageReady) return false;
  gPrefs.putUChar("ver", SCH_STORE_VER);
  gPrefs.putUChar("n", (uint8_t)gCount);
  gPrefs.putUInt("nid", gNextId);
  for (int i = 0; i < gCount; i++) {
    char k[6];
    nvsKey(i, k, sizeof k);
    gPrefs.putBytes(k, &gEvents[i], sizeof(ScheduleEvent));
  }
  gSaveCount++;
  return true;
}

bool scheduleLoad() {
  if (!gStorageReady) return false;
  gCount  = 0;
  gNextId = 1;
  if (gPrefs.getUChar("ver", 0) != SCH_STORE_VER) return false;

  uint8_t n = gPrefs.getUChar("n", 0);
  if (n > SCH_MAX_EVENTS) n = SCH_MAX_EVENTS;
  gNextId = gPrefs.getUInt("nid", 1);

  int loaded = 0;
  for (int i = 0; i < n; i++) {
    char k[6];
    nvsKey(i, k, sizeof k);
    ScheduleEvent tmp;
    const size_t got = gPrefs.getBytes(k, &tmp, sizeof(tmp));
    // 尺寸对不上说明结构变了，丢掉这一条而不是读进垃圾
    if (got != sizeof(ScheduleEvent)) continue;
    tmp.title[SCH_TITLE_LEN - 1] = 0;      // 兜底：保证以 0 结尾
    if (tmp.id == 0) tmp.id = gNextId++;
    if (tmp.id >= gNextId) gNextId = tmp.id + 1;
    gEvents[loaded++] = tmp;
  }
  gCount = loaded;
  return true;
}

void scheduleClearStorage() {
  if (!gStorageReady) return;
  gPrefs.clear();
  gSaveCount++;
}

bool isScheduleStorageReady() { return gStorageReady; }
uint32_t getScheduleSaveCount() { return gSaveCount; }

// ═══════════════════════════════════════════════════════════════════════════
//  状态计算
// ═══════════════════════════════════════════════════════════════════════════
static EventState computeState(const ScheduleEvent &ev, int32_t now, int32_t &remain) {
  remain = ev.targetEpoch - now;
  if (!ev.enabled) return SES_NONE;
  if (ev.reserved & SCH_MARK_DONE) return SES_COMPLETED;      // 用户手动确认完成
  if (remain <= 0) {
    // 到点后先挂 DUE 一段时间让用户看到，再自动 COMPLETED（需求第十二条允许简化）
    if (remain <= -(int32_t)SCH_DONE_AFTER_SEC) return SES_COMPLETED;
    return SES_DUE;
  }
  if ((uint32_t)remain <= SCH_WARN_SEC) return SES_DUE_SOON;
  return SES_UPCOMING;
}

// ═══════════════════════════════════════════════════════════════════════════
//  提醒 —— 需求第十八条的落实点
//
//  每个档位只触发一次。flags 存进 NVS，重启也不重放。
//  推情绪只走 addEmotion() 这个公开 API，**绝不碰 EmotionManager 内部变量**，
//  也**完全不碰屏幕表情** —— 所以 Expression Lock 天然生效。
//
//  ⚠ 这里**不直接写 Flash**，只把 gAlertDirty 立起来，
//    由 scheduleTick() 在一拍结束时统一写一次。
//    否则 10 个事件同一拍一起跨过 DUE_SOON 会写 120 次 NVS，把主循环卡住。
// ═══════════════════════════════════════════════════════════════════════════
static bool gAlertDirty = false;

static void evaluateAlerts(int idx, EventState st, int32_t remain) {
  if (st == SES_NONE || st == SES_COMPLETED) return;

  ScheduleEvent &ev = gEvents[idx];

  if (remain > 0 && (uint32_t)remain <= SCH_WARN_SEC) {
    if (!(ev.flags & SCH_FLAG_SOON)) {          // 跨进 24 小时 → 好奇 + 开心
      ev.flags |= SCH_FLAG_SOON;
      addEmotion(EV_CURIOSITY, 14);
      addEmotion(EV_HAPPINESS, 6);
      gAlertDirty = true;
    }
    if ((uint32_t)remain <= SCH_URGENT_SEC && !(ev.flags & SCH_FLAG_URGENT)) {
      ev.flags |= SCH_FLAG_URGENT;              // 跨进 1 小时 → 兴奋一点
      addEmotion(EV_HAPPINESS, 14);
      addEmotion(EV_CURIOSITY, 6);
      gAlertDirty = true;
    }
  }

  if (remain <= 0 && !(ev.flags & SCH_FLAG_DUE)) {
    ev.flags |= SCH_FLAG_DUE;                   // 到点 → 轻微一惊
    addEmotion(EV_STARTLE, 8);
    gAlertDirty = true;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void scheduleBegin() {
  gCount = 0;
  gNextId = 1;
  gNowEpoch = 0;
  gTimeReady = false;
  gTestMode = false;
  gTestEpoch = 0;
  gLastTickMs = millis();
  gRotateMs = gLastTickMs;
  gMode = 0; gPage = 0; gDetail = 0;
  gLastSig = 0;
  gLastMode = gLastPage = gLastDetail = 0xFF;
  for (int i = 0; i < SCH_MAX_EVENTS; i++) { gRemain[i] = 0; gStateCache[i] = SES_NONE; }

  gStorageReady = gPrefs.begin(SCH_NVS_NS, false);   // false = 读写模式
  if (gStorageReady) scheduleLoad();
  // 不在这里画任何东西：开机画面由主固件决定
}

void scheduleTick() {
  const uint32_t now = millis();
  if (now - gLastTickMs < SCH_TICK_MS) return;
  gLastTickMs = now;

  // ── 取「现在」：测试档优先，否则读主固件 ──
  if (gTestMode) {
    gNowEpoch  = gTestEpoch;
    gTimeReady = true;
  } else {
    gTimeReady = localEpochNow(gNowEpoch);
  }

  // ── 每秒重算状态 + 检查提醒 ──
  gAlertDirty = false;
  for (int i = 0; i < gCount; i++) {
    int32_t remain = 0;
    const EventState st = computeState(gEvents[i], gNowEpoch, remain);
    gRemain[i]     = remain;
    gStateCache[i] = (uint8_t)st;
    evaluateAlerts(i, st, remain);
  }
  // 一拍最多写一次 NVS（可能 10 个事件同一拍一起跨过 DUE_SOON）
  if (gAlertDirty) scheduleSave();

  // ── 设备端：只有在这个视图下才动屏幕 ──
  if (currentView == VIEW_SCHEDULE) {
    if (now - gRotateMs >= SCH_ROTATE_MS) {
      gRotateMs = now;
      scheduleSelectNext();      // 自动轮播（主人选的方案，零按键依赖）
    }
    scheduleRedraw();            // 内部有脏检查，不会每秒全屏重绘
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  增 / 删 / 改
// ═══════════════════════════════════════════════════════════════════════════
static void copyTitle(char* dst, const char* src) {
  size_t i = 0;
  if (src) {
    while (src[i] && i + 1 < SCH_TITLE_LEN) { dst[i] = src[i]; i++; }
  }
  dst[i] = 0;
  // 空标题给个默认名，避免屏幕上出现空白行
  if (i == 0) { copyTitle(dst, "EVENT"); }
}

int addEvent(const char* title, int32_t targetEpoch, EventType type) {
  if (gCount >= SCH_MAX_EVENTS) return -1;
  if (type >= SEV_TYPE_COUNT) type = SEV_GENERAL;

  ScheduleEvent &ev = gEvents[gCount];
  memset(&ev, 0, sizeof(ev));
  ev.id          = gNextId++;
  ev.targetEpoch = targetEpoch;
  ev.type        = (uint8_t)type;
  ev.enabled     = 1;
  ev.flags       = 0;
  ev.reserved    = 0;
  copyTitle(ev.title, title);

  const int idx = gCount++;
  gRemain[idx] = 0;
  gStateCache[idx] = SES_NONE;
  scheduleSave();
  return idx;
}

int addEventYMD(const char* title, int y, int mo, int d, int h, int mi, EventType type) {
  if (mo < 1 || mo > 12) return -1;
  const int dim = daysInMonth(y, mo);
  if (d < 1 || d > dim) return -1;
  if (h < 0 || h > 23 || mi < 0 || mi > 59) return -1;
  return addEvent(title, ymdhmToLocalEpoch(y, mo, d, h, mi), type);
}

int addEventIn(const char* title, int32_t inSeconds, EventType type) {
  if (!gTimeReady) return -1;
  return addEvent(title, gNowEpoch + inSeconds, type);
}

bool removeEventAt(int idx) {
  if (idx < 0 || idx >= gCount) return false;
  for (int i = idx; i + 1 < gCount; i++) gEvents[i] = gEvents[i + 1];
  gCount--;
  // 数组尾部那条也要从 NVS 里抹掉，否则下次 load 会把它读回来
  if (gStorageReady) {
    char k[6];
    nvsKey(gCount, k, sizeof k);
    gPrefs.remove(k);
  }
  scheduleSave();
  return true;
}

bool removeEvent(uint32_t id) {
  const int i = findEventIndex(id);
  if (i < 0) return false;
  return removeEventAt(i);
}

void clearAllEvents() {
  gCount = 0;
  for (int i = 0; i < SCH_MAX_EVENTS; i++) { gRemain[i] = 0; gStateCache[i] = SES_NONE; }
  scheduleClearStorage();
}

bool updateEvent(uint32_t id, const char* title, int32_t targetEpoch, EventType type) {
  const int i = findEventIndex(id);
  if (i < 0) return false;
  ScheduleEvent &ev = gEvents[i];
  if (title) copyTitle(ev.title, title);
  ev.targetEpoch = targetEpoch;
  if (type < SEV_TYPE_COUNT) ev.type = (uint8_t)type;
  // 目标时间变了 → 提醒位必须清掉，否则新的提醒永远不会触发
  ev.flags &= (uint8_t)~SCH_FLAG_DUE;
  ev.flags &= (uint8_t)~SCH_FLAG_SOON;
  ev.flags &= (uint8_t)~SCH_FLAG_URGENT;
  ev.reserved &= (uint8_t)~SCH_MARK_DONE;
  scheduleSave();
  return true;
}

bool updateEventTarget(uint32_t id, int32_t targetEpoch) {
  return updateEvent(id, NULL, targetEpoch, SEV_TYPE_COUNT);
}

bool updateEventTargetYMD(uint32_t id, int y, int mo, int d, int h, int mi) {
  if (mo < 1 || mo > 12) return false;
  if (d < 1 || d > daysInMonth(y, mo)) return false;
  if (h < 0 || h > 23 || mi < 0 || mi > 59) return false;
  return updateEventTarget(id, ymdhmToLocalEpoch(y, mo, d, h, mi));
}

bool setEventEnabled(uint32_t id, bool on) {
  const int i = findEventIndex(id);
  if (i < 0) return false;
  gEvents[i].enabled = on ? 1 : 0;
  if (on) {
    // 重新启用 = 重新开始提醒
    gEvents[i].flags &= (uint8_t)~SCH_FLAG_DUE;
    gEvents[i].flags &= (uint8_t)~SCH_FLAG_SOON;
    gEvents[i].flags &= (uint8_t)~SCH_FLAG_URGENT;
  }
  scheduleSave();
  return true;
}

bool enableEvent(uint32_t id)  { return setEventEnabled(id, true); }
bool disableEvent(uint32_t id) { return setEventEnabled(id, false); }

bool markEventDone(uint32_t id) {
  const int i = findEventIndex(id);
  if (i < 0) return false;
  gEvents[i].reserved |= SCH_MARK_DONE;
  scheduleSave();
  return true;
}

bool markEventUndone(uint32_t id) {
  const int i = findEventIndex(id);
  if (i < 0) return false;
  gEvents[i].reserved &= (uint8_t)~SCH_MARK_DONE;
  scheduleSave();
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  查询
// ═══════════════════════════════════════════════════════════════════════════
int getEventCount()  { return gCount; }

int getEnabledCount() {
  int n = 0;
  for (int i = 0; i < gCount; i++) if (gEvents[i].enabled) n++;
  return n;
}

int findEventIndex(uint32_t id) {
  for (int i = 0; i < gCount; i++) if (gEvents[i].id == id) return i;
  return -1;
}

const ScheduleEvent* getEventAt(int idx) {
  if (idx < 0 || idx >= gCount) return NULL;
  return &gEvents[idx];
}

const ScheduleEvent* getEventById(uint32_t id) {
  const int i = findEventIndex(id);
  return (i < 0) ? NULL : &gEvents[i];
}

int getNextEventIndex() {
  int best = -1;
  int32_t bestRemain = 0;
  for (int i = 0; i < gCount; i++) {
    if (!gEvents[i].enabled) continue;
    if (gStateCache[i] == SES_COMPLETED) continue;
    const int32_t r = gRemain[i];
    if (r <= 0) continue;                    // 已经到点的不算「最近」
    if (best < 0 || r < bestRemain) { best = i; bestRemain = r; }
  }
  return best;
}

const ScheduleEvent* getNextEvent() {
  const int i = getNextEventIndex();
  return (i < 0) ? NULL : &gEvents[i];
}

int32_t getCountdownAt(int idx) {
  if (idx < 0 || idx >= gCount) return 0;
  return gRemain[idx];
}

int32_t getCountdown(uint32_t id) {
  const int i = findEventIndex(id);
  return (i < 0) ? 0 : gRemain[i];
}

void getCountdownParts(int32_t remaining, uint16_t &days, uint8_t &hours,
                       uint8_t &mins, uint8_t &secs) {
  if (remaining < 0) remaining = 0;
  const uint32_t r = (uint32_t)remaining;
  days  = (uint16_t)(r / 86400UL);
  hours = (uint8_t)((r % 86400UL) / 3600UL);
  mins  = (uint8_t)((r % 3600UL) / 60UL);
  secs  = (uint8_t)(r % 60UL);
}

EventState getEventStateAt(int idx) {
  if (idx < 0 || idx >= gCount) return SES_NONE;
  return (EventState)gStateCache[idx];
}

EventState getEventState(uint32_t id) {
  const int i = findEventIndex(id);
  return (i < 0) ? SES_NONE : (EventState)gStateCache[i];
}

bool isEventDue(uint32_t id)       { return getEventState(id) == SES_DUE; }
bool isEventDueSoon(uint32_t id)   { return getEventState(id) == SES_DUE_SOON; }
bool isEventCompleted(uint32_t id) { return getEventState(id) == SES_COMPLETED; }

bool isEventUrgent(uint32_t id) {
  const int i = findEventIndex(id);
  if (i < 0) return false;
  if (gStateCache[i] != SES_DUE_SOON) return false;
  return ((uint32_t)gRemain[i] <= SCH_URGENT_SEC);
}

bool isEventEnabled(uint32_t id) {
  const int i = findEventIndex(id);
  return (i >= 0) && gEvents[i].enabled;
}

bool isEventDone(uint32_t id) {
  const int i = findEventIndex(id);
  return (i >= 0) && ((gEvents[i].reserved & SCH_MARK_DONE) != 0);
}

bool isTimeReady() { return gTimeReady; }

// ── 给第四阶段主动互动预留（本阶段只留接口）──
bool hasUpcomingEvent() {
  for (int i = 0; i < gCount; i++) {
    if (!gEvents[i].enabled) continue;
    if (gStateCache[i] == SES_COMPLETED) continue;
    if (gRemain[i] > 0) return true;
  }
  return false;
}

bool isAnyEventDueSoon() {
  for (int i = 0; i < gCount; i++)
    if (gStateCache[i] == SES_DUE_SOON) return true;
  return false;
}

bool isAnyEventUrgent() {
  for (int i = 0; i < gCount; i++) {
    if (gStateCache[i] != SES_DUE_SOON) continue;
    if ((uint32_t)gRemain[i] <= SCH_URGENT_SEC) return true;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  开发测试
// ═══════════════════════════════════════════════════════════════════════════
void scheduleSetTestTime(int32_t localEpoch) {
  gTestMode  = true;
  gTestEpoch = localEpoch;
}

void scheduleClearTestTime() { gTestMode = false; }

bool getScheduleTestTime(int32_t &out) {
  if (!gTestMode) return false;
  out = gTestEpoch;
  return true;
}

int32_t getScheduleNow() { return gNowEpoch; }

void scheduleResetAlerts() {
  for (int i = 0; i < gCount; i++) {
    gEvents[i].flags &= (uint8_t)~SCH_FLAG_SOON;
    gEvents[i].flags &= (uint8_t)~SCH_FLAG_URGENT;
    gEvents[i].flags &= (uint8_t)~SCH_FLAG_DUE;
  }
  scheduleSave();
}

// ═══════════════════════════════════════════════════════════════════════════
//  名字
// ═══════════════════════════════════════════════════════════════════════════
static const char* const TYPE_KEY[SEV_TYPE_COUNT]   = {
  "general", "exam", "competition", "task", "birthday", "anniversary"
};
static const char* const TYPE_LABEL[SEV_TYPE_COUNT] = {
  "通用", "考试", "比赛", "任务", "生日", "纪念日"
};
static const char* const TYPE_ASCII[SEV_TYPE_COUNT] = {
  "EVENT", "EXAM", "MATCH", "TASK", "BDAY", "ANNIV"
};
static const char* const TYPE_GLYPH[SEV_TYPE_COUNT] = {
  "*", "#", "!", "+", "@", "&"
};

const char* eventTypeKey(EventType t)   { return (t < SEV_TYPE_COUNT) ? TYPE_KEY[t] : "general"; }
const char* eventTypeLabel(EventType t) { return (t < SEV_TYPE_COUNT) ? TYPE_LABEL[t] : "通用"; }
const char* eventTypeAscii(EventType t) { return (t < SEV_TYPE_COUNT) ? TYPE_ASCII[t] : "EVENT"; }
const char* eventTypeGlyph(EventType t) { return (t < SEV_TYPE_COUNT) ? TYPE_GLYPH[t] : "*"; }

static const char* const STATE_KEY[SES_STATE_COUNT]   = {
  "none", "upcoming", "due_soon", "due", "completed"
};
static const char* const STATE_LABEL[SES_STATE_COUNT] = {
  "无", "还早", "快到了", "到点了", "已完成"
};
static const char* const STATE_ASCII[SES_STATE_COUNT] = {
  "-", "UPCOMING", "DUE SOON", "DUE", "DONE"
};

const char* eventStateKey(EventState s)   { return (s < SES_STATE_COUNT) ? STATE_KEY[s] : "none"; }
const char* eventStateLabel(EventState s) { return (s < SES_STATE_COUNT) ? STATE_LABEL[s] : "无"; }
const char* eventStateAscii(EventState s) { return (s < SES_STATE_COUNT) ? STATE_ASCII[s] : "-"; }

// ═══════════════════════════════════════════════════════════════════════════
//  格式化
// ═══════════════════════════════════════════════════════════════════════════

// 把 UTF-8 标题过滤成可打印 ASCII。全是中文时返回 false（调用方退回类型标签）
bool asciiTitle(const char* in, char* out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = 0;
  if (!in) return false;

  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < outLen; ) {
    const uint8_t c = (uint8_t)in[i];
    if (c < 0x80) {
      if (c >= 32 && c < 127) out[o++] = (char)c;
      i += 1;
    } else if ((c & 0xE0) == 0xC0) { i += 2; }
    else if ((c & 0xF0) == 0xE0)   { i += 3; }
    else if ((c & 0xF8) == 0xF0)   { i += 4; }
    else                            { i += 1; }
  }
  out[o] = 0;

  // 去掉被跳过字符留下的多余空格
  size_t s = 0;
  while (out[s] == ' ') s++;
  if (s) { size_t k = 0; while (out[s]) out[k++] = out[s++]; out[k] = 0; }
  size_t L = strlen(out);
  while (L > 0 && out[L - 1] == ' ') out[--L] = 0;
  return (L > 0);
}

int32_t scheduleMakeEpoch(int y, int mo, int d, int h, int mi) {
  return ymdhmToLocalEpoch(y, mo, d, h, mi);
}

// 第七阶段新增：把上面的反向转换暴露出去，给生日 / 纪念日系统复用。
// 只是一个转发，日历算法仍然只有本文件这一份。
void scheduleYMDHM(int32_t localEpoch, int &y, int &mo, int &d, int &h, int &mi) {
  localEpochToYMDHM(localEpoch, y, mo, d, h, mi);
}

// 需求第三十条：≥1天 → X天X小时；<1天 → X小时X分；<1小时 → X分X秒
void formatCountdownAscii(int32_t remaining, char* buf, size_t n) {
  if (!buf || n == 0) return;
  if (remaining <= 0) { snprintf(buf, n, "DUE"); return; }

  const uint32_t r = (uint32_t)remaining;
  if (r >= 86400UL) {
    snprintf(buf, n, "%lud %luh",
             (unsigned long)(r / 86400UL),
             (unsigned long)((r % 86400UL) / 3600UL));
  } else if (r >= 3600UL) {
    snprintf(buf, n, "%luh %lum",
             (unsigned long)(r / 3600UL),
             (unsigned long)((r % 3600UL) / 60UL));
  } else {
    snprintf(buf, n, "%lum %lus",
             (unsigned long)(r / 60UL),
             (unsigned long)(r % 60UL));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  设备端视图 —— 240×240，纯 ASCII，自动轮播
//
//  ⚠ 不做按键导航：项目只有 GPIO9 一个按键，而它已经被 InteractionManager
//    占用。为了不破坏第三阶段，这里用**自动轮播**代替翻页。
// ═══════════════════════════════════════════════════════════════════════════
int getSchedulePageCount() {
  if (gCount <= 0) return 1;
  return (gCount + SCH_LIST_ROWS - 1) / SCH_LIST_ROWS;
}

int getSchedulePage() { return gPage; }
uint8_t getScheduleMode() { return gMode; }

void scheduleSetPage(int p) {
  const int pc = getSchedulePageCount();
  if (p < 0) p = 0;
  if (p >= pc) p = pc - 1;
  gPage = (uint8_t)p;
  gMode = 0;
  gRotateMs = millis();
  scheduleForceRedraw();
}

void scheduleSetMode(uint8_t mode) {
  gMode = (mode == 0) ? 0 : 1;
  gRotateMs = millis();
  scheduleForceRedraw();
}

void scheduleSelectIndex(int idx) {
  if (idx < 0 || idx >= gCount) return;
  gDetail = (uint8_t)idx;
  gMode = 1;
  gRotateMs = millis();
  scheduleForceRedraw();
}

void scheduleSelectNext() {
  const int pc = getSchedulePageCount();
  if (gMode == 0) {
    if (gPage + 1 < pc) {
      gPage++;
    } else if (gCount > 0) {
      gMode = 1;
      gDetail = 0;
    } else {
      gPage = 0;
    }
  } else {
    if (gDetail + 1 < gCount) gDetail++;
    else { gMode = 0; gPage = 0; }
  }
}

// 右对齐打印
static void printRight(const char* s, int16_t xRight, int16_t y, uint8_t size, uint16_t col) {
  int len = 0;
  while (s[len]) len++;
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor(xRight - len * 6 * size, y);
  tft.print(s);
}

static uint16_t stateColour(EventState st, bool urgent) {
  if (urgent)                 return C_ORANGE;
  if (st == SES_DUE)          return C_ORANGE;
  if (st == SES_COMPLETED)    return C_MUTED;
  if (st == SES_DUE_SOON)     return C_GREEN;
  return C_WHITE;
}

static void drawHeader(const char* left, const char* right) {
  tft.fillRect(0, 0, SCH_W, 4, C_ORANGE);
  tft.fillRect(0, 4, SCH_W, SCH_HDR_H - 4, C_DARKBG);
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(8, 9); tft.print(left);
  if (right && right[0]) printRight(right, SCH_W - 8, 9, 2, C_MUTED);
  tft.drawFastHLine(0, SCH_HDR_H, SCH_W, C_GRID);
}

static void drawEmpty() {
  tft.fillScreen(C_DARKBG);
  drawHeader("SCH / ANN", "");
  printCentered("NO EVENTS", 2, 96, C_MUTED);
  printCentered("open 192.168.4.1", 1, 124, C_MUTED);
  printCentered("and add one", 1, 138, C_MUTED);
  tft.fillRect(0, SCH_H - 4, SCH_W, 4, C_ORANGE);
}

static void drawNoTime() {
  tft.fillScreen(C_DARKBG);
  drawHeader("SCH / ANN", "");
  printCentered("NO TIME SYNCED", 2, 96, C_ORANGE);
  printCentered("open the web page once", 1, 124, C_MUTED);
  tft.fillRect(0, SCH_H - 4, SCH_W, 4, C_ORANGE);
}

static void drawListPage() {
  tft.fillScreen(C_DARKBG);

  char pg[16];
  snprintf(pg, sizeof pg, "%d/%d", (int)gPage + 1, getSchedulePageCount());
  drawHeader("SCH / ANN", pg);

  const int first = (int)gPage * SCH_LIST_ROWS;
  for (int r = 0; r < SCH_LIST_ROWS; r++) {
    const int idx = first + r;
    const int16_t y = SCH_ROWS_Y + r * SCH_ROW_H;
    if (y + SCH_ROW_H > SCH_DOTS_Y - 2) break;

    if (idx >= gCount) {                       // 空位：画一条虚线槽
      tft.drawFastHLine(10, y + SCH_ROW_H / 2, SCH_W - 20, C_GRID);
      continue;
    }

    const ScheduleEvent &ev = gEvents[idx];
    const EventState st = (EventState)gStateCache[idx];
    const bool urgent = isEventUrgent(ev.id);

    // 类型标签
    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(10, y + 2);
    tft.print("[");
    tft.print(eventTypeAscii((EventType)ev.type));
    tft.print("]");

    // 标题（ASCII 化；全是中文就退回类型标签）
    char tbuf[26];
    if (!asciiTitle(ev.title, tbuf, sizeof tbuf)) {
      snprintf(tbuf, sizeof tbuf, "%s", eventTypeAscii((EventType)ev.type));
    }
    tft.setTextColor(ev.enabled ? C_WHITE : C_MUTED);
    tft.setCursor(10, y + 13);
    tft.print(tbuf);

    // 倒计时
    char cbuf[20];
    if (!gTimeReady)          snprintf(cbuf, sizeof cbuf, "--");
    else                      formatCountdownAscii(gRemain[idx], cbuf, sizeof cbuf);
    tft.setTextColor(stateColour(st, urgent), C_DARKBG);
    tft.setTextSize(2);
    tft.setCursor(10, y + 26);
    tft.print(cbuf);

    // 状态
    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(10, y + 46);
    if (!ev.enabled) tft.print("DISABLED");
    else             tft.print(eventStateAscii(st));

    tft.drawFastHLine(8, y + SCH_ROW_H - 4, SCH_W - 16, C_GRID);
  }

  // 页码点
  const int pc = getSchedulePageCount();
  if (pc > 1) {
    const int16_t dotW = 10;
    int16_t x = (SCH_W - pc * dotW) / 2;
    for (int i = 0; i < pc; i++) {
      if (i == (int)gPage) tft.fillCircle(x + 4, SCH_DOTS_Y + 4, 3, C_ORANGE);
      else                 tft.drawCircle(x + 4, SCH_DOTS_Y + 4, 3, C_GRID);
      x += dotW;
    }
  }
  tft.fillRect(0, SCH_H - 4, SCH_W, 4, C_ORANGE);
}

static void drawDetailPage() {
  tft.fillScreen(C_DARKBG);

  if (gDetail >= gCount) gDetail = 0;
  char pg[16];
  snprintf(pg, sizeof pg, "%d/%d", (int)gDetail + 1, gCount);
  drawHeader("COUNTDOWN", pg);

  if (gCount == 0) { drawEmpty(); return; }

  const ScheduleEvent &ev = gEvents[gDetail];
  const EventState st = (EventState)gStateCache[gDetail];
  const bool urgent = isEventUrgent(ev.id);
  const int32_t remain = gRemain[gDetail];

  // 类型
  char tb[24];
  snprintf(tb, sizeof tb, "[%s]", eventTypeAscii((EventType)ev.type));
  printCentered(tb, 2, 34, C_MUTED);

  // 标题
  char tbuf[26];
  if (!asciiTitle(ev.title, tbuf, sizeof tbuf)) {
    snprintf(tbuf, sizeof tbuf, "%s", eventTypeAscii((EventType)ev.type));
  }
  printCentered(tbuf, 1, 58, C_WHITE);

  if (!gTimeReady) {
    printCentered("NO TIME", 3, 100, C_ORANGE);
    tft.fillRect(0, SCH_H - 4, SCH_W, 4, C_ORANGE);
    return;
  }

  if (remain <= 0) {
    printCentered("DUE", 6, 90, C_ORANGE);
    printCentered(eventStateAscii(st), 2, 156, C_MUTED);
    int ty, tmo, td, th, tmi;
    localEpochToYMDHM(ev.targetEpoch, ty, tmo, td, th, tmi);
    char dtb[24];
    snprintf(dtb, sizeof dtb, "%04d-%02d-%02d %02d:%02d", ty, tmo, td, th, tmi);
    printCentered(dtb, 1, 190, C_MUTED);
    tft.fillRect(0, SCH_H - 4, SCH_W, 4, C_ORANGE);
    return;
  }

  uint16_t days; uint8_t hh, mm, ss;
  getCountdownParts(remain, days, hh, mm, ss);

  // 大号天数
  char db[12];
  snprintf(db, sizeof db, "%u", (unsigned)days);
  const int dl = (int)strlen(db);
  tft.setTextSize(6); tft.setTextColor(stateColour(st, urgent), C_DARKBG);
  tft.setCursor((SCH_W - dl * 36) / 2, 74);
  tft.print(db);
  printCentered("DAYS", 2, 128, C_MUTED);

  // HH:MM:SS
  char sb[16];
  snprintf(sb, sizeof sb, "%02u:%02u:%02u", (unsigned)hh, (unsigned)mm, (unsigned)ss);
  printCentered(sb, 3, 152, C_WHITE);

  printCentered(eventStateAscii(st), 2, 188, C_MUTED);

  // 目标时刻（本地时间，ASCII）
  int ty, tmo, td, th, tmi;
  localEpochToYMDHM(ev.targetEpoch, ty, tmo, td, th, tmi);
  char dtb[24];
  snprintf(dtb, sizeof dtb, "%04d-%02d-%02d %02d:%02d", ty, tmo, td, th, tmi);
  printCentered(dtb, 1, 206, C_MUTED);

  tft.fillRect(0, SCH_H - 4, SCH_W, 4, C_ORANGE);
}

// 把「将要画的东西」压成一个签名，只有签名变了才真的重绘。
// 列表页用粗粒度倒计时文本（几小时才变一次）；
// 详情页用原始秒数（每秒都变 → 每秒刷新，符合需求第十六条）。
static uint32_t computeSig() {
  uint32_t h = 2166136261u;
  #define SIGMIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
  SIGMIX(gMode); SIGMIX(gPage); SIGMIX(gDetail); SIGMIX(gCount); SIGMIX(gTimeReady);
  if (gMode == 0) {
    const int first = (int)gPage * SCH_LIST_ROWS;
    for (int r = 0; r < SCH_LIST_ROWS; r++) {
      const int idx = first + r;
      if (idx >= gCount) break;
      SIGMIX(gEvents[idx].id);
      SIGMIX(gEvents[idx].enabled);
      SIGMIX(gStateCache[idx]);
      char b[20];
      formatCountdownAscii(gRemain[idx], b, sizeof b);
      for (const char* p = b; *p; p++) SIGMIX((uint8_t)*p);
    }
  } else if (gDetail < gCount) {
    SIGMIX(gEvents[gDetail].id);
    SIGMIX((uint32_t)gRemain[gDetail]);
    SIGMIX(gStateCache[gDetail]);
  }
  #undef SIGMIX
  return h;
}

static void drawScreen() {
  if (gCount == 0)      { drawEmpty();  return; }
  if (!gTimeReady)      { drawNoTime(); return; }
  if (gMode == 0) drawListPage();
  else            drawDetailPage();
}

void scheduleRedraw() {
  if (currentView != VIEW_SCHEDULE) return;
  const uint32_t sig = computeSig();
  if (sig == gLastSig && gMode == gLastMode &&
      gPage == gLastPage && gDetail == gLastDetail) return;
  gLastSig = sig;
  gLastMode = gMode; gLastPage = gPage; gLastDetail = gDetail;
  drawScreen();
}

void scheduleForceRedraw() {
  if (currentView != VIEW_SCHEDULE) return;
  gLastSig = computeSig();
  gLastMode = gMode; gLastPage = gPage; gLastDetail = gDetail;
  drawScreen();
}

void scheduleEnterView() {
  currentView = VIEW_SCHEDULE;
  gMode = 0;
  gPage = 0;
  gDetail = 0;
  gRotateMs = millis();
  scheduleForceRedraw();
}
