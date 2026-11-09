// main/agent_run/ar_ui.c —— Agent Run! 界面实现。
//
// 布局:标题栏、计数区、宠物场景、三格计数、底栏(普通画面);
//       标题栏、工具名与页码点、两张用量卡片、三格计数、底栏(工具专用页面);
//       标题栏、橙色提醒条、宠物场景、等待列表、底栏(等你处理);
//       标题栏、未连接提示、变暗的宠物场景(未连接)。
// 设置菜单打开时,标题栏以下换成一块面板:一级菜单列出音量、亮度两行,二级显示放大的
// 图标、当前值与由矮到高的档位条。
// 像素元素(标题栏、大数字、宠物场景、计数格)画在 canvas 的静态缓冲里,见 ar_raster.h;
// 中文文字用 ar_font_cjk14 的 LVGL 标签。
#include "ar_ui.h"

#include <stdio.h>
#include <string.h>

#include "ar_link.h"
#include "ar_raster.h"
#include "ar_sprite.h"
#include "ar_text.h"
#include "lvgl.h"

LV_FONT_DECLARE(ar_font_cjk14);

// ---- 颜色(与屏幕稿一致) -----------------------------------------------
#define C_BG 0x0F1218
#define C_SCENE 0x141922
#define C_TILE 0x1A1F29
#define C_TEXT 0xE8EAF0
#define C_TEXT2 0x9AA3B2
#define C_DIM 0x6E7A90
#define C_TRACK 0x2A3140
#define C_LINE2 0x3A4356
#define C_DASH_TROT 0x333B4C
#define C_ACCENT 0xFFC54D
#define C_ALERT 0xFF8A3D
#define C_ALERT_INK 0x1A0F05

// ---- 布局常量(屏幕坐标) -----------------------------------------------
#define PAD_X 18
#define CONTENT_W 204
#define HEADER_Y 15
#define HEADER_H 14
#define SCENE_W CONTENT_W
#define SCENE_H 100
#define SCENE_Y_NORMAL 108
#define SCENE_Y_WAITING 92
#define BIG_W 80
#define BIG_H 42
#define BIG_SCALE 6
#define TILE_W 64
#define TILE_H 52
#define TILE_GAP 6
#define TILE_Y 216
#define TILE_CANVAS_W 56
#define TILE_CANVAS_H 44
#define ALERT_NUM_W 48
#define ALERT_NUM_H 28

// 工具专用页面(专用页面方案 §4.2)。卡片内元素的坐标以卡片左上角为原点
#define AGENT_TITLE_Y 38
#define PAGE_DOT 6
#define PAGE_DOT_GAP 6
// 第一个页码点的左边缘;套餐徽标不越过它
#define PAGE_DOTS_X (PAD_X + CONTENT_W - AR_PAGE_COUNT * (PAGE_DOT + PAGE_DOT_GAP) + PAGE_DOT_GAP)
// 套餐徽标(专用页面方案 §9):紧跟工具名,与工具名、页码点的间距,左右内边距,高度
#define PLAN_CHIP_GAP 8
#define PLAN_CHIP_PAD 7
#define PLAN_CHIP_H 18
#define CARD_Y0 60               // 5 小时用量卡片
#define CARD_Y1 132              // 每周用量卡片
#define CARD_H 64
#define CARD_PAD 12
#define CARD_TITLE_Y 6
#define PCT_SCALE 3
#define PCT_W BIG_W              // 百分比画在首页大数字缓冲的上下两半里,见 create_agent_page()
#define PCT_H (BIG_H / 2)
#define PCT_Y 5
#define USAGE_BAR_W 180
#define USAGE_BAR_H 8
#define USAGE_BAR_Y 31
#define CARD_RESET_Y 43
#define TILE_Y_AGENT 208         // 专用页面上计数格上移,给两张卡片让出间距
#define TILE_LABEL_Y 30

// 设置菜单面板:盖住标题栏以下的区域,标题栏的电量与音量图标保持可见。
// 以下除 MENU_Y 外都是面板内坐标
#define MENU_Y 44
#define MENU_H 258
#define MENU_PAD 14              // 标题、当前值与提示的左右边距
#define MENU_RIGHT (CONTENT_W - MENU_PAD)
#define MENU_TITLE_Y 12
#define MENU_HINT_Y 218
#define MENU_MAX_BARS 5
// 设置项图标按 14×12 的格子绘制:一级菜单每格 2 像素,二级每格 3 像素
#define ICON_GRID_W 14
#define ICON_GRID_H 12
#define ROW_ICON_SCALE 2
#define ROW_ICON_W (ICON_GRID_W * ROW_ICON_SCALE)
#define ROW_ICON_H (ICON_GRID_H * ROW_ICON_SCALE)
#define ADJ_ICON_SCALE 3
#define ADJ_ICON_W (ICON_GRID_W * ADJ_ICON_SCALE)
#define ADJ_ICON_H (ICON_GRID_H * ADJ_ICON_SCALE)
// 像素箭头 ‹ ›,与上下调节提示 ▲▼
#define CHEV_W 6
#define CHEV_H 10
#define ARROWS_W 10
#define ARROWS_H 36
// 一级菜单的行;ROW_ 开头的偏移以行的左上角为原点
#define ROW_X 12
#define ROW_Y 44
#define ROW_W 180
#define ROW_H 70
#define ROW_GAP 10
#define ROW_BOX 48               // 行内图标底块
#define ROW_BOX_X 10
#define ROW_BOX_Y 11
#define ROW_TEXT_X 70            // 名称与小档位条的左边界
#define ROW_LABEL_Y 14
#define ROW_BARS_BOTTOM 56
#define ROW_VALUE_RIGHT 158
#define ROW_VALUE_Y 27
#define ROW_CHEV_X 165
#define ROW_CHEV_Y 30
// 二级调节
#define ADJ_BOX_X 60
#define ADJ_BOX_Y 46
#define ADJ_BOX_W 84
#define ADJ_BOX_H 72
#define ADJ_BARS_X 18
#define ADJ_BARS_W 152
#define ADJ_BARS_BOTTOM 184
#define ADJ_BAR_MIN_H 10
#define ADJ_BAR_MAX_H 32
#define ADJ_ARROWS_X 181
#define ADJ_ARROWS_Y (ADJ_BARS_BOTTOM - ARROWS_H)

// ---- 静态像素缓冲 -------------------------------------------------------
LV_DRAW_BUF_DEFINE_STATIC(s_header_buf, CONTENT_W, HEADER_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_big_buf, BIG_W, BIG_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_scene_buf, SCENE_W, SCENE_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_tile0_buf, TILE_CANVAS_W, TILE_CANVAS_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_tile1_buf, TILE_CANVAS_W, TILE_CANVAS_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_tile2_buf, TILE_CANVAS_W, TILE_CANVAS_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_alert_buf, ALERT_NUM_W, ALERT_NUM_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_row0_icon_buf, ROW_ICON_W, ROW_ICON_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_row1_icon_buf, ROW_ICON_W, ROW_ICON_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_row0_chev_buf, CHEV_W, CHEV_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_row1_chev_buf, CHEV_W, CHEV_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_back_buf, CHEV_W, CHEV_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_adj_icon_buf, ADJ_ICON_W, ADJ_ICON_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(s_arrows_buf, ARROWS_W, ARROWS_H, LV_COLOR_FORMAT_RGB565);

// ---- LVGL 对象 -----------------------------------------------------------
static lv_obj_t *s_scr;
static lv_obj_t *s_header;
// 普通画面
static lv_obj_t *s_state_label;
static lv_obj_t *s_big;
static lv_obj_t *s_unit_label;
static lv_obj_t *s_speed_label;
static lv_obj_t *s_bars[4];
static lv_obj_t *s_tiles[3];
static lv_obj_t *s_tile_canvas[3];
static lv_obj_t *s_tile_label[3];   // 计数格的中文标签,只在专用页面显示
static lv_obj_t *s_done_label;
static lv_obj_t *s_wait_short_label;
// 工具专用页面:工具名、页码点与两张用量卡片放在同一分组里整组显隐
typedef struct {
    lv_obj_t *box;
    lv_obj_t *pct;        // 百分比像素字
    lv_obj_t *fill;       // 进度条的已用部分,画在轨道之上
    lv_obj_t *reset;      // 距重置时间或缺数据的原因
    lv_draw_buf_t buf;    // 指向首页大数字缓冲的一半
} usage_card_t;
static lv_obj_t *s_agent;
static lv_obj_t *s_agent_title;
static lv_obj_t *s_plan_chip;     // 工具名右侧的套餐徽标:轨道色圆角底块
static lv_obj_t *s_plan_label;    // 徽标里的套餐名,强调色
static lv_obj_t *s_page_dots[AR_PAGE_COUNT];
static usage_card_t s_cards[2];
// 场景
static lv_obj_t *s_scene;
// 等你处理
static lv_obj_t *s_alert;
static lv_obj_t *s_alert_num;
static lv_obj_t *s_alert_label;
static lv_obj_t *s_list;
static lv_obj_t *s_list_tool[2];
static lv_obj_t *s_list_reason[2];
static lv_obj_t *s_list_divider;
static lv_obj_t *s_others_label;
static lv_obj_t *s_hint_label;
// 未连接
static lv_obj_t *s_disc_hint;
// 蓝牙配对确认
static lv_obj_t *s_pair_title;
static lv_obj_t *s_pair_name;
static lv_obj_t *s_pair_hint;
// 设置菜单:面板上直接放标题、当前值与提示,一级与二级的其余元素各放一组整组显隐
static lv_obj_t *s_menu;
static lv_obj_t *s_menu_title;
static lv_obj_t *s_menu_value;
static lv_obj_t *s_menu_hint;
static lv_obj_t *s_menu_list;
static lv_obj_t *s_menu_adjust;
// 一级菜单的行(下标即 ar_menu_item_t)
typedef struct {
    lv_obj_t *bg;
    lv_obj_t *box;
    lv_obj_t *icon;
    lv_obj_t *label;
    lv_obj_t *value;
    lv_obj_t *chev;
    lv_obj_t *bars[MENU_MAX_BARS];
    lv_draw_buf_t *icon_buf;
    lv_draw_buf_t *chev_buf;
    int bar_count;
} menu_row_t;
static menu_row_t s_rows[AR_MENU_ITEMS];
// 二级调节
static lv_obj_t *s_adj_back;
static lv_obj_t *s_adj_icon;
static lv_obj_t *s_adj_bars[MENU_MAX_BARS];

// 上一次的视图,用于跳过没有变化的重绘
static ar_view_t s_last_view;
static bool s_has_last;
static bool s_last_pairing;

// 宠物调色板:正常与变暗(未连接)两套
static uint16_t s_palette[AR_SPRITE_COLORS];
static uint16_t s_palette_dim[AR_SPRITE_COLORS];

// ------------------------------------------------------------------------
// 小工具
// ------------------------------------------------------------------------

// 取 canvas 静态缓冲对应的可绘制区域
static ar_canvas_t canvas_of(lv_draw_buf_t *buf) {
    ar_canvas_t c = {
        .px = (uint16_t *)buf->data,
        .w = (int)buf->header.w,
        .h = (int)buf->header.h,
        .stride = (int)(buf->header.stride / sizeof(uint16_t)),
    };
    return c;
}

static lv_obj_t *make_canvas(lv_obj_t *parent, lv_draw_buf_t *buf, int x, int y) {
    lv_obj_t *c = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(c, buf);
    lv_obj_set_pos(c, x, y);
    return c;
}

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t color, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &ar_font_cjk14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

// 纯色、无边框、无滚动的矩形容器
static lv_obj_t *make_box(lv_obj_t *parent, uint32_t color, int x, int y, int w, int h, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static void show(lv_obj_t *o, bool visible) {
    if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// 把 canvas 四角涂成外部底色,模拟圆角
static void round_corners(ar_canvas_t *c, int r, uint16_t outside) {
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int dx = r - x, dy = r - y;
            if (dx * dx + dy * dy <= r * r) continue;
            c->px[y * c->stride + x] = outside;
            c->px[y * c->stride + (c->w - 1 - x)] = outside;
            c->px[(c->h - 1 - y) * c->stride + x] = outside;
            c->px[(c->h - 1 - y) * c->stride + (c->w - 1 - x)] = outside;
        }
    }
}

// 在 canvas 中水平居中画一段像素字
static void text_centered(ar_canvas_t *c, int y, const char *s, int scale, uint16_t color) {
    int x = (c->w - ar_raster_text_width(s, scale)) / 2;
    ar_raster_text(c, x, y, s, scale, color);
}

// ------------------------------------------------------------------------
// 像素元素的绘制
// ------------------------------------------------------------------------

// 标题栏:左侧 AGENT RUN!,右侧从左到右依次为音量、链路(USB 或蓝牙)、电量百分比、电池图标
static void draw_header(const ar_view_t *v) {
    ar_canvas_t c = canvas_of(&s_header_buf);
    const uint16_t ink = AR_RGB565(C_TEXT2);
    ar_raster_fill(&c, AR_RGB565(C_BG));
    ar_raster_text(&c, 0, 0, "AGENT RUN!", 2, ink);

    // 电池:15×9 外框 + 2×3 正极,内部按电量填充,低于 20% 用橙色
    int bx = c.w - 18, by = 3;
    ar_raster_rect(&c, bx, by, 15, 1, ink);
    ar_raster_rect(&c, bx, by + 8, 15, 1, ink);
    ar_raster_rect(&c, bx, by, 1, 9, ink);
    ar_raster_rect(&c, bx + 14, by, 1, 9, ink);
    ar_raster_rect(&c, bx + 15, by + 3, 2, 3, ink);
    int x = bx - 4;
    if (v->battery_soc >= 0) {
        int soc = v->battery_soc > 100 ? 100 : v->battery_soc;
        int fill = (11 * soc + 50) / 100;
        uint16_t color = soc < 20 ? AR_RGB565(C_ALERT) : ink;
        ar_raster_rect(&c, bx + 2, by + 2, fill, 5, color);

        // 电池图标左侧的百分比数字,与图标同色
        char pct[5];
        snprintf(pct, sizeof(pct), "%d%%", soc);
        x -= ar_raster_text_width(pct, 1);
        ar_raster_text(&c, x, 4, pct, 1, color);
        x -= 4;
    }
    if (v->link_source == AR_SRC_USB) {
        // USB 插头:两根插针、插头主体、线缆
        x -= 6;
        ar_raster_rect(&c, x + 1, 3, 1, 2, ink);
        ar_raster_rect(&c, x + 4, 3, 1, 2, ink);
        ar_raster_rect(&c, x, 5, 6, 2, ink);
        ar_raster_rect(&c, x + 1, 7, 4, 1, ink);
        ar_raster_rect(&c, x + 2, 8, 2, 3, ink);
        x -= 5;
    } else if (v->link_source == AR_SRC_BLE) {
        // 蓝牙标志:竖线加两侧的折线,5×7
        x -= 5;
        static const uint8_t px[][2] = {
            {3, 0}, {0, 1}, {4, 1}, {1, 2}, {3, 2}, {1, 4}, {3, 4}, {0, 5}, {4, 5}, {3, 6},
        };
        ar_raster_rect(&c, x + 2, 3, 1, 7, ink);
        for (size_t i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
            ar_raster_rect(&c, x + px[i][0], 3 + px[i][1], 1, 1, ink);
        }
        x -= 5;
    }
    // 音量:喇叭 + 档位条(低、中、高依次 1-3 根,由矮到高);静音时为喇叭 + 叉号
    x -= 10;
    ar_raster_rect(&c, x, 5, 2, 4, ink);
    ar_raster_rect(&c, x + 2, 4, 1, 6, ink);
    ar_raster_rect(&c, x + 3, 3, 1, 8, ink);
    if (v->volume == 0) {
        for (int i = 0; i < 4; i++) {
            ar_raster_rect(&c, x + 5 + i, 5 + i, 1, 1, ink);
            ar_raster_rect(&c, x + 8 - i, 5 + i, 1, 1, ink);
        }
    } else {
        for (int i = 0; i < v->volume && i < 3; i++) {
            int h = 2 + 2 * i;
            ar_raster_rect(&c, x + 5 + 2 * i, 8 - h / 2, 1, h, ink);
        }
    }
    lv_obj_invalidate(s_header);
}

// 大数字:工作中合计(睡觉时用暗色)
static int draw_big_number(unsigned value, bool dim) {
    char text[4];
    if (value > 99) value = 99;
    snprintf(text, sizeof(text), "%u", value);
    ar_canvas_t c = canvas_of(&s_big_buf);
    ar_raster_fill(&c, AR_RGB565(C_BG));
    ar_raster_text(&c, 0, 0, text, BIG_SCALE, AR_RGB565(dim ? C_DIM : C_ACCENT));
    lv_obj_invalidate(s_big);
    return ar_raster_text_width(text, BIG_SCALE);
}

// 计数格:上方数字,下方工具代号
static void draw_tile(int index, unsigned value) {
    static lv_draw_buf_t *const bufs[3] = { &s_tile0_buf, &s_tile1_buf, &s_tile2_buf };
    static const char *const names[3] = { "CC", "CODEX", "DSH" };
    char text[4];
    if (value > 99) value = 99;
    snprintf(text, sizeof(text), "%u", value);
    ar_canvas_t c = canvas_of(bufs[index]);
    ar_raster_fill(&c, AR_RGB565(C_TILE));
    text_centered(&c, 2, text, 3, AR_RGB565(value ? C_TEXT : C_DIM));
    text_centered(&c, 28, names[index], 2, AR_RGB565(C_TEXT2));
    lv_obj_invalidate(s_tile_canvas[index]);
}

// 专用页面的计数格:只画数字(上限 999),标签由中文标签对象显示;为 0 时用暗色
static void draw_agent_tile(int index, unsigned value, uint32_t color) {
    static lv_draw_buf_t *const bufs[3] = { &s_tile0_buf, &s_tile1_buf, &s_tile2_buf };
    char text[4];
    if (value > 999) value = 999;
    snprintf(text, sizeof(text), "%u", value);
    ar_canvas_t c = canvas_of(bufs[index]);
    ar_raster_fill(&c, AR_RGB565(C_TILE));
    text_centered(&c, 2, text, 3, AR_RGB565(value ? color : C_DIM));
    lv_obj_invalidate(s_tile_canvas[index]);
}

// 提醒条里的数字
static int draw_alert_number(unsigned value) {
    char text[4];
    if (value > 99) value = 99;
    snprintf(text, sizeof(text), "%u", value);
    ar_canvas_t c = canvas_of(&s_alert_buf);
    ar_raster_fill(&c, AR_RGB565(C_ALERT));
    ar_raster_text(&c, 0, 0, text, 4, AR_RGB565(C_ALERT_INK));
    lv_obj_invalidate(s_alert_num);
    return ar_raster_text_width(text, 4);
}

// 在设置项图标的 14×12 格子里画一块矩形(格子坐标,每格 scale 像素)
static void icon_cells(ar_canvas_t *c, int scale, int gx, int gy, int gw, int gh, uint16_t color) {
    ar_raster_rect(c, gx * scale, gy * scale, gw * scale, gh * scale, color);
}

// 设置项图标,bg 为所在底块的颜色:
// - 音量:喇叭加三道声波,已达到的档位用亮色,其余用轨道色;静音时喇叭变灰并画叉号;
// - 亮度:像素太阳。
// active 为 false(一级菜单里没选中的行)时亮色换成暗色,叉号也一样
static void draw_setting_icon(lv_draw_buf_t *buf, lv_obj_t *canvas, int scale, uint8_t item, uint8_t volume,
                              bool active, uint32_t bg) {
    ar_canvas_t c = canvas_of(buf);
    const uint16_t ink = AR_RGB565(active ? C_ACCENT : C_DIM);
    ar_raster_fill(&c, AR_RGB565(bg));
    if (item == AR_MENU_VOLUME) {
        const uint16_t body = volume ? ink : AR_RGB565(C_DIM);
        icon_cells(&c, scale, 1, 4, 3, 4, body);    // 底座
        icon_cells(&c, scale, 4, 3, 1, 6, body);    // 喇叭口逐列加高
        icon_cells(&c, scale, 5, 2, 1, 8, body);
        icon_cells(&c, scale, 6, 1, 1, 10, body);
        if (volume == 0) {
            const uint16_t cross = AR_RGB565(active ? C_ALERT : C_DIM);
            for (int i = 0; i < 5; i++) {
                icon_cells(&c, scale, 9 + i, 4 + i, 1, 1, cross);
                icon_cells(&c, scale, 13 - i, 4 + i, 1, 1, cross);
            }
        } else {
            // 第 k 道声波:上端一格、竖线、下端一格,越往外越高
            for (int k = 0; k < 3; k++) {
                const uint16_t color = k < volume ? ink : AR_RGB565(C_TRACK);
                const int x = 8 + 2 * k;
                icon_cells(&c, scale, x, 4 - k, 1, 1, color);
                icon_cells(&c, scale, x + 1, 5 - k, 1, 2 + 2 * k, color);
                icon_cells(&c, scale, x, 7 + k, 1, 1, color);
            }
        }
    } else {
        icon_cells(&c, scale, 5, 4, 4, 4, ink);   // 日面
        icon_cells(&c, scale, 6, 1, 2, 2, ink);   // 上下左右四道光
        icon_cells(&c, scale, 6, 9, 2, 2, ink);
        icon_cells(&c, scale, 2, 5, 2, 2, ink);
        icon_cells(&c, scale, 10, 5, 2, 2, ink);
        icon_cells(&c, scale, 3, 2, 1, 1, ink);   // 斜向四个光点
        icon_cells(&c, scale, 10, 2, 1, 1, ink);
        icon_cells(&c, scale, 3, 9, 1, 1, ink);
        icon_cells(&c, scale, 10, 9, 1, 1, ink);
    }
    lv_obj_invalidate(canvas);
}

// 像素箭头:right 为 true 画 ›,否则画 ‹;由 5 个 2×2 方块排成折线
static void draw_chevron(lv_draw_buf_t *buf, lv_obj_t *canvas, bool right, uint32_t color, uint32_t bg) {
    static const uint8_t offset[5] = { 0, 2, 4, 2, 0 };   // 每个方块离尖端反方向的横向偏移
    ar_canvas_t c = canvas_of(buf);
    ar_raster_fill(&c, AR_RGB565(bg));
    for (int i = 0; i < 5; i++) {
        const int x = right ? offset[i] : CHEV_W - 2 - offset[i];
        ar_raster_rect(&c, x, 2 * i, 2, 2, AR_RGB565(color));
    }
    lv_obj_invalidate(canvas);
}

// 二级调节右侧的上下箭头 ▲▼:各三行,逐行加宽,上下对称
static void draw_arrows(void) {
    ar_canvas_t c = canvas_of(&s_arrows_buf);
    const uint16_t ink = AR_RGB565(C_DIM);
    ar_raster_fill(&c, AR_RGB565(C_TILE));
    for (int i = 0; i < 3; i++) {
        const int w = 2 + 4 * i;
        const int x = (ARROWS_W - w) / 2;
        ar_raster_rect(&c, x, 2 * i, w, 2, ink);
        ar_raster_rect(&c, x, ARROWS_H - 2 - 2 * i, w, 2, ink);
    }
}

// ------------------------------------------------------------------------
// 场景
// ------------------------------------------------------------------------

// 地面移动线:按 tick 向左滚动,dash/gap/step 决定疏密与速度
static void draw_ground_dashes(ar_canvas_t *c, int dash, int gap, int step, uint32_t tick, uint16_t color) {
    int period = dash + gap;
    int offset = (int)((tick * (uint32_t)step) % (uint32_t)period);
    for (int x = 6 - offset; x < c->w; x += period) {
        ar_raster_rect(c, x, c->h - 11, dash, 2, color);
    }
}

// 以“距场景底边”的坐标画一块矩形(屏幕稿用的是 bottom 定位)
static void rect_from_bottom(ar_canvas_t *c, int x, int bottom, int w, int h, uint16_t color) {
    ar_raster_rect(c, x, c->h - bottom - h, w, h, color);
}

static void sprite_at(ar_canvas_t *c, ar_pose_t pose, uint32_t tick, int x, int bottom,
                      const uint16_t *palette, uint8_t alpha) {
    const ar_sprite_frame_t *f = ar_sprite_frame(pose, (int)(tick & 1u));
    int scale = 6;
    ar_raster_sprite(c, f, x, c->h - bottom - AR_SPRITE_H * scale, scale, palette, alpha);
}

void ar_ui_animate(ar_screen_t screen, uint32_t tick) {
    ar_canvas_t c = canvas_of(&s_scene_buf);
    const uint16_t bg = AR_RGB565(C_SCENE);
    const uint16_t line2 = AR_RGB565(C_LINE2);
    const uint16_t accent = AR_RGB565(C_ACCENT);
    const int flicker = (tick & 1u) ? 4 : 0;   // 速度线前后抖动,增加动感

    ar_raster_fill(&c, bg);
    ar_raster_rect(&c, 0, c.h - 20, c.w, 2, AR_RGB565(C_TRACK));   // 地面

    switch (screen) {
    case AR_SCREEN_DISCONNECTED:
    case AR_SCREEN_SLEEP: {
        bool dim = screen == AR_SCREEN_DISCONNECTED;
        uint16_t z = AR_RGB565(dim ? C_TRACK : C_DIM);
        int lift = (tick & 1u) ? 2 : 0;   // Z 上下浮动
        ar_raster_text(&c, 146, 30 - lift, "Z", 1, z);
        ar_raster_text(&c, 155, 16 - lift, "Z", 2, z);
        ar_raster_text(&c, 170, 2 - lift / 2, "Z", 2, z);
        sprite_at(&c, AR_POSE_SLEEP, tick, 66, 14, dim ? s_palette_dim : s_palette, 255);
        break;
    }
    case AR_SCREEN_WALK:
        draw_ground_dashes(&c, 8, 14, 2, tick, AR_RGB565(C_TRACK));
        sprite_at(&c, AR_POSE_WALK, tick, 68, 20, s_palette, 255);
        break;
    case AR_SCREEN_TROT:
        rect_from_bottom(&c, 30 - flicker, 62, 26, 3, line2);
        rect_from_bottom(&c, 18 - flicker, 46, 34, 3, line2);
        rect_from_bottom(&c, 70, 22, 4, 4, line2);
        draw_ground_dashes(&c, 10, 12, 4, tick, AR_RGB565(C_DASH_TROT));
        sprite_at(&c, AR_POSE_TROT, tick, 82, 20, s_palette, 255);
        break;
    case AR_SCREEN_RUN:
        ar_raster_rect_blend(&c, 20 - flicker, c.h - 72 - 3, 40, 3, accent, 115);
        rect_from_bottom(&c, 40 - flicker, 58, 28, 3, line2);
        rect_from_bottom(&c, 12 - flicker, 44, 46, 3, line2);
        rect_from_bottom(&c, 60, 22, 3, 3, line2);
        rect_from_bottom(&c, 68, 28, 4, 4, line2);
        rect_from_bottom(&c, 78, 22, 6, 6, line2);
        draw_ground_dashes(&c, 14, 10, 6, tick, line2);
        sprite_at(&c, AR_POSE_RUN, tick, 96, 20, s_palette, 255);
        break;
    case AR_SCREEN_DASH: {
        ar_raster_rect_blend(&c, 16 - flicker, c.h - 76 - 3, 44, 3, accent, 115);
        rect_from_bottom(&c, 34 - flicker, 62, 30, 3, line2);
        ar_raster_rect_blend(&c, 8 - flicker, c.h - 48 - 3, 50, 3, accent, 115);
        rect_from_bottom(&c, 28 - flicker, 34, 26, 3, line2);
        sprite_at(&c, AR_POSE_DASH, 0, 66, 20, s_palette, 56);   // 残影
        uint16_t smoke = AR_RGB565(C_DIM);
        rect_from_bottom(&c, 76 + flicker / 2, 22, 4, 4, smoke);
        rect_from_bottom(&c, 84, 30 - flicker / 2, 5, 5, smoke);
        rect_from_bottom(&c, 92, 22, 6, 6, smoke);
        draw_ground_dashes(&c, 18, 8, 9, tick, line2);
        sprite_at(&c, AR_POSE_DASH, tick, 106, 20, s_palette, 255);
        break;
    }
    case AR_SCREEN_WAITING: {
        sprite_at(&c, AR_POSE_WAVE, tick, 54, 20, s_palette, 255);
        // 头顶的感叹号气泡
        uint16_t alert = AR_RGB565(C_ALERT);
        ar_raster_rect(&c, 149, 4, 20, 22, alert);
        ar_raster_rect(&c, 148, 5, 22, 20, alert);
        ar_raster_rect(&c, 152, 26, 4, 3, alert);   // 气泡尖角
        ar_raster_text(&c, 154, 8, "!", 2, AR_RGB565(C_ALERT_INK));
        break;
    }
    }
    round_corners(&c, 12, AR_RGB565(C_BG));
    lv_obj_invalidate(s_scene);
}

// ------------------------------------------------------------------------
// 创建与更新
// ------------------------------------------------------------------------

static const char *state_text(ar_screen_t screen) {
    switch (screen) {
    case AR_SCREEN_WALK: return AR_TXT_WALK;
    case AR_SCREEN_TROT: return AR_TXT_TROT;
    case AR_SCREEN_RUN: return AR_TXT_RUN;
    case AR_SCREEN_DASH: return AR_TXT_DASH;
    default: return AR_TXT_SLEEP;
    }
}

static const char *tool_text(uint8_t tool) {
    switch (tool) {
    case AR_TOOL_CX: return AR_TXT_TOOL_CX;
    case AR_TOOL_DS: return AR_TXT_TOOL_DS;
    default: return AR_TXT_TOOL_CC;
    }
}

// 透明、铺满父对象的分组容器,只用来整组显隐
static lv_obj_t *make_group(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    return o;
}

// 工具专用页面:工具名、页码点、两张用量卡片(标题、百分比、进度条、距重置时间),
// 以及首页计数格里的中文标签。在设置菜单之前创建,菜单面板叠在它上面
static void create_agent_page(void) {
    static const char *const tile_names[3] = { AR_TXT_TILE_WORKING, AR_TXT_TILE_BACKGROUND, AR_TXT_DONE_TODAY };
    s_agent = make_group(s_scr);
    s_agent_title = make_label(s_agent, C_TEXT, PAD_X, AGENT_TITLE_Y);
    // 页码点靠右,三点依次为首页、Claude Code、Codex
    for (int i = 0; i < AR_PAGE_COUNT; i++) {
        const int x = PAD_X + CONTENT_W - (AR_PAGE_COUNT - i) * (PAGE_DOT + PAGE_DOT_GAP) + PAGE_DOT_GAP;
        s_page_dots[i] = make_box(s_agent, C_TRACK, x, AGENT_TITLE_Y + 5, PAGE_DOT, PAGE_DOT, PAGE_DOT / 2);
    }
    // 套餐徽标:位置与宽度随工具名、套餐名变化,在 update_plan_chip() 里计算
    s_plan_chip = make_box(s_agent, C_TRACK, PAD_X, AGENT_TITLE_Y, PLAN_CHIP_H, PLAN_CHIP_H, PLAN_CHIP_H / 2);
    s_plan_label = make_label(s_plan_chip, C_ACCENT, PLAN_CHIP_PAD, 0);
    lv_label_set_long_mode(s_plan_label, LV_LABEL_LONG_MODE_CLIP);
    show(s_plan_chip, false);

    // 两张卡片的百分比画在首页大数字缓冲的上下两半:专用页面显示时首页大数字是隐藏的,
    // 回到首页时 ar_ui_update 会重画大数字,因此不必另占约 6 KB 内存
    const uint32_t stride = s_big_buf.header.stride;
    for (int i = 0; i < 2; i++) {
        usage_card_t *c = &s_cards[i];
        lv_draw_buf_init(&c->buf, PCT_W, PCT_H, LV_COLOR_FORMAT_RGB565, stride,
                         s_big_buf.data + (size_t)i * PCT_H * stride, PCT_H * stride);
        lv_draw_buf_set_flag(&c->buf, LV_IMAGE_FLAGS_MODIFIABLE);
        c->box = make_box(s_agent, C_TILE, PAD_X, i == 0 ? CARD_Y0 : CARD_Y1, CONTENT_W, CARD_H, 10);
        lv_obj_t *title = make_label(c->box, C_TEXT2, CARD_PAD, CARD_TITLE_Y);
        lv_label_set_text(title, i == 0 ? AR_TXT_USAGE_FIVE_HOUR : AR_TXT_USAGE_WEEK);
        c->pct = make_canvas(c->box, &c->buf, CONTENT_W - CARD_PAD - PCT_W, PCT_Y);
        make_box(c->box, C_TRACK, CARD_PAD, USAGE_BAR_Y, USAGE_BAR_W, USAGE_BAR_H, USAGE_BAR_H / 2);
        c->fill = make_box(c->box, C_ACCENT, CARD_PAD, USAGE_BAR_Y, USAGE_BAR_H, USAGE_BAR_H, USAGE_BAR_H / 2);
        c->reset = make_label(c->box, C_TEXT2, CARD_PAD, CARD_RESET_Y);
    }
    show(s_agent, false);

    // 计数格标签:放进首页的计数格,创建在格内画布之后,叠在画布上方
    for (int i = 0; i < 3; i++) {
        s_tile_label[i] = make_label(s_tiles[i], C_TEXT2, 0, 0);
        lv_label_set_text(s_tile_label[i], tile_names[i]);
        lv_obj_align(s_tile_label[i], LV_ALIGN_TOP_MID, 0, TILE_LABEL_Y);
        show(s_tile_label[i], false);
    }
}

// 设置菜单面板:深色面板加一圈细边框,与现有卡片同一风格;最后创建,叠在其他元素之上。
// 行内元素都直接放在分组里,不再套一层行容器,控制对象嵌套深度(截屏渲染在主任务栈上)
static void create_menu(void) {
    static lv_draw_buf_t *const icon_bufs[AR_MENU_ITEMS] = { &s_row0_icon_buf, &s_row1_icon_buf };
    static lv_draw_buf_t *const chev_bufs[AR_MENU_ITEMS] = { &s_row0_chev_buf, &s_row1_chev_buf };
    LV_DRAW_BUF_INIT_STATIC(s_row0_icon_buf);
    LV_DRAW_BUF_INIT_STATIC(s_row1_icon_buf);
    LV_DRAW_BUF_INIT_STATIC(s_row0_chev_buf);
    LV_DRAW_BUF_INIT_STATIC(s_row1_chev_buf);
    LV_DRAW_BUF_INIT_STATIC(s_back_buf);
    LV_DRAW_BUF_INIT_STATIC(s_adj_icon_buf);
    LV_DRAW_BUF_INIT_STATIC(s_arrows_buf);

    s_menu = make_box(s_scr, C_TILE, PAD_X, MENU_Y, CONTENT_W, MENU_H, 14);
    lv_obj_set_style_border_color(s_menu, lv_color_hex(C_LINE2), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_menu, 1, LV_PART_MAIN);
    s_menu_list = make_group(s_menu);
    s_menu_adjust = make_group(s_menu);

    // 一级菜单:每行依次为行底、图标底块、图标、名称、小档位条、当前值、›
    for (int i = 0; i < AR_MENU_ITEMS; i++) {
        menu_row_t *r = &s_rows[i];
        const int y = ROW_Y + i * (ROW_H + ROW_GAP);
        r->icon_buf = icon_bufs[i];
        r->chev_buf = chev_bufs[i];
        r->bar_count = i == AR_MENU_VOLUME ? AR_VOLUME_LEVELS - 1 : AR_BRIGHTNESS_LEVELS;
        r->bg = make_box(s_menu_list, C_SCENE, ROW_X, y, ROW_W, ROW_H, 12);
        lv_obj_set_style_border_color(r->bg, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        r->box = make_box(s_menu_list, C_BG, ROW_X + ROW_BOX_X, y + ROW_BOX_Y, ROW_BOX, ROW_BOX, 10);
        r->icon = make_canvas(s_menu_list, r->icon_buf, ROW_X + ROW_BOX_X + (ROW_BOX - ROW_ICON_W) / 2,
                              y + ROW_BOX_Y + (ROW_BOX - ROW_ICON_H) / 2);
        r->label = make_label(s_menu_list, C_TEXT2, ROW_X + ROW_TEXT_X, y + ROW_LABEL_Y);
        lv_label_set_text(r->label, i == AR_MENU_VOLUME ? AR_TXT_MENU_VOLUME : AR_TXT_MENU_BRIGHTNESS);
        // 小档位条:由矮到高,音量 3 根、亮度 5 根,根数多时收窄
        const int bar_w = r->bar_count <= 3 ? 10 : 8;
        for (int b = 0; b < r->bar_count; b++) {
            const int h = 4 + 8 * b / (r->bar_count - 1);
            r->bars[b] = make_box(s_menu_list, C_TRACK, ROW_X + ROW_TEXT_X + b * (bar_w + 3),
                                  y + ROW_BARS_BOTTOM - h, bar_w, h, 2);
        }
        r->value = make_label(s_menu_list, C_TEXT2, 0, y + ROW_VALUE_Y);
        r->chev = make_canvas(s_menu_list, r->chev_buf, ROW_X + ROW_CHEV_X, y + ROW_CHEV_Y);
    }

    // 二级调节:标题前的 ‹、放大的图标、由矮到高的档位条、右侧的 ▲▼
    s_adj_back = make_canvas(s_menu_adjust, &s_back_buf, MENU_PAD, MENU_TITLE_Y + 4);
    draw_chevron(&s_back_buf, s_adj_back, false, C_DIM, C_TILE);
    make_box(s_menu_adjust, C_BG, ADJ_BOX_X, ADJ_BOX_Y, ADJ_BOX_W, ADJ_BOX_H, 12);
    s_adj_icon = make_canvas(s_menu_adjust, &s_adj_icon_buf, ADJ_BOX_X + (ADJ_BOX_W - ADJ_ICON_W) / 2,
                             ADJ_BOX_Y + (ADJ_BOX_H - ADJ_ICON_H) / 2);
    for (int i = 0; i < MENU_MAX_BARS; i++) s_adj_bars[i] = make_box(s_menu_adjust, C_TRACK, 0, 0, 1, 1, 4);
    make_canvas(s_menu_adjust, &s_arrows_buf, ADJ_ARROWS_X, ADJ_ARROWS_Y);
    draw_arrows();

    // 标题、当前值与提示两级共用,放在面板上
    s_menu_title = make_label(s_menu, C_TEXT, MENU_PAD, MENU_TITLE_Y);
    s_menu_value = make_label(s_menu, C_ACCENT, 0, MENU_TITLE_Y);
    s_menu_hint = make_label(s_menu, C_DIM, 0, MENU_HINT_Y);
    show(s_menu, false);
}

void ar_ui_create(void) {
    // 宠物调色板;变暗版与场景底色按 55% 混合
    const uint32_t colors[AR_SPRITE_COLORS] = {
        [AR_SPRITE_BODY] = C_ACCENT, [AR_SPRITE_SHADE] = 0xBF943A, [AR_SPRITE_BELLY] = 0xFFD98B,
        [AR_SPRITE_EYE] = 0x141821, [AR_SPRITE_CHEEK] = 0xFF8A8A, [AR_SPRITE_SWEAT] = 0x8FD3FF,
    };
    for (int i = 0; i < AR_SPRITE_COLORS; i++) {
        s_palette[i] = AR_RGB565(colors[i]);
        s_palette_dim[i] = ar_rgb565_blend(AR_RGB565(C_SCENE), s_palette[i], 115);
    }

    LV_DRAW_BUF_INIT_STATIC(s_header_buf);
    LV_DRAW_BUF_INIT_STATIC(s_big_buf);
    LV_DRAW_BUF_INIT_STATIC(s_scene_buf);
    LV_DRAW_BUF_INIT_STATIC(s_tile0_buf);
    LV_DRAW_BUF_INIT_STATIC(s_tile1_buf);
    LV_DRAW_BUF_INIT_STATIC(s_tile2_buf);
    LV_DRAW_BUF_INIT_STATIC(s_alert_buf);

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_header = make_canvas(s_scr, &s_header_buf, PAD_X, HEADER_Y);

    // 普通画面:计数区
    s_state_label = make_label(s_scr, C_TEXT2, PAD_X, 36);
    s_big = make_canvas(s_scr, &s_big_buf, PAD_X, 56);
    s_unit_label = make_label(s_scr, C_TEXT2, PAD_X, 80);
    s_speed_label = make_label(s_scr, C_TEXT2, 194, 58);
    lv_label_set_text(s_speed_label, AR_TXT_SPEED);
    for (int i = 0; i < 4; i++) {
        int h = 7 + 3 * i;
        s_bars[i] = make_box(s_scr, C_TRACK, 195 + 7 * i, 98 - h, 6, h, 1);
    }

    s_scene = make_canvas(s_scr, &s_scene_buf, PAD_X, SCENE_Y_NORMAL);

    // 普通画面:三格计数
    for (int i = 0; i < 3; i++) {
        static lv_draw_buf_t *const bufs[3] = { &s_tile0_buf, &s_tile1_buf, &s_tile2_buf };
        s_tiles[i] = make_box(s_scr, C_TILE, PAD_X + i * (TILE_W + TILE_GAP), TILE_Y, TILE_W, TILE_H, 10);
        s_tile_canvas[i] = make_canvas(s_tiles[i], bufs[i], (TILE_W - TILE_CANVAS_W) / 2,
                                       (TILE_H - TILE_CANVAS_H) / 2);
    }

    // 普通画面:底栏
    s_done_label = make_label(s_scr, C_TEXT2, PAD_X + 6, 276);
    s_wait_short_label = make_label(s_scr, C_TEXT2, 0, 276);

    // 等你处理:提醒条
    s_alert = make_box(s_scr, C_ALERT, PAD_X, 38, CONTENT_W, 46, 12);
    s_alert_num = make_canvas(s_alert, &s_alert_buf, 12, 9);
    s_alert_label = make_label(s_alert, C_ALERT_INK, 0, 13);
    lv_label_set_text(s_alert_label, AR_TXT_WAITING_TITLE);

    // 等你处理:等待列表(最多显示两项)
    s_list = make_box(s_scr, C_TILE, PAD_X, 200, CONTENT_W, 61, 10);
    for (int i = 0; i < 2; i++) {
        s_list_tool[i] = make_label(s_list, C_TEXT, 10, 6 + 30 * i);
        s_list_reason[i] = make_label(s_list, C_ALERT, 0, 6 + 30 * i);
    }
    s_list_divider = make_box(s_list, C_TRACK, 10, 30, CONTENT_W - 20, 1, 0);
    s_others_label = make_label(s_scr, C_TEXT2, PAD_X + 6, 270);
    s_hint_label = make_label(s_scr, C_TEXT2, 0, 270);
    lv_label_set_text(s_hint_label, AR_TXT_MENU_HINT);

    // 未连接:第二行提示(内容随配对状态变化)
    s_disc_hint = make_label(s_scr, C_TEXT2, PAD_X, 60);

    // 蓝牙配对确认
    s_pair_title = make_label(s_scr, C_TEXT, PAD_X, 38);
    lv_label_set_text(s_pair_title, AR_TXT_PAIR_TITLE);
    s_pair_name = make_label(s_scr, C_TEXT2, PAD_X, 60);
    s_pair_hint = make_label(s_scr, C_TEXT2, PAD_X, 222);
    lv_label_set_text(s_pair_hint, AR_TXT_PAIR_HINT);

    create_agent_page();
    create_menu();

    s_has_last = false;
    lv_screen_load(s_scr);
}

// 把标签右对齐到 right(屏幕或父对象坐标)
static void align_right(lv_obj_t *label, int right, int y) {
    lv_obj_update_layout(label);
    lv_obj_set_pos(label, right - lv_obj_get_width(label), y);
}

// 把标签水平居中到面板内容区
static void align_center(lv_obj_t *label, int y) {
    lv_obj_update_layout(label);
    lv_obj_set_pos(label, (lv_obj_get_width(s_menu_list) - lv_obj_get_width(label)) / 2, y);
}

// 音量档位名:静音、低、中、高;越界按最高档
static const char *volume_name(uint8_t level) {
    static const char *const names[AR_VOLUME_LEVELS] = {
        AR_TXT_VOLUME_MUTE, AR_TXT_VOLUME_LOW, AR_TXT_VOLUME_MID, AR_TXT_VOLUME_HIGH,
    };
    return names[level < AR_VOLUME_LEVELS ? level : AR_VOLUME_LEVELS - 1];
}

// 设置项的当前值文字与亮起的档位数:音量为档位名与档位数,亮度为百分比与档位序号加一
static int setting_value(const ar_view_t *v, uint8_t item, lv_obj_t *label) {
    if (item == AR_MENU_VOLUME) {
        lv_label_set_text(label, volume_name(v->volume));
        return v->volume < AR_VOLUME_LEVELS ? v->volume : AR_VOLUME_LEVELS - 1;
    }
    lv_label_set_text_fmt(label, "%u%%", (unsigned)ar_brightness_percent(v->brightness));
    return (v->brightness < AR_BRIGHTNESS_LEVELS ? v->brightness : AR_BRIGHTNESS_LEVELS - 1) + 1;
}

// 一级菜单的一行:选中时行底加深、加黄色描边,图标、档位条与当前值亮起;
// 没选中时整行用暗色。静音时选中行的当前值用橙色
static void update_row(const ar_view_t *v, uint8_t item, bool selected) {
    menu_row_t *r = &s_rows[item];
    const int y = ROW_Y + item * (ROW_H + ROW_GAP);
    const uint32_t row_bg = selected ? C_BG : C_SCENE;
    const uint32_t box_bg = selected ? C_SCENE : C_BG;
    lv_obj_set_style_bg_color(r->bg, lv_color_hex(row_bg), LV_PART_MAIN);
    lv_obj_set_style_border_width(r->bg, selected ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r->box, lv_color_hex(box_bg), LV_PART_MAIN);
    draw_setting_icon(r->icon_buf, r->icon, ROW_ICON_SCALE, item, v->volume, selected, box_bg);
    lv_obj_set_style_text_color(r->label, lv_color_hex(selected ? C_TEXT : C_TEXT2), LV_PART_MAIN);

    const int active = setting_value(v, item, r->value);
    const bool muted = item == AR_MENU_VOLUME && v->volume == 0;
    const uint32_t value_color = !selected ? C_TEXT2 : muted ? C_ALERT : C_ACCENT;
    lv_obj_set_style_text_color(r->value, lv_color_hex(value_color), LV_PART_MAIN);
    align_right(r->value, ROW_X + ROW_VALUE_RIGHT, y + ROW_VALUE_Y);
    for (int b = 0; b < r->bar_count; b++) {
        const uint32_t color = b < active ? (selected ? C_ACCENT : C_DIM) : C_TRACK;
        lv_obj_set_style_bg_color(r->bars[b], lv_color_hex(color), LV_PART_MAIN);
    }
    draw_chevron(r->chev_buf, r->chev, true, selected ? C_ACCENT : C_DIM, row_bg);
}

// 二级的档位条:n 根由矮到高,前 active 根用强调色,铺满图标下方的宽度
static void layout_adjust_bars(int n, int active) {
    const int gap = n <= 3 ? 10 : 8;
    const int w = (ADJ_BARS_W - gap * (n - 1)) / n;
    for (int i = 0; i < MENU_MAX_BARS; i++) {
        show(s_adj_bars[i], i < n);
        if (i >= n) continue;
        const int h = ADJ_BAR_MIN_H + (ADJ_BAR_MAX_H - ADJ_BAR_MIN_H) * i / (n - 1);
        lv_obj_set_pos(s_adj_bars[i], ADJ_BARS_X + i * (w + gap), ADJ_BARS_BOTTOM - h);
        lv_obj_set_size(s_adj_bars[i], w, h);
        lv_obj_set_style_bg_color(s_adj_bars[i], lv_color_hex(i < active ? C_ACCENT : C_TRACK), LV_PART_MAIN);
    }
}

// 按 view 更新设置菜单:一级显示两行设置项,二级显示正在调节的项
static void update_menu(const ar_view_t *v, bool open) {
    show(s_menu, open);
    if (!open) return;
    const bool adjust = v->menu_level == AR_MENU_ADJUST;
    const uint8_t item = v->menu_item < AR_MENU_ITEMS ? v->menu_item : AR_MENU_VOLUME;
    show(s_menu_list, !adjust);
    show(s_menu_adjust, adjust);
    show(s_menu_value, adjust);
    if (!adjust) {
        lv_label_set_text(s_menu_title, AR_TXT_MENU_TITLE);
        lv_obj_set_x(s_menu_title, MENU_PAD);
        for (uint8_t i = 0; i < AR_MENU_ITEMS; i++) update_row(v, i, i == item);
        lv_label_set_text(s_menu_hint, AR_TXT_MENU_LIST_HINT);
    } else {
        // 标题让出左侧的 ‹;静音时当前值用橙色
        const bool volume = item == AR_MENU_VOLUME;
        lv_label_set_text(s_menu_title, volume ? AR_TXT_MENU_VOLUME : AR_TXT_MENU_BRIGHTNESS);
        lv_obj_set_x(s_menu_title, MENU_PAD + CHEV_W + 6);
        draw_setting_icon(&s_adj_icon_buf, s_adj_icon, ADJ_ICON_SCALE, item, v->volume, true, C_BG);
        const int active = setting_value(v, item, s_menu_value);
        const bool muted = volume && v->volume == 0;
        lv_obj_set_style_text_color(s_menu_value, lv_color_hex(muted ? C_ALERT : C_ACCENT), LV_PART_MAIN);
        align_right(s_menu_value, MENU_RIGHT, MENU_TITLE_Y);
        layout_adjust_bars(volume ? AR_VOLUME_LEVELS - 1 : AR_BRIGHTNESS_LEVELS, active);
        lv_label_set_text(s_menu_hint, AR_TXT_MENU_ADJUST_HINT);
    }
    align_center(s_menu_hint, MENU_HINT_Y);
}

// 一张用量卡片:百分比、进度条(≥80% 用警示色)、距重置时间。percent 为 0–100,
// 或 AR_USAGE_UNKNOWN(暂无数据)、AR_USAGE_NO_LIMIT(无此限额),这两种情况显示 -- 与原因
static void update_card(usage_card_t *c, uint8_t percent, uint16_t minutes) {
    const bool known = percent <= 100u;
    const bool alert = ar_usage_alert(percent);
    char text[5];
    if (known) snprintf(text, sizeof(text), "%u%%", (unsigned)percent);
    else snprintf(text, sizeof(text), "--");
    ar_canvas_t cv = canvas_of(&c->buf);
    ar_raster_fill(&cv, AR_RGB565(C_TILE));
    const uint32_t ink = !known ? C_DIM : alert ? C_ALERT : C_TEXT;
    ar_raster_text(&cv, cv.w - ar_raster_text_width(text, PCT_SCALE), 0, text, PCT_SCALE, AR_RGB565(ink));
    lv_obj_invalidate(c->pct);

    // 已用部分至少画成一个圆点,1% 也看得见
    int w = known ? USAGE_BAR_W * percent / 100 : 0;
    show(c->fill, w > 0);
    if (w > 0) {
        if (w < USAGE_BAR_H) w = USAGE_BAR_H;
        lv_obj_set_width(c->fill, w);
        lv_obj_set_style_bg_color(c->fill, lv_color_hex(alert ? C_ALERT : C_ACCENT), LV_PART_MAIN);
    }

    char reset[40];
    if (percent == AR_USAGE_NO_LIMIT) snprintf(reset, sizeof(reset), "%s", AR_TXT_USAGE_NO_LIMIT);
    else if (!known) snprintf(reset, sizeof(reset), "%s", AR_TXT_USAGE_NO_DATA);
    else ar_reset_text(minutes, reset, sizeof(reset));
    lv_obj_set_style_text_color(c->reset, lv_color_hex(known ? C_TEXT2 : C_DIM), LV_PART_MAIN);
    lv_label_set_text(c->reset, reset);
}

// 套餐徽标:紧跟在工具名右侧,宽度随套餐名;放不下时截断文字,不压到页码点。plan 为空串时隐藏
static void update_plan_chip(const char *plan) {
    show(s_plan_chip, plan[0] != '\0');
    if (plan[0] == '\0') return;

    lv_obj_update_layout(s_agent_title);
    const int title_h = lv_obj_get_height(s_agent_title);
    const int x = PAD_X + lv_obj_get_width(s_agent_title) + PLAN_CHIP_GAP;
    const int max_label_w = PAGE_DOTS_X - PLAN_CHIP_GAP - x - 2 * PLAN_CHIP_PAD;

    // 先按内容测出文字宽度,超过可用宽度时固定宽度并截断
    lv_obj_set_width(s_plan_label, LV_SIZE_CONTENT);
    lv_label_set_text(s_plan_label, plan);
    lv_obj_update_layout(s_plan_label);
    int label_w = lv_obj_get_width(s_plan_label);
    if (label_w > max_label_w) {
        label_w = max_label_w;
        lv_obj_set_width(s_plan_label, label_w);
    }
    lv_obj_set_pos(s_plan_label, PLAN_CHIP_PAD, (PLAN_CHIP_H - lv_obj_get_height(s_plan_label)) / 2);
    lv_obj_set_size(s_plan_chip, label_w + 2 * PLAN_CHIP_PAD, PLAN_CHIP_H);
    lv_obj_set_pos(s_plan_chip, x, AGENT_TITLE_Y + (title_h - PLAN_CHIP_H) / 2);
}

// 工具专用页面:工具名与套餐徽标、页码点、两张用量卡片,三格为工作中、后台运行、今日完成,底栏只有切换提示。
// 不显示“等你处理”:有会话等你处理时调用方显示提醒画面,专用页面出现时这个数必为 0
static void update_agent_page(const ar_view_t *v) {
    const bool cc = v->page == AR_PAGE_CC;
    const ar_agent_t *a = &v->agent;
    lv_label_set_text(s_agent_title, cc ? AR_TXT_TOOL_CC : AR_TXT_TOOL_CX);
    update_plan_chip(v->has_agent ? a->plan : "");   // 还没收到 A 帧时不显示套餐
    for (int i = 0; i < AR_PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_page_dots[i], lv_color_hex(i == v->page ? C_ACCENT : C_TRACK), LV_PART_MAIN);
    }
    // 还没收到该工具的 A 帧(主机端是旧版本等)时,用量按暂无数据显示,计数按 0
    update_card(&s_cards[0], v->has_agent ? a->h : AR_USAGE_UNKNOWN, a->hr);
    update_card(&s_cards[1], v->has_agent ? a->w : AR_USAGE_UNKNOWN, a->wr);

    // 工作中沿用状态帧的 cc / cx(含后台运行);刚有会话完成时“今日完成”用强调色
    draw_agent_tile(0, cc ? v->state.cc : v->state.cx, C_TEXT);
    draw_agent_tile(1, v->has_agent ? a->bg : 0, C_TEXT);
    draw_agent_tile(2, v->has_agent ? a->done : 0, v->agent_flash ? C_ACCENT : C_TEXT);
    lv_obj_set_style_text_color(s_tile_label[2], lv_color_hex(v->agent_flash ? C_ACCENT : C_TEXT2), LV_PART_MAIN);

    lv_label_set_text(s_wait_short_label, AR_TXT_PAGE_HINT);
    align_right(s_wait_short_label, PAD_X + CONTENT_W - 6, 276);
}

void ar_ui_update(const ar_view_t *v) {
    if (s_has_last && memcmp(v, &s_last_view, sizeof(*v)) == 0) return;
    const bool screen_changed = !s_has_last || v->screen != s_last_view.screen;
    const bool page_changed = !s_has_last || v->page != s_last_view.page;
    s_last_view = *v;
    s_has_last = true;

    // 配对确认优先于其他画面
    const bool pairing = v->pairing;
    const bool waiting = !pairing && v->screen == AR_SCREEN_WAITING;
    const bool disconnected = !pairing && v->screen == AR_SCREEN_DISCONNECTED;
    const bool normal = !pairing && !waiting && !disconnected;
    // 专用页面只在普通画面下出现;等你处理、未连接时调用方已把 page 换算成首页
    const bool agent_page = normal && v->page != AR_PAGE_HOME;
    // 设置菜单打开时只保留标题栏,画面内容整体隐藏,被面板盖住的动画也不再重绘;
    // 配对确认时不显示菜单
    const bool menu = !pairing && v->menu_level != AR_MENU_CLOSED;

    draw_header(v);

    // 各组元素的显隐
    const bool show_normal = normal && !agent_page && !menu;
    const bool show_agent = agent_page && !menu;
    const bool show_waiting = waiting && !menu;
    const bool show_disconnected = disconnected && !menu;
    show(s_big, show_normal);
    show(s_unit_label, show_normal);
    show(s_speed_label, show_normal);
    for (int i = 0; i < 4; i++) show(s_bars[i], show_normal);
    for (int i = 0; i < 3; i++) {
        show(s_tiles[i], show_normal || show_agent);
        show(s_tile_label[i], show_agent);
        lv_obj_set_y(s_tiles[i], agent_page ? TILE_Y_AGENT : TILE_Y);
    }
    show(s_agent, show_agent);
    show(s_done_label, show_normal);   // 专用页面底栏左侧留空,后台运行已移到中间一格
    show(s_wait_short_label, show_normal || show_agent);
    show(s_alert, show_waiting);
    show(s_list, show_waiting);
    show(s_others_label, show_waiting);
    show(s_hint_label, show_waiting);
    show(s_disc_hint, show_disconnected);
    show(s_state_label, show_normal || show_disconnected);
    show(s_scene, !menu && !agent_page);
    show(s_pair_title, pairing);
    show(s_pair_name, pairing);
    show(s_pair_hint, pairing);
    lv_obj_set_y(s_scene, waiting ? SCENE_Y_WAITING : SCENE_Y_NORMAL);

    if (pairing) {
        lv_label_set_text(s_pair_name, v->ble_name);
    } else if (disconnected) {
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_label_set_text(s_state_label, AR_TXT_DISCONNECTED);
        lv_obj_set_pos(s_state_label, PAD_X, 38);
        if (v->pair_cleared) lv_label_set_text(s_disc_hint, AR_TXT_PAIR_CLEARED);
        else if (!v->ble_bonded) lv_label_set_text_fmt(s_disc_hint, AR_TXT_PAIR_READY_FMT, v->ble_name);
        else lv_label_set_text(s_disc_hint, AR_TXT_DISCONNECTED_HINT);
    } else if (agent_page) {
        update_agent_page(v);
    } else if (normal) {
        const ar_state_t *s = &v->state;
        unsigned total = ar_model_working_total(s);
        // 刚有会话完成时,状态行与底栏“今日完成”用强调色,状态行显示“完成 +N”
        const uint32_t done_color = v->done_flash ? C_ACCENT : C_TEXT2;
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(done_color), LV_PART_MAIN);
        if (v->done_flash) lv_label_set_text_fmt(s_state_label, AR_TXT_DONE_FLASH_FMT, (unsigned)v->done_flash);
        else lv_label_set_text(s_state_label, state_text(v->screen));
        lv_obj_set_pos(s_state_label, PAD_X, 36);
        lv_obj_set_style_text_color(s_done_label, lv_color_hex(done_color), LV_PART_MAIN);

        int w = draw_big_number(total, v->screen == AR_SCREEN_SLEEP);
        lv_label_set_text(s_unit_label, AR_TXT_UNIT_RUNNING);
        lv_obj_set_pos(s_unit_label, PAD_X + w + 8, 80);

        uint8_t bars = ar_model_speed_bars(v->screen);
        for (int i = 0; i < 4; i++) {
            lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(i < bars ? C_ACCENT : C_TRACK), LV_PART_MAIN);
        }
        draw_tile(0, s->cc);
        draw_tile(1, s->cx);
        draw_tile(2, s->ds);

        lv_label_set_text_fmt(s_done_label, "%s %u", AR_TXT_DONE_TODAY, (unsigned)s->done);
        lv_label_set_text_fmt(s_wait_short_label, "%s %u", AR_TXT_WAITING_SHORT, (unsigned)s->wn);
        align_right(s_wait_short_label, PAD_X + CONTENT_W - 6, 276);
    } else {
        const ar_state_t *s = &v->state;
        int w = draw_alert_number(s->wn);
        lv_obj_set_pos(s_alert_label, 12 + w + 10, 13);

        // 最多显示两项;只有一项时列表收短
        int rows = s->w_count < 2 ? s->w_count : 2;
        for (int i = 0; i < 2; i++) {
            bool visible = i < rows;
            show(s_list_tool[i], visible);
            show(s_list_reason[i], visible);
            if (!visible) continue;
            lv_label_set_text(s_list_tool[i], tool_text(s->w[i].tool));
            lv_label_set_text(s_list_reason[i], s->w[i].reason == AR_REASON_QUESTION
                                                    ? AR_TXT_REASON_QUESTION : AR_TXT_REASON_APPROVE);
            align_right(s_list_reason[i], CONTENT_W - 10, 6 + 30 * i);
        }
        show(s_list_divider, rows > 1);
        show(s_list, rows > 0 && !menu);   // 菜单打开时整块隐藏
        lv_obj_set_height(s_list, rows > 1 ? 61 : 31);

        lv_label_set_text_fmt(s_others_label, AR_TXT_OTHERS_FMT, ar_model_working_total(s));
        align_right(s_hint_label, PAD_X + CONTENT_W - 6, 270);
    }

    // 画面或页面切换时立刻重画一次场景,不等下一个动画帧(专用页面期间不画场景);
    // 配对时宠物用招手姿势
    if (screen_changed || page_changed || pairing != s_last_pairing) {
        ar_ui_animate(pairing ? AR_SCREEN_WAITING : v->screen, 0);
    }
    s_last_pairing = pairing;

    update_menu(v, menu);
}
