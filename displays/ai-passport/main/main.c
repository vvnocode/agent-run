// main/main.c —— Agent Run!:AI Passport 上的 Agent 状态牌。
//
// 电脑上的 agent-run 采集程序经 USB 串口或蓝牙推送 Claude Code / Codex / dsh 的会话状态,
// 本固件负责显示:工作中的会话越多,宠物跑得越快;有会话等你处理时停下招手并响一声。
// 设计见外层仓库 docs/specs/2026-11-08-agent-run-design.md 与 -wireless-design.md。
//
// 按键:
//   上 / 下 短按    调亮度(5 档,保存)
//   确定   长按     切换静音(保存)
//   上     按住 5 秒 清除蓝牙配对
//   配对确认时       确定 = 接受,下 = 拒绝
//   任意键           背光关闭时只唤醒屏幕
#include <string.h>

#include "agent_run/ar_ble.h"
#include "agent_run/ar_capture.h"
#include "agent_run/ar_link.h"
#include "agent_run/ar_model.h"
#include "agent_run/ar_settings.h"
#include "agent_run/ar_sound.h"
#include "agent_run/ar_ui.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"

static const char *TAG = "agent_run";

#define INPUT_QUEUE_DEPTH 8
#define APP_TASK_STACK 4096
#define APP_TASK_PRIO 3              // 低于 LVGL 任务(4)
#define APP_TICK_MS 20               // 主循环周期
#define BATTERY_PERIOD_MS 10000      // 电量刷新周期
#define HEAP_LOG_PERIOD_MS 600000    // 内存记录周期(10 分钟)
#define LVGL_LOCK_TIMEOUT_MS 200
#define CLEAR_BOND_HOLD_US (5LL * 1000 * 1000)     // 按住上键这么久清除配对
#define CLEARED_NOTICE_US (3LL * 1000 * 1000)      // “已清除配对”提示的显示时长

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;

// 深睡按键唤醒时那颗键在启动期间仍按着,先屏蔽它的动作,直到松手(与上游 demo 相同的守卫)。
// 本应用不进入深睡,保留守卫是为了以后加入低功耗时行为一致。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 应用状态(只在 app_task 中读写)
static uint8_t s_brightness;
static bool s_muted;
static int s_battery = -1;
static uint8_t s_backlight = 0xFF;          // 当前背光百分比,0xFF 表示尚未设置
static int64_t s_last_key_us;
static int64_t s_disconnected_since_us;
static int64_t s_up_pressed_us;             // 上键按下的时刻,0 表示没按着
static int64_t s_cleared_until_us;          // “已清除配对”提示的截止时刻
static ar_chime_t s_chime;

// 两个时刻之间的毫秒数;from 为 0(从未发生)时返回 UINT32_MAX
static uint32_t ms_since(int64_t from_us, int64_t now_us) {
    if (from_us == 0) return UINT32_MAX;
    int64_t d = (now_us - from_us) / 1000;
    if (d < 0) return 0;
    if (d > UINT32_MAX) return UINT32_MAX;
    return (uint32_t)d;
}

// 按键回调运行在共享的 esp_timer 任务里:只入队,立刻返回
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // 先消费守卫再检查就绪:就绪前到达的松手事件也要能解除守卫
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

// 处理按键。pairing 为 true 时,确定与下键用于接受、拒绝配对
static void handle_key(const input_event_t *in, int64_t now_us, bool pairing) {
    // 上键按住 5 秒清除配对:按 PRESS 与 RELEASE 的间隔计时
    if (in->btn == BSP_BTN_UP && in->event == BSP_BTN_PRESS) {
        s_up_pressed_us = now_us;
        return;
    }
    if (in->btn == BSP_BTN_UP && in->event == BSP_BTN_RELEASE) {
        if (s_up_pressed_us && now_us - s_up_pressed_us >= CLEAR_BOND_HOLD_US) {
            ar_ble_clear_bonds();
            s_cleared_until_us = now_us + CLEARED_NOTICE_US;
        }
        s_up_pressed_us = 0;
        return;
    }
    if (in->event != BSP_BTN_CLICK && in->event != BSP_BTN_LONG) return;

    const bool was_dark = s_backlight == 0;
    s_last_key_us = now_us;
    if (was_dark) return;   // 背光关闭时只唤醒

    if (pairing) {
        if (in->event == BSP_BTN_CLICK && in->btn == BSP_BTN_OK) ar_ble_pair_key(true);
        else if (in->event == BSP_BTN_CLICK && in->btn == BSP_BTN_DOWN) ar_ble_pair_key(false);
        return;
    }
    if (in->event == BSP_BTN_CLICK && (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN)) {
        s_brightness = ar_brightness_step(s_brightness, in->btn == BSP_BTN_UP ? +1 : -1);
        ar_settings_save_brightness(s_brightness);
    } else if (in->event == BSP_BTN_LONG && in->btn == BSP_BTN_OK) {
        s_muted = !s_muted;
        ar_settings_save_muted(s_muted);
        ESP_LOGI(TAG, "静音 %s", s_muted ? "开" : "关");
    }
}

// 画面内容的摘要(不含序号),用于判断“内容 30 秒没变化”
typedef struct {
    ar_screen_t screen;
    uint8_t cc, cx, ds, wn;
} content_key_t;

static void app_task(void *arg) {
    (void)arg;
    ar_screen_t anim_screen = AR_SCREEN_DISCONNECTED;
    uint32_t tick = 0;
    int64_t next_frame_us = 0;
    int64_t next_battery_us = 0;
    int64_t next_heap_us = 0;
    int64_t last_change_us = esp_timer_get_time();
    content_key_t last_content = { .screen = AR_SCREEN_DISCONNECTED };
    uint16_t last_seq = 0;
    bool had_frame = false;
    ar_view_t view;

    for (;;) {
        // 电脑请求截屏时先完成截屏(内部自行加 LVGL 锁),期间主循环暂停,画面不会变化
        ar_capture_service();

        const int64_t now = esp_timer_get_time();

        ar_ble_status_t ble;
        ar_ble_get_status(&ble);
        ar_ble_tick();

        input_event_t in;
        while (xQueueReceive(s_input_queue, &in, 0) == pdTRUE) handle_key(&in, now, ble.confirming);

        if (now >= next_battery_us) {
            s_battery = bsp_battery_soc();
            next_battery_us = now + (int64_t)BATTERY_PERIOD_MS * 1000;
        }

        ar_link_snapshot_t link;
        ar_link_get(&link);
        const bool alive = link.has_state &&
                           ar_model_link_alive(ms_since(link.last_usb_us, now), ms_since(link.last_ble_us, now));
        const ar_screen_t screen = ar_model_screen(&link.state, alive);

        // 每收到一帧新状态,评估一次是否响提示音
        if (link.has_state && (!had_frame || link.state.seq != last_seq)) {
            had_frame = true;
            last_seq = link.state.seq;
            if (ar_chime_update(&s_chime, link.state.wn, (uint32_t)(now / 1000), s_muted)) ar_sound_chime();
        }

        // 内容变化时刻(电池供电时 30 秒无变化降低背光)
        content_key_t content = { .screen = screen };
        if (alive) {
            content.cc = link.state.cc;
            content.cx = link.state.cx;
            content.ds = link.state.ds;
            content.wn = link.state.wn;
        }
        if (memcmp(&content, &last_content, sizeof(content)) != 0) {
            last_content = content;
            last_change_us = now;
        }

        // 背光
        if (screen == AR_SCREEN_DISCONNECTED) {
            if (s_disconnected_since_us == 0) s_disconnected_since_us = now;
        } else {
            s_disconnected_since_us = 0;
        }
        const ar_backlight_in_t bl_in = {
            .screen = screen,
            .on_usb_power = usb_serial_jtag_is_connected(),
            .level = s_brightness,
            .ms_since_change = ms_since(last_change_us, now),
            .ms_since_key = ms_since(s_last_key_us, now),
            .ms_disconnected = s_disconnected_since_us ? ms_since(s_disconnected_since_us, now) : 0,
        };
        const uint8_t normal = ar_brightness_percent(s_brightness);
        uint8_t backlight = ble.confirming ? normal : ar_model_backlight_percent(&bl_in);
        if (backlight != s_backlight) {
            s_backlight = backlight;
            bsp_display_backlight(backlight);
        }

        memset(&view, 0, sizeof(view));   // 清零填充字节,ar_ui_update 用 memcmp 判断变化
        view.screen = screen;
        if (alive) {
            view.state = link.state;
            view.link_source = (uint8_t)link.last_source;
        }
        view.muted = s_muted;
        view.battery_soc = s_battery;
        view.pairing = ble.confirming;
        view.pair_cleared = now < s_cleared_until_us;
        view.ble_bonded = ble.bonded;
        memcpy(view.ble_name, ble.name, sizeof(view.ble_name));

        // 背光降档时动画放慢一倍
        const ar_screen_t want_anim = ble.confirming ? AR_SCREEN_WAITING : screen;
        uint32_t interval_ms = ar_model_frame_interval_ms(want_anim);
        if (backlight < normal) interval_ms *= 2;

        if (bsp_lvgl_lock(LVGL_LOCK_TIMEOUT_MS)) {
            ar_ui_update(&view);
            if (want_anim != anim_screen) {
                anim_screen = want_anim;
                tick = 0;
                next_frame_us = now + (int64_t)interval_ms * 1000;
            } else if (now >= next_frame_us && backlight > 0) {
                tick++;
                ar_ui_animate(anim_screen, tick);
                next_frame_us = now + (int64_t)interval_ms * 1000;
            }
            bsp_lvgl_unlock();
        }

        if (now >= next_heap_us) {
            ESP_LOGI(TAG, "内存:空闲 %u B,最大连续块 %u B,最低空闲 %u B",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            next_heap_us = now + (int64_t)HEAP_LOG_PERIOD_MS * 1000;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_TICK_MS));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Agent Run! 固件 %s 启动", AR_FW_VERSION);
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // 先起串口链路:即使后面的外设初始化失败,主机也能看到 HELLO 与日志
    if (ar_link_start() != ESP_OK) ESP_LOGE(TAG, "串口链路启动失败");

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    // 设置读取同时完成 NVS 初始化,蓝牙绑定存储也依赖 NVS
    ar_settings_load(&s_brightness, &s_muted);
    ar_chime_init(&s_chime);

    if (bsp_audio_init() == ESP_OK) {
        if (ar_sound_init() != ESP_OK) ESP_LOGW(TAG, "提示音任务创建失败");
    } else {
        ESP_LOGW(TAG, "音频初始化失败,提示音不可用");
    }
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue || bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    } else if (s_key_guard.armed) {
        key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    }

    if (bsp_lvgl_lock(1000)) {
        ar_ui_create();
        ar_capture_init();   // 在 BSP 圆角遮罩之后注册,截到的画面与屏幕一致
        bsp_lvgl_unlock();
    }
    s_last_key_us = esp_timer_get_time();

    // 蓝牙失败不影响 USB 使用
    if (ar_ble_start() != ESP_OK) ESP_LOGE(TAG, "蓝牙启动失败,只能通过 USB 使用");

    if (xTaskCreate(app_task, "agent_run", APP_TASK_STACK, NULL, APP_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "主任务创建失败");
        return;
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪");
}
