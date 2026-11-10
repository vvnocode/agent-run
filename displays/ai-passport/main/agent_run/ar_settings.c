// main/agent_run/ar_settings.c —— 设置持久化实现。
//
// 按上游硬件指南:NVS 初始化失败时不得擦除无关数据来掩盖问题,只记录日志并使用默认值。
#include "ar_settings.h"

#include "ar_model.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ar_settings";
static const char *NAMESPACE = "agent_run";
static bool s_nvs_ok;

static void save_u8(const char *key, uint8_t value) {
    if (!s_nvs_ok) return;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, key, value) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void ar_settings_load(uint8_t *brightness_level, uint8_t *volume_level) {
    *brightness_level = AR_BRIGHTNESS_DEFAULT;
    *volume_level = AR_VOLUME_DEFAULT;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败(%s),设置不会保存", esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;   // 首次运行还没有命名空间
    uint8_t v;
    if (nvs_get_u8(h, "bright", &v) == ESP_OK && v < AR_BRIGHTNESS_LEVELS) *brightness_level = v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v < AR_VOLUME_LEVELS) {
        *volume_level = v;
    } else if (nvs_get_u8(h, "mute", &v) == ESP_OK && v != 0) {
        *volume_level = 0;   // 改版前只有静音开关:沿用静音,之后按音量档位保存
    }
    nvs_close(h);
}

void ar_settings_save_brightness(uint8_t level) {
    save_u8("bright", level);
}

void ar_settings_save_volume(uint8_t level) {
    save_u8("vol", level);
}

void ar_settings_load_screen_off(uint8_t *battery_option, uint8_t *usb_option) {
    *battery_option = AR_BATTERY_OFF_DEFAULT;
    *usb_option = AR_USB_OFF_DEFAULT;
    if (!s_nvs_ok) return;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "bat_off", &v) == ESP_OK && v < AR_BATTERY_OFF_OPTIONS) *battery_option = v;
    if (nvs_get_u8(h, "usb_off", &v) == ESP_OK && v < AR_USB_OFF_OPTIONS) *usb_option = v;
    nvs_close(h);
}

void ar_settings_save_battery_off(uint8_t option) {
    save_u8("bat_off", option);
}

void ar_settings_save_usb_off(uint8_t option) {
    save_u8("usb_off", option);
}

void ar_settings_load_dnd(bool *manual, bool *night) {
    *manual = false;
    *night = false;
    if (!s_nvs_ok) return;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "dnd", &v) == ESP_OK) *manual = v != 0;
    if (nvs_get_u8(h, "night", &v) == ESP_OK) *night = v != 0;
    nvs_close(h);
}

void ar_settings_save_dnd(bool manual) {
    save_u8("dnd", manual ? 1 : 0);
}

void ar_settings_save_night(bool night) {
    save_u8("night", night ? 1 : 0);
}
