/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  expression.h — Clawd Mochi 表情管理系统（第一阶段）
 *
 *  设计要点：
 *    · 表情（Expression）= 角色长什么样      → ExpressionData / 渲染器
 *    · 动画（Animation） = 角色怎么动        → 非阻塞 tick 引擎
 *    两者正交，可自由组合：HAPPY + BLINK / HAPPY + BOUNCE / ANGRY + SHAKE …
 *
 *  本模块不依赖任何图片资源，全部由 Adafruit_GFX 代码绘制，
 *  Flash 占用 < 20KB，堆占用 0，可直接被未来的情绪 / 睡眠 / 随机互动等模块调用。
 *
 *  依赖：clawd_mochi.ino 中的全局对象（tft / 调色板 / animBgColor / currentView）
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ═══════════════════════════════════════════════════════════════════════════
//  来自主固件 clawd_mochi.ino 的全局对象 / 函数
// ═══════════════════════════════════════════════════════════════════════════
extern Adafruit_ST7789 tft;
extern uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_GRID;
extern uint16_t C_CLKBG, C_CREAM, C_CLKLINE, C_CLKDEEP;
extern uint16_t animBgColor;
extern uint8_t  animSpeed;
extern uint8_t  currentView;

// 主固件里 C_WHITE / C_BLACK 是 #define，只存在于 .ino 那个编译单元里，
// 其它 .cpp（emotion / interaction / proactive / sleep）看不到它们 ——
// 这里补一份等价定义（数值完全相同，所以 .ino 里那句重复 #define 是合法的）。
#ifndef C_WHITE
  #define C_WHITE ST77XX_WHITE
#endif
#ifndef C_BLACK
  #define C_BLACK ST77XX_BLACK
#endif

int  speedMs(int ms);
void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col);

// ═══════════════════════════════════════════════════════════════════════════
//  角色几何 —— 与原有「大眼睛」视图共用同一套坐标
//  （数值与原 clawd_mochi.ino 完全一致，只是搬到了这里作为唯一出处）
// ═══════════════════════════════════════════════════════════════════════════
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_OX  0
#define EYE_OY  40

#define EXPR_EYE_LX  ((240 - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX)   // 30
#define EXPR_EYE_RX  (EXPR_EYE_LX + EYE_W + EYE_GAP)                // 180
#define EXPR_EYE_Y   ((240 - EYE_H) / 2 - EYE_OY)                   // 50
#define EXPR_EYE_CY  (EXPR_EYE_Y + EYE_H / 2)                       // 80

// 嘴部基准点
#define EXPR_MOUTH_X  120
#define EXPR_MOUTH_Y  158

// ═══════════════════════════════════════════════════════════════════════════
//  视图编号（新增第 6 个视图）
// ═══════════════════════════════════════════════════════════════════════════
#define VIEW_EXPRESSION 5

// ═══════════════════════════════════════════════════════════════════════════
//  表情枚举 —— 15 种
// ═══════════════════════════════════════════════════════════════════════════
enum ExpressionId : uint8_t {
  EXPR_HAPPY = 0,    // 开心
  EXPR_LAUGH,        // 大笑
  EXPR_SHY,          // 撒娇
  EXPR_ANGRY,        // 生气
  EXPR_SAD,          // 悲伤
  EXPR_CRY,          // 哭泣
  EXPR_SLEEPY,       // 困
  EXPR_SURPRISED,    // 惊讶
  EXPR_THINKING,     // 思考
  EXPR_CONFUSED,     // 疑惑
  EXPR_PROUD,        // 得意
  EXPR_LOVE,         // 爱心
  EXPR_SCARED,       // 害怕
  EXPR_EMBARRASSED,  // 害羞
  EXPR_PLAYFUL,      // 调皮
  EXPR_NEUTRAL,      // 平静（第二阶段新增，供 CALM 情绪使用；追加在末尾以免打乱已有 id）
  EXPR_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  动画枚举 —— 与表情正交，可任意组合
// ═══════════════════════════════════════════════════════════════════════════
enum AnimId : uint8_t {
  ANIM_NONE = 0,   // 静止
  ANIM_BLINK,      // 眨眼
  ANIM_BOUNCE,     // 上下跳动
  ANIM_SHAKE,      // 左右晃动
  ANIM_BREATH,     // 呼吸起伏
  ANIM_TREMBLE,    // 颤抖
  // 第五阶段新增：睡眠深呼吸。周期更长、幅度略大，并驱动 Zzz 相位。
  // 追加在末尾，以免打乱第一~四阶段已有的 id。
  ANIM_SLEEP_BREATH,
  ANIM_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  眼睛画法
// ═══════════════════════════════════════════════════════════════════════════
enum EyeStyle : uint8_t {
  EYE_SOLID = 0,     // 实心方块（等价于原 Normal eyes）
  EYE_ARC_UP,        // ^  ^   弯弯笑眼
  EYE_ARC_DOWN,      // v  v
  EYE_CHEVRON,       // >  <   等价于原 Squish eyes
  EYE_ROUND,         // 大圆眼
  EYE_ROUND_SMALL,   // 小圆眼
  EYE_SPARKLE,       // 星星眼
  EYE_HEART,         // 爱心眼
  EYE_SHADES,        // 墨镜
  EYE_FLAT,          // 一字平眼
  EYE_FLAT_SIDE,     // 一字平眼（侧看）
  EYE_TEARY,         // 圆眼带泪
  EYE_SAD,           // 下垂眼
  EYE_CRY_CLOSED,    // 闭眼哭
  EYE_WINK,          // 单眼眨（左实心 / 右弯月）
  EYE_UNEVEN,        // 一大一小
  EYE_CLOSED,        // 闭眼（一字）
  EYE_STYLE_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  嘴型画法
// ═══════════════════════════════════════════════════════════════════════════
enum MouthStyle : uint8_t {
  MOUTH_NONE = 0,
  MOUTH_SMILE,       // 微笑弧
  MOUTH_OPEN,        // 张口（D 形）
  MOUTH_OPEN_BIG,    // 大张嘴
  MOUTH_SMALL_O,     // 小圆嘴
  MOUTH_FROWN,       // 撇嘴
  MOUTH_FROWN_OPEN,  // 哭嘴
  MOUTH_FLAT,        // 一字嘴
  MOUTH_FLAT_SIDE,   // 侧一字嘴
  MOUTH_WAVY,        // 波浪嘴
  MOUTH_GRIN,        // 咧嘴笑
  MOUTH_TONGUE,      // 吐舌
  MOUTH_STYLE_COUNT
};

// ═══════════════════════════════════════════════════════════════════════════
//  附加特效（位掩码）
// ═══════════════════════════════════════════════════════════════════════════
#define FX_NONE        0x0000
#define FX_BLUSH       0x0001   // 腮红
#define FX_TEARS       0x0002   // 泪流
#define FX_SWEAT       0x0004   // 汗滴
#define FX_ANGER       0x0008   // 怒气符号
#define FX_BROWS_ANGRY 0x0010   // 怒眉
#define FX_ZZZ         0x0020   // 睡觉 Zz
#define FX_QUESTION    0x0040   // 问号
#define FX_SPARKLE     0x0080   // 闪光
#define FX_HEARTS      0x0100   // 飘心
#define FX_BROWS_SAD   0x0200   // 八字眉

// ═══════════════════════════════════════════════════════════════════════════
//  数据结构
// ═══════════════════════════════════════════════════════════════════════════

// 单个表情的定义（纯数据，不含任何绘制逻辑）
struct ExpressionDef {
  const char* key;    // 网页 / 接口用的英文 key
  const char* label;  // 中文名
  const char* glyph;  // 网页上的图标字符（UTF-8）
  uint8_t     eye;    // EyeStyle
  uint8_t     mouth;  // MouthStyle
  uint16_t    fx;     // FX_* 位掩码
  uint8_t     anim;   // 默认 AnimId
};

// 一帧的形变参数 —— 由动画层算出，与「是什么表情」完全无关
struct FaceFrame {
  int16_t dx;        // 整体水平偏移（像素）
  int16_t dy;        // 整体垂直偏移（像素）
  uint8_t eyeOpen;   // 眼睛张开度 0..100
  uint8_t fxPhase;   // 特效相位（第五阶段新增）
                     //   0     = 静态特效，与第一~四阶段表现完全一致
                     //   1..4  = 睡眠 Zzz 的 Z → ZZ → ZZZ → 空 循环
};

// ═══════════════════════════════════════════════════════════════════════════
//  对外接口
// ═══════════════════════════════════════════════════════════════════════════

// ── 生命周期 ──
void exprBegin();                    // setup() 中调用一次，初始化配色

// ── 核心控制接口 ──
void         setExpression(ExpressionId id);                        // 立即切换并显示（手动）
bool         requestExpression(ExpressionId id, bool automatic);    // automatic=true 时会被锁拦截
void         playExpression(ExpressionId id, AnimId anim);          // 同时指定表情 + 动画
void         resetExpression();                                     // 回到默认表情 + 默认动画
ExpressionId getCurrentExpression();
const ExpressionDef& getExpressionDef(ExpressionId id);

// ── 锁定 ──
void lockExpression();               // 锁定后，automatic 请求无法覆盖当前表情
void unlockExpression();
bool isExpressionLocked();

// ── 动画（与表情正交）──
void   setExpressionAnimation(AnimId anim);
AnimId getExpressionAnimation();

// ── 渲染 / 驱动 ──
void exprEnterView();                // 切到表情视图并整屏重绘
void exprRedraw();                   // 按当前状态重绘一帧
void expressionTick();               // loop() 中每轮调用，非阻塞

// ── 供 /state 使用 ──
const char* exprKeyOf(ExpressionId id);
const char* exprAnimKeyOf(AnimId id);
