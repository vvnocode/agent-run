// main/agent_run/ar_settings.h —— 亮度与音量设置的 NVS 持久化。
#pragma once

#include <stdint.h>

// 初始化 NVS 并读出设置;失败时返回默认值且之后的保存静默跳过(不擦除 NVS)。
// 没有音量设置时沿用改版前的静音开关:静音读作 0 档,否则用默认档。
void ar_settings_load(uint8_t *brightness_level, uint8_t *volume_level);

void ar_settings_save_brightness(uint8_t level);
// 音量档位见 ar_model.h 的 AR_VOLUME_LEVELS,0 为静音
void ar_settings_save_volume(uint8_t level);
