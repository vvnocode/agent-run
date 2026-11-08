// main/agent_run/ar_capture_math.h —— 分段截屏的纯逻辑(可宿主测试)。
//
// 没有 150 KB 的整屏缓冲,截屏按条带进行:每次整屏刷新时,把 LVGL 刷新区域中
// 落在当前条带内的行拷进条带缓冲,再把条带发给电脑。见 ar_capture.c。
#pragma once

#include <stdint.h>

// 屏幕高 screen_h、每段 band_h 行时需要的段数
int ar_capture_band_count(int screen_h, int band_h);

// 把刷新区域 [ax1,ax2]×[ay1,ay2](屏幕坐标,含端点)中落在条带 [band_y0, band_y0+band_h) 的行,
// 拷到条带缓冲 dst(每行 screen_w 个 RGB565 像素)。src 指向区域左上角,行距 src_stride 字节。
// 超出屏幕宽度的列被裁掉。返回实际拷贝的行数。
int ar_capture_copy_rows(uint16_t *dst, int screen_w, int band_y0, int band_h,
                         int ax1, int ay1, int ax2, int ay2,
                         const uint8_t *src, int src_stride);
