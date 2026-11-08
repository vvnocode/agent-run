// main/agent_run/ar_raster.c —— 像素绘制实现。见 ar_raster.h。
#include "ar_raster.h"

#include <stddef.h>

// 5×7 字形:每个字 7 行,每行低 5 位有效,bit4 为最左列
typedef struct {
    char ch;
    uint8_t rows[AR_GLYPH_H];
} glyph_t;

static const glyph_t GLYPHS[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
};

#define GLYPH_COUNT (sizeof(GLYPHS) / sizeof(GLYPHS[0]))
#define GLYPH_W 5
#define GLYPH_ADVANCE 6

// 查找字形;小写字母按大写处理,找不到返回 NULL(按空格绘制)
static const glyph_t *find_glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (size_t i = 0; i < GLYPH_COUNT; i++) {
        if (GLYPHS[i].ch == ch) return &GLYPHS[i];
    }
    return NULL;
}

uint16_t ar_rgb565_blend(uint16_t dst, uint16_t src, uint8_t alpha) {
    if (alpha == 255) return src;
    if (alpha == 0) return dst;
    // 分别对 5/6/5 位通道做线性插值,+127 实现四舍五入
    uint32_t a = alpha, ia = 255u - alpha;
    uint32_t r = (((src >> 11) & 0x1F) * a + ((dst >> 11) & 0x1F) * ia + 127) / 255;
    uint32_t g = (((src >> 5) & 0x3F) * a + ((dst >> 5) & 0x3F) * ia + 127) / 255;
    uint32_t b = ((src & 0x1F) * a + (dst & 0x1F) * ia + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void ar_raster_fill(ar_canvas_t *c, uint16_t color) {
    for (int y = 0; y < c->h; y++) {
        uint16_t *row = c->px + y * c->stride;
        for (int x = 0; x < c->w; x++) row[x] = color;
    }
}

// 把矩形裁剪到画布内;完全在外或尺寸非正时返回 0
static int clip(const ar_canvas_t *c, int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0) return 0;
    int x0 = *x < 0 ? 0 : *x;
    int y0 = *y < 0 ? 0 : *y;
    int x1 = *x + *w > c->w ? c->w : *x + *w;
    int y1 = *y + *h > c->h ? c->h : *y + *h;
    if (x0 >= x1 || y0 >= y1) return 0;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

void ar_raster_rect(ar_canvas_t *c, int x, int y, int w, int h, uint16_t color) {
    if (!clip(c, &x, &y, &w, &h)) return;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = c->px + yy * c->stride;
        for (int xx = x; xx < x + w; xx++) row[xx] = color;
    }
}

void ar_raster_rect_blend(ar_canvas_t *c, int x, int y, int w, int h, uint16_t color, uint8_t alpha) {
    if (!clip(c, &x, &y, &w, &h)) return;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = c->px + yy * c->stride;
        for (int xx = x; xx < x + w; xx++) row[xx] = ar_rgb565_blend(row[xx], color, alpha);
    }
}

int ar_raster_text_width(const char *s, int scale) {
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    if (n == 0) return 0;
    return (n * GLYPH_ADVANCE - 1) * scale;
}

void ar_raster_text(ar_canvas_t *c, int x, int y, const char *s, int scale, uint16_t color) {
    for (const char *p = s; *p; p++, x += GLYPH_ADVANCE * scale) {
        const glyph_t *g = find_glyph(*p);
        if (!g) continue;
        for (int row = 0; row < AR_GLYPH_H; row++) {
            uint8_t bits = g->rows[row];
            for (int col = 0; col < GLYPH_W; col++) {
                if (bits & (1u << (GLYPH_W - 1 - col))) {
                    ar_raster_rect(c, x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

void ar_raster_sprite(ar_canvas_t *c, const ar_sprite_frame_t *f, int ox, int oy, int scale,
                      const uint16_t *palette, uint8_t alpha) {
    for (uint8_t i = 0; i < f->count; i++) {
        const ar_sprite_rect_t *r = &f->rects[i];
        int x = ox + r->x * scale;
        int y = oy + (r->y + f->dy) * scale;
        if (alpha == 255) {
            ar_raster_rect(c, x, y, r->w * scale, r->h * scale, palette[r->color]);
        } else {
            ar_raster_rect_blend(c, x, y, r->w * scale, r->h * scale, palette[r->color], alpha);
        }
    }
}
