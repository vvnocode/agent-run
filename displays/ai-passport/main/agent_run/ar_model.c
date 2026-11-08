// main/agent_run/ar_model.c —— 画面选择与交互规则实现。见 ar_model.h。
#include "ar_model.h"

// 亮度档位对应的背光百分比
static const uint8_t BRIGHTNESS_PERCENT[AR_BRIGHTNESS_LEVELS] = { 20, 40, 60, 80, 100 };

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

void ar_chime_init(ar_chime_t *c) {
    c->last_wn = 0;
    c->has_chimed = false;
    c->last_chime_ms = 0;
}

bool ar_chime_update(ar_chime_t *c, uint8_t wn, uint32_t now_ms, bool muted) {
    bool increased = wn > c->last_wn;
    c->last_wn = wn;   // 无论响不响都记下,被抑制的增加不会在之后补响
    if (!increased || muted) return false;
    // 无符号差值在毫秒计数回绕时仍然正确
    if (c->has_chimed && (uint32_t)(now_ms - c->last_chime_ms) < AR_CHIME_MIN_INTERVAL_MS) return false;
    c->has_chimed = true;
    c->last_chime_ms = now_ms;
    return true;
}

uint8_t ar_brightness_percent(uint8_t level) {
    if (level >= AR_BRIGHTNESS_LEVELS) level = AR_BRIGHTNESS_LEVELS - 1;
    return BRIGHTNESS_PERCENT[level];
}

uint8_t ar_brightness_step(uint8_t level, int delta) {
    int next = (int)level + delta;
    if (next < 0) next = 0;
    if (next >= AR_BRIGHTNESS_LEVELS) next = AR_BRIGHTNESS_LEVELS - 1;
    return (uint8_t)next;
}

uint8_t ar_model_backlight_percent(const ar_backlight_in_t *in) {
    const uint8_t normal = ar_brightness_percent(in->level);
    if (in->screen == AR_SCREEN_DISCONNECTED) {
        bool idle = in->ms_disconnected >= AR_BACKLIGHT_IDLE_MS && in->ms_since_key >= AR_BACKLIGHT_IDLE_MS;
        return idle ? 0 : normal;
    }
    if (in->on_usb_power || in->screen == AR_SCREEN_WAITING) return normal;
    if (in->ms_since_key < AR_DIM_AFTER_MS || in->ms_since_change < AR_DIM_AFTER_MS) return normal;
    return normal < AR_DIM_PERCENT ? normal : AR_DIM_PERCENT;   // 只降不升
}
