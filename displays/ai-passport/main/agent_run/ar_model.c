// main/agent_run/ar_model.c —— 画面选择与交互规则实现。见 ar_model.h。
#include "ar_model.h"

#include <stdio.h>

#include "ar_text.h"

// 亮度档位对应的背光百分比
static const uint8_t BRIGHTNESS_PERCENT[AR_BRIGHTNESS_LEVELS] = { 20, 40, 60, 80, 100 };

// 音量档位对应的播放音量百分比(ES8311 输出音量,0 为静音)
static const uint8_t VOLUME_PERCENT[AR_VOLUME_LEVELS] = { 0, 40, 60, 80 };

unsigned ar_model_working_total(const ar_state_t *s) {
    return (unsigned)s->cc + (unsigned)s->cx + (unsigned)s->ds;
}

bool ar_model_link_alive(uint32_t usb_since_ms, uint32_t ble_since_ms) {
    return usb_since_ms <= AR_LINK_TIMEOUT_USB_MS || ble_since_ms <= AR_LINK_TIMEOUT_BLE_MS;
}

ar_screen_t ar_model_screen(const ar_state_t *s, bool alive) {
    if (!alive) return AR_SCREEN_DISCONNECTED;
    if (s->wn > 0) return AR_SCREEN_WAITING;
    unsigned total = ar_model_working_total(s);
    if (total == 0) return AR_SCREEN_SLEEP;
    if (total == 1) return AR_SCREEN_WALK;
    if (total <= 3) return AR_SCREEN_TROT;
    if (total <= 6) return AR_SCREEN_RUN;
    return AR_SCREEN_DASH;
}

uint16_t ar_model_frame_interval_ms(ar_screen_t screen) {
    switch (screen) {
    case AR_SCREEN_WAITING: return 300;
    case AR_SCREEN_WALK: return 220;
    case AR_SCREEN_TROT: return 150;
    case AR_SCREEN_RUN: return 100;
    case AR_SCREEN_DASH: return 70;
    case AR_SCREEN_SLEEP:
    case AR_SCREEN_DISCONNECTED:
    default: return 600;
    }
}

uint8_t ar_model_speed_bars(ar_screen_t screen) {
    switch (screen) {
    case AR_SCREEN_WALK: return 1;
    case AR_SCREEN_TROT: return 2;
    case AR_SCREEN_RUN: return 3;
    case AR_SCREEN_DASH: return 4;
    default: return 0;
    }
}

uint8_t ar_page_step(uint8_t page, int delta) {
    if (page >= AR_PAGE_COUNT) page = AR_PAGE_HOME;
    // 先把 delta 归到 [0, AR_PAGE_COUNT),负数也能正确循环
    int step = delta % (int)AR_PAGE_COUNT;
    if (step < 0) step += (int)AR_PAGE_COUNT;
    return (uint8_t)((page + step) % AR_PAGE_COUNT);
}

uint8_t ar_page_shown(uint8_t page, ar_screen_t screen) {
    if (page >= AR_PAGE_COUNT) return AR_PAGE_HOME;
    // 提醒优先:等你处理、未连接时显示首页的对应画面,清空后自然回到用户所在页面
    if (screen == AR_SCREEN_WAITING || screen == AR_SCREEN_DISCONNECTED) return AR_PAGE_HOME;
    return page;
}

bool ar_page_keys_active(ar_screen_t screen, bool menu_open, bool pairing) {
    if (menu_open || pairing) return false;
    return screen != AR_SCREEN_WAITING && screen != AR_SCREEN_DISCONNECTED;
}

void ar_reset_text(uint16_t minutes, char *buf, size_t size) {
    if (size == 0) return;
    if (minutes == AR_RESET_UNKNOWN) {
        snprintf(buf, size, "%s", AR_TXT_RESET_UNKNOWN);
        return;
    }
    if (minutes == 0) minutes = 1;   // 主机向上取整,不会发 0;防御性地按 1 分钟显示
    const unsigned m = minutes;
    if (m < 60u) snprintf(buf, size, AR_TXT_RESET_MIN_FMT, m);
    else if (m < 24u * 60u) snprintf(buf, size, AR_TXT_RESET_HOUR_FMT, m / 60u, m % 60u);
    else snprintf(buf, size, AR_TXT_RESET_DAY_FMT, m / (24u * 60u), m % (24u * 60u) / 60u);
}

bool ar_usage_alert(uint8_t percent) {
    // AR_USAGE_UNKNOWN、AR_USAGE_NO_LIMIT 数值上大于 100,必须先排除
    return percent <= 100u && percent >= AR_USAGE_ALERT_PERCENT;
}

void ar_notice_init(ar_notice_t *n) {
    n->wn = 0;
    n->has_done = false;
    n->done = 0;
}

void ar_notice_offline(ar_notice_t *n) {
    n->has_done = false;
}

ar_notice_ev_t ar_notice_update(ar_notice_t *n, uint8_t wn, uint16_t done) {
    ar_notice_ev_t ev = { .waiting = wn > n->wn, .completed = 0 };
    // 有基准且完成数变大才算新完成;变小(跨天清零)或刚恢复在线时只记基准
    if (n->has_done && done > n->done) ev.completed = (uint16_t)(done - n->done);
    n->wn = wn;
    n->has_done = true;
    n->done = done;
    return ev;
}

void ar_chime_init(ar_chime_t *c) {
    c->has_waiting = false;
    c->last_waiting_ms = 0;
    c->has_any = false;
    c->last_any_ms = 0;
}

// 距上一次的间隔是否已够;无符号差值在毫秒计数回绕时仍然正确
static bool interval_passed(bool has_last, uint32_t last_ms, uint32_t now_ms, uint32_t min_ms) {
    return !has_last || (uint32_t)(now_ms - last_ms) >= min_ms;
}

ar_chime_kind_t ar_chime_pick(ar_chime_t *c, ar_notice_ev_t ev, uint32_t now_ms, bool muted) {
    if (muted) return AR_CHIME_NONE;
    if (ev.waiting) {
        // 等你处理只看自己的间隔,不被刚响过的完成提示音压住;本帧的完成随之合并
        if (!interval_passed(c->has_waiting, c->last_waiting_ms, now_ms, AR_CHIME_MIN_INTERVAL_MS)) {
            return AR_CHIME_NONE;
        }
        c->has_waiting = true;
        c->last_waiting_ms = now_ms;
        c->has_any = true;
        c->last_any_ms = now_ms;
        return AR_CHIME_WAITING;
    }
    if (ev.completed > 0 && interval_passed(c->has_any, c->last_any_ms, now_ms, AR_DONE_CHIME_MIN_INTERVAL_MS)) {
        c->has_any = true;
        c->last_any_ms = now_ms;
        return AR_CHIME_DONE;
    }
    return AR_CHIME_NONE;
}

uint8_t ar_volume_percent(uint8_t level) {
    if (level >= AR_VOLUME_LEVELS) level = AR_VOLUME_LEVELS - 1;
    return VOLUME_PERCENT[level];
}

uint8_t ar_volume_step(uint8_t level, int delta) {
    if (level >= AR_VOLUME_LEVELS) level = AR_VOLUME_LEVELS - 1;
    int next = (int)level + delta;
    if (next < 0) next = 0;
    if (next >= AR_VOLUME_LEVELS) next = AR_VOLUME_LEVELS - 1;
    return (uint8_t)next;
}

uint8_t ar_brightness_percent(uint8_t level) {
    if (level >= AR_BRIGHTNESS_LEVELS) level = AR_BRIGHTNESS_LEVELS - 1;
    return BRIGHTNESS_PERCENT[level];
}

uint8_t ar_brightness_step(uint8_t level, int delta) {
    if (level >= AR_BRIGHTNESS_LEVELS) level = AR_BRIGHTNESS_LEVELS - 1;
    int next = (int)level + delta;
    if (next < 0) next = 0;
    if (next >= AR_BRIGHTNESS_LEVELS) next = AR_BRIGHTNESS_LEVELS - 1;
    return (uint8_t)next;
}

void ar_menu_init(ar_menu_t *m) {
    m->level = AR_MENU_CLOSED;
    m->item = AR_MENU_VOLUME;
}

int ar_menu_key(ar_menu_t *m, ar_key_t key) {
    switch (m->level) {
    case AR_MENU_CLOSED:
        // 每次打开都从默认项(音量)开始
        if (key == AR_KEY_OK) {
            m->level = AR_MENU_LIST;
            m->item = AR_MENU_VOLUME;
        }
        return 0;
    case AR_MENU_LIST:
        if (key == AR_KEY_UP && m->item > 0) m->item--;
        else if (key == AR_KEY_DOWN && m->item + 1u < AR_MENU_ITEMS) m->item++;
        else if (key == AR_KEY_OK) m->level = AR_MENU_ADJUST;
        else if (key == AR_KEY_BACK) m->level = AR_MENU_CLOSED;
        return 0;
    case AR_MENU_ADJUST:
        if (key == AR_KEY_UP) return +1;
        if (key == AR_KEY_DOWN) return -1;
        if (key == AR_KEY_BACK) m->level = AR_MENU_LIST;   // 选中项不变,回到刚才那一行
        return 0;
    default:
        ar_menu_init(m);   // 状态损坏时回首页
        return 0;
    }
}

void ar_menu_tick(ar_menu_t *m, uint32_t ms_since_key) {
    if ((m->level == AR_MENU_LIST && ms_since_key >= AR_MENU_LIST_TIMEOUT_MS) ||
        (m->level == AR_MENU_ADJUST && ms_since_key >= AR_MENU_ADJUST_TIMEOUT_MS)) {
        m->level = AR_MENU_CLOSED;
    }
}

void ar_keywake_init(ar_keywake_t *k) {
    k->swallow = false;
    k->has_release = false;
    k->last_release_ms = 0;
}

void ar_keywake_press(ar_keywake_t *k, bool dark, uint32_t now_ms) {
    if (dark) {
        // 熄屏时按下:开始一次亮屏手势,其后的动作都属于它
        k->swallow = true;
        return;
    }
    // 屏幕亮着时按下:离上次松手已超过连击窗口,说明上一手势已经结束。上一手势若没有
    // 产生动作(三连击等),吞键状态会遗留到这里,清掉它,免得误吞这一次的动作
    if (k->swallow && k->has_release && (uint32_t)(now_ms - k->last_release_ms) >= AR_KEY_GESTURE_GAP_MS) {
        k->swallow = false;
    }
}

void ar_keywake_release(ar_keywake_t *k, uint32_t now_ms) {
    k->has_release = true;
    k->last_release_ms = now_ms;
}

bool ar_keywake_action(ar_keywake_t *k) {
    // 每次手势最多产生一个动作(单击、双击或长按),吞掉一个即结束这次亮屏手势
    if (k->swallow) {
        k->swallow = false;
        return false;
    }
    return true;
}

void ar_power_init(ar_power_t *p) {
    p->known = false;
    p->on_usb = false;
}

bool ar_power_update(ar_power_t *p, bool on_usb, bool screen_on) {
    const bool unplugged = p->known && p->on_usb && !on_usb;
    p->known = true;
    p->on_usb = on_usb;
    // 只在“拔线时屏幕亮着”这一种情况补记亮屏事件,让屏幕按电池规则继续亮一段时间
    return unplugged && screen_on;
}

uint8_t ar_model_backlight_percent(const ar_backlight_in_t *in) {
    const uint8_t normal = ar_brightness_percent(in->level);
    const uint8_t dim = normal < AR_DIM_PERCENT ? normal : AR_DIM_PERCENT;   // 只降不升
    if (!in->on_usb_power) return in->ms_since_wake < AR_BATTERY_OFF_AFTER_MS ? dim : 0;
    if (in->screen == AR_SCREEN_DISCONNECTED && in->ms_disconnected >= AR_BACKLIGHT_IDLE_MS &&
        in->ms_since_wake >= AR_BACKLIGHT_IDLE_MS) {
        return 0;
    }
    if (in->ms_since_wake < AR_USB_DIM_AFTER_MS) return normal;
    if (in->ms_since_wake < AR_USB_OFF_AFTER_MS) return dim;
    return 0;
}

bool ar_model_should_deep_sleep(const ar_sleep_in_t *in) {
    // 有 USB 主机、可能没装电池、正在配对时一律不睡
    if (in->on_usb_power || !in->battery_present || in->confirming) return false;
    // 熄屏满 1 小时即关机,在线时也一样(亮屏期间 ms_dark 为 0)
    if (in->ms_dark >= AR_POWER_OFF_AFTER_DARK_MS) return true;
    // 在线时 ms_disconnected 为 0,自然不满足
    return in->ms_disconnected >= AR_DEEP_SLEEP_AFTER_MS && in->ms_since_key >= AR_DEEP_SLEEP_AFTER_MS;
}
