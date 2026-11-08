// main/agent_run/ar_sound.h —— “等你处理”提示音。
#pragma once

#include "esp_err.h"

// 预先合成提示音 PCM 并启动播放任务。需在 bsp_audio_init() 成功后调用。
esp_err_t ar_sound_init(void);

// 请求播放一次提示音;立即返回,播放在后台任务中进行。未初始化时忽略。
void ar_sound_chime(void);
