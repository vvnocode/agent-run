// main/agent_run/ar_sound.c —— 提示音:“等你处理”与“完成”两种短音,播放时现场合成,不占存储。
//
// PCM 读写会阻塞,必须放在工作任务里(上游硬件指南的运行时规则);
// 调用方只通过任务通知传入“种类 + 音量”,播放任务按需合成到同一块缓冲再写出。
#include "ar_sound.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ar_sound";

#define SAMPLE_RATE 16000
#define RAMP_MS 5              // 每个音起止淡入淡出,避免爆音
#define BUFFER_MS 180          // 缓冲长度,按最长的一段提示音(等你处理)计
#define SOUND_TASK_STACK 3072
#define SOUND_TASK_PRIO 3

#define SAMPLES(ms) ((SAMPLE_RATE * (ms)) / 1000)

// 一个音符;hz 为 0 表示静音间隔
typedef struct {
    float hz;
    uint16_t ms;
} note_t;

// 等你处理:A5 → E6 两声,较响
static const note_t WAITING_NOTES[] = { { 880.0f, 70 }, { 0.0f, 20 }, { 1320.0f, 90 } };
#define WAITING_AMPLITUDE 0.35f

// 完成:E6 → G6 → C7 快速上行三声,比等你处理轻
static const note_t DONE_NOTES[] = {
    { 1319.0f, 40 }, { 0.0f, 10 }, { 1568.0f, 40 }, { 0.0f, 10 }, { 2093.0f, 70 },
};
#define DONE_AMPLITUDE 0.25f

static int16_t s_pcm[SAMPLES(BUFFER_MS)];
static TaskHandle_t s_task;

// 在 s_pcm[start..start+count) 写一段带淡入淡出的正弦波
static void synth_tone(int start, int count, float hz, float amplitude) {
    const int ramp = SAMPLES(RAMP_MS);
    for (int i = 0; i < count; i++) {
        float env = 1.0f;
        if (i < ramp) env = (float)i / (float)ramp;
        else if (i > count - ramp) env = (float)(count - i) / (float)ramp;
        float v = sinf(2.0f * (float)M_PI * hz * (float)i / (float)SAMPLE_RATE);
        s_pcm[start + i] = (int16_t)(v * env * amplitude * 32767.0f);
    }
}

// 把一串音符合成到 s_pcm,返回样本数;超出缓冲的部分截掉
static int synth_notes(const note_t *notes, size_t n, float amplitude) {
    const int capacity = (int)(sizeof(s_pcm) / sizeof(s_pcm[0]));
    int pos = 0;
    memset(s_pcm, 0, sizeof(s_pcm));   // 静音间隔保持为 0
    for (size_t i = 0; i < n && pos < capacity; i++) {
        int count = SAMPLES(notes[i].ms);
        if (pos + count > capacity) count = capacity - pos;
        if (notes[i].hz > 0.0f) synth_tone(pos, count, notes[i].hz, amplitude);
        pos += count;
    }
    return pos;
}

static void sound_task(void *arg) {
    (void)arg;
    for (;;) {
        // 通知值:低 8 位为音量百分比,其上为提示音种类
        uint32_t value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &value, portMAX_DELAY);
        const ar_chime_kind_t kind = (ar_chime_kind_t)(value >> 8);
        const uint8_t volume = (uint8_t)(value & 0xFFu);
        if (kind == AR_CHIME_NONE || volume == 0) continue;

        const int samples = kind == AR_CHIME_DONE
                                ? synth_notes(DONE_NOTES, sizeof(DONE_NOTES) / sizeof(DONE_NOTES[0]), DONE_AMPLITUDE)
                                : synth_notes(WAITING_NOTES, sizeof(WAITING_NOTES) / sizeof(WAITING_NOTES[0]),
                                              WAITING_AMPLITUDE);
        esp_err_t err = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
        if (err == ESP_OK) {
            bsp_audio_set_volume(volume);
            err = bsp_audio_write(s_pcm, (size_t)samples * sizeof(s_pcm[0]));
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "提示音播放失败: %s", esp_err_to_name(err));
    }
}

esp_err_t ar_sound_init(void) {
    if (xTaskCreate(sound_task, "ar_sound", SOUND_TASK_STACK, NULL, SOUND_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ar_sound_play(ar_chime_kind_t kind, uint8_t volume_percent) {
    if (!s_task || kind == AR_CHIME_NONE || volume_percent == 0) return;
    // 覆盖写入:上一声还没开始播时只播最新的一声
    xTaskNotify(s_task, ((uint32_t)kind << 8) | volume_percent, eSetValueWithOverwrite);
}
