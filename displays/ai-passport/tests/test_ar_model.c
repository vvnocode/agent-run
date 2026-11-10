// tests/test_ar_model.c —— Agent Run! 画面选择、链路超时、帧间隔、通知与提示音、再提醒、音量、熄屏时间、
// 手动熄屏、勿扰与本地时间、背光与深睡规则的宿主测试。
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

static void test_brightness(void) {
    assert(ar_brightness_percent(0) == 20);
    assert(ar_brightness_percent(4) == 100);
    assert(ar_brightness_percent(9) == 100);
    // 二级菜单里上下键加减:到最低档与最高档时停住,不循环;越界先按最高档处理
    assert(ar_brightness_step(0, -1) == 0);
    assert(ar_brightness_step(0, +1) == 1);
    assert(ar_brightness_step(3, +1) == 4);
    assert(ar_brightness_step(4, +1) == 4);
    assert(ar_brightness_step(9, -1) == 3);
    assert(ar_brightness_step(1, +2) == 3);   // 快速连按(双击)一次加两档
    assert(AR_BRIGHTNESS_DEFAULT < AR_BRIGHTNESS_LEVELS);
}

static void test_menu_navigation(void) {
    ar_menu_t m;
    ar_menu_init(&m);
    assert(m.level == AR_MENU_CLOSED);
    // 首页:上下键与双击确定都不起作用(上下键留给以后切换用量显示)
    assert(ar_menu_key(&m, AR_KEY_UP) == 0 && m.level == AR_MENU_CLOSED);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == 0 && m.level == AR_MENU_CLOSED);
    assert(ar_menu_key(&m, AR_KEY_BACK) == 0 && m.level == AR_MENU_CLOSED);
    // 单击确定打开一级菜单,默认选中音量
    assert(ar_menu_key(&m, AR_KEY_OK) == 0);
    assert(m.level == AR_MENU_LIST && m.item == AR_MENU_VOLUME);
    // 上下键切换选中项,到头停住;一级菜单里不产生调节量。顺序:音量、亮度、电池熄屏、
    // 接电脑熄屏、勿扰、夜间勿扰(屏幕方案 §7 设计稿 A)
    assert(AR_MENU_ITEMS == 6);
    assert(ar_menu_key(&m, AR_KEY_UP) == 0 && m.item == AR_MENU_VOLUME);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == 0 && m.item == AR_MENU_BRIGHTNESS);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == 0 && m.item == AR_MENU_BATTERY_OFF);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == 0 && m.item == AR_MENU_USB_OFF);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == 0 && m.item == AR_MENU_DND);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == 0 && m.item == AR_MENU_NIGHT);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == 0 && m.item == AR_MENU_NIGHT);
    for (int i = 0; i < 4; i++) ar_menu_key(&m, AR_KEY_UP);
    assert(m.item == AR_MENU_BRIGHTNESS);
    // 单击确定进入二级,上下键给出对选中项的调节量
    assert(ar_menu_key(&m, AR_KEY_OK) == 0);
    assert(m.level == AR_MENU_ADJUST && m.item == AR_MENU_BRIGHTNESS);
    assert(ar_menu_key(&m, AR_KEY_UP) == +1);
    assert(ar_menu_key(&m, AR_KEY_DOWN) == -1);
    // 二级里单击确定不起作用
    assert(ar_menu_key(&m, AR_KEY_OK) == 0 && m.level == AR_MENU_ADJUST);
    // 双击确定返回上一级:二级回一级并保留选中项,一级回首页
    assert(ar_menu_key(&m, AR_KEY_BACK) == 0);
    assert(m.level == AR_MENU_LIST && m.item == AR_MENU_BRIGHTNESS);
    assert(ar_menu_key(&m, AR_KEY_BACK) == 0 && m.level == AR_MENU_CLOSED);
    // 重新打开时回到默认选中音量
    ar_menu_key(&m, AR_KEY_OK);
    assert(m.level == AR_MENU_LIST && m.item == AR_MENU_VOLUME);
}

static void test_menu_timeouts(void) {
    // 用户指定:一级菜单 10 秒、二级 3 秒没有按键就回首页
    assert(AR_MENU_LIST_TIMEOUT_MS == 10000u);
    assert(AR_MENU_ADJUST_TIMEOUT_MS == 3000u);
    // 电池供电时最短的熄屏时间也不早于菜单超时,操作菜单期间屏幕不会灭
    assert(ar_battery_off_ms(0) >= AR_MENU_LIST_TIMEOUT_MS);

    ar_menu_t m;
    ar_menu_init(&m);
    ar_menu_key(&m, AR_KEY_OK);
    ar_menu_tick(&m, AR_MENU_LIST_TIMEOUT_MS - 1);
    assert(m.level == AR_MENU_LIST);
    ar_menu_tick(&m, AR_MENU_LIST_TIMEOUT_MS);
    assert(m.level == AR_MENU_CLOSED);

    // 二级超时直接回首页,不是回一级
    ar_menu_key(&m, AR_KEY_OK);
    ar_menu_key(&m, AR_KEY_OK);
    ar_menu_tick(&m, AR_MENU_ADJUST_TIMEOUT_MS - 1);
    assert(m.level == AR_MENU_ADJUST);
    ar_menu_tick(&m, AR_MENU_ADJUST_TIMEOUT_MS);
    assert(m.level == AR_MENU_CLOSED);

    // 菜单关着时不受影响
    ar_menu_tick(&m, UINT32_MAX);
    assert(m.level == AR_MENU_CLOSED);
}

// 背光计算的默认输入:亮度第 3 档,刚发生过亮屏事件,在线
static ar_backlight_in_t backlight_in(ar_screen_t screen, bool power) {
    ar_backlight_in_t in = {
        .screen = screen,
        .on_power = power,
        .level = 3,
        .ms_since_wake = 0,
        .ms_disconnected = 0,
        .battery_off = AR_BATTERY_OFF_DEFAULT,
        .usb_off = AR_USB_OFF_DEFAULT,
        .locked = false,
    };
    return in;
}

static void test_backlight_on_power(void) {
    // 接着电源(USB 主机或充电器),默认 30 分钟熄屏:亮屏事件后 5 分钟内用设定亮度,之后降到低亮度,满 30 分钟熄屏
    const uint32_t off = ar_usb_off_ms(AR_USB_OFF_DEFAULT);
    assert(off == 1800000u);
    ar_backlight_in_t in = backlight_in(AR_SCREEN_RUN, true);
    in.ms_since_wake = AR_USB_DIM_AFTER_MS - 1;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
    in.ms_since_wake = AR_USB_DIM_AFTER_MS;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = off - 1;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = off;
    assert(ar_model_backlight_percent(&in) == 0);
    // 等你处理也按同一规则,靠通知亮屏重新点亮
    in = backlight_in(AR_SCREEN_WAITING, true);
    in.ms_since_wake = off;
    assert(ar_model_backlight_percent(&in) == 0);

    // 5 分钟熄屏:熄屏时间的一半(2.5 分钟)就调暗,免得没有低亮度阶段
    in = backlight_in(AR_SCREEN_RUN, true);
    in.usb_off = 0;
    assert(ar_usb_off_ms(0) == 300000u);
    in.ms_since_wake = 150000u - 1;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
    in.ms_since_wake = 150000u;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = 300000u;
    assert(ar_model_backlight_percent(&in) == 0);

    // 1 小时熄屏:仍是 5 分钟后调暗,满 1 小时熄屏
    in.usb_off = 3;
    in.ms_since_wake = AR_USB_DIM_AFTER_MS;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = 3600000u - 1;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = 3600000u;
    assert(ar_model_backlight_percent(&in) == 0);

    // 不熄屏:5 分钟后调暗,之后一直保持低亮度
    in.usb_off = AR_USB_OFF_OPTIONS - 1;
    assert(ar_usb_off_ms(in.usb_off) == AR_OFF_NEVER);
    in.ms_since_wake = AR_USB_DIM_AFTER_MS - 1;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
    in.ms_since_wake = 3600000u * 24u;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = NEVER;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
}

static void test_backlight_on_battery(void) {
    // 电池供电,默认 30 秒熄屏:亮屏期间直接用低亮度,亮屏事件后 30 秒熄屏
    const uint32_t off = ar_battery_off_ms(AR_BATTERY_OFF_DEFAULT);
    assert(off == 30000u);
    ar_backlight_in_t in = backlight_in(AR_SCREEN_RUN, false);
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = off - 1;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = off;
    assert(ar_model_backlight_percent(&in) == 0);
    // 其他选项:10 秒、3 分钟
    in.battery_off = 0;
    in.ms_since_wake = 10000u - 1;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = 10000u;
    assert(ar_model_backlight_percent(&in) == 0);
    in.battery_off = AR_BATTERY_OFF_OPTIONS - 1;
    in.ms_since_wake = 180000u - 1;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    in.ms_since_wake = 180000u;
    assert(ar_model_backlight_percent(&in) == 0);
    // 等你处理与未连接同样适用
    in = backlight_in(AR_SCREEN_WAITING, false);
    in.ms_since_wake = off;
    assert(ar_model_backlight_percent(&in) == 0);
    in = backlight_in(AR_SCREEN_DISCONNECTED, false);
    in.ms_disconnected = 1000;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    // 从未有过亮屏事件时视为早已超时
    in.ms_since_wake = NEVER;
    assert(ar_model_backlight_percent(&in) == 0);
    // 设定亮度本来就低于低亮度时不反向调亮
    in = backlight_in(AR_SCREEN_SLEEP, false);
    in.level = 0;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(0));
}

static void test_backlight_off_when_disconnected(void) {
    // 插着 USB 主机但“未连接”:5 分钟且 5 分钟内无亮屏事件即关背光,早于 30 分钟的通用规则
    ar_backlight_in_t in = backlight_in(AR_SCREEN_DISCONNECTED, true);
    in.ms_disconnected = AR_BACKLIGHT_IDLE_MS - 1;
    in.ms_since_wake = AR_BACKLIGHT_IDLE_MS;
    assert(ar_model_backlight_percent(&in) > 0);
    in.ms_disconnected = AR_BACKLIGHT_IDLE_MS;
    assert(ar_model_backlight_percent(&in) == 0);
    // 按键唤醒后再亮 5 分钟
    in.ms_since_wake = 10;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
}

static void test_unplug_keeps_screen_on(void) {
    // 复现:插着 USB 时有会话完成点亮屏幕,1 分钟后拔掉 USB。按电池规则这时已超过 30 秒,
    // 若不把拔线算作亮屏事件,屏幕会在拔线瞬间熄灭
    ar_backlight_in_t in = backlight_in(AR_SCREEN_RUN, true);
    in.ms_since_wake = 60000;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));
    in.on_power = false;
    assert(ar_model_backlight_percent(&in) == 0);

    ar_power_t p;
    ar_power_init(&p);
    assert(!ar_power_update(&p, true, true));   // 开机第一次只记基准
    assert(!ar_power_update(&p, true, true));   // 供电方式没变
    // 拔线时屏幕亮着:记一次亮屏事件,按电池规则继续亮
    assert(ar_power_update(&p, false, true));
    in.ms_since_wake = 0;
    assert(ar_model_backlight_percent(&in) == AR_DIM_PERCENT);
    assert(!ar_power_update(&p, false, true));

    // 屏幕已熄灭时断电不点亮
    ar_power_init(&p);
    ar_power_update(&p, true, false);
    assert(!ar_power_update(&p, false, false));
    // 接上电源一律点亮:熄屏、手动熄屏时都算亮屏事件(手动熄屏由调用方的 wake() 解除)
    assert(ar_power_update(&p, true, false));
    assert(!ar_power_update(&p, true, false));   // 一直接着不重复点亮
    ar_power_update(&p, false, true);
    assert(ar_power_update(&p, true, true));
    // 开机时已接着电源:开机本身算亮屏事件,这里只记基准
    ar_power_init(&p);
    assert(!ar_power_update(&p, true, false));
}

// ---- 外部供电推断(ar_extpower_*)----
// 数据取自 2026-11-10 真机采集(电量 97%,每 500 ms 一点):插上 +75..+94 mV,
// 充满停充 -42..-49 mV,停充后拔掉 -16..-20 mV,其余时间相邻点相差不超过 3 mV。

#define SOC 97

// 依次送入一段电压,每点按 AR_LOAD_STEADY、USB 主机离线处理;返回最后一点之后是否接着电源
static bool feed(ar_extpower_t *p, const int *mv, size_t n) {
    bool on = false;
    for (size_t i = 0; i < n; i++) on = ar_extpower_update(p, mv[i], SOC, false, AR_LOAD_STEADY);
    return on;
}

// 送入一段电压,返回第几点(从 0 起)时判定结果第一次变为 want;一直没变返回 -1
static int feed_until(ar_extpower_t *p, const int *mv, size_t n, bool want) {
    for (size_t i = 0; i < n; i++) {
        if (ar_extpower_update(p, mv[i], SOC, false, AR_LOAD_STEADY) == want) return (int)i;
    }
    return -1;
}

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

static void test_extpower_measured_charger(void) {
    // 电池供电 → 插充电器 → 充满停充 → 停充后拔掉。跳变前后是真机原始数据;停充后的回落段
    // 把真机约 90 秒、20 mV 的回落压缩成 23 点,比实际更陡
    static const int battery[] = {4111, 4111, 4111, 4111, 4112, 4112, 4111, 4111, 4111, 4111, 4112, 4111, 4111, 4111};
    static const int plug[] = {4205, 4203, 4205, 4206, 4207, 4208, 4210, 4212, 4213, 4215, 4215, 4216, 4215, 4215};
    static const int term[] = {4167, 4166, 4165, 4165, 4164, 4163, 4162, 4160, 4158, 4157, 4155, 4153, 4152, 4150,
                               4149, 4148, 4148, 4147, 4147, 4147, 4148, 4147, 4148};
    static const int unplug[] = {4132, 4131, 4130, 4129, 4129, 4128, 4127, 4126, 4126, 4125};
    ar_extpower_t p;
    ar_extpower_init(&p);
    assert(!feed(&p, battery, LEN(battery)));
    // 跳变连续两点成立才算,第二点(1 秒内)判定接上
    assert(feed_until(&p, plug, LEN(plug), true) == 1);
    assert(feed(&p, plug + 2, LEN(plug) - 2));
    // 恒压段停充:降幅 48 mV,仍接着电源
    assert(feed_until(&p, term, LEN(term), false) == -1);
    // 停充后拔掉:降幅 16 mV,第二点判定断开
    assert(feed_until(&p, unplug, LEN(unplug), false) == 1);
    assert(!feed(&p, unplug + 2, LEN(unplug) - 2));
}

static void test_extpower_measured_second_round(void) {
    // 第二轮:停充降幅 42 mV、停充后拔掉降幅 17 mV,同样判对(回落段同样压缩过)
    static const int battery[] = {4114, 4113, 4113, 4114, 4115, 4115, 4113, 4113, 4114, 4113, 4113, 4113, 4113, 4113};
    static const int plug[] = {4208, 4210, 4211, 4211, 4211, 4212, 4214, 4215, 4216, 4214, 4214, 4214, 4215, 4215};
    static const int term[] = {4173, 4172, 4172, 4171, 4171, 4170, 4168, 4166, 4165, 4164, 4163, 4162, 4162, 4161,
                               4160, 4161, 4160, 4160, 4161, 4160, 4160, 4161};
    static const int unplug[] = {4144, 4143, 4143, 4143, 4142, 4142, 4143, 4142};
    ar_extpower_t p;
    ar_extpower_init(&p);
    assert(!feed(&p, battery, LEN(battery)));
    assert(feed_until(&p, plug, LEN(plug), true) == 1);
    assert(feed(&p, plug + 2, LEN(plug) - 2));
    assert(feed_until(&p, term, LEN(term), false) == -1);
    assert(feed_until(&p, unplug, LEN(unplug), false) == 1);
}

static void test_extpower_noise_and_dips_on_battery(void) {
    // 电池供电时的噪声与负载下陷不能判成接上:相邻点 ±3 mV,单点下陷 10 mV,
    // 持续 3 秒(6 点)的下陷 15 mV 后回升。回升只是回到原电平,不是跳变;下陷期间若把基准换成
    // 下陷电平,回升就会被误判成插上
    static const int trace[] = {4120, 4121, 4119, 4122, 4120, 4118, 4121, 4120, 4110, 4120, 4121, 4119,
                                4105, 4105, 4106, 4105, 4104, 4105, 4120, 4121, 4120, 4122, 4120, 4119,
                                4121, 4120, 4120};
    ar_extpower_t p;
    ar_extpower_init(&p);
    assert(feed_until(&p, trace, LEN(trace), true) == -1);
}

static void test_extpower_unplug_while_charging(void) {
    // 恒流段(电压低于恒压段)拔掉:任何达到 AR_PWR_STEP_MV 的降幅都是断开
    static const int battery[] = {3800, 3800, 3801, 3800, 3799, 3800, 3800, 3800};
    static const int charging[] = {3900, 3901, 3902, 3902, 3903, 3904, 3904, 3905};
    static const int unplug[] = {3810, 3809, 3809, 3808};
    ar_extpower_t p;
    ar_extpower_init(&p);
    feed(&p, battery, LEN(battery));
    assert(feed(&p, charging, LEN(charging)));
    assert(feed_until(&p, unplug, LEN(unplug), false) == 1);

    // 恒压段拔掉:降幅达到 AR_PWR_TERM_MAX_MV,不是停充,判断开
    static const int cv[] = {4215, 4215, 4216, 4215, 4215, 4215, 4216, 4215};
    const int drop = 4215 - AR_PWR_TERM_MAX_MV;
    const int cv_unplug[] = {drop, drop, drop - 1, drop - 1};
    ar_extpower_init(&p);
    feed(&p, battery, LEN(battery));
    assert(feed(&p, cv, LEN(cv)));
    assert(feed_until(&p, cv_unplug, LEN(cv_unplug), false) == 1);
    // 降幅差 1 mV 不到上限:按停充处理,仍接着电源
    const int cv_term[] = {drop + 1, drop + 1, drop, drop};
    ar_extpower_init(&p);
    feed(&p, battery, LEN(battery));
    feed(&p, cv, LEN(cv));
    assert(feed_until(&p, cv_term, LEN(cv_term), false) == -1);
}

static void test_extpower_step_threshold(void) {
    // 跳变门限:差 1 mV 不算,刚好 AR_PWR_STEP_MV 算(电池充满、充电器不再充电时,
    // 插上只剩负载转到充电器带来的上跳,实测停充后拔掉的降幅 16..20 mV)
    static const int base[] = {4150, 4150, 4150, 4150, 4150, 4150, 4150, 4150};
    ar_extpower_t p;
    ar_extpower_init(&p);
    feed(&p, base, LEN(base));
    const int small[] = {4150 + AR_PWR_STEP_MV - 1, 4150 + AR_PWR_STEP_MV - 1, 4150 + AR_PWR_STEP_MV - 1};
    assert(feed_until(&p, small, LEN(small), true) == -1);
    ar_extpower_init(&p);
    feed(&p, base, LEN(base));
    const int enough[] = {4150 + AR_PWR_STEP_MV, 4150 + AR_PWR_STEP_MV};
    assert(feed_until(&p, enough, LEN(enough), true) == 1);
    // 只有一点跳上去又回落:是毛刺,不算
    ar_extpower_init(&p);
    feed(&p, base, LEN(base));
    const int spike[] = {4200, 4150, 4150, 4150};
    assert(feed_until(&p, spike, LEN(spike), true) == -1);
}

static void test_extpower_usb_host(void) {
    // USB 主机在线时一定接着电源;离线但电压没有降(Mac 睡眠、USB 挂起)仍接着电源
    static const int idle[] = {4141, 4141, 4140, 4141, 4141, 4140, 4140, 4141, 4140, 4140};
    ar_extpower_t p;
    ar_extpower_init(&p);
    for (size_t i = 0; i < LEN(idle); i++) assert(ar_extpower_update(&p, idle[i], SOC, true, AR_LOAD_STEADY));
    assert(feed_until(&p, idle, LEN(idle), false) == -1);
    // 真机:停充状态下从电脑拔掉,降 18..20 mV,第二点判定断开
    static const int unplug[] = {4123, 4121, 4121, 4122, 4121, 4121};
    assert(feed_until(&p, unplug, LEN(unplug), false) == 1);
    // USB 主机在线期间的降幅不算断开
    ar_extpower_init(&p);
    for (size_t i = 0; i < LEN(idle); i++) ar_extpower_update(&p, idle[i], SOC, true, AR_LOAD_STEADY);
    for (size_t i = 0; i < LEN(unplug); i++) assert(ar_extpower_update(&p, unplug[i], SOC, true, AR_LOAD_STEADY));
    // 读不到电压时只看 USB 主机
    ar_extpower_init(&p);
    assert(!ar_extpower_update(&p, -1, -1, false, AR_LOAD_STEADY));
    assert(ar_extpower_update(&p, -1, -1, true, AR_LOAD_STEADY));
}

static void test_extpower_cv_level(void) {
    // 开机时已在恒压充电(没有插上的跳变):电压连续两点达到 AR_PWR_CV_MV 即判定接着电源。
    // 电池单独供电时实测不超过 4150 mV
    ar_extpower_t p;
    ar_extpower_init(&p);
    assert(!ar_extpower_update(&p, AR_PWR_CV_MV, SOC, false, AR_LOAD_STEADY));
    assert(ar_extpower_update(&p, AR_PWR_CV_MV + 2, SOC, false, AR_LOAD_STEADY));
    ar_extpower_init(&p);
    static const int below[] = {AR_PWR_CV_MV - 1, AR_PWR_CV_MV - 1, AR_PWR_CV_MV - 1};
    assert(feed_until(&p, below, LEN(below), true) == -1);
}

static void test_extpower_own_load_changes(void) {
    // 设备自己改背光会让电压跳一下:从设定亮度调回低亮度,电池上的电压回升约 14 mV(估算)。
    // 调用方传 AR_LOAD_CHANGED 时以新电平为基准,不判成接上
    static const int base[] = {4120, 4120, 4121, 4120, 4120, 4120, 4121, 4120};
    static const int after[] = {4134, 4134, 4135, 4134, 4134, 4135, 4134, 4134};
    ar_extpower_t p;
    ar_extpower_init(&p);
    feed(&p, base, LEN(base));
    ar_extpower_update(&p, after[0], SOC, false, AR_LOAD_CHANGED);
    ar_extpower_update(&p, after[1], SOC, false, AR_LOAD_CHANGED);
    assert(feed_until(&p, after + 2, LEN(after) - 2, true) == -1);
    // 不标负载变化时同样的回升会判成接上(说明上面靠的是基准重置)
    ar_extpower_init(&p);
    feed(&p, base, LEN(base));
    assert(feed_until(&p, after, LEN(after), true) == 1);

    // 提示音期间的点(AR_LOAD_TRANSIENT)不计入:响铃时的大幅下陷不会判成断开
    static const int cv[] = {4215, 4215, 4216, 4215, 4215, 4215, 4216, 4215};
    ar_extpower_init(&p);
    feed(&p, cv, LEN(cv));
    assert(ar_extpower_update(&p, 4100, SOC, false, AR_LOAD_TRANSIENT));
    assert(ar_extpower_update(&p, 4100, SOC, false, AR_LOAD_TRANSIENT));
    assert(feed_until(&p, cv, LEN(cv), false) == -1);
}

static void test_extpower_soc_fallback(void) {
    // 兜底:判定接着电源、USB 主机离线,电量却比接上后的最高值低了 AR_PWR_SOC_FALL,
    // 说明其实在耗电池(拔线没识别出来),改判断开
    static const int battery[] = {4111, 4111, 4111, 4111, 4112, 4112, 4111, 4111};
    static const int plug[] = {4205, 4203, 4205, 4206};
    static const int term[] = {4167, 4166, 4165, 4165, 4164, 4163, 4162, 4160};
    ar_extpower_t p;
    ar_extpower_init(&p);
    feed(&p, battery, LEN(battery));
    feed(&p, plug, LEN(plug));
    feed(&p, term, LEN(term));
    assert(ar_extpower_update(&p, 4160, SOC - AR_PWR_SOC_FALL + 1, false, AR_LOAD_STEADY));
    assert(!ar_extpower_update(&p, 4160, SOC - AR_PWR_SOC_FALL, false, AR_LOAD_STEADY));
    // 读不到电量时不触发兜底;USB 主机在线时不触发兜底
    ar_extpower_init(&p);
    feed(&p, battery, LEN(battery));
    feed(&p, plug, LEN(plug));
    assert(ar_extpower_update(&p, 4206, -1, false, AR_LOAD_STEADY));
    ar_extpower_init(&p);
    ar_extpower_update(&p, 4141, SOC, true, AR_LOAD_STEADY);
    assert(ar_extpower_update(&p, 4141, SOC - 5, true, AR_LOAD_STEADY));
}

static void test_any_key_wakes_dark_screen(void) {
    ar_keywake_t k;
    ar_keywake_init(&k);

    // 屏幕亮着:按键动作照常交给菜单
    ar_keywake_press(&k, false, 1000);
    ar_keywake_release(&k, 1100);
    assert(ar_keywake_action(&k));

    // 熄屏时单击:按下即亮屏(调用方处理),这一下的单击被吞掉,下一次单击照常
    ar_keywake_press(&k, true, 2000);
    ar_keywake_release(&k, 2100);
    assert(!ar_keywake_action(&k));
    ar_keywake_press(&k, false, 3000);
    ar_keywake_release(&k, 3100);
    assert(ar_keywake_action(&k));

    // 熄屏时双击:第二下按下时屏幕已亮,但仍属同一手势,双击整体被吞掉
    ar_keywake_press(&k, true, 4000);
    ar_keywake_release(&k, 4080);
    ar_keywake_press(&k, false, 4150);
    ar_keywake_release(&k, 4230);
    assert(!ar_keywake_action(&k));

    // 熄屏时长按(如按住上键):长按动作被吞掉
    ar_keywake_press(&k, true, 5000);
    assert(!ar_keywake_action(&k));
    ar_keywake_release(&k, 6000);

    // 熄屏时三连击:按键组件不发单击、双击事件,手势结束时没有动作可吞。
    // 下一次手势与上次松手隔开超过 AR_KEY_GESTURE_GAP_MS,其动作不能被误吞
    ar_keywake_press(&k, true, 7000);
    ar_keywake_release(&k, 7060);
    ar_keywake_press(&k, false, 7120);
    ar_keywake_release(&k, 7180);
    ar_keywake_press(&k, false, 7240);
    ar_keywake_release(&k, 7300);
    ar_keywake_press(&k, false, 7300 + AR_KEY_GESTURE_GAP_MS);
    ar_keywake_release(&k, 7400 + AR_KEY_GESTURE_GAP_MS);
    assert(ar_keywake_action(&k));

    // 连击判定窗口(按键组件为 180 ms)必须落在手势间隔之内
    assert(AR_KEY_GESTURE_GAP_MS > 180u);
}

static void test_notice_events(void) {
    ar_notice_t n;
    ar_notice_init(&n);
    // 开机后的第一帧:等你处理按从 0 增加算;完成数只作基准
    ar_notice_ev_t ev = ar_notice_update(&n, 1, 20);
    assert(ev.waiting && ev.completed == 0);
    ev = ar_notice_update(&n, 1, 20);
    assert(!ev.waiting && ev.completed == 0);
    // 完成数增加:报告增量,同时等你处理减少不算通知
    ev = ar_notice_update(&n, 0, 23);
    assert(!ev.waiting && ev.completed == 3);
    // 跨天清零:完成数变小,只更新基准
    ev = ar_notice_update(&n, 0, 0);
    assert(ev.completed == 0);
    ev = ar_notice_update(&n, 0, 1);
    assert(ev.completed == 1);
    // 掉线后恢复:离线期间累计的完成不提示;仍未处理的等你处理不重复提醒
    ev = ar_notice_update(&n, 2, 1);
    assert(ev.waiting);
    ar_notice_offline(&n);
    ev = ar_notice_update(&n, 2, 9);
    assert(!ev.waiting && ev.completed == 0);
    ev = ar_notice_update(&n, 3, 10);
    assert(ev.waiting && ev.completed == 1);
}

static ar_notice_ev_t notice(bool waiting, uint16_t completed) {
    ar_notice_ev_t ev = { .waiting = waiting, .completed = completed };
    return ev;
}

static void test_chime_rules(void) {
    ar_chime_t c;
    ar_chime_init(&c);
    assert(ar_chime_pick(&c, notice(false, 0), 1000, false) == AR_CHIME_NONE);
    assert(ar_chime_pick(&c, notice(true, 0), 2000, false) == AR_CHIME_WAITING);       // 新增等待项:响
    assert(ar_chime_pick(&c, notice(true, 0), 2000 + 29999, false) == AR_CHIME_NONE);  // 30 秒内的第二次:不响
    assert(ar_chime_pick(&c, notice(true, 0), 2000 + 40000, false) == AR_CHIME_WAITING);
    assert(ar_chime_pick(&c, notice(true, 0), 200000, true) == AR_CHIME_NONE);         // 静音:不响

    // 完成:与上一次任意提示音至少间隔 AR_DONE_CHIME_MIN_INTERVAL_MS,期间的完成合并不补响
    ar_chime_init(&c);
    assert(ar_chime_pick(&c, notice(false, 2), 1000, false) == AR_CHIME_DONE);
    assert(ar_chime_pick(&c, notice(false, 1), 1000 + AR_DONE_CHIME_MIN_INTERVAL_MS - 1, false) == AR_CHIME_NONE);
    assert(ar_chime_pick(&c, notice(false, 1), 1000 + AR_DONE_CHIME_MIN_INTERVAL_MS, false) == AR_CHIME_DONE);
    // 同一帧既有等你处理又有完成:只响等你处理;紧随其后的完成不响,等你处理不受完成提示音影响
    ar_chime_init(&c);
    assert(ar_chime_pick(&c, notice(true, 1), 5000, false) == AR_CHIME_WAITING);
    assert(ar_chime_pick(&c, notice(false, 1), 6000, false) == AR_CHIME_NONE);
    ar_chime_init(&c);
    assert(ar_chime_pick(&c, notice(false, 1), 5000, false) == AR_CHIME_DONE);
    assert(ar_chime_pick(&c, notice(true, 0), 6000, false) == AR_CHIME_WAITING);
    assert(AR_DONE_CHIME_MIN_INTERVAL_MS < AR_CHIME_MIN_INTERVAL_MS);

    // 毫秒计数回绕时间隔仍按无符号差值计算
    ar_chime_init(&c);
    assert(ar_chime_pick(&c, notice(true, 0), 0xFFFFFF00u, false) == AR_CHIME_WAITING);
    assert(ar_chime_pick(&c, notice(true, 0), 0x00007600u, false) == AR_CHIME_WAITING);
}

static void test_volume(void) {
    // 四档:静音、低、中、高;中档与改版前的固定音量相同
    assert(AR_VOLUME_LEVELS == 4);
    assert(ar_volume_percent(0) == 0);
    assert(ar_volume_percent(1) > 0);
    assert(ar_volume_percent(1) < ar_volume_percent(2));
    assert(ar_volume_percent(2) == 60);
    assert(ar_volume_percent(2) < ar_volume_percent(3));
    assert(ar_volume_percent(9) == ar_volume_percent(3));   // 越界按最高档
    assert(AR_VOLUME_DEFAULT == 2);
    // 上下键加减:到最低档(静音)与最高档时停住,不循环;越界按最高档处理
    assert(ar_volume_step(0, -1) == 0);
    assert(ar_volume_step(0, +1) == 1);
    assert(ar_volume_step(2, +1) == 3);
    assert(ar_volume_step(3, +1) == 3);
    assert(ar_volume_step(2, -1) == 1);
    assert(ar_volume_step(9, -1) == 2);
}

// 深睡判定的默认输入:电池供电、电量计正常、不在配对确认,未连接与无按键都刚好满时限
static ar_sleep_in_t sleep_in(void) {
    ar_sleep_in_t in = {
        .on_power = false,
        .battery_present = true,
        .confirming = false,
        .ms_disconnected = AR_DEEP_SLEEP_AFTER_MS,
        .ms_since_key = AR_DEEP_SLEEP_AFTER_MS,
    };
    return in;
}

static void test_deep_sleep_after_long_disconnect_on_battery(void) {
    // 深睡排在关背光之后
    assert(AR_DEEP_SLEEP_AFTER_MS > AR_BACKLIGHT_IDLE_MS);

    ar_sleep_in_t in = sleep_in();
    assert(ar_model_should_deep_sleep(&in));
    // 开机后从未按过键也算满足
    in.ms_since_key = NEVER;
    assert(ar_model_should_deep_sleep(&in));

    // 未连接差 1 毫秒不睡;在线(含“睡觉中”)时长为 0,不睡
    in = sleep_in();
    in.ms_disconnected = AR_DEEP_SLEEP_AFTER_MS - 1;
    assert(!ar_model_should_deep_sleep(&in));
    in.ms_disconnected = 0;
    assert(!ar_model_should_deep_sleep(&in));

    // 时限内按过键不睡(按键唤醒后重新计时)
    in = sleep_in();
    in.ms_since_key = AR_DEEP_SLEEP_AFTER_MS - 1;
    assert(!ar_model_should_deep_sleep(&in));
}

// 熄屏关机判定的默认输入:电池供电、在线、刚按过键,只有熄屏时长满 1 小时
static ar_sleep_in_t dark_in(void) {
    ar_sleep_in_t in = {
        .on_power = false,
        .battery_present = true,
        .confirming = false,
        .ms_disconnected = 0,
        .ms_since_key = 0,
        .ms_dark = AR_POWER_OFF_AFTER_DARK_MS,
    };
    return in;
}

static void test_power_off_after_dark_hour_on_battery(void) {
    assert(AR_POWER_OFF_AFTER_DARK_MS == 3600000u);
    // 电池供电时熄屏满 1 小时即关机,在线(含蓝牙连着电脑)也一样
    ar_sleep_in_t in = dark_in();
    assert(ar_model_should_deep_sleep(&in));
    in.ms_dark = AR_POWER_OFF_AFTER_DARK_MS - 1;
    assert(!ar_model_should_deep_sleep(&in));
    // 亮屏期间熄屏时长为 0,不关机
    in.ms_dark = 0;
    assert(!ar_model_should_deep_sleep(&in));

    // 插着 USB 主机、读不到电量计、等待配对确认时都不关机
    in = dark_in();
    in.on_power = true;
    assert(!ar_model_should_deep_sleep(&in));
    in = dark_in();
    in.battery_present = false;
    assert(!ar_model_should_deep_sleep(&in));
    in = dark_in();
    in.confirming = true;
    assert(!ar_model_should_deep_sleep(&in));
}

static void test_no_deep_sleep_when_powered_or_pairing(void) {
    // 接着 USB 主机时不睡
    ar_sleep_in_t in = sleep_in();
    in.on_power = true;
    assert(!ar_model_should_deep_sleep(&in));
    // 电量计读不到(可能没装电池)时不睡,避免 Mac 睡眠期间只靠 USB 供电的设备睡下去
    in = sleep_in();
    in.battery_present = false;
    assert(!ar_model_should_deep_sleep(&in));
    // 等待配对确认时不睡
    in = sleep_in();
    in.confirming = true;
    assert(!ar_model_should_deep_sleep(&in));
}

// 工具专用页面:首页 → Claude Code → Codex → 首页,下键前进、上键后退(专用页面方案 §4.1)
static void test_page_cycle(void) {
    assert(ar_page_step(AR_PAGE_HOME, +1) == AR_PAGE_CC);
    assert(ar_page_step(AR_PAGE_CC, +1) == AR_PAGE_CX);
    assert(ar_page_step(AR_PAGE_CX, +1) == AR_PAGE_HOME);
    assert(ar_page_step(AR_PAGE_HOME, -1) == AR_PAGE_CX);
    assert(ar_page_step(AR_PAGE_CX, -1) == AR_PAGE_CC);
    assert(ar_page_step(AR_PAGE_CC, -1) == AR_PAGE_HOME);
    assert(ar_page_step(AR_PAGE_CC, +2) == AR_PAGE_HOME);   // 下键双击按两步
    assert(ar_page_step(AR_PAGE_CC, -2) == AR_PAGE_CX);
    assert(ar_page_step(200, +1) == AR_PAGE_CC);            // 状态损坏时从首页算起
}

// 画面优先级:等你处理、未连接覆盖专用页面,清空后回到原页面;这些时候上下键不切页
static void test_page_priority_and_keys(void) {
    assert(ar_page_shown(AR_PAGE_CX, AR_SCREEN_WAITING) == AR_PAGE_HOME);
    assert(ar_page_shown(AR_PAGE_CX, AR_SCREEN_DISCONNECTED) == AR_PAGE_HOME);
    assert(ar_page_shown(AR_PAGE_CX, AR_SCREEN_RUN) == AR_PAGE_CX);
    assert(ar_page_shown(AR_PAGE_CC, AR_SCREEN_SLEEP) == AR_PAGE_CC);
    assert(ar_page_shown(AR_PAGE_HOME, AR_SCREEN_WALK) == AR_PAGE_HOME);
    assert(ar_page_shown(9, AR_SCREEN_WALK) == AR_PAGE_HOME);

    assert(ar_page_keys_active(AR_SCREEN_RUN, false, false));
    assert(ar_page_keys_active(AR_SCREEN_SLEEP, false, false));
    assert(!ar_page_keys_active(AR_SCREEN_RUN, true, false));         // 设置菜单打开
    assert(!ar_page_keys_active(AR_SCREEN_RUN, false, true));         // 配对确认
    assert(!ar_page_keys_active(AR_SCREEN_WAITING, false, false));    // 等你处理
    assert(!ar_page_keys_active(AR_SCREEN_DISCONNECTED, false, false));
}

// 距重置时间的文字:不足 1 小时按分钟,不足 1 天按小时与分钟,否则按天与小时
static void test_reset_text(void) {
    char buf[48];
    ar_reset_text(1, buf, sizeof(buf));
    assert(strcmp(buf, "1 分钟后重置") == 0);
    ar_reset_text(59, buf, sizeof(buf));
    assert(strcmp(buf, "59 分钟后重置") == 0);
    ar_reset_text(60, buf, sizeof(buf));
    assert(strcmp(buf, "1 小时 0 分后重置") == 0);
    ar_reset_text(135, buf, sizeof(buf));
    assert(strcmp(buf, "2 小时 15 分后重置") == 0);
    ar_reset_text(1439, buf, sizeof(buf));
    assert(strcmp(buf, "23 小时 59 分后重置") == 0);
    ar_reset_text(1440, buf, sizeof(buf));
    assert(strcmp(buf, "1 天 0 小时后重置") == 0);
    ar_reset_text(3 * 1440 + 5 * 60 + 59, buf, sizeof(buf));
    assert(strcmp(buf, "3 天 5 小时后重置") == 0);
    ar_reset_text(AR_RESET_UNKNOWN, buf, sizeof(buf));
    assert(strcmp(buf, "重置时间未知") == 0);
    ar_reset_text(0, buf, sizeof(buf));
    assert(strcmp(buf, "1 分钟后重置") == 0);
    // 缓冲区不够时截断但仍以 NUL 结尾
    char tiny[4];
    ar_reset_text(135, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));
}

// 用量 ≥80% 时进度条改用警示色;暂无数据、无此限额不算
static void test_usage_alert(void) {
    assert(!ar_usage_alert(79));
    assert(ar_usage_alert(80));
    assert(ar_usage_alert(100));
    assert(!ar_usage_alert(AR_USAGE_UNKNOWN));
    assert(!ar_usage_alert(AR_USAGE_NO_LIMIT));
}

static void test_screen_off_options(void) {
    // 电池供电:10 秒、30 秒、1 分、3 分,默认 30 秒(与改版前相同)
    static const uint32_t battery[] = { 10000u, 30000u, 60000u, 180000u };
    assert(AR_BATTERY_OFF_OPTIONS == 4);
    for (uint8_t i = 0; i < AR_BATTERY_OFF_OPTIONS; i++) assert(ar_battery_off_ms(i) == battery[i]);
    assert(AR_BATTERY_OFF_DEFAULT == 1);
    // 接电脑:5 分、10 分、30 分、1 小时、不熄屏,默认 30 分(与改版前相同)
    static const uint32_t usb[] = { 300000u, 600000u, 1800000u, 3600000u, AR_OFF_NEVER };
    assert(AR_USB_OFF_OPTIONS == 5);
    for (uint8_t i = 0; i < AR_USB_OFF_OPTIONS; i++) assert(ar_usb_off_ms(i) == usb[i]);
    assert(AR_USB_OFF_DEFAULT == 2);
    // 越界按最后一项处理,与音量、亮度一致
    assert(ar_battery_off_ms(200) == 180000u);
    assert(ar_usb_off_ms(200) == AR_OFF_NEVER);

    // 选项加减:到头停住;越界先按最后一项处理
    assert(ar_option_step(1, AR_BATTERY_OFF_OPTIONS, +1) == 2);
    assert(ar_option_step(3, AR_BATTERY_OFF_OPTIONS, +1) == 3);
    assert(ar_option_step(0, AR_BATTERY_OFF_OPTIONS, -1) == 0);
    assert(ar_option_step(2, AR_USB_OFF_OPTIONS, -2) == 0);
    assert(ar_option_step(9, AR_USB_OFF_OPTIONS, 0) == 4);
    assert(ar_option_step(9, AR_USB_OFF_OPTIONS, -1) == 3);
}

static void test_manual_lock(void) {
    // 手动熄屏:直到下一次亮屏事件前一直熄灭,不论供电方式、熄屏设置与画面
    ar_backlight_in_t in = backlight_in(AR_SCREEN_RUN, true);
    in.locked = true;
    assert(ar_model_backlight_percent(&in) == 0);
    in.usb_off = AR_USB_OFF_OPTIONS - 1;   // 不熄屏也照样熄灭
    assert(ar_model_backlight_percent(&in) == 0);
    in = backlight_in(AR_SCREEN_WAITING, false);
    in.locked = true;
    assert(ar_model_backlight_percent(&in) == 0);
    in = backlight_in(AR_SCREEN_DISCONNECTED, true);
    in.locked = true;
    assert(ar_model_backlight_percent(&in) == 0);
    // 解除后按原规则点亮
    in.locked = false;
    assert(ar_model_backlight_percent(&in) == ar_brightness_percent(3));

    // 只在菜单关着、不在配对确认时生效
    assert(ar_lock_allowed(false, false));
    assert(!ar_lock_allowed(true, false));
    assert(!ar_lock_allowed(false, true));
}

static void test_toggle_step(void) {
    // 勿扰类开关:上键开启、下键关闭,连按(双击)也不会来回翻转
    assert(ar_toggle_step(false, +1));
    assert(ar_toggle_step(true, +2));
    assert(!ar_toggle_step(true, -1));
    assert(!ar_toggle_step(false, -2));
    assert(ar_toggle_step(true, 0) && !ar_toggle_step(false, 0));
}

static void test_local_clock(void) {
    ar_clock_t c;
    ar_clock_init(&c);
    uint16_t minute = 0;
    // 还没对过时:时间未知
    assert(!ar_clock_minute(&c, 1000, &minute));
    // 主机没带 tm 或越界时不对时
    ar_clock_sync(&c, AR_TIME_UNKNOWN, 1000);
    ar_clock_sync(&c, 1440, 1000);
    assert(!ar_clock_minute(&c, 1000, &minute));

    // 对时后按本机毫秒计数走时,满一分钟进一
    ar_clock_sync(&c, 21 * 60 + 59, 5000);
    assert(ar_clock_minute(&c, 5000, &minute) && minute == 21 * 60 + 59);
    assert(ar_clock_minute(&c, 5000 + 59999, &minute) && minute == 21 * 60 + 59);
    assert(ar_clock_minute(&c, 5000 + 60000, &minute) && minute == 22 * 60);
    // 跨午夜回到 0
    ar_clock_sync(&c, 1439, 0);
    assert(ar_clock_minute(&c, 60000, &minute) && minute == 0);
    assert(ar_clock_minute(&c, 60000u * 61u, &minute) && minute == 60);
    // 走满一整天回到同一分钟,再多一分钟进到次日 0 点
    assert(ar_clock_minute(&c, 60000u * 1440u, &minute) && minute == 1439);
    assert(ar_clock_minute(&c, 60000u * 1441u, &minute) && minute == 0);
    // 毫秒计数回绕时按无符号差值计算
    ar_clock_sync(&c, 600, 0xFFFFFF00u);
    assert(ar_clock_minute(&c, 0xFFFFFF00u + 120000u, &minute) && minute == 602);
    // 重新对时以新值为准(Mac 改了时间或时区)
    ar_clock_sync(&c, 100, 0x10000000u);
    assert(ar_clock_minute(&c, 0x10000000u, &minute) && minute == 100);
}

static void test_night_window(void) {
    // 用户指定:夜间为 22:00–08:00,含 22:00,不含 08:00
    assert(!ar_is_night(21 * 60 + 59));
    assert(ar_is_night(22 * 60));
    assert(ar_is_night(23 * 60 + 59));
    assert(ar_is_night(0));
    assert(ar_is_night(7 * 60 + 59));
    assert(!ar_is_night(8 * 60));
    assert(!ar_is_night(12 * 60));
}

static void test_dnd(void) {
    ar_clock_t c;
    ar_clock_init(&c);
    // 手动勿扰:不需要时间,一直生效
    assert(ar_dnd_active(true, false, &c, 0));
    assert(!ar_dnd_active(false, false, &c, 0));
    // 夜间勿扰:还没对时不生效,状态为“未同步时间”
    assert(!ar_dnd_active(false, true, &c, 0));
    assert(ar_night_state(&c, 0) == AR_NIGHT_NO_TIME);
    // 对时后按时段生效
    ar_clock_sync(&c, 23 * 60, 0);
    assert(ar_dnd_active(false, true, &c, 0));
    assert(ar_night_state(&c, 0) == AR_NIGHT_ON);
    assert(!ar_dnd_active(false, false, &c, 0));   // 夜间勿扰关着时不生效
    ar_clock_sync(&c, 8 * 60, 0);
    assert(!ar_dnd_active(false, true, &c, 0));
    assert(ar_night_state(&c, 0) == AR_NIGHT_OFF);
    // 走时跨进夜间时段后自动生效
    ar_clock_sync(&c, 21 * 60 + 59, 0);
    assert(!ar_dnd_active(false, true, &c, 0));
    assert(ar_dnd_active(false, true, &c, 60000));
    // 两个开关独立:手动勿扰开着时不看时间
    assert(ar_dnd_active(true, true, &c, 0));
}

static void test_waiting_remind(void) {
    // 用户指定:等你处理超过 3 分钟没处理就再提醒,最多再提醒 2 次
    assert(AR_REMIND_AFTER_MS == 180000u);
    assert(AR_REMIND_MAX == 2);
    // 再提醒的间隔长于等你处理提示音的节流间隔,不会被它压住
    assert(AR_REMIND_AFTER_MS > AR_CHIME_MIN_INTERVAL_MS);

    ar_remind_t r;
    ar_remind_init(&r);
    assert(!ar_remind_update(&r, 0, false, 1000));
    // 新增等你处理:本身由普通提示音负责,这里只开始计时
    const uint32_t t0 = 5000;
    assert(!ar_remind_update(&r, 1, true, t0));
    assert(!ar_remind_update(&r, 1, false, t0 + AR_REMIND_AFTER_MS - 1));
    assert(ar_remind_update(&r, 1, false, t0 + AR_REMIND_AFTER_MS));             // 第 1 次再提醒
    assert(!ar_remind_update(&r, 1, false, t0 + AR_REMIND_AFTER_MS + 1));
    assert(!ar_remind_update(&r, 1, false, t0 + 2 * AR_REMIND_AFTER_MS - 1));
    assert(ar_remind_update(&r, 1, false, t0 + 2 * AR_REMIND_AFTER_MS));         // 第 2 次再提醒
    // 到上限后不再提醒,无论等多久
    assert(!ar_remind_update(&r, 1, false, t0 + 3 * AR_REMIND_AFTER_MS));
    assert(!ar_remind_update(&r, 1, false, t0 + 20 * AR_REMIND_AFTER_MS));

    // 处理完(数量归零)后重新开始;再次出现的等你处理重新计次
    assert(!ar_remind_update(&r, 0, false, 2000000));
    assert(!ar_remind_update(&r, 1, true, 2000000));
    assert(ar_remind_update(&r, 1, false, 2000000 + AR_REMIND_AFTER_MS));

    // 处理了一部分(2 → 1,不是新增):继续原来的计时与计次
    ar_remind_init(&r);
    assert(!ar_remind_update(&r, 2, true, 0));
    assert(!ar_remind_update(&r, 1, false, 60000));
    assert(ar_remind_update(&r, 1, false, AR_REMIND_AFTER_MS));

    // 等待期间又新增一项(1 → 2):重新计时,并重新给足 2 次
    ar_remind_init(&r);
    assert(!ar_remind_update(&r, 1, true, 0));
    assert(ar_remind_update(&r, 1, false, AR_REMIND_AFTER_MS));
    assert(ar_remind_update(&r, 1, false, 2 * AR_REMIND_AFTER_MS));
    const uint32_t t1 = 2 * AR_REMIND_AFTER_MS + 30000;
    assert(!ar_remind_update(&r, 2, true, t1));
    assert(!ar_remind_update(&r, 2, false, t1 + AR_REMIND_AFTER_MS - 1));
    assert(ar_remind_update(&r, 2, false, t1 + AR_REMIND_AFTER_MS));
    assert(ar_remind_update(&r, 2, false, t1 + 2 * AR_REMIND_AFTER_MS));
    assert(!ar_remind_update(&r, 2, false, t1 + 3 * AR_REMIND_AFTER_MS));

    // 没有“新增”就已经在等(开机或掉线恢复时已有等待项):从此刻开始计时,不立刻提醒
    ar_remind_init(&r);
    assert(!ar_remind_update(&r, 1, false, 7000));
    assert(!ar_remind_update(&r, 1, false, 7000 + AR_REMIND_AFTER_MS - 1));
    assert(ar_remind_update(&r, 1, false, 7000 + AR_REMIND_AFTER_MS));

    // 毫秒计数回绕时按无符号差值计算
    ar_remind_init(&r);
    assert(!ar_remind_update(&r, 1, true, 0xFFFFFF00u));
    assert(!ar_remind_update(&r, 1, false, 0x00000100u));
    assert(ar_remind_update(&r, 1, false, 0xFFFFFF00u + AR_REMIND_AFTER_MS));
}

int main(void) {
    test_page_cycle();
    test_page_priority_and_keys();
    test_reset_text();
    test_usage_alert();
    test_speed_levels();
    test_waiting_overrides_speed();
    test_link_timeout_per_source();
    test_frame_intervals_get_faster();
    test_brightness();
    test_menu_navigation();
    test_menu_timeouts();
    test_backlight_on_power();
    test_backlight_on_battery();
    test_backlight_off_when_disconnected();
    test_unplug_keeps_screen_on();
    test_extpower_measured_charger();
    test_extpower_measured_second_round();
    test_extpower_noise_and_dips_on_battery();
    test_extpower_unplug_while_charging();
    test_extpower_step_threshold();
    test_extpower_usb_host();
    test_extpower_cv_level();
    test_extpower_own_load_changes();
    test_extpower_soc_fallback();
    test_any_key_wakes_dark_screen();
    test_notice_events();
    test_chime_rules();
    test_waiting_remind();
    test_volume();
    test_screen_off_options();
    test_manual_lock();
    test_toggle_step();
    test_local_clock();
    test_night_window();
    test_dnd();
    test_deep_sleep_after_long_disconnect_on_battery();
    test_power_off_after_dark_hour_on_battery();
    test_no_deep_sleep_when_powered_or_pairing();
    return 0;
}
