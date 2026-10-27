/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   CLAWD MOCHI — ESP32-C3 Super Mini + ST7789 1.54" 240×240
 *
 *   Wiring:
 *     SDA → GPIO 10  (hardware SPI MOSI)
 *     SCL → GPIO 8   (hardware SPI SCK)
 *     RST → GPIO 2
 *     DC  → GPIO 1
 *     CS  → GPIO 4
 *     BL  → GPIO 3
 *     VCC → 3V3
 *     GND → GND
 *
 *   WiFi: "ClaWD-Mochi"  pw: clawd1234  → http://192.168.4.1
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_wifi.h"         // 新增：esp_wifi_set_max_tx_power()（WiFi.h 已含，显式再包一层无副作用）
#include "esp_netif.h"        // 新增：AP 掉线后重新拉起 DHCP 需要 esp_netif_*

// ── 表情管理系统（第一阶段新增）───────────────────────────────
#include "expression.h"       // 枚举 / 接口 / 角色几何（EYE_* 现在定义在这里）
#include "expression_ui.h"    // 网页端「表情管理」界面片段

// ── 情绪系统（第二阶段新增）───────────────────────────────────
#include "emotion.h"          // EmotionManager：情绪值 / 衰减 / 判定 / 映射
#include "emotion_ui.h"       // 网页端「情绪调试」界面片段

// ── 用户互动系统（第三阶段新增）───────────────────────────────
#include "interaction.h"      // InteractionManager：按键 / 戳一下 → 事件 → 情绪
#include "interaction_ui.h"   // 网页端「互动调试」界面片段

// ── 随机主动互动系统（第四阶段新增）───────────────────────────
#include "proactive.h"        // ProactiveInteractionManager：桌宠自己偶尔动一下
#include "proactive_ui.h"     // 网页端「主动互动调试」界面片段

// ── 睡眠 / 唤醒系统（第五阶段新增）─────────────────────────────
#include "sleep.h"            // SleepManager：AWAKE / DROWSY / SLEEPING / WAKING
#include "sleep_ui.h"         // 网页端「睡眠调试」界面片段

#include "schedule.h"         // ScheduleManager：日程 / 倒计时（第六阶段）
#include "schedule_ui.h"      // 网页端「日程调试」界面片段

#include "birthday.h"         // BirthdayManager：生日 / 纪念日（第七阶段）
#include "birthday_ui.h"      // 网页端「特殊日期调试」界面片段

// ── 生日动画（本轮新增，VIEW_BDCAKE = 12）─────────────────────
// 和上面的 birthday.*（纪念日提醒 / 庆祝）是两件事：
// 这是主人点「生日动画」进来播放的 240×240 像素场景（蛋糕 / 蜡烛 /
// 玉玉兽 / 王飚飚 / 气球彩带 / 底部对话框），画面来自生日动画 index.html。
#include "bdcake.h"           // 设备屏：整屏场景渲染（离屏画布 + 20fps 非阻塞）
#include "bdcake_ui.h"        // 网页端：canvas 动画 + 许愿 / 吹蜡烛两个按钮

// ── Clock data type ───────────────────────────────────────────
// ⚠ 必须定义在所有函数之前：Arduino 会把自动生成的函数原型插到文件顶部，
//   结构体若写在使用它的函数旁边，原型里的 ClockDT 还没被声明，
//   会报 “'ClockDT' was not declared in this scope”（首编译实测踩到）。
struct ClockDT {
  int year, mon, mday, wday;  // wday: 0 = Sunday
  int hour, min, sec;
};

// ── Pins ──────────────────────────────────────────────────────
#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── WiFi ──────────────────────────────────────────────────────
const char* AP_SSID = "ClaWD-Mochi";
const char* AP_PASS = "clawd1234";
WebServer server(80);

// ═══════════════════════════════════════════════════════════════
//  WiFi 稳定性加固（本次修改新增）
//  问题：热点时有时无 / 连上就掉 / 192.168.4.1 打不开
//  对策：固定信道 + 限制连接数 + 明确不隐藏 SSID + 降发射功率
//        + 关闭省电 + 安卓联网探测重定向（防止手机自动切回流量）
// ═══════════════════════════════════════════════════════════════
#define AP_CHANNEL      1      // 固定信道 1：避免自动跳信道导致掉线（路由器在 1 就改成 6）
#define AP_MAX_STA      4      // 最大连接数：够用即可，避免连接数占满后拒绝新连接
#define AP_HIDDEN       false  // 不隐藏 SSID：确保手机能搜到

// 发射功率：两段式策略
#define WIFI_TX_POWER_BOOT  WIFI_POWER_19_5dBm   // 开机 15 秒内满功率 → 手机更容易搜到热点
#define WIFI_TX_POWER_RUN   WIFI_POWER_8_5dBm    // 稳定运行降到 8.5dBm → C3 Super Mini 供电更稳、不易掉线
#define WIFI_BOOT_BOOST_MS  15000UL              // 满功率持续时长

// 状态与计时
static bool     g_apReady      = false;   // softAP 是否成功启动
static int      g_lastStaNum   = -1;      // 上一次的已连接数量（-1 = 还没读过）
static uint32_t g_lastWifiCheck = 0;      // 上次健康检查时间
static uint32_t g_lastSerialHb  = 0;      // 上次串口心跳时间
static uint32_t g_netReqCount   = 0;      // HTTP 请求计数（配合 /net 诊断）

// ── 本次修改（问题 5）：断连原因区分 + 防抖自动恢复 ──────────────
// 目的：① 每秒都能查到「是 AP 挂了」还是「手机自己断开」；
//       ② AP 真的挂了自动重拉，但用「稳定时长 + 退避」避免频繁重启，
//          否则热点反复消失，手机反而更难搜到（正是问题 2 的成因）。
static uint32_t g_staCount   = 0;   // 当前在线的手机数量
static uint32_t g_lastDownMs = 0;   // 最近一次手机断开的时间（0 = 从未断开）
static uint32_t g_dropCount  = 0;   // 手机断开总次数
static uint32_t g_recCount   = 0;   // AP 自动恢复总次数
static uint32_t g_upSinceMs  = 0;   // 本段「AP 正常」开始时间（用于算稳定时长）
static bool     g_staSeen    = false; // 是否曾经有手机连上过

// ── 设置发射功率（带日志）────────────────────────────────────
static void wifiApplyTxPower(wifi_power_t p, const char* tag) {
  esp_err_t e = esp_wifi_set_max_tx_power((int8_t)p);
  if (e == ESP_OK) {
    Serial.printf("[WiFi] TX power = %d (0.25dBm/单位)  [%s]\n", (int)p, tag);
  } else {
    Serial.printf("[WiFi] 设置 TX power 失败: %s  [%s]\n", esp_err_to_name(e), tag);
  }
}

// ── WiFi 事件：记录手机连上 / 断开 ───────────────────────────
// 本次修改（问题 5）：断开事件单独打印「原因类别」，方便区分
//   · 手机主动断开（用户关了 WiFi / 走远了）→ 正常现象，AP 不需要重启
//   · AP 自己挂了 → 由 wifiNetTick() 的 IP 检查发现并自动恢复
//
// 【编译错误修复】原写法 `void onWifiNetEvent(arduino_event_info_t info)` +
// `switch (info.event_id)` 编译报错：'union arduino_event_info_t' has no member named 'event_id'。
// 原因：ESP32 核心 3.3.11 里这是两种不同的类型（见 libraries/Network/src/NetworkEvents.h）：
//   · union arduino_event_info_t —— 只有事件数据（wifi_ap_staconnected 等），没有事件 ID；
//   · struct arduino_event_t     —— 才是 { arduino_event_id_t event_id; arduino_event_info_t event_info; }。
// 双参数回调 NetworkEventFuncCb 的签名是：
//   std::function<void(arduino_event_id_t event, arduino_event_info_t info)>
// 事件 ID 是第一个参数，所以这里必须收 (event, info)，并用 event 做 switch。
// info 仍然保留在形参里（签名要求），日志用不到它所以不引用，避免 unused 警告。
static void onWifiNetEvent(arduino_event_id_t event, arduino_event_info_t info) {
  (void)info;   // 签名要求必须有第二个参数；本回调只用事件 ID 打日志，显式忽略
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[WiFi] AP 已启动");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      g_staSeen = true;
      Serial.println("[WiFi] 手机已连接 AP");
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.println("[WiFi] 已给手机分配 IP（网页可以打开了）");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      // 手机断开：这里只计数+打印。不重启热点 —— 重启会让 SSID 消失几十秒，
      // 手机会更难重新搜到，反而加重「连不上」的观感。
      g_lastDownMs = millis();
      g_dropCount++;
      Serial.printf("[WiFi] 手机断开 AP（第 %lu 次）—— 热点保持不动，等待自动重连\n",
                    (unsigned long)g_dropCount);
      break;
    default:
      break;
  }
}

// ── 手机联网探测拦截（安卓/Chrome 关键修复）──────────────────
// 安卓连上没有外网的热点时会去请求这几个地址做「连通性检查」；
// 检查失败（超时）就判定该 WiFi 不可用并自动切回移动数据 → 表现就是「一会儿连上一会儿断开」。
// 这里统一回 302 跳转到控制页，让安卓认为这是需要登录的门户网络，
// 从而保留连接并弹出「登录网络」提示，不再自动断开。
static const char* const CAPTIVE_URLS[] = {
  "/generate_204",     // Android 原生
  "/gen_204",          // Android / Chrome
  "/connectivitycheck",// Chrome
  "/mobile/status.php",
  "/hotspot-detect.html",   // Apple
  "/library/test/success.html",
  "/ncsi.txt",         // Windows
  "/canonical.html",
};
static const size_t CAPTIVE_URL_COUNT = sizeof(CAPTIVE_URLS) / sizeof(CAPTIVE_URLS[0]);

static void routeCaptive() {
  g_netReqCount++;
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

// ── 网络诊断页：/net 返回 JSON，方便浏览器/串口排查 ──────────
static void routeNet() {
  g_netReqCount++;
  IPAddress ip = WiFi.softAPIP();
  IPAddress gw;
  IPAddress mask;
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap) {
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ap, &info) == ESP_OK) {
      // 注意：esp_netif_ip_info_t 的这三个字段是结构体 esp_ip4_addr_t（内含 uint32_t addr），
      // IPAddress 没有对应构造函数，必须显式取 .addr，否则编译报错。
      gw   = IPAddress(info.gw.addr);
      mask = IPAddress(info.netmask.addr);
    }
  }
  char buf[400];
  snprintf(buf, sizeof(buf),
    "{\"ap\":\"%s\",\"ip\":\"%s\",\"gw\":\"%s\",\"mask\":\"%s\",\"ch\":%d,"
    "\"sta\":%d,\"tx\":%d,\"heap\":%u,\"up\":%lu,\"req\":%lu}",
    WiFi.softAPSSID().c_str(), ip.toString().c_str(), gw.toString().c_str(),
    mask.toString().c_str(), WiFi.channel(), (int)WiFi.softAPgetStationNum(),
    (int)WIFI_TX_POWER_RUN, (unsigned)ESP.getFreeHeap(),
    (unsigned long)(millis() / 1000), (unsigned long)g_netReqCount);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", buf);
}

// ── AP 启动 / 恢复 ───────────────────────────────────────────
static bool wifiStartAP(const char* tag) {
  WiFi.persistent(false);            // 不把 AP 配置写 NVS，避免旧配置残留影响启动
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);              // 关闭省电：桌面宠物长期供电，省电只会增加延迟和掉线
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, AP_HIDDEN, AP_MAX_STA);
  delay(120);                        // 等 AP 和 DHCP 服务就绪
  IPAddress ip = WiFi.softAPIP();
  g_apReady = ok && (ip != IPAddress((uint32_t)0));
  wifiApplyTxPower(WIFI_TX_POWER_BOOT, tag);
  // 本次修改（问题 5）：记录本段「AP 正常」的起点，并复位连接计数，
  // 这样恢复之后可以判断热点到底稳定了多久，避免刚起来又立刻重启。
  g_upSinceMs  = millis();
  g_lastStaNum = -1;

  Serial.printf("[WiFi] softAP %s  ssid=%s  ch=%d  max=%d  ip=%s\n",
                g_apReady ? "OK" : "FAILED",
                WiFi.softAPSSID().c_str(), WiFi.channel(), AP_MAX_STA,
                ip.toString().c_str());
  if (g_apReady) {
    Serial.printf("[WiFi] 打开浏览器访问 http://%s/\n", ip.toString().c_str());
  }
  return g_apReady;
}

// ── AP 异常时的恢复动作 ──────────────────────────────────────
// 本次修改（问题 5）：加恢复计数日志，并把「稳定了多久才出问题」一并打出来，
// 便于判断是电源/干扰导致反复挂，还是单次异常。
static void wifiRecoverAP() {
  g_recCount++;
  uint32_t upSec = (g_upSinceMs ? (millis() - g_upSinceMs) / 1000UL : 0);
  Serial.printf("[WiFi] 检测到 AP 异常（第 %lu 次恢复；本段正常时长 %lu 秒），重新初始化 …\n",
                (unsigned long)g_recCount, (unsigned long)upSec);
  WiFi.softAPdisconnect(true);       // 先关掉，避免半死状态
  delay(300);
  if (!wifiStartAP("recover")) {
    Serial.println("[WiFi] 恢复失败，2 秒后再试");
    delay(2000);
    WiFi.mode(WIFI_OFF);
    delay(200);
    if (!wifiStartAP("retry")) {
      Serial.println("[WiFi] AP 仍然失败（热点可能不可用）");
    }
  }
  // 重新拉起 AP 的 DHCP 服务端
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap) {
    esp_netif_dhcps_stop(ap);
    esp_netif_dhcps_start(ap);
    Serial.println("[WiFi] DHCP 服务已重新拉起");
  }
}

// ── 非阻塞健康检查，由 loop() 每 10 秒调用一次 ───────────────
// 本次修改（问题 5）：加入「稳定时长 + 退避」防抖。
//   规则：只有在「本次 AP 已稳定运行 ≥ 30 秒」时才允许自动重启热点。
//   这样既能在 AP 真挂时自愈，又不会因为偶发的一次异常就反复重启热点
//   （反复重启会让 SSID 反复消失，手机更难搜到 —— 见问题 2）。
static void wifiNetTick() {
  uint32_t now = millis();

  // 1) 延迟设置发射功率
  //    esp_wifi_set_max_tx_power() 必须在 WiFi 启动之后调用才生效，
  //    所以这里在 AP 起来后把「满功率」降回「稳定功率」。
  static bool txDropped = false;
  if (!txDropped && g_apReady && now > WIFI_BOOT_BOOST_MS) {
    wifiApplyTxPower(WIFI_TX_POWER_RUN, "run");
    txDropped = true;
  }

  // 2) 连接数跟踪：每到 2 秒采一次，方便快速打印断连/重连（比 10 秒更及时）
  static uint32_t lastStaMs = 0;
  if (now - lastStaMs >= 2000UL) {
    lastStaMs = now;
    int sta = (int)WiFi.softAPgetStationNum();
    if (sta != g_lastStaNum) {
      if (sta > g_lastStaNum && g_lastStaNum >= 0) {
        Serial.printf("[WiFi] 手机重新连上了，当前连接数 %d\n", sta);
      } else if (sta == 0 && g_lastStaNum > 0) {
        Serial.println("[WiFi] 手机已全部断开：热点仍在广播，等待自动重连（无需手动操作）");
      } else {
        Serial.printf("[WiFi] 连接数: %d\n", sta);
      }
      g_lastStaNum = sta;
    }
    g_staCount = (uint32_t)(sta > 0 ? sta : 0);
  }

  if (now - g_lastWifiCheck < 10000UL) return;   // 下面这些 10 秒一次就够
  g_lastWifiCheck = now;

  // 3) 健康检查：IP 是否还有效
  IPAddress ip = WiFi.softAPIP();
  bool apAlive = (ip != IPAddress((uint32_t)0));
  if (!apAlive || !g_apReady) {
    uint32_t upMs = (g_upSinceMs ? (now - g_upSinceMs) : 0xFFFFFFFFUL);
    if (upMs < 30000UL) {
      // 刚恢复过又出问题：只记日志，不立刻再重启，避免陷入重启循环
      Serial.printf("[WiFi] AP 仍异常，但距上次启动仅 %lu 秒，先不复位（避免频繁重启）\n",
                    (unsigned long)(upMs / 1000UL));
    } else {
      wifiRecoverAP();
    }
    return;
  }

  // 4) 每 30 秒一条心跳，方便看串口判断是否稳定
  if (now - g_lastSerialHb >= 30000UL) {
    g_lastSerialHb = now;
    Serial.printf("[WiFi] 心跳 ip=%s sta=%lu 断开%lu次 恢复%lu次 heap=%u req=%lu\n",
                  ip.toString().c_str(), (unsigned long)g_staCount,
                  (unsigned long)g_dropCount, (unsigned long)g_recCount,
                  (unsigned)ESP.getFreeHeap(), (unsigned long)g_netReqCount);
  }
}

// ── 统一初始化：setup() 里只调这一个 ─────────────────────────
static void wifiNetBegin() {
  Serial.println();
  Serial.println("[WiFi] 正在创建热点 ClaWD-Mochi …");
  // 事件回调仅用于打印日志；个别核心版本可能没有该事件枚举，故加条件编译保护
  // 【编译错误修复】必须用双参数重载：onWifiNetEvent 的签名是
  //   (arduino_event_id_t, arduino_event_info_t) → 匹配 NetworkEventFuncCb。
  // 第二个实参 ARDUINO_EVENT_MAX = 订阅全部事件（WiFi.onEvent 的默认值也是它，
  // 这里显式写出，避免和单参数重载 NetworkEventCb 混淆而再次选错重载）。
#if defined(ARDUINO_EVENT_WIFI_AP_STACONNECTED)
  WiFi.onEvent(onWifiNetEvent, ARDUINO_EVENT_MAX);
  Serial.println("[WiFi] 事件日志已挂载");
#endif
  wifiStartAP("boot");

  // 手机联网探测统一重定向（防止安卓自动切回移动数据）
  for (size_t i = 0; i < CAPTIVE_URL_COUNT; i++) {
    server.on(CAPTIVE_URLS[i], HTTP_GET, routeCaptive);
  }
  Serial.println("[WiFi] 已启用手机联网探测重定向（防掉线）");
}

// ── Display ───────────────────────────────────────────────────
#define DISP_W 240
#define DISP_H 240

// ── Eye constants ─────────────────────────────────────────────
// EYE_W / EYE_H / EYE_GAP / EYE_OX / EYE_OY 已移到 expression.h，
// 数值完全不变，作为「角色几何」的唯一出处，表情系统共用同一套坐标。

// ── Colours ───────────────────────────────────────────────────
uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
uint16_t C_CLKBG, C_CREAM, C_CLKLINE, C_CLKDEEP;   // warm palette (clock view)
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

// ── State ─────────────────────────────────────────────────────
#define VIEW_EYES_NORMAL 0
#define VIEW_EYES_SQUISH 1
#define VIEW_CODE        2
#define VIEW_DRAW        3
#define VIEW_CLOCK       4   // clock + calendar screen (web key 'c')
// 视图 12 = 生日动画。值写在这里是为了和 0..11 的对照表集中可查，
// 真正的定义在 bdcake.h（各模块用 #ifndef 兜底，避免重复定义）。
// #define VIEW_BDCAKE   12  ← 见 bdcake.h

uint8_t  currentView  = VIEW_EYES_NORMAL;
bool     busy         = false;
bool     backlightOn  = true;
// 动画速度总阀钮（本次修改）
// 语义改成「慢速倍率」：1 = 当前最快的速度，数值越大动画越慢（1..10）。
// 由网页「// 速度」滑块经 /speed?v=N 设定，默认 4（比原来慢 4 倍）。
// 这个倍率会同时作用于：
//   · 表情模块里的抖动/晃动/跳动/呼吸（见 expression.cpp 的 animPeriod()）
//   · 官方「正常眼睛 / 开心挤眼」的逐帧延迟（见 speedMs()）
// 这样无论表情是表情库、情绪、主动互动还是睡眠驱动的，节奏都一致变慢。
uint8_t  animSpeed    = 10;  // 慢速倍率 1..10（越大越慢），默认 10 = 最慢

uint16_t animBgColor  = 0;   // background for eye/logo animations
uint16_t drawBgColor  = 0;   // background for canvas

// ── Clock state (synced from the web page, kept with millis()) ──
bool     timeSynced   = false;  // false until the page sends /time
long     bootEpochSec = 0;      // UTC epoch seconds at last sync
long     tzOffsetSec  = 0;      // seconds east of UTC (browser offset)
uint32_t bootMs       = 0;      // millis() at the moment of sync

// ── Terminal ──────────────────────────────────────────────────
#define TERM_COLS      15
#define TERM_ROWS       8
#define TERM_CHAR_W    12
#define TERM_CHAR_H    20
#define TERM_PAD_X      8
#define TERM_PAD_Y     18

bool    termMode    = false;
String  termLines[TERM_ROWS];
uint8_t termRow     = 0;
uint8_t termCol     = 0;

// ── Logo data ─────────────────────────────────────────────────
#define LOGO_CX 120
#define LOGO_CY 105

#define LOGO_TRI_COUNT 162
static const int16_t LOGO_TRIS[][6] PROGMEM = {
  {120,105,65,134,100,114},{120,105,100,114,101,113},{120,105,101,113,100,112},
  {120,105,100,112,99,112},{120,105,99,112,93,111},{120,105,93,111,73,111},
  {120,105,73,111,55,110},{120,105,55,110,38,109},{120,105,38,109,34,108},
  {120,105,34,108,30,103},{120,105,30,103,30,100},{120,105,30,100,34,98},
  {120,105,34,98,39,98},{120,105,39,98,50,99},{120,105,50,99,67,100},
  {120,105,67,100,80,101},{120,105,80,101,98,103},{120,105,98,103,101,103},
  {120,105,101,103,101,102},{120,105,101,102,100,101},{120,105,100,101,100,100},
  {120,105,100,100,82,88},{120,105,82,88,63,76},{120,105,63,76,53,69},
  {120,105,53,69,48,65},{120,105,48,65,45,61},{120,105,45,61,44,54},
  {120,105,44,54,49,49},{120,105,49,49,55,49},{120,105,55,49,57,49},
  {120,105,57,49,64,55},{120,105,64,55,78,66},{120,105,78,66,96,79},
  {120,105,96,79,99,81},{120,105,99,81,100,81},{120,105,100,81,100,80},
  {120,105,100,80,99,78},{120,105,99,78,89,60},{120,105,89,60,78,41},
  {120,105,78,41,73,34},{120,105,73,34,72,29},{120,105,72,29,72,28},
  {120,105,72,28,72,27},{120,105,72,27,71,26},{120,105,71,26,71,25},
  {120,105,71,25,71,24},{120,105,71,24,77,16},{120,105,77,16,80,15},
  {120,105,80,15,87,16},{120,105,87,16,91,19},{120,105,91,19,95,29},
  {120,105,95,29,103,46},{120,105,103,46,114,68},{120,105,114,68,118,75},
  {120,105,118,75,119,81},{120,105,119,81,120,83},{120,105,120,83,121,83},
  {120,105,121,83,121,82},{120,105,121,82,122,69},{120,105,122,69,124,54},
  {120,105,124,54,126,34},{120,105,126,34,126,28},{120,105,126,28,129,21},
  {120,105,129,21,135,18},{120,105,135,18,139,20},{120,105,139,20,143,25},
  {120,105,143,25,142,28},{120,105,142,28,140,42},{120,105,140,42,136,64},
  {120,105,136,64,133,78},{120,105,133,78,135,78},{120,105,135,78,136,76},
  {120,105,136,76,144,67},{120,105,144,67,156,51},{120,105,156,51,162,45},
  {120,105,162,45,168,38},{120,105,168,38,172,35},{120,105,172,35,180,35},
  {120,105,180,35,185,43},{120,105,185,43,183,52},{120,105,183,52,175,62},
  {120,105,175,62,168,71},{120,105,168,71,159,83},{120,105,159,83,153,94},
  {120,105,153,94,154,94},{120,105,154,94,155,94},{120,105,155,94,176,90},
  {120,105,176,90,188,88},{120,105,188,88,201,85},{120,105,201,85,208,88},
  {120,105,208,88,208,91},{120,105,208,91,206,97},{120,105,206,97,191,101},
  {120,105,191,101,174,104},{120,105,174,104,148,110},{120,105,148,110,148,111},
  {120,105,148,111,148,111},{120,105,148,111,160,112},{120,105,160,112,165,112},
  {120,105,165,112,177,112},{120,105,177,112,200,114},{120,105,200,114,205,118},
  {120,105,205,118,209,123},{120,105,209,123,208,126},{120,105,208,126,199,131},
  {120,105,199,131,187,128},{120,105,187,128,159,121},{120,105,159,121,149,119},
  {120,105,149,119,147,119},{120,105,147,119,147,120},{120,105,147,120,156,128},
  {120,105,156,128,170,141},{120,105,170,141,189,158},{120,105,189,158,190,163},
  {120,105,190,163,188,166},{120,105,188,166,185,166},{120,105,185,166,169,153},
  {120,105,169,153,162,148},{120,105,162,148,148,136},{120,105,148,136,147,136},
  {120,105,147,136,147,137},{120,105,147,137,150,142},{120,105,150,142,168,168},
  {120,105,168,168,169,176},{120,105,169,176,168,179},{120,105,168,179,163,180},
  {120,105,163,180,158,179},{120,105,158,179,148,165},{120,105,148,165,137,149},
  {120,105,137,149,129,134},{120,105,129,134,128,135},{120,105,128,135,123,189},
  {120,105,123,189,120,192},{120,105,120,192,115,194},{120,105,115,194,110,191},
  {120,105,110,191,108,185},{120,105,108,185,110,174},{120,105,110,174,113,160},
  {120,105,113,160,116,148},{120,105,116,148,118,134},{120,105,118,134,119,129},
  {120,105,119,129,119,129},{120,105,119,129,118,129},{120,105,118,129,107,144},
  {120,105,107,144,91,166},{120,105,91,166,78,180},{120,105,78,180,75,181},
  {120,105,75,181,70,178},{120,105,70,178,70,173},{120,105,70,173,73,169},
  {120,105,73,169,91,146},{120,105,91,146,102,132},{120,105,102,132,109,124},
  {120,105,109,124,109,123},{120,105,109,123,108,123},{120,105,108,123,61,153},
  {120,105,61,153,52,155},{120,105,52,155,49,151},{120,105,49,151,49,146},
  {120,105,49,146,51,144},{120,105,51,144,65,134},{120,105,65,134,65,134},
};

#define LOGO_SEG_COUNT 162
static const int16_t LOGO_SEGS[][4] PROGMEM = {
  {65,134,100,114},{100,114,101,113},{101,113,100,112},{100,112,99,112},
  {99,112,93,111},{93,111,73,111},{73,111,55,110},{55,110,38,109},
  {38,109,34,108},{34,108,30,103},{30,103,30,100},{30,100,34,98},
  {34,98,39,98},{39,98,50,99},{50,99,67,100},{67,100,80,101},
  {80,101,98,103},{98,103,101,103},{101,103,101,102},{101,102,100,101},
  {100,101,100,100},{100,100,82,88},{82,88,63,76},{63,76,53,69},
  {53,69,48,65},{48,65,45,61},{45,61,44,54},{44,54,49,49},
  {49,49,55,49},{55,49,57,49},{57,49,64,55},{64,55,78,66},
  {78,66,96,79},{96,79,99,81},{99,81,100,81},{100,81,100,80},
  {100,80,99,78},{99,78,89,60},{89,60,78,41},{78,41,73,34},
  {73,34,72,29},{72,29,72,28},{72,28,72,27},{72,27,71,26},
  {71,26,71,25},{71,25,71,24},{71,24,77,16},{77,16,80,15},
  {80,15,87,16},{87,16,91,19},{91,19,95,29},{95,29,103,46},
  {103,46,114,68},{114,68,118,75},{118,75,119,81},{119,81,120,83},
  {120,83,121,83},{121,83,121,82},{121,82,122,69},{122,69,124,54},
  {124,54,126,34},{126,34,126,28},{126,28,129,21},{129,21,135,18},
  {135,18,139,20},{139,20,143,25},{143,25,142,28},{142,28,140,42},
  {140,42,136,64},{136,64,133,78},{133,78,135,78},{135,78,136,76},
  {136,76,144,67},{144,67,156,51},{156,51,162,45},{162,45,168,38},
  {168,38,172,35},{172,35,180,35},{180,35,185,43},{185,43,183,52},
  {183,52,175,62},{175,62,168,71},{168,71,159,83},{159,83,153,94},
  {153,94,154,94},{154,94,155,94},{155,94,176,90},{176,90,188,88},
  {188,88,201,85},{201,85,208,88},{208,88,208,91},{208,91,206,97},
  {206,97,191,101},{191,101,174,104},{174,104,148,110},{148,110,148,111},
  {148,111,148,111},{148,111,160,112},{160,112,165,112},{165,112,177,112},
  {177,112,200,114},{200,114,205,118},{205,118,209,123},{209,123,208,126},
  {208,126,199,131},{199,131,187,128},{187,128,159,121},{159,121,149,119},
  {149,119,147,119},{147,119,147,120},{147,120,156,128},{156,128,170,141},
  {170,141,189,158},{189,158,190,163},{190,163,188,166},{188,166,185,166},
  {185,166,169,153},{169,153,162,148},{162,148,148,136},{148,136,147,136},
  {147,136,147,137},{147,137,150,142},{150,142,168,168},{168,168,169,176},
  {169,176,168,179},{168,179,163,180},{163,180,158,179},{158,179,148,165},
  {148,165,137,149},{137,149,129,134},{129,134,128,135},{128,135,123,189},
  {123,189,120,192},{120,192,115,194},{115,194,110,191},{110,191,108,185},
  {108,185,110,174},{110,174,113,160},{113,160,116,148},{116,148,118,134},
  {118,134,119,129},{119,129,119,129},{119,129,118,129},{118,129,107,144},
  {107,144,91,166},{91,166,78,180},{78,180,75,181},{75,181,70,178},
  {70,178,70,173},{70,173,73,169},{73,169,91,146},{91,146,102,132},
  {102,132,109,124},{109,124,109,123},{109,123,108,123},{108,123,61,153},
  {61,153,52,155},{52,155,49,151},{49,151,49,146},{49,146,51,144},
  {51,144,65,134},{65,134,65,134},
};

// ═════════════════════════════════════════════════════════════
//  HELPERS
// ═════════════════════════════════════════════════════════════

// 逐帧延迟：完全跟随速度阀钮（本次重写）
// 上一版为了不让 delay() 卡住主循环，把倍率封顶在 2 倍 —— 结果滑杆拉到 2 以上
// 完全没反应，用户直接判定"阀钮没用"。现在官方两只眼睛动画已改成
// 非阻塞步进（见 eyesAnimTick()），所以这里可以放心地 1..10 全量放大。
int speedMs(int ms) {
  uint8_t f = animSpeed ? animSpeed : 1;
  uint32_t v = (uint32_t)ms * (uint32_t)f;
  if (v > 20000UL) v = 20000UL;      // 二重保险（极端档位也不至于无限长）
  return (int)v;
}

uint16_t hexToRgb565(String hex) {
  hex.replace("#", "");
  if (hex.length() != 6) return C_WHITE;
  long v = strtol(hex.c_str(), nullptr, 16);
  return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(TFT_BLK, on ? HIGH : LOW);
}

void initColours() {
  // C_ORANGE = tft.color565(170, 72, 28);
  C_ORANGE = tft.color565(218, 17, 0);
  C_DARKBG = tft.color565(10,  12,  16);
  C_MUTED  = tft.color565(90,  88,  86);
  C_GREEN  = tft.color565(80, 220, 130);
  C_GRID   = tft.color565(58,  62,  68);
  // Warm "Clawd" palette used only by the clock/calendar view
  C_CLKBG   = tft.color565(215, 119, 87);   // rgb(215,119,87) — Clawd orange bg
  C_CREAM   = tft.color565(255, 241, 226);  // cream "belly" text
  C_CLKLINE = tft.color565(196,  96,  58);  // grid lines, deeper terracotta
  C_CLKDEEP = tft.color565(84,   32,  14);  // today highlight, warm umber
  animBgColor = C_ORANGE;
  drawBgColor = C_ORANGE;
}

// ═════════════════════════════════════════════════════════════
//  LOGO
// ═════════════════════════════════════════════════════════════

void drawLogoFilled(uint16_t bg, uint16_t fg) {
  tft.fillScreen(bg);
  for (uint16_t i = 0; i < LOGO_TRI_COUNT; i++) {
    tft.fillTriangle(
      pgm_read_word(&LOGO_TRIS[i][0]), pgm_read_word(&LOGO_TRIS[i][1]),
      pgm_read_word(&LOGO_TRIS[i][2]), pgm_read_word(&LOGO_TRIS[i][3]),
      pgm_read_word(&LOGO_TRIS[i][4]), pgm_read_word(&LOGO_TRIS[i][5]),
      fg);
  }
  tft.setTextColor(fg); tft.setTextSize(2);
  tft.setCursor(LOGO_CX - 54, 210); tft.print("Anthropic");
  tft.setCursor(LOGO_CX - 53, 210); tft.print("Anthropic");
}

// ═════════════════════════════════════════════════════════════
//  VIEWS
// ═════════════════════════════════════════════════════════════

// Eye helpers — shared constants via #define EYE_*
inline int16_t eyeLX(int16_t ox) {
  return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
inline int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

void drawNormalEyes(int16_t ox = 0, bool blink = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  if (!blink) {
    tft.fillRect(lx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx, ey, EYE_W, EYE_H, C_BLACK);
  } else {
    tft.fillRect(lx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
    tft.fillRect(rx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
  }
}

void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      tft.drawLine(cx - reach/2, cy - arm + t, cx + reach/2, cy + t,      col);
      tft.drawLine(cx + reach/2, cy + t,       cx - reach/2, cy + arm + t, col);
    } else {
      tft.drawLine(cx + reach/2, cy - arm + t, cx - reach/2, cy + t,      col);
      tft.drawLine(cx - reach/2, cy + t,       cx + reach/2, cy + arm + t, col);
    }
  }
}

void drawSquishEyes(bool closed = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm   = EYE_H / 2;
  const int16_t reach = EYE_W / 2;
  const int16_t lcx   = lx + EYE_W / 2;
  const int16_t rcx   = rx + EYE_W / 2;
  if (!closed) {
    drawChevron(lcx, cy, arm, reach, 10, true,  C_BLACK);
    drawChevron(rcx, cy, arm, reach, 10, false, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);
    tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
  }
}

void drawCodeView() {
  termMode = false;
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,          DISP_W, 4, C_ORANGE);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_ORANGE); tft.setTextSize(4);
  tft.setCursor((DISP_W - 144) / 2, DISP_H / 2 - 52); tft.print("Claude");
  tft.setTextColor(C_WHITE);  tft.setTextSize(4);
  tft.setCursor((DISP_W - 96) / 2,  DISP_H / 2 + 8);  tft.print("Code");
  tft.fillRect((DISP_W - 96) / 2, DISP_H / 2 + 52, 96, 3, C_ORANGE);
}

// ═════════════════════════════════════════════════════════════
//  TERMINAL
// ═════════════════════════════════════════════════════════════

void termClear() {
  for (uint8_t i = 0; i < TERM_ROWS; i++) termLines[i] = "";
  termRow = 0; termCol = 0;
}

void termDrawHeader() {
  tft.fillRect(0, 0, DISP_W, TERM_PAD_Y + 1, C_DARKBG);
  tft.setTextColor(C_ORANGE); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, 4); tft.print("clawd@mochi terminal");
  tft.drawFastHLine(0, TERM_PAD_Y, DISP_W, C_ORANGE);
}

// Prefix "clawd:~$ " in green, drawn only when the row has content
void termDrawPrefix(int16_t yy) {
  tft.setTextColor(C_GREEN); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, yy + 6);
  tft.print("clawd:~$ ");
}

#define PREFIX_PX 54   // 9 chars × 6px = 54px at textSize 1

void termDrawLine(uint8_t r) {
  const int16_t yy = TERM_PAD_Y + 4 + r * TERM_CHAR_H;
  tft.fillRect(0, yy, DISP_W, TERM_CHAR_H, C_DARKBG);
  // show prefix only on the currently active (cursor) line
  if (r == termRow) termDrawPrefix(yy);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(TERM_PAD_X + PREFIX_PX, yy + 1);
  tft.print(termLines[r]);
  if (r == termRow) {
    const int16_t cx = TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W;
    tft.fillRect(cx, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  }
}

void termDrawLastChar() {
  if (termCol == 0) return;
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  const uint8_t prev  = termCol - 1;
  // erase prev cell (had cursor block)
  tft.fillRect(baseX + prev * TERM_CHAR_W, yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(baseX + prev * TERM_CHAR_W, yy + 1);
  tft.print(termLines[termRow][prev]);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
}

void termDrawBackspace() {
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  // erase deleted char + old cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W * 2, TERM_CHAR_H - 1, C_DARKBG);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  // if line now empty, erase the prefix too
  if (termLines[termRow].length() == 0) {
    tft.fillRect(0, yy, TERM_PAD_X + PREFIX_PX, TERM_CHAR_H, C_DARKBG);
  }
}

void termFullRedraw() {
  tft.fillScreen(C_DARKBG);
  termDrawHeader();
  for (uint8_t r = 0; r < TERM_ROWS; r++) termDrawLine(r);
}

void termScroll() {
  for (uint8_t i = 0; i < TERM_ROWS - 1; i++) termLines[i] = termLines[i + 1];
  termLines[TERM_ROWS - 1] = "";
  termRow = TERM_ROWS - 1;
  termFullRedraw();
}

void termAddChar(char c) {
  if (c == '\n' || c == '\r') {
    const int16_t yy = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
    // erase cursor on current row
    tft.fillRect(TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W,
                 yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
    termRow++; termCol = 0;
    if (termRow >= TERM_ROWS) { termScroll(); return; }
    termDrawLine(termRow);  // draws prefix on new line
  } else if (c == '\b' || c == 127) {
    if (termCol > 0) {
      termCol--;
      termLines[termRow].remove(termLines[termRow].length() - 1);
      termDrawBackspace();
    }
  } else if (c >= 32 && c < 127) {
    if (termCol >= TERM_COLS) {
      termRow++; termCol = 0;
      if (termRow >= TERM_ROWS) { termScroll(); return; }
    }
    // draw prefix on first char of this line
    if (termCol == 0) termDrawPrefix(TERM_PAD_Y + 4 + termRow * TERM_CHAR_H);
    termLines[termRow] += c;
    termCol++;
    termDrawLastChar();
  }
}

// ═════════════════════════════════════════════════════════════
//  ANIMATIONS
// ═════════════════════════════════════════════════════════════

// ── 官方两只眼睛动画：非阻塞步进（本轮重写）──────────────────
// 旧实现用 delay() 串行播放：速度阀钮一放大就会把主循环阻塞好几秒，
// 所以上一版被迫把倍率封顶 2 倍 → 滑杆拉到 2 以上「完全没反应」。
// 现在改成状态机：每轮 loop() 只走一步，步间间隔由 speedMs() 决定，
// 主循环永不阻塞，速度阀钮可以 1..10 全量生效。
#define EYE_SEQ_NORMAL 1
#define EYE_SEQ_SQUISH 2
static uint8_t  gEyeSeq    = 0;   // 0 = 空闲
static uint8_t  gEyeStep   = 0;
static uint32_t gEyeNextMs = 0;

static const int16_t EYE_WIGGLE_OFFS[5] = {-16, 16, -16, 16, 0};

void animNormalEyes() {          // 名字不变，调用方无需修改；只负责"开始"
  currentView = VIEW_EYES_NORMAL;
  gEyeSeq = EYE_SEQ_NORMAL; gEyeStep = 0; gEyeNextMs = 0; busy = true;
}

void animSquishEyes() {
  currentView = VIEW_EYES_SQUISH;
  gEyeSeq = EYE_SEQ_SQUISH; gEyeStep = 0; gEyeNextMs = 0; busy = true;
}

bool eyesAnimBusy() { return gEyeSeq != 0; }

// loop() 每轮调用：到点走一步
static void eyesAnimTick() {
  if (gEyeSeq == 0) return;

  // 视图被别处切走（比如点功能入口 / 回桌宠）→ 立刻放弃剩余步骤，
  // 否则残存的步进会把眼睛画到别人家的屏幕上（很典型的串屏 bug）。
  if ((gEyeSeq == EYE_SEQ_NORMAL && currentView != VIEW_EYES_NORMAL) ||
      (gEyeSeq == EYE_SEQ_SQUISH && currentView != VIEW_EYES_SQUISH)) {
    gEyeSeq = 0; gEyeNextMs = 0; busy = false;
    return;
  }

  const uint32_t now = millis();
  if (gEyeNextMs != 0 && (int32_t)(now - gEyeNextMs) < 0) return;   // 还没到时间

  if (gEyeSeq == EYE_SEQ_NORMAL) {
    // 0..4 来回晃 → 5 闭眼 → 6 睁眼 → 7 闭眼 → 8 睁眼收尾（与旧 delay 版完全同序列）
    switch (gEyeStep) {
      case 0: case 1: case 2: case 3: case 4:
        drawNormalEyes(EYE_WIGGLE_OFFS[gEyeStep]);
        gEyeNextMs = now + speedMs(80);
        break;
      case 5: drawNormalEyes(0, true);  gEyeNextMs = now + speedMs(100); break;
      case 6: drawNormalEyes(0, false); gEyeNextMs = now + speedMs(70);  break;
      case 7: drawNormalEyes(0, true);  gEyeNextMs = now + speedMs(70);  break;
      default:                          // 8
        drawNormalEyes(0, false);
        gEyeSeq = 0; gEyeNextMs = 0; busy = false;
        break;
    }
  } else {
    // 3 个循环 ×（睁 160 / 闭 100），最后停在睁开
    if (gEyeStep < 6) {
      const bool closed = (gEyeStep & 1) != 0;
      drawSquishEyes(closed);
      gEyeNextMs = now + speedMs(closed ? 100 : 160);
    } else {
      drawSquishEyes(false);
      gEyeSeq = 0; gEyeNextMs = 0; busy = false;
    }
  }
  gEyeStep++;
}

void animLogoReveal() {
  busy = true;
  tft.fillScreen(animBgColor);
  for (uint16_t i = 0; i < LOGO_SEG_COUNT; i++) {
    int16_t x1 = pgm_read_word(&LOGO_SEGS[i][0]);
    int16_t y1 = pgm_read_word(&LOGO_SEGS[i][1]);
    int16_t x2 = pgm_read_word(&LOGO_SEGS[i][2]);
    int16_t y2 = pgm_read_word(&LOGO_SEGS[i][3]);
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, C_WHITE);
    // 开机动画不跟速度阀钮走（否则 10 档时开机要好几秒），固定小延时
    if (i % 4 == 0) { server.handleClient(); delay(8); }
  }
  drawLogoFilled(animBgColor, C_WHITE);
  delay(1500);
  busy = false;
}

// ═════════════════════════════════════════════════════════════
//  CLOCK & CALENDAR  (new view — time synced from the web page)
// ═════════════════════════════════════════════════════════════

// Layout of the clock/calendar screen
#define CLK_TIME_X  30      // left edge of the big HH:MM digits
#define CLK_TIME_Y  12      // top edge of the big digits (textSize 6)
#define CLK_DATE_Y  70      // "YYYY-MM-DD" row
#define CLK_WDAY_Y  90      // weekday name row
#define CLK_HDR_Y   118     // "MONTH YYYY" header row
#define CLK_LTR_Y   138     // weekday letters row (S M T W T F S)
#define CLK_GRID_Y  152     // top edge of the calendar grid
#define CLK_X0      8       // left edge of the grid
#define CLK_COL_W   32      // grid column width (7 × 32 = 224 px)
#define CLK_ROW_H   13      // grid row height
#define CLK_ROWS    6       // maximum number of week rows drawn

const char* const MONTH_SHORT[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                     "JUL","AUG","SEP","OCT","NOV","DEC"};
const char* const WEEKDAY_FULL[7] = {"SUNDAY","MONDAY","TUESDAY","WEDNESDAY",
                                     "THURSDAY","FRIDAY","SATURDAY"};

// struct ClockDT 已上移到文件顶部（函数原型之前），见文件开头说明。

// last drawn year|month|day key (so we know when the date changed)
int lastDayKey = -1;
uint32_t lastClockMs = 0;

bool leapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, int m) {
  switch (m) {
    case 2: return leapYear(y) ? 29 : 28;
    case 4: case 6: case 9: case 11: return 30;
    default: return 31;
  }
}

// Translate the synced epoch into local calendar fields.
void epochToLocalDT(ClockDT &dt) {
  unsigned long el = millis() - bootMs;
  if (el > 2592000000UL) {         // rebase after 30 days (millis() wrap)
    bootEpochSec += (long)(el / 1000UL);
    bootMs = millis();
    el = 0;
  }
  long ls = bootEpochSec + (long)(el / 1000UL) + tzOffsetSec;

  long q = ls / 86400L;
  long r = ls % 86400L;
  if (r < 0) { r += 86400L; q--; }
  const long days = q;             // whole days since 1970-01-01
  const int  sod  = (int)r;        // seconds elapsed in the current day

  dt.hour = sod / 3600;
  dt.min  = (sod % 3600) / 60;
  dt.sec  = sod % 60;

  // civil-from-days (proleptic Gregorian, integer-only)
  long z   = days + 719468L;
  long era = z / 146097L;
  long doe = z - era * 146097L;
  long yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L;
  long y   = yoe + era * 400L;
  long doy = doe - (365L * yoe + yoe / 4L - yoe / 100L);
  long mp  = (5L * doy + 2L) / 153L;
  long d   = doy - (153L * mp + 2L) / 5L + 1L;
  long m   = mp < 10L ? mp + 3L : mp - 9L;
  y += (m <= 2L);
  dt.year = (int)y;
  dt.mon  = (int)m;
  dt.mday = (int)d;

  long wd = (days + 4L) % 7L;      // 1970-01-01 was a Thursday
  if (wd < 0) wd += 7L;
  dt.wday = (int)wd;               // 0 = Sunday
}

// ── 供睡眠系统复用（第五阶段新增）─────────────────────────────
// 只是把上面这套时间读出来，**不新建第二套 RTC / NTP / 时间同步**。
// 返回 false = 网页还没校时，此时调用方一律按「白天」处理。
// 声明在 sleep.h 里，所以 sleep.cpp 直接就能用。
bool localTimeHM(uint8_t &hour, uint8_t &minute) {
  if (!timeSynced) return false;
  ClockDT dt;
  epochToLocalDT(dt);
  hour   = (uint8_t)dt.hour;
  minute = (uint8_t)dt.min;
  return true;
}

// ── 供日程系统复用（第六阶段新增）─────────────────────────────
// 返回「本地 epoch 秒」——就是 epochToLocalDT() 内部那个 ls，
// 与它同一个基准、同一套 30 天 rebase，**不是第二套时间系统**。
// 日程的倒计时全靠它：remaining = targetEpoch - localEpochNow()。
// 返回 false = 网页还没校时。
// ⚠ 返回类型必须与 schedule.h 的声明一致（bool）；写成 int32_t 会报
//   “ambiguating new declaration of 'localEpochNow(int32_t&)'”（首编译实测踩到）。
bool localEpochNow(int32_t &out) {
  if (!timeSynced) { out = 0; return false; }
  unsigned long el = millis() - bootMs;
  if (el > 2592000000UL) {          // 与 epochToLocalDT 相同的 30 天 rebase
    bootEpochSec += (long)(el / 1000UL);
    bootMs = millis();
    el = 0;
  }
  out = (int32_t)(bootEpochSec + (long)(el / 1000UL) + tzOffsetSec);
  return true;
}

// Print a string horizontally centred on the 240 px wide screen.
void printCentered(const char* s, uint8_t size, int16_t y, uint16_t col) {
  int len = 0;
  while (s[len]) len++;
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor((DISP_W - len * 6 * size) / 2, y);
  tft.print(s);
}

// Big HH:MM digits — redrawn every second so the colon blinks.
void drawClockTime(const ClockDT &dt) {
  tft.fillRect(CLK_TIME_X, 4, 180, 64, C_CLKBG);
  char buf[4];
  tft.setTextSize(6); tft.setTextColor(C_CREAM);
  snprintf(buf, sizeof buf, "%02d", dt.hour);
  tft.setCursor(CLK_TIME_X, CLK_TIME_Y); tft.print(buf);
  if (dt.sec & 1) {                // blink the colon on odd seconds
    tft.setTextColor(C_CREAM);
    tft.setCursor(CLK_TIME_X + 72, CLK_TIME_Y); tft.print(":");
  }
  tft.setTextColor(C_CREAM);
  snprintf(buf, sizeof buf, "%02d", dt.min);
  tft.setCursor(CLK_TIME_X + 108, CLK_TIME_Y); tft.print(buf);
}

void drawClockDateRow(const ClockDT &dt) {
  tft.fillRect(0, CLK_DATE_Y - 6, DISP_W, 18, C_CLKBG);
  char b[16];
  snprintf(b, sizeof b, "%04d-%02d-%02d", dt.year, dt.mon, dt.mday);
  printCentered(b, 2, CLK_DATE_Y - 2, C_CREAM);
  tft.fillRect(0, CLK_WDAY_Y - 4, DISP_W, 20, C_CLKBG);
  printCentered(WEEKDAY_FULL[dt.wday], 2, CLK_WDAY_Y, C_CREAM);
}

void drawClockCalendar(const ClockDT &dt) {
  // "MONTH YYYY" header
  tft.fillRect(0, CLK_HDR_Y - 4, DISP_W, 20, C_CLKBG);
  char mb[16];
  snprintf(mb, sizeof mb, "%s %d", MONTH_SHORT[dt.mon - 1], dt.year);
  printCentered(mb, 2, CLK_HDR_Y - 2, C_CREAM);

  // weekday letters above the columns (S=Sunday … S=Saturday)
  const char lts[8] = "SMTWTFS";
  tft.setTextSize(1); tft.setTextColor(C_CREAM);
  for (int c = 0; c < 7; c++) {
    tft.setCursor(CLK_X0 + c * CLK_COL_W + 13, CLK_LTR_Y);
    tft.print(lts[c]);
  }

  const int16_t gh = CLK_ROWS * CLK_ROW_H;   // grid height
  const int16_t gw = 7 * CLK_COL_W;          // grid width
  tft.fillRect(CLK_X0, CLK_GRID_Y, gw, gh, C_CLKBG);   // clear old grid
  tft.drawRect(CLK_X0, CLK_GRID_Y, gw, gh, C_CLKLINE);
  for (int c = 1; c < 7; c++)
    tft.drawFastVLine(CLK_X0 + c * CLK_COL_W, CLK_GRID_Y, gh, C_CLKLINE);
  for (int rRow = 1; rRow < CLK_ROWS; rRow++)
    tft.drawFastHLine(CLK_X0, CLK_GRID_Y + rRow * CLK_ROW_H, gw, C_CLKLINE);

  const int dim = daysInMonth(dt.year, dt.mon);
  int wd1 = (dt.wday - (dt.mday - 1)) % 7;   // weekday of the 1st of month
  if (wd1 < 0) wd1 += 7;

  for (int day = 1; day <= dim; day++) {
    const int idx  = wd1 + day - 1;
    const int rRow = idx / 7;
    const int c    = idx % 7;
    if (rRow >= CLK_ROWS) break;
    const int16_t x = CLK_X0 + c * CLK_COL_W;
    const int16_t y = CLK_GRID_Y + rRow * CLK_ROW_H;
    const bool today = (day == dt.mday);
    if (today) tft.fillCircle(x + CLK_COL_W / 2, y + CLK_ROW_H / 2, 6, C_CLKDEEP);
    char nb[4];
    snprintf(nb, sizeof nb, "%d", day);
    const int16_t tw = (day >= 10) ? 12 : 6;
    tft.setTextSize(1);
    tft.setTextColor(C_CREAM);
    tft.setCursor(x + (CLK_COL_W - tw) / 2, y + (CLK_ROW_H - 8) / 2);
    tft.print(nb);
  }
}

void drawClockFull() {
  tft.fillScreen(C_CLKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_CREAM);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_CREAM);

  if (!timeSynced) {
    // nothing synced yet: ask the user to open the web page
    tft.setTextSize(6); tft.setTextColor(C_CREAM);
    tft.setCursor(CLK_TIME_X, CLK_TIME_Y); tft.print("--:--");
    printCentered("NO TIME SYNCED", 2, CLK_DATE_Y - 2, C_CREAM);
    printCentered("connect to ClaWD-Mochi wifi", 1, 96, C_CLKDEEP);
    printCentered("open http://192.168.4.1",    1, 112, C_CLKDEEP);
    printCentered("the page auto-syncs the time", 1, 128, C_CLKDEEP);
    lastClockMs = millis();
    lastDayKey  = -1;
    return;
  }

  ClockDT dt;
  epochToLocalDT(dt);
  drawClockTime(dt);
  drawClockDateRow(dt);
  drawClockCalendar(dt);
  lastClockMs = millis();
  lastDayKey  = (dt.year << 9) | (dt.mon << 5) | dt.mday;
}

// Called from loop() once a second while the clock view is active.
void clockTick() {
  if (currentView != VIEW_CLOCK || busy || !timeSynced) return;
  uint32_t now = millis();
  if (now - lastClockMs < 950) return;
  lastClockMs = now;
  ClockDT dt;
  epochToLocalDT(dt);
  drawClockTime(dt);               // blink colon / advance minutes
  int dk = (dt.year << 9) | (dt.mon << 5) | dt.mday;
  if (dk != lastDayKey) {          // the date rolled over
    lastDayKey = dk;
    drawClockDateRow(dt);
    drawClockCalendar(dt);
  }
}

// ═════════════════════════════════════════════════════════════
//  WEB PAGE
// ═════════════════════════════════════════════════════════════
const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px 52px;gap:14px;min-height:100vh}

.hdr{text-align:center;padding:2px 0 4px}
.mascot{font-size:15px;color:#c96a3e;line-height:1.3;font-weight:bold;
  font-family:'Courier New',monospace;display:block;letter-spacing:1px}
.sitename{font-size:10px;color:#5a5048;margin-top:8px;letter-spacing:3px}

.sec{width:100%;max-width:390px;font-size:10px;color:#8a8278;
  letter-spacing:2px;font-weight:bold;padding:0 2px}

/* Busy bar */
.busy{width:100%;max-width:390px;height:2px;background:#2e2a28;
  border-radius:1px;overflow:hidden;opacity:0;transition:opacity .2s}
.busy.show{opacity:1}
.busy-i{height:100%;width:30%;background:#c96a3e;border-radius:1px;
  animation:sl 1s linear infinite}
@keyframes sl{0%{margin-left:-30%}100%{margin-left:100%}}

/* Controls */
.ctrl{display:flex;gap:8px;width:100%;max-width:390px}
.cbtn{flex:1;background:#252428;border:1.5px solid #38343a;border-radius:10px;
  color:#b8b4ac;font-family:'Courier New',monospace;font-size:11px;font-weight:bold;
  padding:12px 4px;cursor:pointer;text-align:center;transition:all .12s}
.cbtn:active:not(:disabled){transform:scale(.94)}
.cbtn:disabled{opacity:.3;cursor:default}
.cbtn.on{border-color:#c96a3e;color:#c96a3e;background:#201408}
.cbtn.dim{border-color:#2e2a28;color:#4a4540}

/* View grid */
.vgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;width:100%;max-width:390px}
.vbtn{background:#252428;border:1.5px solid #38343a;border-radius:12px;
  color:#d8d4cc;font-family:'Courier New',monospace;
  padding:14px 6px 10px;cursor:pointer;text-align:center;
  transition:all .12s;user-select:none}
.vbtn:active:not(:disabled){transform:scale(.94)}
.vbtn:disabled{opacity:.3;cursor:default}
.vbtn .ic{font-size:20px;display:block;margin-bottom:4px;line-height:1;color:#c96a3e}
.vbtn .nm{font-size:12px;font-weight:bold;color:#e8e4dc}
.vbtn .ht{font-size:9px;color:#8a8278;margin-top:3px}
.vbtn.active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="1"].active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="2"].active{border-color:#4a8acd;background:#0c1628}
.vbtn[data-v="3"].active{border-color:#38343a;background:#201c18}
.vbtn[data-v="4"].active{border-color:#d9a441;background:#1d1505}
.vbtn.wide{grid-column:1 / -1}

/* ── 功能入口（整合新增）：每个新功能一个进入按钮 ── */
.fgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;width:100%;max-width:390px}
.fbtn{background:#232128;border:1.5px solid #38343a;border-radius:12px;color:#d8d4cc;
  font-family:'Courier New',monospace;padding:11px 6px 9px 6px;cursor:pointer;text-align:left;
  transition:all .12s;user-select:none;position:relative}
.fbtn:active:not(:disabled){transform:scale(.96)}
.fbtn:disabled{opacity:.3;cursor:default}
.fbtn .fi{position:absolute;left:9px;top:11px;font-size:14px;line-height:1;color:#c96a3e}
.fbtn .fn{display:block;font-size:11.5px;font-weight:bold;color:#e8e4dc;padding-left:20px}
.fbtn .fh{display:block;font-size:9px;color:#8a8278;margin-top:3px;padding-left:20px}
.fbtn:hover{border-color:#c96a3e}
.fbtn.active{border-color:#c96a3e;background:#201408}
.fbtn.active .fn{color:#e0a63c}
.fbtn.open{border-color:#8a7a2a}
.fbtn.wide{grid-column:1 / -1}
/* ── 本次修改（问题 1/2）：功能入口中文化 + 合并入口的副标题 ──
   「日程与纪念日」由原来的 Schedule + Birthday & Anniversary 两个入口合并而来，
   用 .fh 那行小字说明它内部同时包含倒计时和纪念日两种数据。 */
.fbtn .fn.cn{font-size:12px;letter-spacing:1px}
.fbtn .fh.cn{font-size:9.5px}
.fbtn.merged{border-color:#4a3a28}
.fbtn.merged .fi{color:#e0a63c}
/* 模块面板已改到各自独立的操作界面（/feature?v=N），主页面不再挂载它们，
   因此这里不再需要"默认收起"的规则 —— 官方页面 HTML 也因此保持纯插入、零删改 */

/* Speed slider */
.speed-row{width:100%;max-width:390px;display:flex;align-items:center;gap:10px}
.sl{font-size:10px;color:#6a6058;white-space:nowrap;min-width:36px}
input[type=range]{flex:1;accent-color:#c96a3e;cursor:pointer;height:20px}
.sv{font-size:11px;color:#c96a3e;min-width:44px;text-align:right;font-weight:bold}

/* Terminal */
.twrap{width:100%;max-width:390px;display:none;flex-direction:column;gap:8px}
.twrap.open{display:flex}
.thdr{display:flex;justify-content:space-between;align-items:center}
.tttl{font-size:11px;color:#28b878;letter-spacing:1px;font-weight:bold}
.tx{background:#0c1e12;border:2px solid #1a4828;border-radius:9px;
  color:#28b878;font-family:'Courier New',monospace;font-size:13px;
  font-weight:bold;padding:10px 18px;cursor:pointer}
.tx:active{background:#081410}
.trow{display:flex;gap:6px}
.tin{flex:1;background:#0c1018;border:1.5px solid #1a2820;border-radius:9px;
  color:#40d880;font-family:'Courier New',monospace;font-size:15px;
  padding:11px;outline:none}
.tin::placeholder{color:#2a3828}
.tgo{background:#1a9060;border:none;border-radius:9px;color:#fff;
  font-family:'Courier New',monospace;font-size:22px;font-weight:bold;
  padding:11px 16px;cursor:pointer;min-width:52px}
.tgo:active{background:#0f6040}

/* Canvas */
.cwrap{width:100%;max-width:390px;background:#222028;border:1.5px solid #38343a;
  border-radius:12px;padding:12px;flex-direction:column;gap:10px;display:none}
.cwrap.open{display:flex}
.crow{display:flex;gap:8px}
.ci{display:flex;flex-direction:column;align-items:center;gap:4px;flex:1}
.cl{font-size:10px;color:#7a7068;letter-spacing:1px;font-weight:bold}
.cs{width:100%;height:38px;border-radius:7px;border:1.5px solid #38343a;cursor:pointer;padding:0}
.dacts{display:flex;gap:7px}
.db{flex:1;background:#1c1820;border:1.5px solid #38343a;border-radius:9px;
  color:#c0bab8;font-family:'Courier New',monospace;font-size:11px;
  font-weight:bold;padding:11px 4px;cursor:pointer;transition:all .12s}
.db:active{transform:scale(.95);background:#281838}
.db.hi{border-color:#c96a3e;color:#c96a3e}
canvas{width:100%;border-radius:8px;border:1.5px solid #38343a;
  touch-action:none;cursor:crosshair;display:block}

/* Toast */
.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);
  background:#252428;border:1.5px solid #38343a;border-radius:9px;
  font-size:12px;color:#d8d4cc;padding:7px 16px;opacity:0;
  transition:opacity .18s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
</style>
</head>
<body>

<div class="hdr">
  <span class="mascot">&#x2590;&#x259B;&#x2588;&#x2588;&#x2588;&#x259C;&#x258C;<br>&#x259C;&#x2588;&#x2588;&#x2588;&#x2588;&#x2588;&#x259B;<br>&#x2598;&#x2598;&nbsp;&#x259D;&#x259D;</span>
  <div class="sitename">CLAWD &middot; MOCHI &middot; CONTROLLER</div>
</div>

<div class="busy" id="busy"><div class="busy-i"></div></div>

<div class="sec">// 控制</div>
<div class="ctrl">
  <button class="cbtn on" id="blBtn" onclick="toggleBL()">&#9728; 屏幕开</button>
</div>

<div class="sec">// 画面</div>
<div class="vgrid">
  <button class="vbtn active" data-v="0" onclick="setView(0)">
    <span class="ic">&#9632; &#9632;</span>
    <span class="nm">正常眼睛</span>
    <span class="ht">左右晃动 + 眨眼</span>
  </button>
  <button class="vbtn" data-v="1" onclick="setView(1)">
    <span class="ic">&gt; &lt;</span>
    <span class="nm">开心挤眼</span>
    <span class="ht">睁眼 / 闭眼</span>
  </button>
  <button class="vbtn" data-v="2" onclick="setView(2)">
    <span class="ic">{ }</span>
    <span class="nm">Claude Code</span>
    <span class="ht">打开终端</span>
  </button>
  <button class="vbtn" data-v="3" onclick="toggleCanvas()">
    <span class="ic">&#11035;</span>
    <span class="nm">画板</span>
    <span class="ht">在屏幕上涂画</span>
  </button>
  <button class="vbtn wide" data-v="4" onclick="setView(4)">
    <span class="ic">&#8986;</span>
    <span class="nm">时钟与日历</span>
    <span class="ht">实时时间 + 月历</span>
  </button>
</div>

<!-- ══ 功能入口（本次修改：问题 1 合并 + 问题 2 中文化）══
     合并：原来的「Schedule」和「Birthday &amp; Anniversary」两个独立入口，
           现在合成一个「日程与纪念日」入口，主界面只保留一个按钮。
           合并后的次界面内部用「倒计时 / 纪念日」两个页签区分两种数据，
           两种能力（倒计时提醒 + 纪念日庆祝动画）都完整保留。
     中文化：所有功能名、按钮名改成中文；设备屏幕只有 5x7 英文字库，
           屏幕上的标题仍保持英文（见 featureTftName()），这是刻意为之。 -->
<div class="sec">// 功能 &mdash; 点击进入</div>
<div class="fgrid">
  <button class="fbtn" data-f="5" onclick="openFeature(5)">
    <span class="fi">&#9786;</span><span class="fn cn">表情</span><span class="fh cn">16 种表情 / 6 种动画</span>
  </button>
  <button class="fbtn" data-f="6" onclick="openFeature(6)">
    <span class="fi">&#9683;</span><span class="fn cn">情绪</span><span class="fh cn">7 项数值 / 自然衰减</span>
  </button>
  <button class="fbtn" data-f="7" onclick="openFeature(7)">
    <span class="fi">&#9757;</span><span class="fn cn">互动</span><span class="fh cn">轻点 / 连击 / 长按</span>
  </button>
  <button class="fbtn" data-f="8" onclick="openFeature(8)">
    <span class="fi">&#10022;</span><span class="fn cn">主动互动</span><span class="fh cn">它会自己动起来</span>
  </button>
  <button class="fbtn" data-f="9" onclick="openFeature(9)">
    <span class="fi">&#9790;</span><span class="fn cn">睡眠与唤醒</span><span class="fh cn">4 状态睡眠机</span>
  </button>
  <!-- 本轮新增：生日动画（独立的 240×240 像素场景 + 许愿 / 吹蜡烛两个按钮） -->
  <button class="fbtn" data-f="12" onclick="openFeature(12)">
    <span class="fi">&#127874;</span><span class="fn cn">生日动画</span><span class="fh cn">许愿 / 吹蜡烛 / 气球</span>
  </button>
  <button class="fbtn wide merged" data-f="10" onclick="openFeature(10)">
    <span class="fi">&#9733;</span><span class="fn cn">日程与纪念日</span><span class="fh cn">倒计时提醒 + 纪念日庆祝（已合并）</span>
  </button>
</div>

<div class="sec">// 速度</div>
<div class="speed-row">
  <span class="sl">快</span>
  <input type="range" id="spd" min="1" max="10" value="10" step="1" oninput="setSpeed(this.value)">
  <span class="sv" id="spdV">10× 最慢</span>
</div>
<div class="sec" style="font-size:9px;color:#6a6058;letter-spacing:1px;padding:0 2px;text-transform:none">
  数值越大动画越慢（1 = 最快，10 = 最慢）· 主要影响表情抖动 / 晃动节奏
</div>

<div class="ctrl">
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">背景色</span>
    <input type="color" class="cs" id="bgCol" value="#aa4818" oninput="onBgChange(this.value)">
  </div>
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">画笔颜色</span>
    <input type="color" class="cs" id="penCol" value="#000000">
  </div>
</div>

<div class="sec">// 终端</div>
<div class="twrap" id="twrap">
  <div class="thdr">
    <span class="tttl">&#9658; clawd:~$</span>
    <button class="tx" onclick="closeTerm()">&#x2715; 退出终端</button>
  </div>
  <div class="trow">
    <input class="tin" id="tin" type="text" placeholder="在这里输入..."
           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
    <button class="tgo" onclick="termEnter()">&#8629;</button>
  </div>
</div>

<div class="cwrap" id="cwrap">
  <div class="dacts">
    <button class="db hi" onclick="clearAll()">&#11035; 清空</button>
    <button class="db" style="border-color:#28b878;color:#28b878" onclick="toggleCanvas()">&#10003; 完成</button>
  </div>
  <canvas id="cvs" width="240" height="240"></canvas>
</div>

<div class="toast" id="toast"></div>

<script>
let activeView  = 0;
let termOpen    = false;
let canvasOpen  = false;
let blOn        = true;
let isBusy      = false;
let drawing     = false;
let lastX = 0, lastY = 0;
let tt;

// 速度档位文案（本次修改）：animSpeed 现在是「慢速倍率」1..10，越大越慢
function spdText(v) {
  v = parseInt(v, 10); if (!(v >= 1)) v = 1; if (v > 10) v = 10;
  if (v === 1)  return '1× 最快';
  if (v === 10) return '10× 最慢';
  return v + '× 慢';
}

// ── Toast ──────────────────────────────────────────────────────
function toast(msg, ok=true) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.borderColor = ok ? '#28b878' : '#c96a3e';
  el.classList.add('show');
  clearTimeout(tt);
  tt = setTimeout(() => el.classList.remove('show'), 1300);
}

// ── Busy ────────────────────────────────────────────────────────
function setBusy(b) {
  isBusy = b;
  document.getElementById('busy').classList.toggle('show', b);
  const locked = b || termOpen;
  document.querySelectorAll('.vbtn').forEach(el => {
    // when canvas open, keep canvas btn (data-v=3) active so user can exit
    el.disabled = canvasOpen ? parseInt(el.dataset.v) !== 3 : locked;
  });
  document.querySelectorAll('.lbtn').forEach(el => el.disabled = locked || canvasOpen);
  // 表情管理界面（/expr.js 动态生成）跟随同一套 busy 规则
  document.querySelectorAll('.ebtn,.mbtn').forEach(el => el.disabled = locked);
  document.querySelectorAll('.cbtn').forEach(el => {
    if (el.id !== 'blBtn') el.disabled = locked;
  });
}

// ── HTTP ────────────────────────────────────────────────────────
async function req(path) {
  try { const r = await fetch(path); return r.ok; }
  catch(e) { toast('no connection', false); return false; }
}

async function waitNotBusy() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch('/state');
      const j = await r.json();
      if (!j.busy) return;
    } catch(e) {}
    await new Promise(r => setTimeout(r, 150));
  }
}

// Send the phone's current time to the ESP32 so the clock stays accurate.
async function syncDeviceTime() {
  const e = Math.round(Date.now() / 1000);
  const tz = -new Date().getTimezoneOffset() * 60;
  return req('/time?e=' + e + '&tz=' + tz);
}

// ── Background colour ───────────────────────────────────────────
async function onBgChange(hex) {
  if (canvasOpen) {
    await req('/draw/clear?bg=' + encodeURIComponent(hex));
  } else {
    await req('/redraw?bg=' + encodeURIComponent(hex));
  }
  redrawCanvas(hex);
}

// ── Speed ───────────────────────────────────────────────────────
async function setSpeed(v) {
  document.getElementById('spdV').textContent = spdText(v);
  await req('/speed?v=' + v);
}

// ── 功能入口（整合新增）────────────────────────────────────────
// 主界面只放按钮；点一下跳到该功能**自己的操作界面** /feature?v=N，
// 那个页面打开时会自动请求 /enter?v=N，把设备屏幕切到对应视图，并带「← back」返回。
function openFeature(v) {
  if (isBusy || termOpen || canvasOpen) return;   // 终端/画板开着时不跳走
  location.href = '/feature?v=' + v;
}

// ── Views ───────────────────────────────────────────────────────
async function setView(v) {
  if (isBusy || termOpen || canvasOpen) return;
  if (v === 3) { toggleCanvas(); return; }  // canvas button in grid
  const keys = ['w','s','d','','c'];
  if (v === 4) await syncDeviceTime();      // fresh time before clock view
  if (!await req('/cmd?k=' + keys[v])) return;
  activeView = v;
  // 回到官方视图时取消功能入口高亮（面板保持用户已展开的状态）
  activeFeature = 0;
  document.querySelectorAll('.fbtn').forEach(function (b) { b.classList.remove('active'); });
  document.querySelectorAll('.vbtn').forEach(b =>
    b.classList.toggle('active', parseInt(b.dataset.v) === v));
  if (v === 2) {
    termOpen = true;
    document.getElementById('twrap').classList.add('open');
    setBusy(false);   // re-run to apply termOpen lock
    setBusy(false);
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    const cvb = document.getElementById('cvBtn'); if (cvb) cvb.disabled = true;
    document.getElementById('tin').focus();
    toast('terminal open');
    return;
  }
  setBusy(true);
  await waitNotBusy();
  setBusy(false);
}

// ── Logo animations (kept for startup, not exposed in UI) ──────

// ── Backlight ───────────────────────────────────────────────────
async function toggleBL() {
  blOn = !blOn;
  await req('/backlight?on=' + (blOn ? 1 : 0));
  const b = document.getElementById('blBtn');
  b.textContent = blOn ? '\u2600 display on' : '\u25cb display off';
  b.classList.toggle('on', blOn);
  b.classList.toggle('dim', !blOn);
}

// ── Canvas toggle ───────────────────────────────────────────────
async function toggleCanvas() {
  canvasOpen = !canvasOpen;
  document.getElementById('cwrap').classList.toggle('open', canvasOpen);
  const b = document.getElementById('cvBtn');
  if (b) { b.classList.toggle('on', canvasOpen); b.textContent = canvasOpen ? '\u2b1b canvas on' : '\u2b1b canvas'; }
  // highlight the canvas vbtn (data-v=3) in the grid
  document.querySelectorAll('.vbtn').forEach(btn =>
    btn.classList.toggle('active', canvasOpen && parseInt(btn.dataset.v) === 3));
  await req('/canvas?on=' + (canvasOpen ? 1 : 0));
  if (canvasOpen) {
    const bg = document.getElementById('bgCol').value;
    redrawCanvas(bg);
    await req('/draw/clear?bg=' + encodeURIComponent(bg));
    // lock all other buttons
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    toast('canvas active');
  } else {
    setBusy(false);   // re-evaluate locks
    toast('canvas off');
  }
}

// ── Terminal ────────────────────────────────────────────────────
const tin = document.getElementById('tin');
let lastVal = '';
tin.addEventListener('input', async () => {
  const cur = tin.value, prev = lastVal;
  if (cur.length > prev.length) {
    await req('/char?c=' + encodeURIComponent(cur[cur.length - 1]));
  } else if (cur.length < prev.length) {
    await req('/char?c=%08');
  }
  lastVal = cur;
});
async function termEnter() {
  await req('/char?c=%0A');
  tin.value = ''; lastVal = ''; tin.focus();
}
tin.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); termEnter(); }
});
async function closeTerm() {
  await req('/cmd?k=q');
  termOpen = false;
  document.getElementById('twrap').classList.remove('open');
  setBusy(false);
  toast('terminal closed');
}

// ── Canvas drawing — send full stroke on finger lift ────────────
const cvs = document.getElementById('cvs');
const ctx = cvs.getContext('2d');
let strokePts = [];

function getPos(e) {
  const r = cvs.getBoundingClientRect();
  const sx = cvs.width / r.width, sy = cvs.height / r.height;
  const s = e.touches ? e.touches[0] : e;
  return { x: (s.clientX - r.left) * sx, y: (s.clientY - r.top) * sy };
}

function redrawCanvas(hex) {
  ctx.fillStyle = hex;
  ctx.fillRect(0, 0, cvs.width, cvs.height);
}

function startDraw(e) {
  e.preventDefault();
  drawing = true;
  strokePts = [];
  const p = getPos(e); lastX = p.x; lastY = p.y;
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  // draw dot on canvas preview only — no display send yet
  ctx.beginPath(); ctx.arc(p.x, p.y, 2, 0, Math.PI * 2);
  ctx.fillStyle = document.getElementById('penCol').value; ctx.fill();
}
function moveDraw(e) {
  if (!drawing) return; e.preventDefault();
  const p = getPos(e);
  ctx.beginPath(); ctx.moveTo(lastX, lastY); ctx.lineTo(p.x, p.y);
  ctx.strokeStyle = document.getElementById('penCol').value;
  ctx.lineWidth = 4; ctx.lineCap = 'round'; ctx.stroke();
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  lastX = p.x; lastY = p.y;
}
async function endDraw(e) {
  if (!drawing) return; drawing = false;
  if (!canvasOpen || strokePts.length < 1) return;
  const pen = document.getElementById('penCol').value.replace('#', '');
  const pts = strokePts.map(p => p.x + ',' + p.y).join(';');
  await req('/draw/stroke?pen=' + pen + '&pts=' + encodeURIComponent(pts));
  strokePts = [];
}

cvs.addEventListener('mousedown',  startDraw);
cvs.addEventListener('mousemove',  moveDraw);
cvs.addEventListener('mouseup',    endDraw);
cvs.addEventListener('mouseleave', endDraw);
cvs.addEventListener('touchstart', startDraw, {passive:false});
cvs.addEventListener('touchmove',  moveDraw,  {passive:false});
cvs.addEventListener('touchend',   endDraw);

// Clear = clear both web canvas and display
async function clearAll() {
  const bg = document.getElementById('bgCol').value;
  redrawCanvas(bg);
  await req('/draw/clear?bg=' + encodeURIComponent(bg));
  toast('cleared');
}

// Init: sync speed and backlight from ESP32, reset bg to default
(async () => {
  try {
    const r = await fetch('/state');
    const j = await r.json();
    // Sync speed
    const spd = j.speed || 1;
    document.getElementById('spd').value = spd;
    document.getElementById('spdV').textContent = spdText(spd);
    // Sync backlight
    if (j.bl === false) {
      blOn = false;
      const b = document.getElementById('blBtn');
      b.textContent = '\u25cb display off';
      b.classList.remove('on'); b.classList.add('dim');
    }
  } catch(e) {}
  // Always reset bg picker to default orange on page load
  document.getElementById('bgCol').value = '#aa4818';
  redrawCanvas('#aa4818');
  // Auto-sync the device clock now and every minute while the page is open
  syncDeviceTime();
  setInterval(syncDeviceTime, 60000);
})();
</script>
</body>
</html>
)rawhtml";

// ═════════════════════════════════════════════════════════════
//  WEB ROUTES
// ═════════════════════════════════════════════════════════════

void routeRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

void routeCmd() {
  if (!server.hasArg("k") || server.arg("k").isEmpty()) {
    server.send(400, "application/json", "{\"e\":1}"); return;
  }
  const char c = server.arg("k")[0];

  if (termMode) {
    if (c == 'q') { termMode = false; drawCodeView(); }
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }

  server.send(200, "application/json", "{\"ok\":1}");
  switch (c) {
    case 'w': currentView = VIEW_EYES_NORMAL; animNormalEyes(); break;
    case 's': currentView = VIEW_EYES_SQUISH; animSquishEyes(); break;
    case 'd':
      currentView = VIEW_CODE; drawCodeView();
      termMode = true; termClear(); termFullRedraw(); break;
    case 'c':
      currentView = VIEW_CLOCK;
      termMode = false;
      drawClockFull();
      break;
    case 'a':
      currentView = VIEW_EYES_NORMAL;
      animLogoReveal();
      break;
  }
}

void routeChar() {
  if (!termMode) { server.send(200, "application/json", "{\"ok\":1}"); return; }
  const String val = server.arg("c");
  if (val.length() > 0) termAddChar(val[0]);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeSpeed() {
  // 本次修改：animSpeed 改为「慢速倍率」1..10（越大越慢）
  if (server.hasArg("v")) animSpeed = constrain(server.arg("v").toInt(), 1, 10);
  server.send(200, "application/json", "{\"ok\":1}");
}

// 按 currentView 重绘当前画面。
// 第七阶段把它从 routeRedraw 里抽出来，让生日庆祝收尾时也能用同一份分派
// （否则要在 birthday.cpp 里再写一套 switch，视图一多就会漂移）。
void redrawCurrentView() {
  switch (currentView) {
    case VIEW_EYES_NORMAL: drawNormalEyes(); break;
    case VIEW_EYES_SQUISH: drawSquishEyes(); break;
    case VIEW_CODE:        drawCodeView();   break;
    case VIEW_DRAW:        tft.fillScreen(drawBgColor); break;
    case VIEW_CLOCK:       drawClockFull();  break;
    case VIEW_EXPRESSION:  exprRedraw();     break;   // 表情视图跟随背景色重绘
    case VIEW_EMOTION:     emotionRedraw();  break;   // 情绪调试屏（自带深色配色）
    case VIEW_INTERACTION: interactionRedraw(); break; // 互动调试屏
    case VIEW_PROACTIVE:   proactiveRedraw();   break; // 主动互动调试屏
    case VIEW_SLEEP:       sleepRedraw();       break; // 睡眠调试屏（第五阶段新增）
    case VIEW_SCHEDULE:    scheduleRedraw();    break; // 日程 / 倒计时屏（第六阶段新增）
    case VIEW_BIRTHDAY:    birthdayRedraw();    break; // 生日 / 纪念日屏（第七阶段新增）
    case VIEW_BDCAKE:      bdcakeRedraw();      break; // 生日动画（本轮新增）
  }
}

// /redraw?bg=hex — set animBg and immediately redraw current view
void routeRedraw() {
  if (server.hasArg("bg")) {
    animBgColor = hexToRgb565(server.arg("bg"));
    drawBgColor = animBgColor;
  }
  redrawCurrentView();
  server.send(200, "application/json", "{\"ok\":1}");
}

// /enter?v=<viewId> — 统一功能入口（整合新增）
// 网页上每个新功能的“进入”按钮都打到这里：把设备屏幕切到该功能的视图。
// 各模块的 xxxEnterView() 自己负责 currentView 赋值 + 整屏重绘，
// 所以这里只做分派，不重复实现绘制（避免视图一多就漂移）。
// 整合新增：除表情视图之外的模块调试屏（情绪/互动/主动/睡眠/日程）。
// 生日庆祝用它判断“用户正在看调试屏时要不要让位”，避免抢用户的屏幕。
// 放在 .ino 而不是 birthday.cpp，是为了不让 birthday 反向依赖
// interaction.h / proactive.h（那会形成双向耦合，也会违反静态自检的耦合规则）。
bool isOtherModuleDebugView() {
  return currentView == VIEW_EMOTION    || currentView == VIEW_INTERACTION ||
         currentView == VIEW_PROACTIVE  || currentView == VIEW_SLEEP       ||
         currentView == VIEW_SCHEDULE   || currentView == VIEW_BDCAKE;
}

// ═══════════════════════════════════════════════════════════════
//  本次新增：让「手机上的动作」在桌宠屏幕上真的看得见
//
//  表情渲染有一个硬前提：设备必须停在 VIEW_EXPRESSION
//  （expression.cpp 的 renderNow() 会 `if (currentView != VIEW_EXPRESSION) return;`）。
//  而手机进各功能次界面时，autoEnter 会把设备切到对应的「XX 调试屏」，
//  于是情绪 / 主动互动 / 睡眠 / 互动 这些改动只改了内部状态，
//  脸上的表情一点变化都没有 —— 用户直接判定「这个功能坏了」。
//
//  凡是「用户从手机点了按钮、并且会改变表情」的接口，动作做完后再调一次本函数，
//  把屏幕切回表情视图。规则：
//    · 终端 / 画板 / 时钟：用户正在干活，不抢；
//    · 已经在表情视图：什么都不用做；
//    · 表情被锁定：尊重锁定，不抢画面（与 interaction 的既有语义一致）。
// ═══════════════════════════════════════════════════════════════
void showFaceIfNeeded() {
  if (isExpressionLocked()) return;
  if (currentView == VIEW_CODE || currentView == VIEW_DRAW ||
      currentView == VIEW_CLOCK || currentView == VIEW_EXPRESSION ||
      currentView == VIEW_BDCAKE) return;   // 生日动画也是「展示型」视图，不抢
  termMode = false;          // 清掉终端残留，避免 /char 还往终端写
  exprEnterView();           // 切到表情视图 + 整屏重绘
}

// ── 每个新功能一个独立操作界面（整合新增）──────────────────────
// 主界面只放按钮；点一下跳到 /feature?v=N，这个页面：
//   ① 显示该功能的名字 + 「← back」返回主界面
//   ② 打开时自动请求 /enter?v=N，把设备屏幕切到该功能视图
//   ③ 挂载该功能自己的面板（/xxx.css + /xxx.js），面板 JS 自己会填进挂载点
// 这样"一个主界面 + 多个次界面"的结构就落在设备网页上了。
static const char* featureKey(int v) {
  switch (v) {
    case VIEW_EXPRESSION:  return "expr";
    case VIEW_EMOTION:     return "emo";
    case VIEW_INTERACTION: return "ia";
    case VIEW_PROACTIVE:   return "pro";
    case VIEW_SLEEP:       return "sleep";
    case VIEW_SCHEDULE:    return "sched";
    case VIEW_BIRTHDAY:    return "bday";
    case VIEW_BDCAKE:      return "bc";
  }
  return nullptr;
}
// ── 本次修改（问题 1 + 问题 2）──────────────────────────────────
// 问题 2：featureTitle() 全部改成中文 —— 网页主界面、次界面标题、浏览器标题栏都用它。
// 问题 1：VIEW_BIRTHDAY 不再单独出现在主界面入口里；它和 VIEW_SCHEDULE 共用
//         「日程与纪念日」这一个入口，只在次界面内部用页签区分两种数据。
static const char* featureTitle(int v) {
  switch (v) {
    case VIEW_EXPRESSION:  return "表情";
    case VIEW_EMOTION:     return "情绪";
    case VIEW_INTERACTION: return "互动";
    case VIEW_PROACTIVE:   return "主动互动";
    case VIEW_SLEEP:       return "睡眠与唤醒";
    // 合并后的统一名称：同时包含「倒计时」和「纪念日」两种数据
    case VIEW_SCHEDULE:    return "日程与纪念日";
    case VIEW_BIRTHDAY:    return "日程与纪念日";
    case VIEW_BDCAKE:      return "生日动画";
  }
  return "";
}

// 设备屏幕（TFT）只有 5x7 英文字库，显示不了中文，
// 所以推到桌宠屏幕上的标题必须另给一条英文名 —— 这是刻意为之，不是漏改。
static const char* featureTftName(int v) {
  switch (v) {
    case VIEW_EXPRESSION:  return "EXPRESSION";
    case VIEW_EMOTION:     return "EMOTION";
    case VIEW_INTERACTION: return "INTERACTION";
    case VIEW_PROACTIVE:   return "PROACTIVE";
    case VIEW_SLEEP:       return "SLEEP";
    case VIEW_SCHEDULE:    return "SCHEDULE";
    case VIEW_BIRTHDAY:    return "ANNIVERSARY";
    case VIEW_BDCAKE:      return "BIRTHDAY";
  }
  return "";
}

// 合并模块（日程 + 纪念日）共用同一个功能入口编号 = VIEW_SCHEDULE。
// 次界面里用页签切换两种数据：tab=sched → VIEW_SCHEDULE，tab=bday → VIEW_BIRTHDAY。
static const char* featureSlot(int v) {
  switch (v) {
    case VIEW_EXPRESSION:  return "exprSlot";
    case VIEW_EMOTION:     return "emoSlot";
    case VIEW_INTERACTION: return "iaSlot";
    case VIEW_PROACTIVE:   return "proSlot";
    case VIEW_SLEEP:       return "sleepSlot";
    case VIEW_SCHEDULE:    return "schedSlot";
    case VIEW_BIRTHDAY:    return "bdaySlot";
    case VIEW_BDCAKE:      return "bcSlot";
  }
  return "";
}

void routeFeature() {
  const int v = server.hasArg("v") ? server.arg("v").toInt() : -1;
  const char* key = featureKey(v);
  if (!key) { server.send(404, "text/plain", "unknown feature"); return; }

  // 【问题 1】日程与纪念日共用一个入口：/feature?v=10(VIEW_SCHEDULE) 同时挂两个面板，
  // 顶部多一条页签在「倒计时(sched)」和「纪念日(bday)」之间切换。
  const bool isMerged = (v == VIEW_SCHEDULE);

  // 本轮新增：生日动画的次界面不用「遥控器」卡片。
  // 理由：它本身就是那块画面（手机上的 canvas 与桌宠屏同源），
  // 点按钮会直接把动作推给桌宠（/bc/wish、/bc/blow），
  // 再放一个「显示到桌宠屏幕」按钮只会让人以为还要额外推一次。
  const bool isBcake = (v == VIEW_BDCAKE);

  // 【问题 4】手机是控制端、桌宠屏幕是显示端：页面上明确区分，
  // 并提供两个显式按钮：「显示到桌宠屏幕」把当前选择推给设备，
  // 「回到桌宠」把设备切回桌宠本体画面（正常眼睛）。
  String p;
  p.reserve(2600);
  p += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"UTF-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">"
         "<title>Clawd Mochi | ");
  p += featureTitle(v);
  p += F("</title><style>"
         "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}"
         "body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;"
         "display:flex;flex-direction:column;align-items:center;padding:18px 14px 60px;gap:12px;min-height:100vh}"
         /* 顶栏：返回 + 标题 + 状态灯 */
         ".fbar{width:100%;max-width:420px;display:flex;align-items:center;gap:10px;flex-wrap:wrap}"
         ".fback{display:inline-block;background:#252428;border:1.5px solid #38343a;border-radius:10px;"
         "color:#c96a3e;text-decoration:none;font-size:12px;font-weight:bold;padding:10px 14px}"
         ".fback:active{transform:scale(.95)}"
         ".fttl{flex:1;font-size:15px;font-weight:bold;color:#e8e4dc}"
         ".fst{font-size:9.5px;color:#6a6058;letter-spacing:1px}"
         ".fst.ok{color:#28b878}.fst.bad{color:#e0703c}"
         /* 桌宠状态卡片：说明"手机是遥控器，桌宠屏幕才是显示端" */
         ".scr{width:100%;max-width:420px;background:#222028;border:1.5px solid #38343a;"
         "border-radius:12px;padding:11px 12px;display:flex;flex-direction:column;gap:9px}"
         ".scrln{font-size:10.5px;color:#8a8278;letter-spacing:.5px;line-height:1.5}"
         ".act{display:flex;gap:8px}"
         ".ab{flex:1;font-family:'Courier New',monospace;font-size:11.5px;font-weight:bold;"
         "border-radius:9px;padding:11px 4px;cursor:pointer;transition:all .12s;border:1.5px solid #38343a}"
         ".ab.push{background:#201408;border-color:#c96a3e;color:#e0a63c}"
         ".ab.pet{background:#181a20;border-color:#38343a;color:#b8b4ac}"
         ".ab:active{transform:scale(.95)}"
         /* 合并模块的页签 */
         ".tabs{width:100%;max-width:420px;display:flex;gap:8px}"
         ".tab{flex:1;text-align:center;font-size:13px;font-weight:bold;padding:11px 4px;cursor:pointer;"
         "background:#232128;border:1.5px solid #38343a;border-radius:10px;color:#b8b4ac}"
         ".tab.on{background:#201408;border-color:#c96a3e;color:#e0a63c}"
         ".pane{width:100%;display:none;flex-direction:column;align-items:center;gap:12px}"
         ".pane.on{display:flex}"
         "</style><link rel=\"stylesheet\" href=\"/");
  p += key;
  p += F(".css\"></head><body>"
         "<div class=\"fbar\"><a class=\"fback\" href=\"/\">&#8592; 返回</a>"
         "<span class=\"fttl\">");
  p += featureTitle(v);
  p += F("</span><span class=\"fst\" id=\"st\">正在连接桌宠…</span></div>");

  // 桌宠屏幕控制卡（每个次界面都有，生日动画除外 —— 见上面 isBcake 的说明）
  if (!isBcake) {
    p += F("<div class=\"scr\">"
           "<div class=\"scrln\">这边是<b>遥控器</b>，真正的画面在<b>桌宠屏幕上</b>。"
           "下面的操作只改参数，按「显示到桌宠屏幕」才会推过去。</div>"
           "<div class=\"act\">"
           "<button class=\"ab push\" id=\"bpush\" onclick=\"applyView()\">&#9654; 显示到桌宠屏幕</button>"
           "<button class=\"ab pet\" onclick=\"backToPet()\">&#8962; 回到桌宠</button>"
           "</div></div>");
  }

  // 合并模块：两个页签
  if (isMerged) {
    p += F("<div class=\"tabs\">"
           "<div class=\"tab on\" id=\"tabS\" onclick=\"showTab('sched')\">倒计时提醒</div>"
           "<div class=\"tab\" id=\"tabB\" onclick=\"showTab('bday')\">纪念日庆祝</div>"
           "</div>");
  }

  // 面板挂载点
  // 【本次修复】原实现把挂载点写死成 "<div id=\"schedSlot\">"，
  // 导致表情/情绪/互动/主动互动/睡眠这 5 个次界面里没有自己的挂载点
  // （面板 JS 找的是 exprSlot / emoSlot / iaSlot / proSlot / sleepSlot，
  //  取不到 document.getElementById(...) 就直接 return，页面于是空白）。
  // 只有「日程与纪念日」碰巧正常 —— 因为它的挂载点正好就是 schedSlot。
  // 现在改为用 featureSlot(v) 生成对应挂载点：
  //   · 普通功能：单个 pane + 该功能的 slot；
  //   · 合并模块(VIEW_SCHEDULE)：featureSlot() 返回 schedSlot，
  //     再额外挂一个 paneB/bdaySlot 给「纪念日」页签（值不变，行为与之前一致）。
  p += F("<div class=\"pane on\" id=\"paneS\"><div id=\"");
  p += featureSlot(v);
  p += F("\"></div>");
  if (isMerged) {
    // 合并模块：第二个面板（纪念日）挂在自己的 pane 里，靠页签切换显示
    p += F("</div><div class=\"pane\" id=\"paneB\"><div id=\"bdaySlot\"></div></div>");
  } else {
    p += F("</div>");
  }

  // 交互脚本：
  //  autoEnter()  —— 进页面就把设备切到该功能的视图（失败会自动重试 6 次，
  //                 这样 WiFi 刚重连上时不用手动刷新页面）
  //  applyView()  —— 「显示到桌宠屏幕」：把当前页签对应的视图推给设备
  //  backToPet()  —— 「回到桌宠」：切回桌宠本体画面
  p += F("<script>"
         "var V=");
  p += v;
  p += F(",TAB='sched',tries=0;"
         "function st(t,cls){var e=document.getElementById('st');if(e){e.textContent=t;"
         "e.className='fst'+(cls?' '+cls:'');}}"
         "function ping(ok){st(ok?'已连接桌宠':'桌宠未响应，重试中…',ok?'ok':'bad');}"
         "function autoEnter(){fetch('/enter?v='+V).then(function(r){if(!r.ok)throw 0;ping(1);})"
         "[\"catch\"](function(){ping(0);if(++tries<6)setTimeout(autoEnter,1500);});}"
         "function applyView(){var v=(V==10)?(TAB=='bday'?11:10):V;"
         "fetch('/enter?v='+v).then(function(r){if(!r.ok)throw 0;ping(1);})"
         "[\"catch\"](function(){ping(0);});}"
         "function backToPet(){fetch('/back').then(function(){ping(1);})[\"catch\"](function(){ping(0);});}"
         "function showTab(k){TAB=k;var s=document.getElementById('paneS'),b=document.getElementById('paneB'),"
         "ts=document.getElementById('tabS'),tb=document.getElementById('tabB');"
         "if(s)s.className='pane'+(k=='sched'?' on':'');"
         "if(b)b.className='pane'+(k=='bday'?' on':'');"
         "if(ts)ts.className='tab'+(k=='sched'?' on':'');"
         "if(tb)tb.className='tab'+(k=='bday'?' on':'');"
         "applyView();}"
         "setTimeout(autoEnter,150);"
         "</script>");

  // 模块自己的面板脚本（合并模块要挂两份）
  p += F("<script defer src=\"/");
  p += key;
  p += F(".js\"></script>");
  if (isMerged) {
    p += F("<script defer src=\"/bday.js\"></script>");
  }
  p += F("</body></html>");

  server.send(200, "text/html; charset=utf-8", p);
}

// 【问题 4】把桌宠屏幕切回桌宠本体画面（正常眼睛 + 晃动）。
// 手机点「回到桌宠」时调用，避免设备一直停在功能调试屏上。
void routeBackToPet() {
  server.send(200, "application/json", "{\"ok\":1}");   // 先回包，再整屏重绘
  termMode   = false;
  currentView = VIEW_EYES_NORMAL;
  animNormalEyes();
  Serial.println("[HTTP] /back → 桌宠屏幕切回本体画面");
}

void routeEnter() {
  const int v = server.hasArg("v") ? server.arg("v").toInt() : -1;
  server.send(200, "application/json", "{\"ok\":1}");   // 先回包，再整屏重绘
  termMode = false;   // 从终端切到功能视图时清掉 termMode，否则 /char 还会往终端写
  switch (v) {
    case VIEW_EXPRESSION:  if (currentView != VIEW_EXPRESSION)  exprEnterView();        break;
    case VIEW_EMOTION:     if (currentView != VIEW_EMOTION)     emotionEnterView();     break;
    case VIEW_INTERACTION: if (currentView != VIEW_INTERACTION) interactionEnterView(); break;
    case VIEW_PROACTIVE:   if (currentView != VIEW_PROACTIVE)   proactiveEnterView();   break;
    case VIEW_SLEEP:       if (currentView != VIEW_SLEEP)       sleepEnterView();       break;
    case VIEW_SCHEDULE:    if (currentView != VIEW_SCHEDULE)    scheduleEnterView();    break;
    case VIEW_BIRTHDAY:    if (currentView != VIEW_BIRTHDAY)    birthdayEnterView();    break;
    case VIEW_BDCAKE:      if (currentView != VIEW_BDCAKE)      bdcakeEnterView();      break;
  }
}

void routeCanvas() {
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  if (on) { currentView = VIEW_DRAW; tft.fillScreen(drawBgColor); }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawClear() {
  const String bg = server.hasArg("bg") ? server.arg("bg") : "#aa4818";
  drawBgColor = hexToRgb565(bg);
  animBgColor = drawBgColor;  // keep in sync
  currentView = VIEW_DRAW; termMode = false;
  tft.fillScreen(drawBgColor);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawStroke() {
  if (!server.hasArg("pts") || !server.hasArg("pen")) {
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }
  const uint16_t color = hexToRgb565(server.arg("pen"));
  const String   data  = server.arg("pts");
  currentView = VIEW_DRAW;

  struct Pt { int16_t x, y; };
  Pt prev = {-1, -1};
  int start = 0;
  while (start < (int)data.length()) {
    int semi = data.indexOf(';', start);
    if (semi == -1) semi = data.length();
    String entry = data.substring(start, semi);
    const int comma = entry.indexOf(',');
    if (comma > 0) {
      const int16_t x = entry.substring(0, comma).toInt();
      const int16_t y = entry.substring(comma + 1).toInt();
      if (prev.x >= 0) {
        tft.drawLine(prev.x, prev.y, x, y, color);
        tft.drawLine(prev.x + 1, prev.y, x + 1, y, color);
        tft.drawLine(prev.x, prev.y + 1, x, y + 1, color);
      } else {
        tft.fillCircle(x, y, 2, color);
      }
      prev = {x, y};
    }
    start = semi + 1;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeBacklight() {
  setBacklight(server.hasArg("on") && server.arg("on") == "1");
  server.send(200, "application/json", "{\"ok\":1}");
}

// /time?e=<epochSec>&tz=<offsetSec> — sync the clock from the web page
void routeTime() {
  if (!server.hasArg("e")) {
    server.send(400, "application/json", "{\"e\":1}"); return;
  }
  bootEpochSec = server.arg("e").toInt();
  tzOffsetSec  = server.hasArg("tz") ? server.arg("tz").toInt() : 0;
  bootMs       = millis();
  timeSynced   = true;
  lastDayKey   = -1;
  if (currentView == VIEW_CLOCK) drawClockFull();
  server.send(200, "application/json", "{\"ok\":1}");
}

// Convert RGB565 back to #RRGGBB for state endpoint
String rgb565ToHex(uint16_t c) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5)  & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

void routeState() {
  String j = "{\"view\":"; j += currentView;
  j += ",\"busy\":";   j += busy        ? "true" : "false";
  j += ",\"term\":";   j += termMode    ? "true" : "false";
  j += ",\"bl\":";     j += backlightOn ? "true" : "false";
  j += ",\"speed\":";  j += animSpeed;
  j += ",\"expr\":";   j += (int)getCurrentExpression();
  j += ",\"anim\":";   j += (int)getExpressionAnimation();
  j += ",\"locked\":"; j += isExpressionLocked() ? "true" : "false";
  j += "}";
  server.send(200, "application/json", j);
}

// ═════════════════════════════════════════════════════════════
//  EXPRESSION ROUTES — 第一阶段新增
// ═════════════════════════════════════════════════════════════

// 统一回包：当前表情 / 动作 / 锁定状态
void exprSendState() {
  String j = "{\"expr\":";  j += (int)getCurrentExpression();
  j += ",\"anim\":";        j += (int)getExpressionAnimation();
  j += ",\"locked\":";      j += isExpressionLocked() ? "true" : "false";
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);
}

// /expr?id=<n> —— 点击某个表情：立即切换 + 进入表情视图
void routeExpr() {
  if (!server.hasArg("id")) { server.send(400, "application/json", "{\"e\":1}"); return; }
  const int id = server.arg("id").toInt();
  if (id < 0 || id >= EXPR_COUNT) { server.send(400, "application/json", "{\"e\":1}"); return; }

  termMode = false;
  const bool alreadyInExpr = (currentView == VIEW_EXPRESSION);
  setExpression((ExpressionId)id);   // 已在表情视图时内部直接重绘；否则只置脏
  if (!alreadyInExpr) exprEnterView();   // 切视图 + 整屏重绘（全程只画一帧）
  exprSendState();
}

// /expr/lock?on=0|1
void routeExprLock() {
  if (server.hasArg("on") && server.arg("on") == "1") {
    lockExpression();
  } else {
    unlockExpression();
    emotionApply();          // 解锁后立刻按当前情绪恢复表情（第二阶段接入）
  }
  exprSendState();
}

// /expr/anim?v=<n> —— 只换动作，保持当前表情
void routeExprAnim() {
  if (server.hasArg("v")) {
    const int v = server.arg("v").toInt();
    if (v >= 0 && v < ANIM_COUNT) setExpressionAnimation((AnimId)v);
  }
  exprSendState();
}

// /expr/list —— 表情清单（EXPR_DEFS 是唯一出处，加表情只改那一个文件）
void routeExprList() {
  String j = "[";
  for (uint8_t i = 0; i < EXPR_COUNT; i++) {
    const ExpressionDef& d = getExpressionDef((ExpressionId)i);
    if (i) j += ",";
    j += "{\"id\":";        j += (int)i;
    j += ",\"key\":\"";     j += d.key;
    j += "\",\"label\":\""; j += d.label;
    j += "\",\"glyph\":\""; j += d.glyph;
    j += "\",\"anim\":\"";  j += exprAnimKeyOf((AnimId)d.anim);
    j += "\"}";
  }
  j += "]";
  server.send(200, "application/json; charset=utf-8", j);
}

// /expr.css 与 /expr.js —— 表情管理界面的前端资源
// 显式带 charset：这两个文件里有中文，不能靠浏览器猜编码
void routeExprCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", EXPR_UI_CSS);
}

void routeExprJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", EXPR_UI_JS);
}

// ═════════════════════════════════════════════════════════════
//  EMOTION ROUTES — 第二阶段新增
// ═════════════════════════════════════════════════════════════

// /emo/list —— 当前情绪 + 七个值 + 基线 / 步长（网页面板靠它渲染）
void routeEmoList() {
  String j = "{\"emotion\":"; j += (int)getCurrentEmotion();
  j += ",\"key\":\"";     j += emotionKey(getCurrentEmotion());
  j += "\",\"label\":\""; j += emotionLabel(getCurrentEmotion());
  j += "\",\"locked\":";  j += isExpressionLocked() ? "true" : "false";

  j += ",\"values\":[";
  for (uint8_t i = 0; i < EV_COUNT; i++) {
    if (i) j += ",";
    j += (int)getEmotionValue((EmotionValue)i);
  }
  j += "],\"keys\":[";
  for (uint8_t i = 0; i < EV_COUNT; i++) {
    if (i) j += ",";
    j += "\""; j += emotionValueKey((EmotionValue)i); j += "\"";
  }
  j += "],\"labels\":[";
  for (uint8_t i = 0; i < EV_COUNT; i++) {
    if (i) j += ",";
    j += "\""; j += emotionValueLabel((EmotionValue)i); j += "\"";
  }
  j += "],\"base\":[";
  for (uint8_t i = 0; i < EV_COUNT; i++) {
    if (i) j += ",";
    j += (int)emotionValueBaseline((EmotionValue)i);
  }
  j += "],\"step\":[";
  for (uint8_t i = 0; i < EV_COUNT; i++) {
    if (i) j += ",";
    j += (int)emotionValueStep((EmotionValue)i);
  }
  j += "]}";
  server.send(200, "application/json; charset=utf-8", j);
}

// /emo/add?v=<0..6>&d=<delta> —— 加减某个情绪值
void routeEmoAdd() {
  if (server.hasArg("v") && server.hasArg("d")) {
    const int v = server.arg("v").toInt();
    const int d = server.arg("d").toInt();
    if (v >= 0 && v < EV_COUNT)
      addEmotion((EmotionValue)v, (int8_t)constrain(d, -100, 100));
  }
  showFaceIfNeeded();     // 新增：把表情推到桌宠屏幕（否则设备还停在情绪调试屏上看不出变化）
  routeEmoList();
}

// /emo/set?e=<0..9> —— 强制指定情绪（压制自动判定 5 秒，仅供调试）
void routeEmoSet() {
  if (server.hasArg("e")) setEmotionByIndex((uint8_t)server.arg("e").toInt());
  showFaceIfNeeded();     // 新增
  routeEmoList();
}

// /emo/reset —— 七个值全部回到基线，情绪回到 CALM
void routeEmoReset() {
  resetEmotion();
  showFaceIfNeeded();     // 新增
  routeEmoList();
}

// /emo/view —— 在设备 240×240 上切到情绪调试屏
void routeEmoView() {
  emotionEnterView();
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeEmoCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", EMO_UI_CSS);
}

void routeEmoJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", EMO_UI_JS);
}

// ═════════════════════════════════════════════════════════════
//  INTERACTION ROUTES — 第三阶段新增
// ═════════════════════════════════════════════════════════════

// /touch?e=down|up —— 网页「戳一下」的按下 / 松开分开送，所以网页也能识别长按
void routeTouch() {
  const String e = server.arg("e");
  if (e == "down")      interactionPress();
  else if (e == "up")   interactionRelease();
  else                  interactionClick();
  server.send(200, "application/json", "{\"ok\":1}");
}

// /ia/list —— 互动状态（网页面板靠它渲染）
void routeIaList() {
  String j = "{\"last\":\"";     j += interactionKey(getLastInteraction());
  j += "\",\"lastLabel\":\"";    j += interactionLabel(getLastInteraction());
  j += "\",\"clicks\":";         j += (int)getClickCount();
  j += ",\"total\":";            j += (int)getTotalClicks();
  j += ",\"idle\":";             j += (unsigned long)getIdleTime();
  j += ",\"pressed\":";          j += isPressed()          ? "true" : "false";
  j += ",\"ever\":";             j += hasUserInteracted()  ? "true" : "false";
  j += ",\"emotion\":\"";        j += emotionKey(getCurrentEmotion());
  j += "\",\"emotionLabel\":\""; j += emotionLabel(getCurrentEmotion());
  j += "\",\"feedback\":";       j += (int)getFeedbackLeft();
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);
}

// /ia/test?e=1..4 —— 注入一个测试事件（1=单击 2=连击×3 3=过度×8 4=长按）
void routeIaTest() {
  if (server.hasArg("e")) {
    const int e = server.arg("e").toInt();
    if (e >= 1 && e < IA_COUNT) interactionInject((InteractionEvent)e);
  }
  routeIaList();
}

void routeIaReset() {
  interactionReset();
  routeIaList();
}

// /ia/view —— 在设备 240×240 上切到互动调试屏
void routeIaView() {
  interactionEnterView();
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeIaCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", IA_UI_CSS);
}

void routeIaJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", IA_UI_JS);
}

// ═════════════════════════════════════════════════════════════
//  PROACTIVE ROUTES — 第四阶段新增
// ═════════════════════════════════════════════════════════════

// /pro/list —— 主动互动状态（网页面板靠它渲染）
void routeProList() {
  String j = "{\"enabled\":";    j += isProactiveEnabled() ? "true" : "false";
  j += ",\"action\":\"";         j += proactiveKey(getProactiveAction());
  j += "\",\"actionLabel\":\"";  j += proactiveLabel(getProactiveAction());
  j += "\",\"next\":\"";         j += proactiveKey(getNextPlannedAction());
  j += "\",\"nextLabel\":\"";    j += proactiveLabel(getNextPlannedAction());
  j += "\",\"idle\":";           j += (unsigned long)getIdleTime();
  j += ",\"triggerIn\":";        j += (unsigned long)getNextTriggerIn();
  j += ",\"sinceLast\":";        j += (unsigned long)getSinceLastProactive();
  j += ",\"everFired\":";        j += hasProactiveEverFired() ? "true" : "false";
  j += ",\"locked\":";           j += isExpressionLocked() ? "true" : "false";
  j += ",\"emotion\":\"";        j += emotionKey(getCurrentEmotion());
  j += "\",\"emotionLabel\":\""; j += emotionLabel(getCurrentEmotion());
  j += "\"}";
  server.send(200, "application/json; charset=utf-8", j);
}

// /pro/set?on=0|1 —— 总开关
void routeProSet() {
  if (server.hasArg("on") && server.arg("on") == "1") enableProactive();
  else                                               disableProactive();
  routeProList();
}

// /pro/trigger —— 立刻触发一次（调试）
void routeProTrigger() {
  const bool started = proactiveTriggerNow();
  if (started) showFaceIfNeeded();   // 新增：主动行为要在脸上看得见
  String j = "{\"started\":"; j += started ? "true" : "false"; j += "}";
  server.send(200, "application/json", j);
}

// /pro/reset —— 重掷下一次触发时间
void routeProReset() {
  proactiveResetTimer();
  routeProList();
}

// /pro/view —— 在设备 240×240 上切到主动互动调试屏
void routeProView() {
  proactiveEnterView();
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeProCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", PRO_UI_CSS);
}

void routeProJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", PRO_UI_JS);
}

// ═════════════════════════════════════════════════════════════
//  SLEEP ROUTES — 第五阶段新增
// ═════════════════════════════════════════════════════════════

// /sleep/list —— 睡眠状态（网页面板靠它渲染）
void routeSlList() {
  char clk[24];
  if (!isTimeSynced()) {
    snprintf(clk, sizeof clk, "--:--");
  } else {
    snprintf(clk, sizeof clk, "%02u:%02u",
             (unsigned)getSleepHour(), (unsigned)getSleepMinute());
  }

  char tt[8] = "";
  uint8_t th, tm;
  if (getSleepTestTime(th, tm)) snprintf(tt, sizeof tt, "%02u:%02u", (unsigned)th, (unsigned)tm);

  String j = "{\"state\":";        j += (int)getSleepState();
  j += ",\"stateKey\":\"";         j += sleepStateKey(getSleepState());
  j += "\",\"stateLabel\":\"";     j += sleepStateLabel(getSleepState());
  j += "\",\"reasonKey\":\"";      j += sleepReasonKey(getSleepReason());
  j += "\",\"reasonLabel\":\"";    j += sleepReasonLabel(getSleepReason());
  j += ",\"sleeping\":";           j += isSleeping()     ? "true" : "false";
  j += ",\"drowsy\":";             j += isDrowsy()       ? "true" : "false";
  j += ",\"waking\":";             j += isWaking()       ? "true" : "false";
  j += ",\"enabled\":";            j += isSleepEnabled() ? "true" : "false";
  j += ",\"testMode\":";           j += isSleepTestMode() ? "true" : "false";
  j += ",\"night\":";              j += isNightTimeNow() ? "true" : "false";
  j += ",\"synced\":";             j += isTimeSynced()   ? "true" : "false";
  j += ",\"clock\":\"";            j += clk;
  j += "\",\"testTime\":\"";       j += tt;
  j += "\",\"idle\":";             j += (unsigned long)getSleepIdleTime();
  j += ",\"drowsyIn\":";           j += (unsigned long)getDrowsyIn();
  j += ",\"sleepIn\":";            j += (unsigned long)getSleepIn();
  j += ",\"cooldown\":";           j += (unsigned long)getWakeCooldownLeft();
  j += ",\"locked\":";             j += isExpressionLocked()      ? "true" : "false";
  j += ",\"proPaused\":";          j += isProactiveSleeping()     ? "true" : "false";
  j += ",\"emoPaused\":";          j += isEmotionApplyPaused()    ? "true" : "false";
  j += "}";
  // 本次修改：加 no-store，避免手机浏览器缓存状态 JSON（否则面板可能一直显示旧值）
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send(200, "application/json; charset=utf-8", j);
}

// /sleep/set?on=0|1 —— 睡眠总开关
void routeSlSet() {
  if (server.hasArg("on") && server.arg("on") == "1") enableSleep();
  else                                               disableSleep();
  routeSlList();
}

// /sleep/test?on=0|1 —— 测试档（60s 犯困 / 80s 睡着，省得真等 10 分钟）
void routeSlTest() {
  sleepSetTestMode(server.hasArg("on") && server.arg("on") == "1");
  routeSlList();
}

// /sleep/now —— 立刻睡着；顺手切到表情视图，好直接看睡颜
void routeSlNow() {
  forceSleep();
  showFaceIfNeeded();     // 统一走这个helper：尊重表情锁、不抢终端/画板/时钟
  routeSlList();
}

// /sleep/drowsy —— 立刻犯困
void routeSlDrowsy() {
  forceDrowsy();
  showFaceIfNeeded();
  routeSlList();
}

// /sleep/wake —— 唤醒
void routeSlWake() {
  wakeUp();
  showFaceIfNeeded();     // 新增：唤醒后也要能看到表情变化
  routeSlList();
}

// /sleep/reset —— 清唤醒冷却 + 重新开始计时
void routeSlReset() {
  sleepResetTimer();
  routeSlList();
}

// /sleep/time?h=23&m=0 或 ?clear=1
// 模拟时间：只为测跨午夜判断，**不动真实的时间系统**
void routeSlTime() {
  if (server.hasArg("clear")) {
    sleepClearTestTime();
  } else if (server.hasArg("h")) {
    sleepSetTestTime((int8_t)server.arg("h").toInt(),
                     server.hasArg("m") ? (int8_t)server.arg("m").toInt() : (int8_t)0);
  }
  routeSlList();
}

// /sleep/view —— 在设备 240×240 上切到睡眠调试屏
void routeSlView() {
  sleepEnterView();
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeSlCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", SL_UI_CSS);
}

void routeSlJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", SL_UI_JS);
}

// ═════════════════════════════════════════════════════════════
//  SCHEDULE ROUTES — 第六阶段新增
// ═════════════════════════════════════════════════════════════

static const char* gScErr = "";   // 上一次操作的错误码，随 /sched/list 回给网页

static EventType schedArgType() {
  if (!server.hasArg("type")) return SEV_GENERAL;
  const String t = server.arg("type");
  if (t == "exam")        return SEV_EXAM;
  if (t == "competition") return SEV_COMPETITION;
  if (t == "task")        return SEV_TASK;
  if (t == "birthday")    return SEV_BIRTHDAY;
  if (t == "anniversary") return SEV_ANNIVERSARY;
  return SEV_GENERAL;
}

// 标题可能带引号 / 反斜杠，拼 JSON 前必须转义（UTF-8 字节原样透传）
static void schedAppendEsc(String &out, const char* s) {
  if (!s) return;
  for (const char* p = s; *p; p++) {
    const char c = *p;
    if (c == '"' || c == '\\')   { out += '\\'; out += c; }
    else if (c == '\n')          { out += "\\n"; }
    else if (c == '\r')          { out += "\\r"; }
    else if (c == '\t')          { out += "\\t"; }
    else if ((uint8_t)c >= 0x20) { out += c; }
  }
}

void routeScList() {
  String j = "{";
  j += "\"synced\":";     j += isTimeReady() ? "true" : "false";
  j += ",\"now\":";       j += getScheduleNow();
  int32_t tt = 0;
  const bool hasT = getScheduleTestTime(tt);
  j += ",\"testTime\":";  j += hasT ? tt : 0;
  j += ",\"count\":";     j += getEventCount();
  j += ",\"max\":";       j += SCH_MAX_EVENTS;
  j += ",\"saveCount\":"; j += (unsigned long)getScheduleSaveCount();
  j += ",\"storage\":";   j += isScheduleStorageReady() ? "true" : "false";
  j += ",\"pageCount\":"; j += getSchedulePageCount();
  j += ",\"mode\":";      j += (int)getScheduleMode();
  j += ",\"err\":\"";     j += gScErr;  j += "\"";

  const ScheduleEvent* nx = getNextEvent();
  j += ",\"nextTitle\":\"";
  if (nx) schedAppendEsc(j, nx->title);
  j += "\"";

  j += ",\"events\":[";
  const int n = getEventCount();
  for (int i = 0; i < n; i++) {
    const ScheduleEvent* ev = getEventAt(i);
    if (!ev) continue;
    if (i) j += ',';
    const EventState st = getEventStateAt(i);
    uint16_t d; uint8_t h, mi, s;
    getCountdownParts(getCountdownAt(i), d, h, mi, s);

    j += "{\"id\":";            j += ev->id;
    j += ",\"title\":\"";       schedAppendEsc(j, ev->title);
    j += "\",\"type\":\"";      j += eventTypeKey((EventType)ev->type);
    j += "\",\"typeLabel\":\""; j += eventTypeLabel((EventType)ev->type);
    j += "\",\"target\":";      j += ev->targetEpoch;
    j += ",\"remain\":";        j += getCountdownAt(i);
    j += ",\"state\":\"";       j += eventStateKey(st);
    j += "\",\"stateLabel\":\""; j += eventStateLabel(st);
    j += "\",\"enabled\":";     j += ev->enabled ? "true" : "false";
    j += ",\"done\":";          j += isEventDone(ev->id) ? "true" : "false";
    j += ",\"urgent\":";        j += isEventUrgent(ev->id) ? "true" : "false";
    j += ",\"days\":";          j += d;
    j += ",\"hours\":";         j += h;
    j += ",\"mins\":";          j += mi;
    j += ",\"secs\":";          j += s;
    j += "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// /sched/add?in=<sec>&title=&type=          （N 秒之后，测试用）
// /sched/add?y=&mo=&d=&h=&mi=&title=&type=  （绝对日期）
void routeScAdd() {
  const String title = server.hasArg("title") ? server.arg("title") : String("EVENT");
  const EventType ty = schedArgType();
  int idx = -1;

  if (server.hasArg("in")) {
    idx = addEventIn(title.c_str(), (int32_t)server.arg("in").toInt(), ty);
    gScErr = (idx >= 0) ? "" : (isTimeReady() ? "full" : "no_time");
  } else {
    idx = addEventYMD(title.c_str(),
                      server.arg("y").toInt(),  server.arg("mo").toInt(),
                      server.arg("d").toInt(),
                      server.hasArg("h")  ? server.arg("h").toInt()  : 0,
                      server.hasArg("mi") ? server.arg("mi").toInt() : 0,
                      ty);
    gScErr = (idx >= 0) ? "" : "bad_time";
  }
  routeScList();
}

// /sched/upd?id=&title=&type=&y=&mo=&d=&h=&mi=
void routeScUpd() {
  const uint32_t id = (uint32_t)server.arg("id").toInt();
  const String title = server.arg("title");
  const EventType ty = schedArgType();
  const int y  = server.arg("y").toInt();
  const int mo = server.arg("mo").toInt();
  const int d  = server.arg("d").toInt();
  const int h  = server.hasArg("h")  ? server.arg("h").toInt()  : 0;
  const int mi = server.hasArg("mi") ? server.arg("mi").toInt() : 0;

  bool ok = false;
  if (mo >= 1 && mo <= 12 && d >= 1 && d <= daysInMonth(y, mo) &&
      h >= 0 && h <= 23 && mi >= 0 && mi <= 59) {
    ok = updateEvent(id, title.length() ? title.c_str() : NULL,
                     scheduleMakeEpoch(y, mo, d, h, mi), ty);
  }
  gScErr = ok ? "" : "bad_time";
  routeScList();
}

void routeScDel() {
  gScErr = removeEvent((uint32_t)server.arg("id").toInt()) ? "" : "not_found";
  routeScList();
}

void routeScEn() {
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  gScErr = setEventEnabled((uint32_t)server.arg("id").toInt(), on) ? "" : "not_found";
  routeScList();
}

void routeScDone() {
  const uint32_t id = (uint32_t)server.arg("id").toInt();
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  gScErr = (on ? markEventDone(id) : markEventUndone(id)) ? "" : "not_found";
  routeScList();
}

void routeScClear() {
  clearAllEvents();
  gScErr = "";
  routeScList();
}

void routeScAlerts() {
  scheduleResetAlerts();
  gScErr = "";
  routeScList();
}

// /sched/time?now=1 | ?add=<sec> | ?e=<epoch> | ?clear=1
// 只动 ScheduleManager 的测试时钟，**不碰主固件的时间系统**
void routeScTime() {
  if (server.hasArg("clear")) {
    scheduleClearTestTime();
    gScErr = "";
  } else if (!isTimeReady()) {
    gScErr = "no_time";
  } else if (server.hasArg("now")) {
    scheduleSetTestTime(getScheduleNow());
    gScErr = "";
  } else if (server.hasArg("add")) {
    scheduleSetTestTime(getScheduleNow() + (int32_t)server.arg("add").toInt());
    gScErr = "";
  } else if (server.hasArg("e")) {
    scheduleSetTestTime((int32_t)server.arg("e").toInt());
    gScErr = "";
  } else {
    gScErr = "bad_arg";
  }
  routeScList();
}

// /sched/view            进入日程视图
// /sched/view?m=0|1      切列表页 / 倒计时详情页
void routeScView() {
  if (currentView != VIEW_SCHEDULE) scheduleEnterView();
  if (server.hasArg("m")) scheduleSetMode((uint8_t)server.arg("m").toInt());
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeScCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", SCH_UI_CSS);
}

void routeScJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", SCH_UI_JS);
}

// ═════════════════════════════════════════════════════════════
//  BIRTHDAY ROUTES — 第七阶段新增
// ═════════════════════════════════════════════════════════════

static const char* gBdErr = "";   // 上一次操作的错误码，随 /bday/list 回给网页

static SpecialDayType bdArgType() {
  if (!server.hasArg("type")) return SDT_BIRTHDAY;
  const int t = server.arg("type").toInt();
  if (t < 0 || t >= (int)SDT_TYPE_COUNT) return SDT_BIRTHDAY;
  return (SpecialDayType)t;
}

void routeBdList() {
  String j = "{";
  j += "\"timeReady\":"; j += isTimeReady() ? "true" : "false";

  char b[16];
  int ty, tmo, td;
  if (getTodayDate(ty, tmo, td)) snprintf(b, sizeof b, "%04d-%02d-%02d", ty, tmo, td);
  else                           snprintf(b, sizeof b, "--");
  j += ",\"today\":\"";    j += b; j += "\"";

  b[0] = 0;
  if (getTestDate(ty, tmo, td)) snprintf(b, sizeof b, "%04d-%02d-%02d", ty, tmo, td);
  j += ",\"testDate\":\""; j += b; j += "\"";

  j += ",\"count\":";      j += getSpecialDayCount();
  j += ",\"max\":";        j += SD_MAX;
  j += ",\"saveCount\":";  j += (unsigned long)getBirthdaySaveCount();
  j += ",\"storage\":";    j += isBirthdayStorageReady() ? "true" : "false";
  j += ",\"celePhase\":\""; j += celePhaseKey((CelePhase)getCelePhase()); j += "\"";
  j += ",\"err\":\"";      j += gBdErr; j += "\"";

  const int ni = getUpcomingSpecialDayIndex();
  j += ",\"nextTitle\":\"";
  if (ni >= 0) {
    const SpecialDay* sd = getSpecialDayAt(ni);
    if (sd) schedAppendEsc(j, sd->title);
  }
  j += "\",\"nextDays\":";
  j += (ni >= 0) ? getDaysUntilNext(ni) : -1;

  j += ",\"days\":[";
  const int n = getSpecialDayCount();
  for (int i = 0; i < n; i++) {
    const SpecialDay* sd = getSpecialDayAt(i);
    if (!sd) continue;
    if (i) j += ',';
    const SpecialDayState st = getSpecialDayState(i);
    j += "{\"id\":";             j += sd->id;
    j += ",\"title\":\"";        schedAppendEsc(j, sd->title);
    j += "\",\"type\":";         j += (int)sd->type;
    j += ",\"typeLabel\":\"";    j += sdTypeLabel((SpecialDayType)sd->type); j += "\"";
    j += ",\"month\":";          j += (int)sd->month;
    j += ",\"day\":";            j += (int)sd->day;
    j += ",\"year\":";           j += (unsigned)sd->year;
    j += ",\"enabled\":";        j += sd->enabled ? "true" : "false";
    j += ",\"state\":\"";        j += sdStateKey(st);
    j += "\",\"stateLabel\":\""; j += sdStateLabel(st); j += "\"";
    j += ",\"daysUntil\":";      j += getDaysUntilNext(i);
    j += ",\"daysSince\":";      j += getDaysSinceLast(i);
    j += ",\"years\":";          j += getAnniversaryYears(i);
    j += ",\"occYear\":";        j += (unsigned)getOccurrenceYear(i);
    j += ",\"stageMask\":";      j += (int)sd->stageMask;
    j += "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// /bday/add?type=&title=&mo=&d=&y=
void routeBdAdd() {
  const String title = server.hasArg("title") ? server.arg("title") : String("");
  const int idx = addSpecialDay(title.c_str(),
                                (uint8_t)server.arg("mo").toInt(),
                                (uint8_t)server.arg("d").toInt(),
                                (uint16_t)server.arg("y").toInt(),
                                bdArgType());
  gBdErr = (idx >= 0) ? "" : ((getSpecialDayCount() >= SD_MAX) ? "full" : "bad_date");
  routeBdList();
}

// /bday/upd?id=&type=&title=&mo=&d=&y=
void routeBdUpd() {
  const String title = server.arg("title");
  const bool ok = updateSpecialDay((uint32_t)server.arg("id").toInt(),
                                   title.length() ? title.c_str() : NULL,
                                   (uint8_t)server.arg("mo").toInt(),
                                   (uint8_t)server.arg("d").toInt(),
                                   (uint16_t)server.arg("y").toInt(),
                                   bdArgType());
  gBdErr = ok ? "" : "bad_date";
  routeBdList();
}

void routeBdDel() {
  gBdErr = removeSpecialDay((uint32_t)server.arg("id").toInt()) ? "" : "not_found";
  routeBdList();
}

void routeBdEn() {
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  gBdErr = setSpecialDayEnabled((uint32_t)server.arg("id").toInt(), on) ? "" : "not_found";
  routeBdList();
}

void routeBdClear() {
  clearAllSpecialDays();
  gBdErr = "";
  routeBdList();
}

void routeBdReset() {
  clearSpecialDayTriggers();
  gBdErr = "";
  routeBdList();
}

// /bday/test?kind=birthday|anniv|d7|d3|d1
// 测试按钮会先把视图切到表情视图，这样庆祝能立刻播出来（不然会挂起等条件）
void routeBdTest() {
  const String k = server.hasArg("kind") ? server.arg("kind") : String("");
  if (k == "birthday" || k == "anniv") {
    if (currentView == VIEW_CODE || currentView == VIEW_DRAW) exprEnterView();
  }
  if      (k == "birthday") triggerBirthdayTest();
  else if (k == "anniv")    triggerAnniversaryTest();
  else if (k == "d7")       triggerUpcomingTest(7);
  else if (k == "d3")       triggerUpcomingTest(3);
  else if (k == "d1")       triggerUpcomingTest(1);
  else                      gBdErr = "bad_kind";
  routeBdList();
}

// /bday/date?y=&mo=&d=  或  ?clear=1
// 只动 BirthdayManager 的测试日期，**不碰主固件的时间系统**
void routeBdDate() {
  if (server.hasArg("clear")) {
    clearTestDate();
    gBdErr = "";
  } else {
    const bool ok = setTestDate(server.arg("y").toInt(),
                                server.arg("mo").toInt(),
                                server.arg("d").toInt());
    gBdErr = ok ? "" : "bad_date";
  }
  routeBdList();
}

// /bday/preview?id= —— 不管今天是不是，直接播一次该事件的庆祝
void routeBdPreview() {
  const int i = findSpecialDayIndex((uint32_t)server.arg("id").toInt());
  if (i < 0) { gBdErr = "not_found"; routeBdList(); return; }
  if (currentView == VIEW_CODE || currentView == VIEW_DRAW) exprEnterView();
  const SpecialDay* sd = getSpecialDayAt(i);
  const int yrs = getAnniversaryYears(i);
  triggerCelebration((SpecialDayType)sd->type, sd->title,
                     (uint8_t)((yrs > 0 && yrs < 256) ? yrs : 0));
  gBdErr = "";
  routeBdList();
}

// /bday/view —— 在设备 240×240 上切到特殊日期屏
void routeBdView() {
  if (currentView != VIEW_BIRTHDAY) birthdayEnterView();
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeBdCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", BDAY_UI_CSS);
}

void routeBdJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", BDAY_UI_JS);
}

// ═══════════════════════════════════════════════════════════════
//  生日动画（R7 新增，VIEW_BDCAKE = 12；R8 加自诊断 + 手动推屏）
//  网页两个按钮 → 这三个接口 → 桌宠屏同步播放（画面上也会有同样的动作）
//   · /bc/wish   闭上眼睛许愿吧（3 秒后 WISH GRANTED）
//   · /bc/blow   吹蜡烛：烛火熄灭 + 气球彩带 + HAPPY BIRTHDAY
//   · /bc/reset  重新点亮蜡烛
//   · /bc/show   强制桌宠切到生日动画视图（白屏/没切过去时的手动救急按钮）
//   · /bc/state  桌宠侧健康状态（视图号 / 剩余堆 / 启动次数 / 复位原因 / fps）
//  回包一律先发，再动屏幕（避免客户端等重绘），与项目其它路由一致。
// ═══════════════════════════════════════════════════════════════

// 统一的健康状态 JSON：网页靠它判断「桌宠到底有没有进到生日动画视图」
// 字段：ok / f 烛火 / w 许愿中 / view 当前视图 / cv 画布就绪 / heap 剩余堆 /
//       hmin 历史最低堆 / up 运行秒数 / boot 启动次数 / rr 复位原因码 /
//       rrname 复位原因名 / fps / fms 整帧耗时
void bcSendState() {
  String j;
  j.reserve(260);
  j += "{\"ok\":1,\"f\":";
  j += bdcakeFlamesOn() ? 1 : 0;
  j += ",\"w\":";
  j += bdcakeIsWishing() ? 1 : 0;
  j += ",\"view\":";
  j += currentView;
  j += ",\"cv\":";
  j += bdcakeCanvasReady() ? 1 : 0;
  j += ",\"heap\":";
  j += (unsigned long)bdcakeHeap();
  j += ",\"hmin\":";
  j += (unsigned long)bdcakeHeapMin();
  j += ",\"up\":";
  j += (unsigned long)bdcakeUptimeSec();
  j += ",\"boot\":";
  j += (unsigned long)bdcakeBootCount();
  j += ",\"rr\":";
  j += (unsigned)bdcakeResetReason();
  j += ",\"rrname\":\"";
  j += bdcakeResetReasonName();
  j += "\",\"fps\":";
  j += (unsigned)bdcakeFps();
  j += ",\"fms\":";
  j += (unsigned)bdcakeFrameMs();
  j += "}";
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", j);
}

void routeBcWish() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":1}");
  bdcakeWish();
  Serial.println("[HTTP] /bc/wish → 桌宠：许愿");
}

void routeBcBlow() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":1}");
  bdcakeBlow();
  Serial.println("[HTTP] /bc/blow → 桌宠：吹蜡烛");
}

void routeBcReset() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":1}");
  bdcakeReset();
  Serial.println("[HTTP] /bc/reset → 桌宠：重新点亮蜡烛");
}

// /bc/show —— 「推到桌宠屏幕」：不管现在在哪个视图都重新初始化面板并重画一帧
// （面板被意外复位/断电抖成白屏时，这个按钮就是现场的「救屏」操作）
void routeBcShow() {
  bdcakeEnterView();                     // 内含 bcDisplayInit()：重跑 tft.init + 首帧
  Serial.printf("[HTTP] /bc/show → 桌宠：生日动画视图（view=%u cv=%d heap=%u）\n",
                (unsigned)currentView, bdcakeCanvasReady() ? 1 : 0,
                (unsigned)bdcakeHeap());
  bcSendState();
}

// 网页打开时把桌宠状态同步过来（手机上刷新页面不会「以为蜡烛还点着」）
void routeBcState() {
  bcSendState();
}

void routeBcCss() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/css; charset=utf-8", BC_UI_CSS);
}

void routeBcJs() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "application/javascript; charset=utf-8", BC_UI_JS);
}

void routeNotFound() {
  g_netReqCount++;
  Serial.printf("[HTTP] 404  %s  from %s\n",
                server.uri().c_str(), server.client().remoteIP().toString().c_str());
  server.send(404, "text/plain", "not found");
}
// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  // 新增：USB CDC On Boot 时，串口监视器打开之前打印的日志会丢失。
  // 这里最多等 1.2 秒（无监视器也照常继续启动，不会卡死设备），
  // 确保 [WiFi]/[HTTP] 的开机日志都能看到。
  for (uint8_t i = 0; i < 24 && !Serial; i++) delay(50);

  pinMode(TFT_BLK, OUTPUT);
  setBacklight(true);

  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  initColours();
  exprBegin();                    // 表情管理系统初始化（第一阶段新增）
  emotionBegin();                 // 情绪系统初始化（第二阶段新增）
  interactionBegin();             // 用户互动系统初始化（第三阶段新增）
  proactiveBegin();               // 随机主动互动初始化（第四阶段新增）
  sleepBegin();                   // 睡眠 / 唤醒初始化（第五阶段新增）
  scheduleBegin();                // 日程 / 倒计时初始化（第六阶段新增，含从 NVS 载入）
  birthdayBegin();                // 生日 / 纪念日初始化（第七阶段新增，含从 NVS 载入）
  bdcakeBegin();                  // 生日动画初始化（本轮新增：调色板 + 随机种子，不占屏）

  // ── Boot splash ────────────────────────────────────────────
  tft.fillScreen(animBgColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 - 22); tft.print("Clawd");
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 + 14); tft.print("Mochi");
  delay(1200);

  // ── Logo shown once at startup ─────────────────────────────
  animLogoReveal();

  // ── Start WiFi（本次修改：改用 wifiNetBegin 做稳定性加固）────
  //  旧写法：WiFi.softAP(AP_SSID, AP_PASS);  → 信道自动跳、功率过高、无日志
  wifiNetBegin();

  // ── WiFi info screen (stays until first web request) ───────
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_WHITE);  tft.setTextSize(2);
  tft.setCursor(12, 16);  tft.print("WiFi: ClaWD-Mochi");
  tft.setTextColor(C_MUTED);  tft.setTextSize(1);
  tft.setCursor(12, 44);  tft.print("password: clawd1234");
  tft.setTextColor(C_WHITE);  tft.setTextSize(2);
  tft.setCursor(12, 68);  tft.print("Open browser:");
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(12, 94);  tft.print("192.168.4.1");
  tft.setTextColor(C_MUTED);  tft.setTextSize(1);
  tft.setCursor(12, 124); tft.print("press any button to start");

  // ── Register routes ────────────────────────────────────────
  server.on("/",            HTTP_GET, routeRoot);
  server.on("/cmd",         HTTP_GET, routeCmd);
  server.on("/char",        HTTP_GET, routeChar);
  server.on("/speed",       HTTP_GET, routeSpeed);
  server.on("/redraw",      HTTP_GET, routeRedraw);
  server.on("/canvas",      HTTP_GET, routeCanvas);
  server.on("/draw/clear",  HTTP_GET, routeDrawClear);
  server.on("/draw/stroke", HTTP_GET, routeDrawStroke);
  server.on("/backlight",   HTTP_GET, routeBacklight);
  server.on("/time",        HTTP_GET, routeTime);
  server.on("/state",       HTTP_GET, routeState);
  // ── 统一功能入口（整合新增）──
  server.on("/enter",       HTTP_GET, routeEnter);
  server.on("/feature",     HTTP_GET, routeFeature);   // 每个新功能的独立操作界面
  // ── 表情管理系统（第一阶段新增）──
  server.on("/expr",        HTTP_GET, routeExpr);
  server.on("/expr/lock",   HTTP_GET, routeExprLock);
  server.on("/expr/anim",   HTTP_GET, routeExprAnim);
  server.on("/expr/list",   HTTP_GET, routeExprList);
  server.on("/expr.css",    HTTP_GET, routeExprCss);
  server.on("/expr.js",     HTTP_GET, routeExprJs);
  // ── 情绪系统（第二阶段新增）──
  server.on("/emo/list",    HTTP_GET, routeEmoList);
  server.on("/emo/add",     HTTP_GET, routeEmoAdd);
  server.on("/emo/set",     HTTP_GET, routeEmoSet);
  server.on("/emo/reset",   HTTP_GET, routeEmoReset);
  server.on("/emo/view",    HTTP_GET, routeEmoView);
  server.on("/emo.css",     HTTP_GET, routeEmoCss);
  server.on("/emo.js",      HTTP_GET, routeEmoJs);
  // ── 用户互动系统（第三阶段新增）──
  server.on("/touch",       HTTP_GET, routeTouch);
  server.on("/ia/list",     HTTP_GET, routeIaList);
  server.on("/ia/test",     HTTP_GET, routeIaTest);
  server.on("/ia/reset",    HTTP_GET, routeIaReset);
  server.on("/ia/view",     HTTP_GET, routeIaView);
  server.on("/ia.css",      HTTP_GET, routeIaCss);
  server.on("/ia.js",       HTTP_GET, routeIaJs);
  // ── 随机主动互动系统（第四阶段新增）──
  server.on("/pro/list",    HTTP_GET, routeProList);
  server.on("/pro/set",     HTTP_GET, routeProSet);
  server.on("/pro/trigger", HTTP_GET, routeProTrigger);
  server.on("/pro/reset",   HTTP_GET, routeProReset);
  server.on("/pro/view",    HTTP_GET, routeProView);
  server.on("/pro.css",     HTTP_GET, routeProCss);
  server.on("/pro.js",      HTTP_GET, routeProJs);
  // ── 睡眠 / 唤醒系统（第五阶段新增）──
  server.on("/sleep/list",   HTTP_GET, routeSlList);
  server.on("/sleep/set",    HTTP_GET, routeSlSet);
  server.on("/sleep/test",   HTTP_GET, routeSlTest);
  server.on("/sleep/now",    HTTP_GET, routeSlNow);
  server.on("/sleep/drowsy", HTTP_GET, routeSlDrowsy);
  server.on("/sleep/wake",   HTTP_GET, routeSlWake);
  server.on("/sleep/reset",  HTTP_GET, routeSlReset);
  server.on("/sleep/time",   HTTP_GET, routeSlTime);
  server.on("/sleep/view",   HTTP_GET, routeSlView);
  server.on("/sleep.css",    HTTP_GET, routeSlCss);
  server.on("/sleep.js",     HTTP_GET, routeSlJs);
  // ── 日程 / 倒计时系统（第六阶段新增）──
  server.on("/sched/list",   HTTP_GET, routeScList);
  server.on("/sched/add",    HTTP_GET, routeScAdd);
  server.on("/sched/upd",    HTTP_GET, routeScUpd);
  server.on("/sched/del",    HTTP_GET, routeScDel);
  server.on("/sched/en",     HTTP_GET, routeScEn);
  server.on("/sched/done",   HTTP_GET, routeScDone);
  server.on("/sched/clear",  HTTP_GET, routeScClear);
  server.on("/sched/alerts", HTTP_GET, routeScAlerts);
  server.on("/sched/time",   HTTP_GET, routeScTime);
  server.on("/sched/view",   HTTP_GET, routeScView);
  server.on("/sched.css",    HTTP_GET, routeScCss);
  server.on("/sched.js",     HTTP_GET, routeScJs);
  // ── 生日 / 纪念日系统（第七阶段新增）──
  server.on("/bday/list",    HTTP_GET, routeBdList);
  server.on("/bday/add",     HTTP_GET, routeBdAdd);
  server.on("/bday/upd",     HTTP_GET, routeBdUpd);
  server.on("/bday/del",     HTTP_GET, routeBdDel);
  server.on("/bday/en",      HTTP_GET, routeBdEn);
  server.on("/bday/clear",   HTTP_GET, routeBdClear);
  server.on("/bday/reset",   HTTP_GET, routeBdReset);
  server.on("/bday/test",    HTTP_GET, routeBdTest);
  server.on("/bday/date",    HTTP_GET, routeBdDate);
  server.on("/bday/preview", HTTP_GET, routeBdPreview);
  server.on("/bday/view",    HTTP_GET, routeBdView);
  server.on("/bday.css",     HTTP_GET, routeBdCss);
  server.on("/bday.js",      HTTP_GET, routeBdJs);
  // ── 生日动画（本轮新增，VIEW_BDCAKE = 12）──
  server.on("/bc/wish",      HTTP_GET, routeBcWish);
  server.on("/bc/blow",      HTTP_GET, routeBcBlow);
  server.on("/bc/reset",     HTTP_GET, routeBcReset);
  server.on("/bc/state",     HTTP_GET, routeBcState);
  server.on("/bc/show",      HTTP_GET, routeBcShow);
  server.on("/bc.css",       HTTP_GET, routeBcCss);
  server.on("/bc.js",        HTTP_GET, routeBcJs);
  server.on("/net",         HTTP_GET, routeNet);        // 新增：网络诊断页
  server.on("/back",        HTTP_GET, routeBackToPet);  // 新增（问题 4）：桌宠屏幕切回本体画面
  (void)featureTftName(VIEW_SCHEDULE);                  // 保留：桌宠屏幕用英文名的唯一出处
  server.onNotFound(routeNotFound);
  server.begin();
  server.enableDelay(false);   // 新增：请求处理不再 delay(1)，减少 WebServer 阻塞
  Serial.println("[HTTP] 服务器已启动 :80");

  // WiFi info stays on screen — first button press triggers setView/cmd
  // which will replace it with the correct view
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();
  wifiNetTick();                  // 新增：WiFi 健康检查 / 自动恢复（非阻塞，10 秒一拍）
  eyesAnimTick();                 // 新增：官方「正常眼睛 / 开心挤眼」非阻塞步进（受速度阀钮控制）
  clockTick();
  expressionTick();               // 表情动画驱动（非阻塞，不在表情视图时零开销）
  emotionTick();                  // 情绪衰减 + 判定（非阻塞，5 秒一拍）
  interactionTick();              // 按键轮询 + 连击/长按/idle（非阻塞）
  sleepTick();                    // 睡眠 / 唤醒状态机（第五阶段新增，非阻塞）
                                  //   ↑ 必须排在 interactionTick 之后（要用最新的 idle）
                                  //   ↓ 必须排在 proactiveTick 之前（要先设好睡眠闸门）
  proactiveTick();                // 主动互动状态机（非阻塞，随机 60~180s 一次）
  scheduleTick();                 // 日程 / 倒计时（第六阶段新增，非阻塞，1 秒一拍）
                                  //   排在最后：它只读时间、只推情绪，
                                  //   不抢输入也不抢屏幕，放哪都不影响别人
  bdcakeTick();                   // 生日动画（本轮新增，非阻塞，50ms 一帧，只在 VIEW_BDCAKE 画）
  birthdayTick();                 // 生日 / 纪念日（第七阶段新增，非阻塞）
                                  //   它可能切 currentView（庆祝场景）→ 放最后，
                                  //   让其它模块先跑完，下一轮再看到新视图
}
