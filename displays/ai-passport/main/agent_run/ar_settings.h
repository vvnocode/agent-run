// main/agent_run/ar_settings.h —— 亮度与静音设置的 NVS 持久化。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 初始化 NVS 并读出设置;失败时返回默认值且之后的保存静默跳过(不擦除 NVS)。
void ar_settings_load(uint8_t *brightness_level, bool *muted);

void ar_settings_save_brightness(uint8_t level);
void ar_settings_save_muted(bool muted);
