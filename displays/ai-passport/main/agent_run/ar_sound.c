// main/agent_run/ar_sound.c —— 提示音:880 Hz 与 1320 Hz 两个短音,开机时合成一次,不占存储。
//
// PCM 读写会阻塞,必须放在工作任务里(上游硬件指南的运行时规则);
// 调用方只通过任务通知触发播放。
#include "ar_sound.h"

#include <math.h>
#include <stdint.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ar_sound";

#define SAMPLE_RATE 16000
#define TONE1_HZ 880.0f
#define TONE2_HZ 1320.0f
#define TONE1_MS 70
#define GAP_MS 20
#define TONE2_MS 90
#define RAMP_MS 5              // 起止淡入淡出,避免爆音
#define AMPLITUDE 0.35f
#define VOLUME_PERCENT 60
#define SOUND_TASK_STACK 3072
#define SOUND_TASK_PRIO 3

#define SAMPLES(ms) ((SAMPLE_RATE * (ms)) / 1000)
#define TOTAL_SAMPLES (SAMPLES(TONE1_MS) + SAMPLES(GAP_MS) + SAMPLES(TONE2_MS))

static int16_t s_pcm[TOTAL_SAMPLES];
static TaskHandle_t s_task;

// 在 s_pcm[start..start+count) 写一段带淡入淡出的正弦波
static void synth_tone(int start, int count, float hz) {
    const int ramp = SAMPLES(RAMP_MS);
    for (int i = 0; i < count; i++) {
        float env = 1.0f;
        if (i < ramp) env = (float)i / (float)ramp;
        else if (i > count - ramp) env = (float)(count - i) / (float)ramp;
        float v = sinf(2.0f * (float)M_PI * hz * (float)i / (float)SAMPLE_RATE);
        s_pcm[start + i] = (int16_t)(v * env * AMPLITUDE * 32767.0f);
    }
}

static void sound_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_err_t err = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
        if (err == ESP_OK) {
            bsp_audio_set_volume(VOLUME_PERCENT);
            err = bsp_audio_write(s_pcm, sizeof(s_pcm));
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "提示音播放失败: %s", esp_err_to_name(err));
    }
}

esp_err_t ar_sound_init(void) {
    synth_tone(0, SAMPLES(TONE1_MS), TONE1_HZ);
    // 中间的静音段在静态数组里已是 0
    synth_tone(SAMPLES(TONE1_MS) + SAMPLES(GAP_MS), SAMPLES(TONE2_MS), TONE2_HZ);
    if (xTaskCreate(sound_task, "ar_sound", SOUND_TASK_STACK, NULL, SOUND_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ar_sound_chime(void) {
    if (s_task) xTaskNotifyGive(s_task);
}
