// tests/test_ar_model.c —— Agent Run! 画面选择、链路超时、帧间隔、提示音与背光规则的宿主测试。
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "agent_run/ar_model.h"

#define NEVER UINT32_MAX

static ar_state_t state(uint8_t cc, uint8_t cx, uint8_t ds, uint8_t wn) {
    ar_state_t st;
    memset(&st, 0, sizeof(st));
    st.cc = cc;
    st.cx = cx;
    st.ds = ds;
    st.wn = wn;
    return st;
}

static void test_speed_levels(void) {
    // 档位边界:0 睡觉、1 慢走、2-3 小跑、4-6 狂奔、>=7 冲刺(三个工具合计)
    const struct { uint8_t cc, cx, ds; ar_screen_t want; } cases[] = {
        {0, 0, 0, AR_SCREEN_SLEEP},
        {1, 0, 0, AR_SCREEN_WALK},
        {1, 1, 0, AR_SCREEN_TROT},
        {1, 1, 1, AR_SCREEN_TROT},
        {2, 1, 1, AR_SCREEN_RUN},
        {3, 2, 1, AR_SCREEN_RUN},
        {3, 2, 2, AR_SCREEN_DASH},
        {99, 99, 99, AR_SCREEN_DASH},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ar_state_t st = state(cases[i].cc, cases[i].cx, cases[i].ds, 0);
        assert(ar_model_screen(&st, true) == cases[i].want);
    }
    ar_state_t st = state(99, 99, 99, 0);
    assert(ar_model_working_total(&st) == 297);
}

static void test_waiting_overrides_speed(void) {
    ar_state_t st = state(5, 0, 0, 1);
    assert(ar_model_screen(&st, true) == AR_SCREEN_WAITING);
    assert(ar_model_screen(&st, false) == AR_SCREEN_DISCONNECTED);
}

static void test_link_timeout_per_source(void) {
    // USB 10 秒、蓝牙 15 秒内收到过帧即视为在线;任一来源在线即可
    assert(!ar_model_link_alive(NEVER, NEVER));
    assert(ar_model_link_alive(AR_LINK_TIMEOUT_USB_MS, NEVER));
    assert(!ar_model_link_alive(AR_LINK_TIMEOUT_USB_MS + 1, NEVER));
    assert(ar_model_link_alive(NEVER, AR_LINK_TIMEOUT_BLE_MS));
    assert(!ar_model_link_alive(NEVER, AR_LINK_TIMEOUT_BLE_MS + 1));
    assert(ar_model_link_alive(AR_LINK_TIMEOUT_USB_MS + 1, 1000));
    assert(AR_LINK_TIMEOUT_BLE_MS > AR_LINK_TIMEOUT_USB_MS);
}

static void test_frame_intervals_get_faster(void) {
    assert(ar_model_frame_interval_ms(AR_SCREEN_SLEEP) == 600);
    assert(ar_model_frame_interval_ms(AR_SCREEN_DISCONNECTED) == 600);
    assert(ar_model_frame_interval_ms(AR_SCREEN_WAITING) == 300);
    assert(ar_model_frame_interval_ms(AR_SCREEN_WALK) == 220);
    assert(ar_model_frame_interval_ms(AR_SCREEN_TROT) == 150);
    assert(ar_model_frame_interval_ms(AR_SCREEN_RUN) == 100);
    assert(ar_model_frame_interval_ms(AR_SCREEN_DASH) == 70);
    assert(ar_model_speed_bars(AR_SCREEN_SLEEP) == 0);
    assert(ar_model_speed_bars(AR_SCREEN_WALK) == 1);
    assert(ar_model_speed_bars(AR_SCREEN_TROT) == 2);
    assert(ar_model_speed_bars(AR_SCREEN_RUN) == 3);
    assert(ar_model_speed_bars(AR_SCREEN_DASH) == 4);
}

static void test_chime_rules(void) {
    ar_chime_t c;
    ar_chime_init(&c);
    assert(!ar_chime_update(&c, 0, 1000, false));
    assert(ar_chime_update(&c, 1, 2000, false));           // 新增等待项:响
    assert(!ar_chime_update(&c, 1, 3000, false));          // 数量不变:不响
    assert(!ar_chime_update(&c, 2, 2000 + 29999, false));  // 30 秒内的第二次:不响
    assert(!ar_chime_update(&c, 2, 2000 + 40000, false));  // 那次增加已经过去,不补响
    assert(ar_chime_update(&c, 3, 2000 + 40000, false));   // 间隔已够的新增:响
    assert(!ar_chime_update(&c, 0, 100000, false));
    assert(!ar_chime_update(&c, 1, 200000, true));         // 静音:不响
    assert(!ar_chime_update(&c, 1, 300000, false));        // 静音期间的增加不在解除后补响
    // 毫秒计数回绕时间隔仍按无符号差值计算
    ar_chime_init(&c);
    assert(ar_chime_update(&c, 1, 0xFFFFFF00u, false));
    assert(!ar_chime_update(&c, 0, 0xFFFFFF10u, false));
    assert(ar_chime_update(&c, 1, 0x00007600u, false));
}

static void test_brightness(void) {
    assert(ar_brightness_percent(0) == 20);
    assert(ar_brightness_percent(4) == 100);
    assert(ar_brightness_percent(9) == 100);
    assert(ar_brightness_step(4, +1) == 4);
    assert(ar_brightness_step(0, -1) == 0);
    assert(ar_brightness_step(2, +1) == 3);
    assert(ar_brightness_step(2, -1) == 1);
    assert(AR_BRIGHTNESS_DEFAULT < AR_BRIGHTNESS_LEVELS);
}

static ar_backlight_in_t backlight_in(ar_screen_t screen, bool usb_power) {
    ar_backlight_in_t in = {
        .screen = screen,
        .on_usb_power = usb_power,
        .level = 3,
        .ms_since_change = 0,
        .ms_since_key = NEVER,
        .ms_disconnected = 0,
    };
    return in;
}

static void test_backlight_on_usb_power(void) {
    ar_backlight_in_t in = backlight_in(AR_SCREEN_RUN, true);
    in.ms_since_change = 3600000;   // 插着 USB 时状态长时间不变也不降亮度
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
}

static void test_backlight_dims_on_battery(void) {
    ar_backlight_in_t in = backlight_in(AR_SCREEN_RUN, false);
    in.ms_since_change = AR_DIM_AFTER_MS - 1;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
    in.ms_since_change = AR_DIM_AFTER_MS;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    // 按键后恢复
    in.ms_since_key = 1000;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
    // 等你处理时始终用设定亮度
    in = backlight_in(AR_SCREEN_WAITING, false);
    in.ms_since_change = 3600000;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
    // 设定亮度本来就低于降档值时不反向调亮
    in = backlight_in(AR_SCREEN_SLEEP, false);
    in.level = 0;
    in.ms_since_change = 3600000;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(0));
}

static void test_backlight_off_when_disconnected(void) {
    ar_backlight_in_t in = backlight_in(AR_SCREEN_DISCONNECTED, true);
    in.ms_disconnected = AR_BACKLIGHT_IDLE_MS - 1;
    assert(ar_model_backlight_percent(&in) > 0);
    in.ms_disconnected = AR_BACKLIGHT_IDLE_MS;
    in.ms_since_key = AR_BACKLIGHT_IDLE_MS;
    assert(ar_model_backlight_percent(&in) == 0);
    // 按键唤醒后再亮 5 分钟
    in.ms_since_key = 10;
    assert(ar_model_backlight_percent(&in) > 0);
}

int main(void) {
    test_speed_levels();
    test_waiting_overrides_speed();
    test_link_timeout_per_source();
    test_frame_intervals_get_faster();
    test_chime_rules();
    test_brightness();
    test_backlight_on_usb_power();
    test_backlight_dims_on_battery();
    test_backlight_off_when_disconnected();
    return 0;
}
