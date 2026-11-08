// main/agent_run/ar_raster.h —— 直接写 RGB565 缓冲的像素绘制(纯 C,可宿主测试)。
//
// 宠物、像素字和场景装饰都由一组矩形组成。若逐个交给 LVGL 绘制,每个矩形都会占用
// 一个绘制任务,24 KB 的 LVGL 内存池容易耗尽;这里改为把它们直接画进 canvas 的
// 静态缓冲,LVGL 只需绘制一张图。
#pragma once

#include <stdint.h>

// 0xRRGGBB → RGB565(LVGL 内部的小端 RGB565,字节交换由显示驱动负责)
#define AR_RGB565(rgb) \
    ((uint16_t)((((rgb) >> 8) & 0xF800) | (((rgb) >> 5) & 0x07E0) | (((rgb) >> 3) & 0x001F)))

// 一块可绘制的像素区域;stride 以像素为单位,可以大于 w
typedef struct {
    uint16_t *px;
    int w;
    int h;
    int stride;
} ar_canvas_t;

// 精灵(宠物)的一个矩形:坐标单位是逻辑像素,color 是调色板下标
typedef struct {
    uint8_t x, y, w, h, color;
} ar_sprite_rect_t;

// 精灵的一帧:矩形列表 + 整体纵向偏移(用于奔跑时身体上下起伏)
typedef struct {
    const ar_sprite_rect_t *rects;
    uint8_t count;
    int8_t dy;
} ar_sprite_frame_t;

// 按 alpha(0-255)把 src 混合到 dst 上
uint16_t ar_rgb565_blend(uint16_t dst, uint16_t src, uint8_t alpha);

void ar_raster_fill(ar_canvas_t *c, uint16_t color);

// 填充矩形,超出画布的部分被裁剪
void ar_raster_rect(ar_canvas_t *c, int x, int y, int w, int h, uint16_t color);

// 半透明填充矩形
void ar_raster_rect_blend(ar_canvas_t *c, int x, int y, int w, int h, uint16_t color, uint8_t alpha);

// 5×7 像素字:每字宽 5、字距 1,放大 scale 倍。支持数字、大写字母(小写按大写画)、
// 空格与 ! % - . : 等少量符号,其他字符按空格处理。
int ar_raster_text_width(const char *s, int scale);
void ar_raster_text(ar_canvas_t *c, int x, int y, const char *s, int scale, uint16_t color);

// 像素字的字形高度(放大前)
#define AR_GLYPH_H 7

// 绘制一帧精灵:左上角放在 (ox, oy),逻辑像素放大 scale 倍;alpha < 255 时半透明
// (用于冲刺时的残影)。palette 按 ar_sprite_rect_t.color 取色。
void ar_raster_sprite(ar_canvas_t *c, const ar_sprite_frame_t *f, int ox, int oy, int scale,
                      const uint16_t *palette, uint8_t alpha);
