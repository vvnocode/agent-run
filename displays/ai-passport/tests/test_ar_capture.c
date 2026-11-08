// tests/test_ar_capture.c —— 分段截屏的行拷贝与分段规划(纯逻辑)的宿主测试。
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "agent_run/ar_capture_math.h"

#define SCREEN_W 8
#define BAND_H 3

// 构造一块“刷新区域”的源像素:像素值 = 行号 * 100 + 列号,便于断言落点
static void fill_source(uint16_t *src, int x1, int y1, int x2, int y2, int stride_px) {
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            src[(y - y1) * stride_px + (x - x1)] = (uint16_t)(y * 100 + x);
}

static void test_band_count(void) {
    assert(ar_capture_band_count(320, 64) == 5);
    assert(ar_capture_band_count(320, 80) == 4);
    assert(ar_capture_band_count(320, 100) == 4);
    assert(ar_capture_band_count(10, 3) == 4);
    assert(ar_capture_band_count(0, 3) == 0);
}

static void test_copies_only_rows_inside_band(void) {
    uint16_t dst[SCREEN_W * BAND_H];
    uint16_t src[SCREEN_W * 4];
    memset(dst, 0, sizeof(dst));
    // 刷新区域覆盖第 2..5 行全宽;条带为第 3..5 行
    fill_source(src, 0, 2, SCREEN_W - 1, 5, SCREEN_W);
    int copied = ar_capture_copy_rows(dst, SCREEN_W, 3, BAND_H, 0, 2, SCREEN_W - 1, 5,
                                      (const uint8_t *)src, SCREEN_W * 2);
    assert(copied == 3);
    assert(dst[0] == 300 && dst[SCREEN_W - 1] == 307);     // 第 3 行
    assert(dst[2 * SCREEN_W + 4] == 504);                    // 第 5 行第 4 列
}

static void test_partial_width_area_and_stride(void) {
    uint16_t dst[SCREEN_W * BAND_H];
    uint16_t src[6 * 2];   // 区域宽 4,源缓冲行距 6 像素
    memset(dst, 0xFF, sizeof(dst));
    fill_source(src, 2, 0, 5, 1, 6);
    int copied = ar_capture_copy_rows(dst, SCREEN_W, 0, BAND_H, 2, 0, 5, 1, (const uint8_t *)src, 6 * 2);
    assert(copied == 2);
    assert(dst[2] == 2 && dst[5] == 5);           // 第 0 行的第 2..5 列
    assert(dst[SCREEN_W + 3] == 103);             // 第 1 行
    assert(dst[0] == 0xFFFF && dst[6] == 0xFFFF); // 区域外的列不动
}

static void test_area_outside_band_or_screen(void) {
    uint16_t dst[SCREEN_W * BAND_H];
    uint16_t src[SCREEN_W * 2];
    memset(dst, 0, sizeof(dst));
    fill_source(src, 0, 10, SCREEN_W - 1, 11, SCREEN_W);
    assert(ar_capture_copy_rows(dst, SCREEN_W, 0, BAND_H, 0, 10, SCREEN_W - 1, 11,
                                (const uint8_t *)src, SCREEN_W * 2) == 0);
    // 超出屏幕宽度的列被裁掉,不越界写 dst
    uint16_t wide[12];
    for (int i = 0; i < 12; i++) wide[i] = (uint16_t)(1000 + i);
    assert(ar_capture_copy_rows(dst, SCREEN_W, 0, BAND_H, 0, 0, 11, 0, (const uint8_t *)wide, 12 * 2) == 1);
    assert(dst[SCREEN_W - 1] == 1000 + SCREEN_W - 1);
    assert(dst[SCREEN_W] == 0);
}

int main(void) {
    test_band_count();
    test_copies_only_rows_inside_band();
    test_partial_width_area_and_stride();
    test_area_outside_band_or_screen();
    return 0;
}
