// main/agent_run/ar_capture.c —— FAP_SCREENSHOT_V1 串口截屏实现。
//
// 设备没有 PSRAM,也拿不出 150 KB 的整屏缓冲。做法(参考上游社区文档
// fap-screenshot-without-frame-buffer):截屏时临时申请一块 64 行的条带缓冲,分 5 段
// 整屏刷新;每段在 LVGL 的 FLUSH_START 事件里把落在本段的行拷进缓冲,刷新完即发给电脑。
// - BSP 的圆角遮罩也挂在 FLUSH_START 上且先注册,所以截到的画面和屏幕一致;
//   字节交换在之后的 flush 回调里才做,拷到的是小端 RGB565。
// - 控制台日志与负载共用同一条 USB 流,传输期间关闭日志,结束后恢复。
// - 用驱动接口按 512 字节分块写出,检查返回值,短写时继续写剩余部分。
#include "ar_capture.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ar_capture_math.h"
#include "bsp_display.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

static const char *TAG = "ar_capture";

#define SCREEN_W 240
#define SCREEN_H 320
// 条带缓冲 240×40×2 = 19.2 KB,分 8 条渲染。LVGL 内存池调到 32 KB 后,开机空闲堆的最大连续块
// 约 28 KB,原来 64 行(30 KB)的条带申请不到
#define BAND_H 40
#define WRITE_CHUNK 512            // 小于驱动 1 KB 的发送环形缓冲
#define WRITE_TIMEOUT_MS 100
#define MAX_STALLS 30              // 连续这么多次写不进去就放弃(约 3 秒)
#define LVGL_LOCK_TIMEOUT_MS 1000

static volatile bool s_requested;
static volatile bool s_capturing;   // 只有截屏刷新期间,钩子才拷贝
static uint16_t *s_band;
static int s_band_y0;

// FLUSH_START:把本次刷新区域中落在当前条带的行拷进条带缓冲
static void flush_event(lv_event_t *event) {
    if (!s_capturing || !s_band) return;
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!area || !buf || !buf->data) return;
    ar_capture_copy_rows(s_band, SCREEN_W, s_band_y0, BAND_H, area->x1, area->y1, area->x2, area->y2,
                         buf->data, (int)buf->header.stride);
}

// 写出全部字节;对方长时间不读(例如中途拔线)时返回 false
static bool write_all(const void *data, size_t len) {
    const uint8_t *p = data;
    int stalls = 0;
    while (len > 0) {
        size_t chunk = len < WRITE_CHUNK ? len : WRITE_CHUNK;
        int n = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(WRITE_TIMEOUT_MS));
        if (n <= 0) {
            if (++stalls > MAX_STALLS) return false;
            continue;
        }
        stalls = 0;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

void ar_capture_init(void) {
    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_add_event_cb(disp, flush_event, LV_EVENT_FLUSH_START, NULL);
}

void ar_capture_request(void) {
    s_requested = true;
}

void ar_capture_service(void) {
    if (!s_requested) return;
    s_requested = false;
    if (!usb_serial_jtag_is_driver_installed()) return;

    s_band = heap_caps_malloc(SCREEN_W * BAND_H * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_band) {
        ESP_LOGW(TAG, "截屏缓冲申请失败,最大连续块 %u B",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }

    // 传输期间不能有任何日志字节混进负载
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_NONE);

    char header[64];
    int header_len = snprintf(header, sizeof(header), "FAP_SCREENSHOT_V1 %d %d RGB565LE %d\n",
                              SCREEN_W, SCREEN_H, SCREEN_W * SCREEN_H * 2);
    bool ok = write_all(header, (size_t)header_len);

    const int bands = ar_capture_band_count(SCREEN_H, BAND_H);
    for (int b = 0; ok && b < bands; b++) {
        s_band_y0 = b * BAND_H;
        const int rows = SCREEN_H - s_band_y0 < BAND_H ? SCREEN_H - s_band_y0 : BAND_H;
        memset(s_band, 0, SCREEN_W * BAND_H * sizeof(uint16_t));
        if (!bsp_lvgl_lock(LVGL_LOCK_TIMEOUT_MS)) {
            ok = false;
            break;
        }
        // 整屏失效后立即刷新,钩子在每个刷新分块里拷贝本条带的行
        s_capturing = true;
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
        s_capturing = false;
        bsp_lvgl_unlock();
        ok = write_all(s_band, (size_t)rows * SCREEN_W * sizeof(uint16_t));
    }

    heap_caps_free(s_band);
    s_band = NULL;
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "截屏%s", ok ? "完成" : "中断");
}
