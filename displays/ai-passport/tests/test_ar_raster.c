// tests/test_ar_raster.c —— Agent Run! 像素绘制(矩形、混色、像素字、精灵)的宿主测试。
#include <assert.h>
#include <string.h>

#include "agent_run/ar_raster.h"
#include "agent_run/ar_sprite.h"

#define W 20
#define H 10
#define STRIDE 24   // 每行多留 4 个像素,检查绘制不会越过行宽
#define GUARD 0x5A5A

static uint16_t s_mem[STRIDE * H + 2 * STRIDE];   // 前后各留一行保护区

static ar_canvas_t canvas(void) {
    for (size_t i = 0; i < sizeof(s_mem) / sizeof(s_mem[0]); i++) s_mem[i] = GUARD;
    ar_canvas_t c = { .px = s_mem + STRIDE, .w = W, .h = H, .stride = STRIDE };
    return c;
}

static uint16_t at(const ar_canvas_t *c, int x, int y) {
    return c->px[y * c->stride + x];
}

static int count_color(const ar_canvas_t *c, uint16_t color) {
    int n = 0;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++)
            if (at(c, x, y) == color) n++;
    return n;
}

static void assert_guards_intact(void) {
    for (int x = 0; x < STRIDE; x++) {
        assert(s_mem[x] == GUARD);
        assert(s_mem[STRIDE * (H + 1) + x] == GUARD);
    }
    for (int y = 0; y < H; y++)
        for (int x = W; x < STRIDE; x++)
            assert(s_mem[STRIDE * (y + 1) + x] == GUARD);
}

static void test_rgb565(void) {
    assert(AR_RGB565(0xFFFFFF) == 0xFFFF);
    assert(AR_RGB565(0x000000) == 0x0000);
    assert(AR_RGB565(0xFF0000) == 0xF800);
    assert(AR_RGB565(0x00FF00) == 0x07E0);
    assert(AR_RGB565(0x0000FF) == 0x001F);
}

static void test_fill_and_clipped_rect(void) {
    ar_canvas_t c = canvas();
    ar_raster_fill(&c, 0x0000);
    assert(count_color(&c, 0x0000) == W * H);
    ar_raster_rect(&c, -3, -2, 5, 4, 0x1111);     // 左上角越界
    assert(count_color(&c, 0x1111) == 2 * 2);
    ar_raster_rect(&c, W - 1, H - 1, 10, 10, 0x2222);   // 右下角越界
    assert(count_color(&c, 0x2222) == 1);
    ar_raster_rect(&c, 100, 100, 5, 5, 0x3333);   // 完全在外
    ar_raster_rect(&c, 2, 2, 0, 5, 0x3333);       // 宽度为 0
    assert(count_color(&c, 0x3333) == 0);
    assert_guards_intact();
}

static void test_blend(void) {
    assert(ar_rgb565_blend(0x0000, 0xFFFF, 255) == 0xFFFF);
    assert(ar_rgb565_blend(0x1234, 0xFFFF, 0) == 0x1234);
    uint16_t mid = ar_rgb565_blend(0x0000, 0xFFFF, 128);
    int r = (mid >> 11) & 0x1F, g = (mid >> 5) & 0x3F, b = mid & 0x1F;
    assert(r >= 15 && r <= 16 && g >= 31 && g <= 32 && b >= 15 && b <= 16);
    ar_canvas_t c = canvas();
    ar_raster_fill(&c, 0x0000);
    ar_raster_rect_blend(&c, 0, 0, 2, 2, 0xFFFF, 255);
    assert(at(&c, 1, 1) == 0xFFFF && at(&c, 2, 2) == 0x0000);
}

static void test_text(void) {
    assert(ar_raster_text_width("", 2) == 0);
    assert(ar_raster_text_width("A", 1) == 5);
    assert(ar_raster_text_width("AB", 2) == (2 * 6 - 1) * 2);
    ar_canvas_t c = canvas();
    ar_raster_fill(&c, 0x0000);
    // "-" 只有第 3 行的 5 个像素
    ar_raster_text(&c, 1, 1, "-", 1, 0xFFFF);
    assert(count_color(&c, 0xFFFF) == 5);
    for (int x = 1; x <= 5; x++) assert(at(&c, x, 4) == 0xFFFF);
    // 小写按大写绘制;放大 2 倍时像素数是 4 倍
    ar_raster_fill(&c, 0x0000);
    ar_raster_text(&c, 0, 0, "z", 1, 0xFFFF);
    int small = count_color(&c, 0xFFFF);
    assert(small > 0);
    ar_raster_fill(&c, 0x0000);
    ar_raster_text(&c, 0, 0, "Z", 1, 0xFFFF);
    assert(count_color(&c, 0xFFFF) == small);
    // 越界文字被裁剪,不破坏保护区
    ar_raster_text(&c, 15, 5, "88", 3, 0xFFFF);
    assert_guards_intact();
}

static void test_sprite_scaling_and_offset(void) {
    static const ar_sprite_rect_t rects[] = { {1, 2, 3, 1, 1} };
    const ar_sprite_frame_t frame = { rects, 1, -1 };
    const uint16_t palette[] = { 0x0000, 0xAAAA };
    ar_canvas_t c = canvas();
    ar_raster_fill(&c, 0x0000);
    ar_raster_sprite(&c, &frame, 4, 2, 2, palette, 255);
    // x = 4 + 1*2 .. +3*2-1 = 6..11;y = 2 + (2-1)*2 .. +1 = 4..5
    assert(count_color(&c, 0xAAAA) == 6 * 2);
    assert(at(&c, 6, 4) == 0xAAAA && at(&c, 11, 5) == 0xAAAA);
    assert(at(&c, 5, 4) == 0x0000 && at(&c, 12, 4) == 0x0000 && at(&c, 6, 3) == 0x0000);
    assert_guards_intact();
}

static void test_all_sprite_frames_fit_grid(void) {
    for (int pose = 0; pose < AR_POSE_COUNT; pose++) {
        for (int f = 0; f < AR_SPRITE_FRAMES; f++) {
            const ar_sprite_frame_t *frame = ar_sprite_frame((ar_pose_t)pose, f);
            assert(frame && frame->count > 0);
            for (int i = 0; i < frame->count; i++) {
                const ar_sprite_rect_t *r = &frame->rects[i];
                int y = r->y + frame->dy;
                assert(r->w > 0 && r->h > 0);
                assert(r->x + r->w <= AR_SPRITE_W);
                assert(y >= 0 && y + r->h <= AR_SPRITE_H);
                assert(r->color < AR_SPRITE_COLORS);
            }
        }
    }
}

int main(void) {
    test_rgb565();
    test_fill_and_clipped_rect();
    test_blend();
    test_text();
    test_sprite_scaling_and_offset();
    test_all_sprite_frames_fit_grid();
    return 0;
}
