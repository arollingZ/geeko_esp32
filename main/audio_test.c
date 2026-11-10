/*
 * audio_test.c —— 喇叭/功放验证任务实现
 */
#include "audio_test.h"

#include "audio_i2s.h"
#include "esp_log.h"
#include "esp_system.h"
#include "feature_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_test";

#define TONE_FREQ_HZ     440
#define TONE_MS          1500
#define SILENCE_MS       1500
/* 振幅调到满量程的 50%（约 -6 dBFS）。之前 8% 太小，功放增益不高时
 * 很容易误判成"没声音"。确认能听到之后再按需调低。 */
#define TONE_AMPLITUDE   0.50f

static void audio_test_task(void *arg)
{
    (void)arg;

    esp_err_t err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

#if APP_AUDIO_BEEP
    ESP_LOGI(TAG, "beep test: %d Hz / %d ms every %d ms (amplitude %.0f%%)",
             TONE_FREQ_HZ, TONE_MS, SILENCE_MS, TONE_AMPLITUDE * 100);
#else
    ESP_LOGI(TAG, "beep disabled: writing silence only, clocks stay active");
#endif

    uint32_t beeps = 0;
    (void)beeps;   /* APP_AUDIO_BEEP=0 时只用于计数日志，会被优化掉 */

    while (true) {
        err = audio_write_silence(SILENCE_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "silence failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
#if APP_AUDIO_BEEP
        err = audio_play_tone(TONE_FREQ_HZ, TONE_MS, TONE_AMPLITUDE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tone failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        beeps++;
        if (beeps == 1 || beeps % 10 == 0) {
            ESP_LOGI(TAG, "beep #%u done (heap free %u)", (unsigned)beeps,
                     (unsigned)esp_get_free_heap_size());
        }
#endif
    }
}

esp_err_t audio_test_start(void)
{
    /* 4KB 栈：里面有 sin() 和 I2S 写，别给太紧 */
    BaseType_t ok = xTaskCreate(audio_test_task, "audio_test", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create audio_test task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
