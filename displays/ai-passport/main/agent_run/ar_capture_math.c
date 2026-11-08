// main/agent_run/ar_capture_math.c —— 分段截屏的纯逻辑实现。见 ar_capture_math.h。
#include "ar_capture_math.h"

#include <string.h>

int ar_capture_band_count(int screen_h, int band_h) {
    if (screen_h <= 0 || band_h <= 0) return 0;
    return (screen_h + band_h - 1) / band_h;
}

int ar_capture_copy_rows(uint16_t *dst, int screen_w, int band_y0, int band_h,
                         int ax1, int ay1, int ax2, int ay2,
                         const uint8_t *src, int src_stride) {
    // 行:区域与条带求交
    int y_from = ay1 > band_y0 ? ay1 : band_y0;
    int y_to = ay2 < band_y0 + band_h - 1 ? ay2 : band_y0 + band_h - 1;
    // 列:区域与屏幕求交
    int x_from = ax1 > 0 ? ax1 : 0;
    int x_to = ax2 < screen_w - 1 ? ax2 : screen_w - 1;
    if (y_from > y_to || x_from > x_to) return 0;

    const int count = x_to - x_from + 1;
    for (int y = y_from; y <= y_to; y++) {
        const uint8_t *row = src + (y - ay1) * src_stride + (x_from - ax1) * 2;
        memcpy(&dst[(y - band_y0) * screen_w + x_from], row, (size_t)count * 2);
    }
    return y_to - y_from + 1;
}
