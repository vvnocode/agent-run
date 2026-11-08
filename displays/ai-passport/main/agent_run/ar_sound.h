// main/agent_run/ar_sound.h —— “等你处理”与“完成”提示音。
#pragma once

#include <stdint.h>

#include "ar_model.h"
#include "esp_err.h"

// 启动播放任务。需在 bsp_audio_init() 成功后调用。
esp_err_t ar_sound_init(void);

// 请求以 volume_percent(1-100)播放一次 kind 提示音;立即返回,播放在后台任务中进行。
// 未初始化、AR_CHIME_NONE 或音量为 0 时忽略。
void ar_sound_play(ar_chime_kind_t kind, uint8_t volume_percent);
