// main/agent_run/ar_sprite.h —— 宠物“小跑”的像素帧数据。
//
// 16×12 逻辑像素,与 Design 画布屏幕稿中的 SVG 矩形一一对应;每种姿势 2 帧。
#pragma once

#include "ar_raster.h"

#define AR_SPRITE_W 16
#define AR_SPRITE_H 12
#define AR_SPRITE_FRAMES 2

// 调色板下标
enum {
    AR_SPRITE_BODY = 0,   // 主色
    AR_SPRITE_SHADE,      // 阴影(主色压暗)
    AR_SPRITE_BELLY,      // 肚皮(主色提亮)
    AR_SPRITE_EYE,        // 眼睛、鼻子、嘴
    AR_SPRITE_CHEEK,      // 腮红
    AR_SPRITE_SWEAT,      // 汗滴
    AR_SPRITE_COLORS,
};

typedef enum {
    AR_POSE_SLEEP = 0,   // 侧卧睡觉(帧 2 吸气鼓起)
    AR_POSE_WALK,        // 走路(帧 2 双腿并拢)
    AR_POSE_TROT,        // 小跑(帧 2 收腿)
    AR_POSE_RUN,         // 狂奔(帧 2 收腿、身体上浮)
    AR_POSE_DASH,        // 冲刺(在狂奔基础上张嘴、两滴汗)
    AR_POSE_WAVE,        // 正对屏幕招手(帧 2 放下爪子)
    AR_POSE_COUNT,
} ar_pose_t;

// 取某个姿势的第 frame 帧(frame 取模 AR_SPRITE_FRAMES)
const ar_sprite_frame_t *ar_sprite_frame(ar_pose_t pose, int frame);
