/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  birthday.cpp — Clawd Mochi 生日 / 纪念日系统（第七阶段）
 *
 *  一句话：记住几个每年都会回来的日子，到了那天让桌宠开心一下。
 *
 *  ── 日期怎么算 ──────────────────────────────────────────────────────────
 *  本文件**没有任何日历运算**。全部走 schedule.cpp 那一份：
 *      scheduleYMDHM(epoch, y, mo, d, h, mi)   ← 今天几月几号
 *      scheduleMakeEpoch(y, mo, d, 0, 0)       ← 某天 00:00 的本地 epoch
 *  两个都是本地 epoch 的午夜，相减必然整除 86400 → 天数精确。
 *  **不做 365 天近似，闰年交给 daysFromCivil。**
 *
 *  2 月 29 日（主人拍板）：平年提前到 2 月 28 日庆祝。
 *      2027-02-28 → 当天触发（daysUntil = 0）
 *      2027-03-01 → 下一次是 2028-02-29（daysUntil = 365）
 *
 *  ── 同一天只触发一次，但明年要能再触发 ──────────────────────────────────
 *  每个 SpecialDay 存一个 lastKey = 「下一次发生日的 dateKey」。
 *  lastKey 一变 = 进入新一轮 → stageMask 清零 → 新的一轮 4 个档位可以重新响。
 *  重启后 lastKey / stageMask 从 NVS 读回来，所以**不会重复播**。
 *
 *  ── 庆祝怎么播 ──────────────────────────────────────────────────────────
 *      阶段 1  CELE_FACE   0 ~ 2.2s     requestExpression(HAPPY) + ANIM_BOUNCE
 *                                       ← 纯复用表情系统，本模块不画脸
 *      阶段 2  CELE_SCENE  2.2 ~ 9.5s   currentView = VIEW_BIRTHDAY
 *                                        蛋糕（0s）→ 彩带（+1.5s）→ 横幅（+3.5s）
 *      阶段 3  CELE_WRAPUP            恢复原视图 + emotionApply() 交还情绪系统
 *
 *  三个「不打扰用户」的闸门（都在 tryStartCelebration 里）：
 *      ① currentView 不是终端 / 画布 / 模块调试屏（表情视图除外，
 *         因为庆祝第一阶段就要在它上面播脸），也不是已经在庆祝的视图
 *      ② 不能是 SLEEPING / DROWSY          —— 不叫醒睡觉的桌宠
 *      ③ 不能是 Expression Lock            —— 挂起，解锁后再放
 *  条件不满足就一直挂着，每轮 loop 检查一次（都是廉价读取）。
 *
 *  资源：8 × 40B = 320B（事件数组）+ 缓存 ~80B + 标量 ~60B ≈ 460B RAM，零堆。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#include "birthday.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 主固件的居中打印（定义在 clawd_mochi.ino，这里只声明不重写）
void printCentered(const char* s, uint8_t size, int16_t y, uint16_t col);
// 整合新增：定义在 clawd_mochi.ino —— 用户是否正开着某个模块的调试屏
bool isOtherModuleDebugView();

// ═══════════════════════════════════════════════════════════════════════════
//  常量
// ═══════════════════════════════════════════════════════════════════════════
#define BD_W            240
#define BD_H            240
#define BDAY_NVS_NS     "bday"
#define BDAY_STORE_VER  1

#define BD_HDR_H         28
#define BD_ROW_H         62
#define BD_ROWS_Y        30
#define BD_DOTS_Y       228

#define CONF_N           10     // 彩带粒子数

// ═══════════════════════════════════════════════════════════════════════════
//  静态状态（全部固定大小，无 String、无 new）
// ═══════════════════════════════════════════════════════════════════════════
static SpecialDay gEvents[SD_MAX];
static int        gCount  = 0;
static uint32_t   gNextId = 1;

// 每天算一次的缓存，让查询函数 O(1)
static int     gDaysUntil[SD_MAX];
static int     gDaysSince[SD_MAX];
static uint8_t gStateCache[SD_MAX];

// 存储
static Preferences gPrefs;
static bool        gStorageReady = false;
static uint32_t    gSaveCount    = 0;

// 今天
static bool    gTodayValid    = false;
static int     gTodayY = 0, gTodayMo = 0, gTodayD = 0, gTodayH = 0, gTodayMi = 0;
static int32_t gTodayMidnight = 0;

// 测试日期（覆盖「今天」，不影响真实时间系统）
static bool gTestDateValid = false;
static int  gTestY = 2026, gTestMo = 1, gTestD = 1;

// 节拍
static uint32_t gLastDailyMs = 0;
static uint32_t gLastFrameMs = 0;
static bool     gDirty       = false;

// 庆祝
static CelePhase gCelePhase = CELE_IDLE;
static uint32_t  gCeleStart   = 0;
static uint32_t  gSceneStart  = 0;
static bool      gCelePending = false;
static uint8_t   gCeleType    = SDT_BIRTHDAY;
static char      gCeleTitle[SD_TITLE_LEN];
static uint8_t   gCeleYears   = 0;
static uint8_t   gSavedView   = 0;      // 庆祝前的视图，收尾时还回去

// 设备端列表分页
static uint8_t  gPage     = 0;
static uint32_t gRotateMs = 0;
static uint32_t gLastSig  = 0;

// 庆祝配色（birthdayBegin 里初始化）
static uint16_t BD_BODY, BD_PINK, BD_PLATE, BD_CANDLE1, BD_CANDLE2, BD_FLAME, BD_FLAME2;

// 前向声明：CRUD 里改了数据要立刻重算，但它的定义在下面
static void dailyEvaluate();

// ═══════════════════════════════════════════════════════════════════════════
//  存储（Preferences / NVS，命名空间 "bday"）
// ═══════════════════════════════════════════════════════════════════════════
static void nvsKey(int i, char* k, size_t n) { snprintf(k, n, "s%d", i); }

bool birthdaySave() {
  if (!gStorageReady) return false;
  gPrefs.putUChar("ver", BDAY_STORE_VER);
  gPrefs.putUChar("n", (uint8_t)gCount);
  gPrefs.putUInt("nid", gNextId);
  for (int i = 0; i < gCount; i++) {
    char k[6];
    nvsKey(i, k, sizeof k);
    gPrefs.putBytes(k, &gEvents[i], sizeof(SpecialDay));
  }
  gSaveCount++;
  return true;
}

bool birthdayLoad() {
  if (!gStorageReady) return false;
  gCount  = 0;
  gNextId = 1;
  if (gPrefs.getUChar("ver", 0) != BDAY_STORE_VER) return false;

  uint8_t n = gPrefs.getUChar("n", 0);
  if (n > SD_MAX) n = SD_MAX;
  gNextId = gPrefs.getUInt("nid", 1);

  int loaded = 0;
  for (int i = 0; i < n; i++) {
    char k[6];
    nvsKey(i, k, sizeof k);
    SpecialDay tmp;
    const size_t got = gPrefs.getBytes(k, &tmp, sizeof(tmp));
    if (got != sizeof(SpecialDay)) continue;   // 结构变了就丢掉，不读垃圾
    tmp.title[SD_TITLE_LEN - 1] = 0;
    if (tmp.id == 0) tmp.id = gNextId++;
    if (tmp.id >= gNextId) gNextId = tmp.id + 1;
    if (tmp.month < 1 || tmp.month > 12) continue;
    if (tmp.day   < 1 || tmp.day   > 31) continue;
    if (tmp.type >= SDT_TYPE_COUNT) tmp.type = SDT_SPECIAL;
    gEvents[loaded++] = tmp;
  }
  gCount = loaded;
  for (int i = 0; i < SD_MAX; i++) {
    gDaysUntil[i] = -1;
    gDaysSince[i] = -1;
    gStateCache[i] = SDS_NORMAL;
  }
  return true;
}

void birthdayClearStorage() {
  if (!gStorageReady) return;
  gPrefs.clear();
  gSaveCount++;
}

bool     isBirthdayStorageReady() { return gStorageReady; }
uint32_t getBirthdaySaveCount()   { return gSaveCount; }

// ═══════════════════════════════════════════════════════════════════════════
//  今天 / 发生日 —— 全部通过 schedule.cpp 的日历
// ═══════════════════════════════════════════════════════════════════════════
static uint32_t dateKeyOf(int32_t epoch) {
  int y, mo, d, h, mi;
  scheduleYMDHM(epoch, y, mo, d, h, mi);
  return (uint32_t)y * 10000u + (uint32_t)mo * 100u + (uint32_t)d;
}

static bool refreshToday() {
  int32_t nowEpoch;
  if (gTestDateValid) {
    nowEpoch = scheduleMakeEpoch(gTestY, gTestMo, gTestD, 12, 0);
  } else {
    if (!isTimeReady()) { gTodayValid = false; return false; }
    nowEpoch = getScheduleNow();
  }
  scheduleYMDHM(nowEpoch, gTodayY, gTodayMo, gTodayD, gTodayH, gTodayMi);
  gTodayMidnight = scheduleMakeEpoch(gTodayY, gTodayMo, gTodayD, 0, 0);
  gTodayValid = true;
  return true;
}

// 2/29 在平年提前到 2/28（主人拍板）
static int occDayInYear(const SpecialDay &sd, int y) {
  if (sd.month == 2 && sd.day == 29 && !leapYear(y)) return 28;
  return sd.day;
}

static int32_t nextOccurrenceEpoch(const SpecialDay &sd) {
  int y = gTodayY;
  int d = occDayInYear(sd, y);
  if (sd.month < gTodayMo || (sd.month == gTodayMo && d < gTodayD)) {
    y += 1;
    d = occDayInYear(sd, y);
  }
  return scheduleMakeEpoch(y, sd.month, d, 0, 0);
}

static int32_t prevOccurrenceEpoch(const SpecialDay &sd) {
  int y = gTodayY;
  int d = occDayInYear(sd, y);
  if (sd.month > gTodayMo || (sd.month == gTodayMo && d > gTodayD)) {
    y -= 1;
    d = occDayInYear(sd, y);
  }
  return scheduleMakeEpoch(y, sd.month, d, 0, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  增 / 删 / 改
// ═══════════════════════════════════════════════════════════════════════════
static void copyTitle(char* dst, const char* src) {
  size_t i = 0;
  if (src) { while (src[i] && i + 1 < SD_TITLE_LEN) { dst[i] = src[i]; i++; } }
  dst[i] = 0;
  if (i == 0) copyTitle(dst, "SPECIAL DAY");
}

// 用 2000（闰年）校验，所以 2/29 是合法输入
static bool validMonthDay(uint8_t month, uint8_t day) {
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > daysInMonth(2000, month)) return false;
  return true;
}

int addSpecialDay(const char* title, uint8_t month, uint8_t day,
                  uint16_t year, SpecialDayType type) {
  if (gCount >= SD_MAX) return -1;
  if (!validMonthDay(month, day)) return -1;
  if (type >= SDT_TYPE_COUNT) type = SDT_SPECIAL;

  SpecialDay &sd = gEvents[gCount];
  memset(&sd, 0, sizeof(sd));
  sd.id       = gNextId++;
  copyTitle(sd.title, title);
  sd.month    = month;
  sd.day      = day;
  sd.year     = year;
  sd.type     = (uint8_t)type;
  sd.enabled  = 1;
  sd.stageMask = 0;
  sd.lastKey   = 0;          // 0 → 首次判定认作新周期

  const int idx = gCount++;
  gDaysUntil[idx] = -1;
  gDaysSince[idx] = -1;
  gStateCache[idx] = SDS_NORMAL;

  birthdaySave();
  dailyEvaluate();
  return idx;
}

int addBirthday(const char* title, uint8_t month, uint8_t day, uint16_t birthYear) {
  return addSpecialDay(title, month, day, birthYear, SDT_BIRTHDAY);
}

int addAnniversary(const char* title, uint8_t month, uint8_t day, uint16_t startYear) {
  return addSpecialDay(title, month, day, startYear, SDT_ANNIVERSARY);
}

bool removeSpecialDayAt(int idx) {
  if (idx < 0 || idx >= gCount) return false;
  for (int i = idx; i + 1 < gCount; i++) {
    gEvents[i]      = gEvents[i + 1];
    gDaysUntil[i]   = gDaysUntil[i + 1];
    gDaysSince[i]   = gDaysSince[i + 1];
    gStateCache[i]  = gStateCache[i + 1];
  }
  gCount--;
  if (gStorageReady) {
    char k[6];
    nvsKey(gCount, k, sizeof k);
    gPrefs.remove(k);        // 尾部那条也要从 NVS 抹掉，否则下次 load 会读回来
  }
  birthdaySave();
  return true;
}

bool removeSpecialDay(uint32_t id) {
  const int i = findSpecialDayIndex(id);
  return (i < 0) ? false : removeSpecialDayAt(i);
}

void clearAllSpecialDays() {
  gCount = 0;
  for (int i = 0; i < SD_MAX; i++) {
    gDaysUntil[i] = -1;
    gDaysSince[i] = -1;
    gStateCache[i] = SDS_NORMAL;
  }
  birthdayClearStorage();
}

bool updateSpecialDay(uint32_t id, const char* title, uint8_t month, uint8_t day,
                      uint16_t year, SpecialDayType type) {
  const int i = findSpecialDayIndex(id);
  if (i < 0) return false;
  if (!validMonthDay(month, day)) return false;

  SpecialDay &sd = gEvents[i];
  if (title) copyTitle(sd.title, title);
  const bool dateChanged = (sd.month != month) || (sd.day != day);
  sd.month = month;
  sd.day   = day;
  sd.year  = year;
  if (type < SDT_TYPE_COUNT) sd.type = (uint8_t)type;
  // 日期变了 → 档位必须清零，否则新日期的提醒永远不会响
  if (dateChanged) { sd.stageMask = 0; sd.lastKey = 0; }

  birthdaySave();
  dailyEvaluate();
  return true;
}

bool setSpecialDayEnabled(uint32_t id, bool on) {
  const int i = findSpecialDayIndex(id);
  if (i < 0) return false;
  gEvents[i].enabled = on ? 1 : 0;
  if (on) { gEvents[i].stageMask = 0; gEvents[i].lastKey = 0; }   // 重新启用 = 重新开始提醒
  birthdaySave();
  dailyEvaluate();
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  查询
// ═══════════════════════════════════════════════════════════════════════════
int getSpecialDayCount() { return gCount; }

int getEnabledSpecialDayCount() {
  int n = 0;
  for (int i = 0; i < gCount; i++) if (gEvents[i].enabled) n++;
  return n;
}

int findSpecialDayIndex(uint32_t id) {
  for (int i = 0; i < gCount; i++) if (gEvents[i].id == id) return i;
  return -1;
}

const SpecialDay* getSpecialDayAt(int idx) {
  if (idx < 0 || idx >= gCount) return NULL;
  return &gEvents[idx];
}

const SpecialDay* getSpecialDayById(uint32_t id) {
  const int i = findSpecialDayIndex(id);
  return (i < 0) ? NULL : &gEvents[i];
}

int32_t getNextOccurrence(int idx) {
  if (idx < 0 || idx >= gCount || !gTodayValid) return 0;
  return nextOccurrenceEpoch(gEvents[idx]);
}

uint16_t getOccurrenceYear(int idx) {
  if (idx < 0 || idx >= gCount || !gTodayValid) return 0;
  int y, mo, d, h, mi;
  scheduleYMDHM(nextOccurrenceEpoch(gEvents[idx]), y, mo, d, h, mi);
  return (uint16_t)y;
}

int getDaysUntilNext(int idx) {
  if (idx < 0 || idx >= gCount) return -1;
  if (!gTodayValid || !gEvents[idx].enabled) return -1;
  return gDaysUntil[idx];
}

int getDaysSinceLast(int idx) {
  if (idx < 0 || idx >= gCount) return -1;
  if (!gTodayValid || !gEvents[idx].enabled) return -1;
  return gDaysSince[idx];
}

int getAnniversaryYears(int idx) {
  if (idx < 0 || idx >= gCount || !gTodayValid) return 0;
  const SpecialDay &sd = gEvents[idx];
  if (sd.year == 0) return 0;
  int y, mo, d, h, mi;
  scheduleYMDHM(nextOccurrenceEpoch(sd), y, mo, d, h, mi);
  const int n = y - (int)sd.year;
  return (n < 0) ? 0 : n;
}

// ═══════════════════════════════════════════════════════════════════════════
//  今日判断
// ═══════════════════════════════════════════════════════════════════════════
SpecialDayState getSpecialDayState(int idx) {
  if (idx < 0 || idx >= gCount) return SDS_NORMAL;
  if (!gTodayValid || !gEvents[idx].enabled) return SDS_NORMAL;
  return (SpecialDayState)gStateCache[idx];
}

int getTodaySpecialDayIndex() {
  for (int i = 0; i < gCount; i++) {
    if (!gEvents[i].enabled) continue;
    if (gStateCache[i] == SDS_TODAY) return i;
  }
  return -1;
}

const SpecialDay* getTodaySpecialDay() {
  const int i = getTodaySpecialDayIndex();
  return (i < 0) ? NULL : &gEvents[i];
}

bool isTodaySpecialDay() { return getTodaySpecialDayIndex() >= 0; }

static bool anyTypeToday(SpecialDayType t) {
  for (int i = 0; i < gCount; i++) {
    if (!gEvents[i].enabled) continue;
    if (gEvents[i].type != (uint8_t)t) continue;
    if (gStateCache[i] == SDS_TODAY) return true;
  }
  return false;
}

bool isBirthdayToday()    { return anyTypeToday(SDT_BIRTHDAY); }
bool isAnniversaryToday() { return anyTypeToday(SDT_ANNIVERSARY); }

bool isSpecialDayUpcoming() {
  for (int i = 0; i < gCount; i++) {
    if (!gEvents[i].enabled) continue;
    if (gStateCache[i] == SDS_TODAY || gStateCache[i] == SDS_UPCOMING) return true;
  }
  return false;
}

int getUpcomingSpecialDayIndex() {
  int best = -1, bestD = 0;
  for (int i = 0; i < gCount; i++) {
    if (!gEvents[i].enabled) continue;
    const int du = gDaysUntil[i];
    if (du < 0 || du > SD_UPCOMING_DAYS) continue;
    if (best < 0 || du < bestD) { best = i; bestD = du; }
  }
  return best;
}

bool getTodayDate(int &y, int &mo, int &d) {
  if (!gTodayValid) return false;
  y = gTodayY; mo = gTodayMo; d = gTodayD;
  return true;
}

static int minDaysOfType(SpecialDayType t) {
  int best = -1;
  for (int i = 0; i < gCount; i++) {
    if (!gEvents[i].enabled) continue;
    if (gEvents[i].type != (uint8_t)t) continue;
    const int du = gDaysUntil[i];
    if (du < 0) continue;
    if (best < 0 || du < best) best = du;
  }
  return best;
}

int getDaysUntilNextBirthday()    { return minDaysOfType(SDT_BIRTHDAY); }
int getDaysUntilNextAnniversary() { return minDaysOfType(SDT_ANNIVERSARY); }

// ═══════════════════════════════════════════════════════════════════════════
//  每天判定 + 档位触发
//
//  ⚠ 这里**不直接写 Flash**，只把 gDirty 立起来，由 dailyEvaluate() 末尾统一写一次。
//    否则多个事件同一拍一起跨过阈值会写很多次 NVS。
// ═══════════════════════════════════════════════════════════════════════════
static void onStageFire(int i, uint8_t stage) {
  const SpecialDay &sd = gEvents[i];

  if (stage == SD_STAGE_TODAY) {
    // 需求第十四条：只 addEmotion（临时情绪），不用 setEmotion 锁死；
    // 之后由 EmotionManager 自己衰减回去。
    switch ((SpecialDayType)sd.type) {
      case SDT_BIRTHDAY:
        addEmotion(EV_HAPPINESS, 40);
        addEmotion(EV_ENERGY,    15);
        break;
      case SDT_ANNIVERSARY:
        addEmotion(EV_HAPPINESS, 30);
        addEmotion(EV_SHYNESS,   15);
        break;
      default:
        addEmotion(EV_HAPPINESS, 25);
        addEmotion(EV_CURIOSITY, 10);
        break;
    }
    const int yrs = getAnniversaryYears(i);
    triggerCelebration((SpecialDayType)sd.type, sd.title,
                       (uint8_t)((yrs > 0 && yrs < 256) ? yrs : 0));
    return;
  }

  // 提前提醒：只轻推情绪，不播庆祝（需求第十三条：不要每天都疯狂提醒）
  if (stage == SD_STAGE_D1) {
    addEmotion(EV_HAPPINESS, 12);
    addEmotion(EV_CURIOSITY,  8);
  } else if (stage == SD_STAGE_D3) {
    addEmotion(EV_CURIOSITY, 12);
    addEmotion(EV_HAPPINESS,  8);
  } else {
    addEmotion(EV_CURIOSITY, 10);
    addEmotion(EV_HAPPINESS,  5);
  }
}

static void evaluateSpecialDay(int i) {
  SpecialDay &sd = gEvents[i];

  if (!sd.enabled) {
    gDaysUntil[i] = -1;
    gDaysSince[i] = -1;
    gStateCache[i] = SDS_NORMAL;
    return;
  }

  const int32_t occ = nextOccurrenceEpoch(sd);
  const int32_t prv = prevOccurrenceEpoch(sd);
  const int du = (int)((occ - gTodayMidnight) / 86400);   // 两个午夜相减，必然整除
  const int ds = (int)((gTodayMidnight - prv) / 86400);
  gDaysUntil[i] = du;
  gDaysSince[i] = ds;

  // 新周期 → 清档位（明年能再触发）
  const uint32_t key = dateKeyOf(occ);
  if (sd.lastKey != key) { sd.lastKey = key; sd.stageMask = 0; gDirty = true; }

  // 状态：TODAY > UPCOMING > PASSED > NORMAL
  if (du == 0)                      gStateCache[i] = SDS_TODAY;
  else if (du <= SD_UPCOMING_DAYS)  gStateCache[i] = SDS_UPCOMING;
  else if (ds <= SD_PASSED_DAYS)    gStateCache[i] = SDS_PASSED;
  else                              gStateCache[i] = SDS_NORMAL;

  // 档位：只挑最高优先级那一档
  uint8_t stage = 0;
  if (du == 0)                        stage = SD_STAGE_TODAY;
  else if (du <= 1)                   stage = SD_STAGE_D1;
  else if (du <= 3)                   stage = SD_STAGE_D3;
  else if (du <= SD_UPCOMING_DAYS)    stage = SD_STAGE_D7;

  if (stage && !(sd.stageMask & stage)) {
    // 把「本该更早响但错过了」的档位一并标记为已消费，
    // 否则设备停机一周后重启会在同一拍把 4 个档位一起爆出来。
    if (du <= 7) sd.stageMask |= SD_STAGE_D7;
    if (du <= 3) sd.stageMask |= SD_STAGE_D3;
    if (du <= 1) sd.stageMask |= SD_STAGE_D1;
    if (du == 0) sd.stageMask |= SD_STAGE_TODAY;
    gDirty = true;
    onStageFire(i, stage);
  }
}

static void dailyEvaluate() {
  if (!refreshToday()) return;
  for (int i = 0; i < gCount; i++) evaluateSpecialDay(i);
  if (gDirty) { birthdaySave(); gDirty = false; }   // 一拍最多写一次 NVS
}

// ═══════════════════════════════════════════════════════════════════════════
//  庆祝状态机
// ═══════════════════════════════════════════════════════════════════════════
static void tryStartCelebration(uint32_t now) {
  if (!gCelePending) return;
  if (gCelePhase != CELE_IDLE) return;

  // ① 用户在终端敲字 / 在画布上画的时候不打扰
  if (currentView == VIEW_CODE || currentView == VIEW_DRAW) return;
  // ② 已经在庆祝视图里了，不重复开
  if (currentView == VIEW_BIRTHDAY) return;
  // ②b 整合新增：用户正开着某个模块的调试屏时也让位（表情视图除外，见文件头①）
  if (isOtherModuleDebugView()) return;
  // ③ 不叫醒睡觉的桌宠
  if (isSleeping() || isDrowsy()) return;
  // ④ 尊重 Expression Lock：锁着就挂起，解锁后再放（需求第十七条）
  if (isExpressionLocked()) return;

  gCelePending = false;
  gCelePhase   = CELE_FACE;
  gCeleStart   = now;
  gLastFrameMs = now;
  gSavedView   = currentView;      // 记下原视图，收尾时还回去

  // 第一阶段：纯复用表情系统。automatic=true → 锁定天然生效（上面已判过）
  requestExpression(EXPR_HAPPY, true);
  setExpressionAnimation(ANIM_BOUNCE);
}

static void stepCelebration(uint32_t now) {
  const uint32_t el = now - gCeleStart;

  switch (gCelePhase) {
    case CELE_FACE:
      if (el >= CELE_FACE_MS) {
        gCelePhase  = CELE_SCENE;
        gSceneStart = now;
        currentView = VIEW_BIRTHDAY;     // 进庆祝场景
        birthdayForceRedraw();
      }
      break;

    case CELE_SCENE:
      // 用户中途切走了视图（比如点了网页上的视图按钮）→ 立刻让位，直接收尾。
      // 不加这个判断的话，场景会继续每 160ms 重绘，把用户的画面盖掉。
      if (currentView != VIEW_BIRTHDAY) {
        gCelePhase = CELE_WRAPUP;
        break;
      }
      if (el >= CELE_TOTAL_MS) {
        gCelePhase = CELE_WRAPUP;
      } else {
        birthdayRedraw();                // 内部按帧率 + 脏检查
      }
      break;

    case CELE_WRAPUP:
      // 只在用户没切走的情况下恢复 —— 尊重用户操作
      if (currentView == VIEW_BIRTHDAY) {
        currentView = gSavedView;
        redrawCurrentView();           // 用主固件同一份视图分派把画面还回去
      }
      emotionApply();                  // 交还情绪系统（内部自己判断是否被睡眠暂停）
      gCelePhase = CELE_IDLE;
      gCeleYears = 0;
      break;

    default:
      break;
  }
}

void triggerCelebration(SpecialDayType type, const char* title, uint8_t years) {
  gCeleType  = (uint8_t)type;
  copyTitle(gCeleTitle, title);
  gCeleYears = years;
  gCelePending = true;
  tryStartCelebration(millis());
}

bool    isCelebrating()  { return gCelePhase != CELE_IDLE; }
uint8_t getCelePhase()   { return (uint8_t)gCelePhase; }

// ═══════════════════════════════════════════════════════════════════════════
//  生命周期
// ═══════════════════════════════════════════════════════════════════════════
void birthdayBegin() {
  gCount = 0;
  gNextId = 1;
  gTodayValid = false;
  gTestDateValid = false;
  gCelePhase = CELE_IDLE;
  gCelePending = false;
  gCeleYears = 0;
  gCeleTitle[0] = 0;
  gCeleType = SDT_BIRTHDAY;
  gDirty = false;
  gPage = 0;
  gLastSig = 0;
  gLastDailyMs = millis();
  gLastFrameMs = gLastDailyMs;
  gRotateMs    = gLastDailyMs;
  for (int i = 0; i < SD_MAX; i++) {
    gDaysUntil[i] = -1;
    gDaysSince[i] = -1;
    gStateCache[i] = SDS_NORMAL;
  }

  // 庆祝配色
  BD_BODY    = tft.color565(255, 226, 180);
  BD_PINK    = tft.color565(255, 140, 160);
  BD_PLATE   = tft.color565( 96,  76,  64);
  BD_CANDLE1 = tft.color565(255, 255, 255);
  BD_CANDLE2 = tft.color565(120, 200, 255);
  BD_FLAME   = tft.color565(255, 180,  60);
  BD_FLAME2  = tft.color565(255, 245, 170);

  gStorageReady = gPrefs.begin(BDAY_NVS_NS, false);
  if (gStorageReady) birthdayLoad();
  // 不在这里画任何东西：开机画面由主固件决定
}

void birthdayTick() {
  const uint32_t now = millis();

  // 挂起中的庆祝：条件一满足就开始（每轮都查，都是廉价读取）
  if (gCelePending && gCelePhase == CELE_IDLE) tryStartCelebration(now);

  // 庆祝进行中：按自己的帧率推进，不参与下面的日常判定
  if (gCelePhase != CELE_IDLE) {
    if (now - gLastFrameMs >= CELE_FRAME_MS) {
      gLastFrameMs = now;
      stepCelebration(now);
    }
    return;
  }

  // 日常判定：1 秒一拍
  if (now - gLastDailyMs < BDAY_TICK_MS) return;
  gLastDailyMs = now;
  dailyEvaluate();

  // 设备端列表：1 秒一次刷新（内部还有脏检查），顺便自动轮播
  if (currentView == VIEW_BIRTHDAY) {
    if (now - gRotateMs >= SCH_ROTATE_MS) { gRotateMs = now; birthdaySelectNext(); }
    birthdayRedraw();
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  开发测试
// ═══════════════════════════════════════════════════════════════════════════
int triggerBirthdayTest() {
  triggerCelebration(SDT_BIRTHDAY, "TEST BIRTHDAY", 0);
  return 0;
}

int triggerAnniversaryTest() {
  triggerCelebration(SDT_ANNIVERSARY, "TEST ANNIV", 3);
  return 0;
}

int triggerUpcomingTest(uint8_t daysBefore) {
  if (daysBefore <= 1)      { addEmotion(EV_HAPPINESS, 12); addEmotion(EV_CURIOSITY,  8); }
  else if (daysBefore <= 3) { addEmotion(EV_CURIOSITY, 12); addEmotion(EV_HAPPINESS,  8); }
  else                      { addEmotion(EV_CURIOSITY, 10); addEmotion(EV_HAPPINESS,  5); }
  return 0;
}

void clearSpecialDayTriggers() {
  for (int i = 0; i < gCount; i++) {
    gEvents[i].stageMask = 0;
    gEvents[i].lastKey   = 0;
  }
  gDirty = false;
  birthdaySave();
  dailyEvaluate();
}

bool setTestDate(int y, int mo, int d) {
  if (y < 2000 || y > 2099) return false;
  if (mo < 1 || mo > 12) return false;
  if (d < 1 || d > daysInMonth(y, mo)) return false;
  gTestDateValid = true;
  gTestY = y; gTestMo = mo; gTestD = d;
  dailyEvaluate();
  return true;
}

void clearTestDate() {
  gTestDateValid = false;
  dailyEvaluate();
}

bool getTestDate(int &y, int &mo, int &d) {
  if (!gTestDateValid) return false;
  y = gTestY; mo = gTestMo; d = gTestD;
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  名字
// ═══════════════════════════════════════════════════════════════════════════
static const char* const SDT_KEY[SDT_TYPE_COUNT]   = { "birthday", "anniversary", "special" };
static const char* const SDT_LABEL[SDT_TYPE_COUNT] = { "生日", "纪念日", "特殊日期" };
static const char* const SDT_ASCII[SDT_TYPE_COUNT] = { "BDAY", "ANNIV", "SPECIAL" };
static const char* const SDT_GLYPH[SDT_TYPE_COUNT] = { "🎂", "❤️", "🎉" };

const char* sdTypeKey(SpecialDayType t)   { return (t < SDT_TYPE_COUNT) ? SDT_KEY[t]   : "special"; }
const char* sdTypeLabel(SpecialDayType t) { return (t < SDT_TYPE_COUNT) ? SDT_LABEL[t] : "特殊日期"; }
const char* sdTypeAscii(SpecialDayType t) { return (t < SDT_TYPE_COUNT) ? SDT_ASCII[t] : "SPECIAL"; }
const char* sdTypeGlyph(SpecialDayType t) { return (t < SDT_TYPE_COUNT) ? SDT_GLYPH[t] : "🎉"; }

static const char* const SDS_KEY[SDS_STATE_COUNT]   = { "normal", "upcoming", "today", "passed" };
static const char* const SDS_LABEL[SDS_STATE_COUNT] = { "还早", "快到了", "就是今天", "刚过去" };
static const char* const SDS_ASCII[SDS_STATE_COUNT] = { "NORMAL", "SOON", "TODAY", "PASSED" };

const char* sdStateKey(SpecialDayState s)   { return (s < SDS_STATE_COUNT) ? SDS_KEY[s]   : "normal"; }
const char* sdStateLabel(SpecialDayState s) { return (s < SDS_STATE_COUNT) ? SDS_LABEL[s] : "还早"; }
const char* sdStateAscii(SpecialDayState s) { return (s < SDS_STATE_COUNT) ? SDS_ASCII[s] : "NORMAL"; }

static const char* const CELE_KEY[4] = { "idle", "face", "scene", "wrapup" };
const char* celePhaseKey(CelePhase p) { return ((uint8_t)p < 4) ? CELE_KEY[p] : "idle"; }

// ═══════════════════════════════════════════════════════════════════════════
//  设备端视图 —— 240×240，纯 ASCII
//
//  两个模式：
//    庆祝进行中（CELE_SCENE）→ 庆祝场景（蛋糕 / 彩带 / 横幅）
//    其它                    → 特殊日期列表（每页 3 条，自动轮播）
// ═══════════════════════════════════════════════════════════════════════════
static void printRight(const char* s, int16_t xRight, int16_t y, uint8_t size, uint16_t col) {
  int len = 0;
  while (s[len]) len++;
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor(xRight - len * 6 * size, y);
  tft.print(s);
}

int getBirthdayPageCount() {
  if (gCount <= 0) return 1;
  return (gCount + SCH_LIST_ROWS - 1) / SCH_LIST_ROWS;
}

int  getBirthdayPage() { return gPage; }

void birthdaySetPage(int p) {
  const int pc = getBirthdayPageCount();
  if (p < 0) p = 0;
  if (p >= pc) p = pc - 1;
  gPage = (uint8_t)p;
  gRotateMs = millis();
  birthdayForceRedraw();
}

void birthdaySelectNext() {
  const int pc = getBirthdayPageCount();
  if (gPage + 1 < pc) gPage++;
  else                gPage = 0;
}

// ── 庆祝场景 ──────────────────────────────────────────────────────────────
static void drawCake(uint32_t now) {
  const int16_t cx = 120;

  // 盘子
  tft.fillRoundRect(cx - 74, 172, 148, 9, 4, BD_PLATE);

  // 下层蛋糕 + 糖霜
  tft.fillRect(cx - 60, 130, 120, 42, BD_BODY);
  tft.fillRect(cx - 60, 130, 120, 11, BD_PINK);

  // 上层蛋糕 + 糖霜
  tft.fillRect(cx - 42, 96, 84, 34, BD_BODY);
  tft.fillRect(cx - 42, 96, 84, 10, BD_PINK);

  // 蜡烛 ×3 + 火焰（每 160ms 闪一下）
  const int16_t  cxs[3]  = { (int16_t)(cx - 22), cx, (int16_t)(cx + 22) };
  const uint16_t ccol[3] = { BD_CANDLE1, BD_CANDLE2, BD_CANDLE1 };
  const bool flick = ((now / 160) & 1) == 0;
  for (int i = 0; i < 3; i++) {
    tft.fillRect(cxs[i] - 3, 62, 6, 34, ccol[i]);
    if (flick) {
      tft.fillCircle(cxs[i], 56, 5, BD_FLAME);
      tft.fillCircle(cxs[i], 53, 3, BD_FLAME2);
    } else {
      tft.fillRect(cxs[i] - 2, 50, 5, 12, BD_FLAME);
    }
  }

  // 装饰小圆点
  tft.fillCircle(cx - 34, 148, 3, BD_PINK);
  tft.fillCircle(cx,      148, 3, BD_PINK);
  tft.fillCircle(cx + 34, 148, 3, BD_PINK);
  tft.fillCircle(cx - 20, 116, 3, BD_PINK);
  tft.fillCircle(cx + 20, 116, 3, BD_PINK);
}

// ⚠ 元素值最大 214 > 127，必须用 uint8_t；写成 int8_t 会报 narrowing conversion
//   （首编译实测踩到）。
static const uint8_t CONF_X [CONF_N] = {  18,  46,  74, 102, 130, 158, 186, 214,  32, 200 };
static const uint8_t CONF_Y0[CONF_N] = {  30,  90, 150,  60, 120, 180,  45, 105, 210,  15 };
static const uint8_t CONF_C [CONF_N] = {   0,   1,   2,   1,   0,   2,   1,   0,   2,   1 };
static const uint8_t CONF_DY[CONF_N] = {   5,   7,   4,   6,   8,   5,   7,   4,   6,   8 };

static void drawConfetti(uint32_t now) {
  const uint32_t t = now / 45;
  for (int i = 0; i < CONF_N; i++) {
    const uint32_t y = ((uint32_t)CONF_Y0[i] +
                        ((t % 236u) * (uint32_t)CONF_DY[i]) % 236u) % 236u;
    const int16_t  x = (int16_t)(CONF_X[i] + (int16_t)(((t / 4u) + (uint32_t)i) % 7u) - 3);
    const uint16_t c = (CONF_C[i] == 0) ? C_ORANGE
                     : (CONF_C[i] == 1) ? BD_PINK : C_CREAM;
    tft.fillRect(x, (int16_t)y + 4, 4, 4, c);
  }
}

static void drawScene(uint32_t now) {
  const uint32_t el = now - gSceneStart;

  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, BD_W, 4, C_ORANGE);

  // 类型标签
  char tb[20];
  snprintf(tb, sizeof tb, "[%s]", sdTypeAscii((SpecialDayType)gCeleType));
  printCentered(tb, 1, 10, C_MUTED);

  // 名字（ASCII 化；全是中文就退回类型标签）
  char nb[26];
  if (!asciiTitle(gCeleTitle, nb, sizeof nb)) {
    snprintf(nb, sizeof nb, "%s", sdTypeAscii((SpecialDayType)gCeleType));
  }
  printCentered(nb, 2, 24, C_CREAM);

  // 蛋糕（一开始就在）
  drawCake(now);

  // 彩带：1.5 秒后开始下
  if (el >= CELE_SUB_CONF_MS) drawConfetti(now);

  // 横幅：3.5 秒后出现
  if (el >= CELE_SUB_BANNER_MS) {
    const char* banner = "HAPPY BIRTHDAY";
    if (gCeleType == SDT_ANNIVERSARY)   banner = "HAPPY ANNIVERSARY";
    else if (gCeleType == SDT_SPECIAL)  banner = "SPECIAL DAY";
    printCentered(banner, 2, 192, C_ORANGE);

    char sb[26];
    if (gCeleYears > 0) {
      if (gCeleType == SDT_BIRTHDAY) snprintf(sb, sizeof sb, "AGE %u",  (unsigned)gCeleYears);
      else                           snprintf(sb, sizeof sb, "YEAR %u", (unsigned)gCeleYears);
    } else {
      snprintf(sb, sizeof sb, "CONGRATS");
    }
    printCentered(sb, 1, 214, C_MUTED);
  }

  tft.fillRect(0, BD_H - 4, BD_W, 4, C_ORANGE);
}

// ── 特殊日期列表 ──────────────────────────────────────────────────────────
// 本次修改（问题 1/2）：本模块已与「日程 / 倒计时」合并为「日程与纪念日」。
// 桌宠屏幕只有 5x7 英文字库，显示不了中文，所以这里用 ASCII 标题
// （ANNIV / BDAY）与日程屏（SCH / ANN）呼应，网页端才是中文。
static void drawList() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, BD_W, 4, C_ORANGE);
  tft.fillRect(0, 4, BD_W, BD_HDR_H - 4, C_DARKBG);
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(8, 9); tft.print("ANNIV / BDAY");
  if (gCount > 0) {
    char pg[12];
    snprintf(pg, sizeof pg, "%d/%d", (int)gPage + 1, getBirthdayPageCount());
    printRight(pg, BD_W - 8, 9, 2, C_MUTED);
  }
  tft.drawFastHLine(0, BD_HDR_H, BD_W, C_GRID);

  if (gCount == 0) {
    printCentered("NO SPECIAL DAYS", 2, 96, C_MUTED);
    printCentered("open 192.168.4.1", 1, 124, C_MUTED);
    printCentered("and add one", 1, 138, C_MUTED);
    tft.fillRect(0, BD_H - 4, BD_W, 4, C_ORANGE);
    return;
  }
  if (!gTodayValid) {
    printCentered("NO TIME SYNCED", 2, 100, C_ORANGE);
    printCentered("open the web page once", 1, 128, C_MUTED);
    tft.fillRect(0, BD_H - 4, BD_W, 4, C_ORANGE);
    return;
  }

  const int first = (int)gPage * SCH_LIST_ROWS;
  for (int r = 0; r < SCH_LIST_ROWS; r++) {
    const int i = first + r;
    const int16_t y = BD_ROWS_Y + r * BD_ROW_H;
    if (y + BD_ROW_H > BD_DOTS_Y - 2) break;

    if (i >= gCount) {
      tft.drawFastHLine(10, y + BD_ROW_H / 2, BD_W - 20, C_GRID);
      continue;
    }

    const SpecialDay &sd = gEvents[i];
    const SpecialDayState st = (SpecialDayState)gStateCache[i];
    const int du = gDaysUntil[i];

    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(10, y + 2);
    tft.print("[");
    tft.print(sdTypeAscii((SpecialDayType)sd.type));
    tft.print("]");

    char tbuf[26];
    if (!asciiTitle(sd.title, tbuf, sizeof tbuf)) {
      snprintf(tbuf, sizeof tbuf, "%s", sdTypeAscii((SpecialDayType)sd.type));
    }
    tft.setTextColor(sd.enabled ? C_WHITE : C_MUTED);
    tft.setCursor(10, y + 13);
    tft.print(tbuf);

    char cb[16];
    if (!sd.enabled)            snprintf(cb, sizeof cb, "OFF");
    else if (du == 0)           snprintf(cb, sizeof cb, "TODAY!");
    else if (du > 0)            snprintf(cb, sizeof cb, "IN %dD", du);
    else                        snprintf(cb, sizeof cb, "--");

    uint16_t col = C_WHITE;
    if (!sd.enabled)              col = C_MUTED;
    else if (du == 0)             col = C_ORANGE;
    else if (du <= SD_UPCOMING_DAYS) col = C_GREEN;
    tft.setTextColor(col, C_DARKBG);
    tft.setTextSize(2);
    tft.setCursor(10, y + 26);
    tft.print(cb);

    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(10, y + 46);
    tft.print(sd.enabled ? sdStateAscii(st) : "DISABLED");

    const int yrs = getAnniversaryYears(i);
    if (yrs > 0) {
      char yb[18];
      snprintf(yb, sizeof yb, "  %s %d",
               (sd.type == SDT_BIRTHDAY) ? "AGE" : "YEAR", yrs);
      tft.print(yb);
    } else if (sd.year > 0) {
      char yb[18];
      snprintf(yb, sizeof yb, "  SINCE %u", (unsigned)sd.year);
      tft.print(yb);
    }

    tft.drawFastHLine(8, y + BD_ROW_H - 4, BD_W - 16, C_GRID);
  }

  // 页码点
  const int pc = getBirthdayPageCount();
  if (pc > 1) {
    int16_t x = (int16_t)((BD_W - pc * 10) / 2);
    for (int i = 0; i < pc; i++) {
      if (i == (int)gPage) tft.fillCircle(x + 4, BD_DOTS_Y + 4, 3, C_ORANGE);
      else                 tft.drawCircle(x + 4, BD_DOTS_Y + 4, 3, C_GRID);
      x += 10;
    }
  }
  tft.fillRect(0, BD_H - 4, BD_W, 4, C_ORANGE);
}

// 把「将要画的东西」压成签名，只有变了才真的重绘
static uint32_t listSig() {
  uint32_t h = 2166136261u;
  #define BSIG(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
  BSIG(gPage); BSIG(gCount); BSIG(gTodayValid);
  const int first = (int)gPage * SCH_LIST_ROWS;
  for (int r = 0; r < SCH_LIST_ROWS; r++) {
    const int i = first + r;
    if (i >= gCount) break;
    BSIG(gEvents[i].id);
    BSIG(gEvents[i].enabled);
    BSIG(gStateCache[i]);
    BSIG(gDaysUntil[i]);
    BSIG(getAnniversaryYears(i));
  }
  #undef BSIG
  return h;
}

void birthdayRedraw() {
  if (currentView != VIEW_BIRTHDAY) return;
  if (gCelePhase == CELE_SCENE) { drawScene(millis()); return; }   // 场景按帧率自己刷
  const uint32_t sig = listSig();
  if (sig == gLastSig) return;
  gLastSig = sig;
  drawList();
}

void birthdayForceRedraw() {
  if (currentView != VIEW_BIRTHDAY) return;
  if (gCelePhase == CELE_SCENE) { drawScene(millis()); return; }
  gLastSig = listSig();
  drawList();
}

void birthdayEnterView() {
  currentView = VIEW_BIRTHDAY;
  gPage = 0;
  gRotateMs = millis();
  birthdayForceRedraw();
}
