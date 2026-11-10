// main/agent_run/ar_settings.h —— 亮度、音量、熄屏时间与勿扰设置的 NVS 持久化。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 初始化 NVS 并读出设置;失败时返回默认值且之后的保存静默跳过(不擦除 NVS)。
// 没有音量设置时沿用改版前的静音开关:静音读作 0 档,否则用默认档。
void ar_settings_load(uint8_t *brightness_level, uint8_t *volume_level);

void ar_settings_save_brightness(uint8_t level);
// 音量档位见 ar_model.h 的 AR_VOLUME_LEVELS,0 为静音
void ar_settings_save_volume(uint8_t level);

// 读出电池供电、接电脑两项熄屏时间的选项序号(见 ar_model.h 的 ar_battery_off_ms()、
// ar_usb_off_ms());没有保存过或值越界时用默认选项。须在 ar_settings_load() 之后调用
void ar_settings_load_screen_off(uint8_t *battery_option, uint8_t *usb_option);

void ar_settings_save_battery_off(uint8_t option);
void ar_settings_save_usb_off(uint8_t option);

// 读出勿扰(手动)与夜间勿扰两个开关,没有保存过时都为关闭。须在 ar_settings_load() 之后调用
void ar_settings_load_dnd(bool *manual, bool *night);

void ar_settings_save_dnd(bool manual);
void ar_settings_save_night(bool night);
