/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  expression_data.h — 表情定义表
 *
 *  这里是「表情是什么」的唯一出处：改表情只需改这张表，不用碰任何绘制代码。
 *
 *  注意：ESP32 的 Flash 是内存映射的，const 数组直接可寻址读取，
 *        不需要 PROGMEM / pgm_read_*（那些是 AVR 时代的东西）。
 *        整张表约 320 字节，落在 .rodata 段（Flash）。
 *
 *  文件编码必须保持 UTF-8（中文名与 emoji 图标直接写在字符串里）。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

#include "expression.h"

static const ExpressionDef EXPR_DEFS[EXPR_COUNT] = {
  //  key           label    glyph   眼型             嘴型               附加特效                                默认动画
  { "happy",       "开心",  "😊",  EYE_ARC_UP,      MOUTH_SMILE,       FX_NONE,                                ANIM_BLINK   },
  { "laugh",       "大笑",  "😄",  EYE_CHEVRON,     MOUTH_OPEN_BIG,    FX_NONE,                                ANIM_BOUNCE  },
  { "shy",         "撒娇",  "🥰",  EYE_SPARKLE,     MOUTH_SMALL_O,     FX_BLUSH,                               ANIM_BREATH  },
  { "angry",       "生气",  "😡",  EYE_TEARY,       MOUTH_FROWN,       FX_BROWS_ANGRY | FX_ANGER,              ANIM_TREMBLE },
  { "sad",         "悲伤",  "😔",  EYE_SAD,         MOUTH_FROWN,       FX_BROWS_SAD,                           ANIM_BREATH  },
  { "cry",         "哭泣",  "😭",  EYE_CRY_CLOSED,  MOUTH_FROWN_OPEN,  FX_TEARS,                               ANIM_TREMBLE },
  { "sleepy",      "困",    "😴",  EYE_CLOSED,      MOUTH_SMALL_O,     FX_ZZZ,                                 ANIM_BREATH  },
  { "surprised",   "惊讶",  "😮",  EYE_ROUND,       MOUTH_SMALL_O,     FX_SWEAT,                               ANIM_NONE    },
  { "thinking",    "思考",  "🤔",  EYE_FLAT,        MOUTH_FLAT_SIDE,   FX_QUESTION,                            ANIM_NONE    },
  { "confused",    "疑惑",  "😕",  EYE_UNEVEN,      MOUTH_WAVY,        FX_SWEAT,                               ANIM_NONE    },
  { "proud",       "得意",  "😎",  EYE_SHADES,      MOUTH_GRIN,        FX_NONE,                                ANIM_NONE    },
  { "love",        "爱心",  "😍",  EYE_HEART,       MOUTH_OPEN,        FX_HEARTS,                              ANIM_BOUNCE  },
  { "scared",      "害怕",  "😱",  EYE_ROUND,       MOUTH_WAVY,        FX_SWEAT | FX_BROWS_SAD,                ANIM_TREMBLE },
  { "embarrassed", "害羞",  "😳",  EYE_FLAT,        MOUTH_FLAT,        FX_BLUSH,                               ANIM_NONE    },
  { "playful",     "调皮",  "😜",  EYE_WINK,        MOUTH_TONGUE,      FX_SPARKLE,                             ANIM_SHAKE   },
  // 第二阶段新增：平静。复用已有的 EYE_SOLID + MOUTH_NONE，
  // 画出来就是原项目那两只方块眼，没有新增任何绘制代码。
  { "neutral",     "平静",  "😐",  EYE_SOLID,       MOUTH_NONE,        FX_NONE,                                ANIM_BLINK   },
};

// 动画的对外 key（网页 / 接口用）
static const char* const ANIM_KEYS[ANIM_COUNT] = {
  "none", "blink", "bounce", "shake", "breath", "tremble",
  "sleep_breath"   // 第五阶段新增
};
