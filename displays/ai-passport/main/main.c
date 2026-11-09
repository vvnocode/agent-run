// main/main.c —— Agent Run!:AI Passport 上的 Agent 状态牌。
//
// 电脑上的 agent-run 采集程序经 USB 串口或蓝牙推送 Claude Code / Codex / dsh 的会话状态,
// 本固件负责显示:工作中的会话越多,宠物跑得越快;有会话等你处理时停下招手并响一声;
// 有会话完成时显示“完成 +N”并响一声轻提示音。
// 设计见外层仓库 docs/specs/2026-11-08-agent-run-design.md 与 -wireless-design.md。
//
// 按键(设置菜单的规则见 ar_menu_key()):
//   首页   确定 单击  打开设置菜单(一级:音量、亮度,默认选中音量);上下键暂不起作用
//   一级   上 / 下    切换选中项;确定单击进入二级;确定双击回首页;10 秒无按键回首页
//   二级   上 / 下    调节并保存(调音量时按新音量试听,调亮度时按设定亮度点亮);
//                     确定双击回一级;3 秒无按键回首页
//   上     按住 5 秒 清除蓝牙配对
//   配对确认时       确定 = 接受,下 = 拒绝
//   任意键           背光关闭时按下即亮屏,这次手势不执行按键操作;深睡时唤醒设备(重新启动)
//
// 亮屏与熄屏:按键、通知(等你处理增加、有会话完成)、连上电脑、配对确认都会点亮屏幕,
// 之后按供电方式调暗、熄屏,规则见 ar_model_backlight_percent()。拔掉 USB 时屏幕若亮着,
// 也算一次亮屏事件,见 ar_power_update()。
// 电池供电时,“未连接”且无按键满 30 分钟,或熄屏满 1 小时,进入深睡(关机,按任意键唤醒),
// 规则见 ar_model_should_deep_sleep()。
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
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"

static const char *TAG = "agent_run";

#define INPUT_QUEUE_DEPTH 8
// 截屏时本任务调用 lv_refr_now() 渲染整屏,栈与 LVGL 任务一致(esp_lvgl_port 默认 7168);
// 4096 在显示设置菜单这类多层浮层时会栈溢出
#define APP_TASK_STACK 7168
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
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 应用状态(只在 app_task 中读写)
static uint8_t s_brightness;
static uint8_t s_volume;                    // 音量档位,0 为静音
static int s_battery = -1;
static uint8_t s_backlight = 0xFF;          // 当前背光百分比,0xFF 表示尚未设置
static int64_t s_last_key_us;
static int64_t s_last_wake_us;              // 最近一次亮屏事件(按键、通知、连上电脑、配对确认、开机)
static int64_t s_disconnected_since_us;
static int64_t s_up_pressed_us;             // 上键按下的时刻,0 表示没按着
static int64_t s_cleared_until_us;          // “已清除配对”提示的截止时刻
static ar_notice_t s_notice;
static ar_chime_t s_chime;
static uint16_t s_done_flash_count;         // “完成 +N”里的 N,提示期间新完成的轮数累加
static ar_menu_t s_menu;                    // 设置菜单的层级与选中项
static int64_t s_done_flash_until_us;       // “完成 +N”提示的截止时刻
static ar_keywake_t s_keywake;              // 熄屏时按键亮屏,并吞掉这次手势的动作
static ar_power_t s_power;                  // 供电方式,拔掉 USB 时据此补记亮屏事件
static int64_t s_dark_since_us;             // 屏幕熄灭的时刻,亮着时为 0

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

// 二级菜单里按调节量修改选中项并保存;到头时不变,但音量仍试听一声,确认当前档位(静音档不响)
static void apply_adjust(uint8_t item, int delta) {
    if (item == AR_MENU_VOLUME) {
        const uint8_t volume = ar_volume_step(s_volume, delta);
        if (volume != s_volume) {
            s_volume = volume;
            ar_settings_save_volume(s_volume);
            ESP_LOGI(TAG, "音量 %u 档", (unsigned)s_volume);
        }
        ar_sound_play(AR_CHIME_WAITING, ar_volume_percent(s_volume));
    } else if (item == AR_MENU_BRIGHTNESS) {
        const uint8_t brightness = ar_brightness_step(s_brightness, delta);
        if (brightness != s_brightness) {
            s_brightness = brightness;
            ar_settings_save_brightness(s_brightness);
        }
    }
}

// 处理按键。pairing 为 true 时,确定与下键用于接受、拒绝配对
static void handle_key(const input_event_t *in, int64_t now_us, bool pairing) {
    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    // 任意键按下都立刻亮屏:按键组件对三连击等手势不发单击、双击、长按事件,
    // 只在这些事件上亮屏会漏掉它们。熄屏时按下的这次手势只亮屏,其动作由 ar_keywake 吞掉
    if (in->event == BSP_BTN_PRESS) {
        ar_keywake_press(&s_keywake, s_backlight == 0, now_ms);
        s_last_key_us = now_us;
        s_last_wake_us = now_us;
        // 上键按住 5 秒清除配对:按 PRESS 与 RELEASE 的间隔计时
        if (in->btn == BSP_BTN_UP) s_up_pressed_us = now_us;
        return;
    }
    if (in->event == BSP_BTN_RELEASE) {
        ar_keywake_release(&s_keywake, now_ms);
        if (in->btn == BSP_BTN_UP) {
            if (s_up_pressed_us && now_us - s_up_pressed_us >= CLEAR_BOND_HOLD_US) {
                ar_ble_clear_bonds();
                s_cleared_until_us = now_us + CLEARED_NOTICE_US;
            }
            s_up_pressed_us = 0;
        }
        return;
    }
    // 长按目前不分配功能,但仍算按键:推迟熄屏与深睡
    if (in->event != BSP_BTN_CLICK && in->event != BSP_BTN_DOUBLE && in->event != BSP_BTN_LONG) return;

    s_last_key_us = now_us;
    s_last_wake_us = now_us;
    if (!ar_keywake_action(&s_keywake)) return;   // 亮屏那次手势的动作不执行

    if (pairing) {
        if (in->event == BSP_BTN_CLICK && in->btn == BSP_BTN_OK) ar_ble_pair_key(true);
        else if (in->event == BSP_BTN_CLICK && in->btn == BSP_BTN_DOWN) ar_ble_pair_key(false);
        return;
    }
    if (in->event == BSP_BTN_LONG) return;

    // 换算成菜单按键:确定单击为进入,双击为返回上一级;上下键快速连按两下会被
    // 识别成双击,按两步处理,免得调节时丢档
    ar_key_t key;
    int presses = in->event == BSP_BTN_DOUBLE ? 2 : 1;
    if (in->btn == BSP_BTN_UP) {
        key = AR_KEY_UP;
    } else if (in->btn == BSP_BTN_DOWN) {
        key = AR_KEY_DOWN;
    } else if (in->btn == BSP_BTN_OK) {
        key = in->event == BSP_BTN_DOUBLE ? AR_KEY_BACK : AR_KEY_OK;
        presses = 1;
    } else {
        return;
    }
    int delta = 0;
    for (int i = 0; i < presses; i++) delta += ar_menu_key(&s_menu, key);
    if (delta != 0) apply_adjust(s_menu.item, delta);
}

// 关闭外设并进入深睡,只能按键唤醒;唤醒等同重启,开机流程照常恢复显示与蓝牙。
// 按键正按着或拿不到 LVGL 锁时放弃本次入睡直接返回,下个周期再判断。
// 一旦开始关闭外设就不再返回:要么深睡,要么重启。
static void enter_deep_sleep(void) {
    // 按着键时低电平唤醒条件在入睡瞬间就成立,会立刻醒回来;读失败(-1)同样放弃
    if (bsp_button_read_mv() < BSP_BTN_MV_RELEASED_MIN) return;
    // 持有 LVGL 锁直到入睡:LVGL 不再刷屏,这是显示深睡准备的前提
    if (!bsp_lvgl_lock(LVGL_LOCK_TIMEOUT_MS)) return;

    ESP_LOGI(TAG, "电池供电,未连接满 %u 分钟或熄屏满 %u 分钟,进入深睡,按任意键唤醒",
             (unsigned)(AR_DEEP_SLEEP_AFTER_MS / 60000u), (unsigned)(AR_POWER_OFF_AFTER_DARK_MS / 60000u));

    // 各步失败只记日志、继续入睡:深睡会切断这些外设的活动,留在运行态反而更耗电
    if (ar_ble_stop() != ESP_OK) ESP_LOGW(TAG, "蓝牙停止失败");
    if (bsp_audio_sleep() != ESP_OK) ESP_LOGW(TAG, "ES8311 休眠失败");
    if (bsp_audio_prepare_deep_sleep() != ESP_OK) ESP_LOGW(TAG, "I2S 引脚深睡准备失败");
    // 电量计保持工作,不调用 bsp_battery_sleep():休眠后再激活会重启估算,
    // 醒来约 10 秒内读到 0%;保持工作只多耗电量计自身的电流
    if (bsp_display_prepare_deep_sleep() != ESP_OK) ESP_LOGW(TAG, "显示深睡准备失败");

    // 按键脚交回数字输入作为唤醒源。失败或此刻键被按下时按键已不可用,只能重启
    int level = 0;
    if (bsp_button_prepare_deep_sleep(&level) != ESP_OK || level == 0 ||
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW) != ESP_OK) {
        ESP_LOGW(TAG, "无法设置按键唤醒,改为重启");
        esp_restart();
    }
    esp_deep_sleep_start();
}

static void app_task(void *arg) {
    (void)arg;
    ar_screen_t anim_screen = AR_SCREEN_DISCONNECTED;
    uint32_t tick = 0;
    int64_t next_frame_us = 0;
    int64_t next_battery_us = 0;
    int64_t next_heap_us = 0;
    uint16_t last_seq = 0;
    bool had_frame = false;
    bool was_alive = false;        // 上一轮链路是否在线,用于识别连上与掉线
    bool was_confirming = false;   // 上一轮是否在配对确认,用于识别配对请求的出现
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
        // 菜单超时回首页;出现配对请求时关闭菜单,把画面和按键让给配对确认
        ar_menu_tick(&s_menu, ms_since(s_last_key_us, now));
        if (ble.confirming) ar_menu_init(&s_menu);

        if (now >= next_battery_us) {
            s_battery = bsp_battery_soc();
            next_battery_us = now + (int64_t)BATTERY_PERIOD_MS * 1000;
        }

        ar_link_snapshot_t link;
        ar_link_get(&link);
        const bool alive = link.has_state &&
                           ar_model_link_alive(ms_since(link.last_usb_us, now), ms_since(link.last_ble_us, now));
        const ar_screen_t screen = ar_model_screen(&link.state, alive);

        // 连上电脑与出现配对请求都点亮屏幕;掉线后,恢复时的第一帧完成数只作基准
        if (alive && !was_alive) s_last_wake_us = now;
        if (!alive && was_alive) ar_notice_offline(&s_notice);
        was_alive = alive;
        if (ble.confirming && !was_confirming) s_last_wake_us = now;
        was_confirming = ble.confirming;

        // 每收到一帧新状态:识别通知,点亮屏幕,选择提示音,记录“完成 +N”
        if (link.has_state && (!had_frame || link.state.seq != last_seq)) {
            had_frame = true;
            last_seq = link.state.seq;
            const ar_notice_ev_t ev = ar_notice_update(&s_notice, link.state.wn, link.state.done);
            if (ev.waiting || ev.completed > 0) s_last_wake_us = now;
            const ar_chime_kind_t kind = ar_chime_pick(&s_chime, ev, (uint32_t)(now / 1000), s_volume == 0);
            ar_sound_play(kind, ar_volume_percent(s_volume));
            if (ev.completed > 0) {
                s_done_flash_count = now < s_done_flash_until_us ? (uint16_t)(s_done_flash_count + ev.completed)
                                                                 : ev.completed;
                s_done_flash_until_us = now + (int64_t)AR_DONE_FLASH_MS * 1000;
            }
        }

        // 背光
        if (screen == AR_SCREEN_DISCONNECTED) {
            if (s_disconnected_since_us == 0) s_disconnected_since_us = now;
        } else {
            s_disconnected_since_us = 0;
        }
        // 拔掉 USB 时屏幕若亮着,补记一次亮屏事件:否则按电池规则(亮屏事件后 30 秒)
        // 计时早已超时,亮着的屏幕会在拔线瞬间熄灭
        const bool on_usb = usb_serial_jtag_is_connected();
        if (ar_power_update(&s_power, on_usb, s_backlight > 0)) s_last_wake_us = now;
        const ar_backlight_in_t bl_in = {
            .screen = screen,
            .on_usb_power = on_usb,
            .level = s_brightness,
            .ms_since_wake = ms_since(s_last_wake_us, now),
            .ms_disconnected = s_disconnected_since_us ? ms_since(s_disconnected_since_us, now) : 0,
        };
        const uint8_t normal = ar_brightness_percent(s_brightness);
        uint8_t backlight = ble.confirming ? normal : ar_model_backlight_percent(&bl_in);
        // 在二级菜单调亮度时按设定亮度点亮,电池供电时也一样,让用户看到实际效果
        if (s_menu.level == AR_MENU_ADJUST && s_menu.item == AR_MENU_BRIGHTNESS) backlight = normal;
        if (backlight != s_backlight) {
            s_backlight = backlight;
            bsp_display_backlight(backlight);
        }
        // 记录熄屏的起点,供“熄屏满 1 小时关机”计时;一亮屏就清零
        if (backlight == 0) {
            if (s_dark_since_us == 0) s_dark_since_us = now;
        } else {
            s_dark_since_us = 0;
        }

        // 深睡:电池供电且长时间未连接,或熄屏满 1 小时。电量计读不到时可能没装电池,不睡
        const ar_sleep_in_t sleep_in = {
            .on_usb_power = bl_in.on_usb_power,
            .battery_present = s_battery >= 0,
            .confirming = ble.confirming,
            .ms_disconnected = bl_in.ms_disconnected,
            .ms_since_key = ms_since(s_last_key_us, now),
            .ms_dark = s_dark_since_us ? ms_since(s_dark_since_us, now) : 0,
        };
        if (ar_model_should_deep_sleep(&sleep_in)) enter_deep_sleep();

        memset(&view, 0, sizeof(view));   // 清零填充字节,ar_ui_update 用 memcmp 判断变化
        view.screen = screen;
        if (alive) {
            view.state = link.state;
            view.link_source = (uint8_t)link.last_source;
        }
        view.volume = s_volume;
        view.brightness = s_brightness;
        view.menu_level = s_menu.level;
        view.menu_item = s_menu.item;
        view.done_flash = now < s_done_flash_until_us ? s_done_flash_count : 0;
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
    ar_settings_load(&s_brightness, &s_volume);
    ar_notice_init(&s_notice);
    ar_menu_init(&s_menu);
    ar_chime_init(&s_chime);
    ar_keywake_init(&s_keywake);
    ar_power_init(&s_power);

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
    s_last_wake_us = s_last_key_us;   // 开机算一次亮屏事件

    // 蓝牙失败不影响 USB 使用
    if (ar_ble_start() != ESP_OK) ESP_LOGE(TAG, "蓝牙启动失败,只能通过 USB 使用");

    if (xTaskCreate(app_task, "agent_run", APP_TASK_STACK, NULL, APP_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "主任务创建失败");
        return;
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪");
}
