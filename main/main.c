/*
 * ESP32-S3M-N16R8（正点原子 DNESP32S3M WiFi 版）功能演示 demo
 *
 * 功能清单（各功能由 main/feature_config.h 的宏开关控制）：
 *   1. TF 卡数据读写 + 读写准确性验证（SPI 模式）   —— 当前屏蔽：暂无 TF 卡
 *   2. WiFi 配网（SoftAP ESP32_DEMO + Web 配网 + 断线重连）—— 已实现
 *   3. LED 状态指示（IO1，指示网络状态）            —— 已实现
 *   4. LCD 显示（ST7735S，80x160）                  —— 待实现
 *
 * 主循环以 1s 为周期调度各功能（非阻塞轮询），方便后续功能并行扩展。
 */

#include <stdbool.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "feature_config.h"

#if APP_ENABLE_TF_CARD
#include "tf_card.h"
#endif

#if APP_ENABLE_WIFI
#include "wifi_mgr.h"
#endif

#if APP_ENABLE_LED
#include "led_status.h"
#endif

#if APP_ENABLE_LCD
#include "lcd_st7789.h"
#if !APP_LCD_COLOR_TEST
#include "lcd_ui.h"
#endif
#endif

#if APP_ENABLE_I2C_SCAN
#include "codec_bus.h"
#endif

#if APP_ENABLE_CODEC_REGS
#include "es8388_diag.h"
#endif

#if APP_ENABLE_AUDIO_TEST
#include "audio_i2s.h"
#include "audio_test.h"
#endif

#if APP_ENABLE_RECORDER
/* 录音模块用 AUDIO_SAMPLE_RATE 打印配置，这里也要 include。
 * 不能只在 APP_ENABLE_AUDIO_TEST 分支里 include：两个开关是独立的，
 * 录音功能启用而 beep 测试关闭时就会漏掉。 */
#include "audio_i2s.h"
#include "recorder.h"
#endif

#if APP_ENABLE_BUTTON
#include "button.h"
#endif

static const char *TAG = "APP";

#define MAIN_LOOP_PERIOD_MS     1000  /* 主循环周期 */
#define TFCARD_RETRY_PERIOD_MS 5000  /* TF 卡挂载失败重试间隔 */

#if APP_ENABLE_BUTTON && APP_ENABLE_RECORDER
/* 按键事件 -> 录音/回放。
 * 注意：本回调在按键任务里执行，只能做非阻塞操作。
 * recorder_* 都是"投递命令后立即返回"，符合要求。 */
static void on_button_event(button_event_t evt)
{
    switch (evt) {
    case BUTTON_EVT_LONG_START:   /* 按住满 300ms：开始录音 */
        recorder_record_start();
        break;
    case BUTTON_EVT_LONG_END:     /* 松开：停止录音 */
        recorder_record_stop();
        break;
    case BUTTON_EVT_SHORT_PRESS:  /* 短按：回放 */
        recorder_play_start();
        break;
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ESP32-S3M-N16R8 demo project");
    ESP_LOGI(TAG, " ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, " features: TF_CARD=%d WIFI=%d LED=%d LCD=%d I2C_SCAN=%d "
             "CODEC_REGS=%d AUDIO=%d/BEEP=%d RECORDER=%d BUTTON=%d",
             APP_ENABLE_TF_CARD, APP_ENABLE_WIFI, APP_ENABLE_LED, APP_ENABLE_LCD,
             APP_ENABLE_I2C_SCAN, APP_ENABLE_CODEC_REGS,
             APP_ENABLE_AUDIO_TEST, APP_AUDIO_BEEP,
             APP_ENABLE_RECORDER, APP_ENABLE_BUTTON);
    ESP_LOGI(TAG, "==================================================");

#if APP_ENABLE_WIFI
    /* 功能 2：WiFi 管理（SoftAP 常开 + Web 配网 + 断线重连），非阻塞启动 */
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_LOGI(TAG, " feature 2: WiFi provisioning started (AP: " WIFI_MGR_AP_SSID ")");
#endif

#if APP_ENABLE_LED
    /* 功能 3：LED 状态指示（内部起独立任务，按 wifi_mgr 状态闪烁） */
    ESP_ERROR_CHECK(led_status_init());
#endif

#if APP_ENABLE_LCD
    /* 功能 4：LCD 初始化（内部按 APP_LCD_COLOR_TEST 决定是否跑刷屏测试） */
    ESP_ERROR_CHECK(lcd_init());
#if APP_LCD_COLOR_TEST
    ESP_LOGI(TAG, " feature 4: LCD color sweep test running");
#else
    /* 正常显示：东八区时间 + 当前连接热点名，20fps */
    ESP_ERROR_CHECK(lcd_ui_init());
    ESP_LOGI(TAG, " feature 4: LCD UI running (time + ssid, %d fps)",
             1000 / LCD_UI_REFRESH_MS);
#endif
#endif

#if APP_ENABLE_I2C_SCAN
    /* 功能 5：ES8388 控制通路验证（第 1 步）—— 只扫描，不发声 */
    ESP_ERROR_CHECK(codec_bus_init());
    codec_bus_scan();
#endif

#if APP_ENABLE_CODEC_REGS
    /* 功能 6：寄存器读写验证（第 4 步）—— 开机跑一次，不发声。
     * 故意不用 ESP_ERROR_CHECK：诊断失败只告警，不能让设备陷入重启循环。 */
    if (es8388_diag_run() != ESP_OK) {
        ESP_LOGW(TAG, " codec register diagnostics reported failures (see above)");
    }
#endif

#if APP_ENABLE_AUDIO_TEST
    /* 功能 7：I2S 时钟 + 喇叭验证（第 2/3/5 步）—— 内部起独立任务 */
    ESP_ERROR_CHECK(audio_test_start());
    ESP_LOGI(TAG, " feature 7: audio test task started (%d Hz, beep=%d)",
             AUDIO_SAMPLE_RATE, APP_AUDIO_BEEP);
#endif

#if APP_ENABLE_RECORDER
    /* 功能 8：录音 + 回放。内部任务会异步初始化 I2S 全双工，
     * 不阻塞开机；用户第一次按键时通路已就绪。 */
    ESP_ERROR_CHECK(recorder_init());
    ESP_LOGI(TAG, " feature 8: recorder ready (mono %d Hz, max %d s)",
             AUDIO_SAMPLE_RATE, REC_MAX_SECONDS);
#endif

#if APP_ENABLE_BUTTON && APP_ENABLE_RECORDER
    /* 功能 8：BOOT 按键（长按录音 / 短按回放） */
    ESP_ERROR_CHECK(button_init(on_button_event));
#endif

#if APP_ENABLE_TF_CARD
    bool self_test_done = false;
    int tf_wait_ticks = 0; /* 距下次 TF 卡重试的主循环轮数 */
#endif

    /* I2S 时钟一旦运行，I2C 访问会持续 timeout（实测），
     * 所以只要音频/录音在用，就不要做周期性 I2C 复扫，否则刷满错误日志。 */
#if APP_ENABLE_I2C_SCAN && !APP_ENABLE_AUDIO_TEST && !APP_ENABLE_RECORDER
    int i2c_rescan_ticks = 10; /* 每 10s 复扫一次 */
#endif

    while (true) {
#if APP_ENABLE_TF_CARD
        /* 功能 1：TF 卡挂载 + 自检（非阻塞重试，不阻塞 WiFi 功能） */
        if (!tf_card_is_mounted()) {
            if (tf_wait_ticks > 0) {
                tf_wait_ticks--;
            } else {
                esp_err_t err = tf_card_init();
                if (err == ESP_OK) {
                    tf_card_print_info();
                } else {
                    ESP_LOGW(TAG, "TF card not ready (%s), retry in %d s ...",
                             esp_err_to_name(err), TFCARD_RETRY_PERIOD_MS / 1000);
                    tf_wait_ticks = TFCARD_RETRY_PERIOD_MS / MAIN_LOOP_PERIOD_MS - 1;
                }
            }
        } else if (!self_test_done) {
            if (tf_card_selftest() == ESP_OK) {
                self_test_done = true;
                ESP_LOGI(TAG, "TF card is ready for use at %s", TFCARD_MOUNT_POINT);
            } else {
                ESP_LOGW(TAG, "self test failed, remount and retry in %d s ...",
                         TFCARD_RETRY_PERIOD_MS / 1000);
                tf_card_deinit();
                tf_wait_ticks = TFCARD_RETRY_PERIOD_MS / MAIN_LOOP_PERIOD_MS;
            }
        }
#endif

#if APP_ENABLE_WIFI
        /* 功能 2：外网连通性验证与状态心跳（内部节流，不阻塞） */
        wifi_mgr_poll();
#endif

#if APP_ENABLE_I2C_SCAN && !APP_ENABLE_AUDIO_TEST && !APP_ENABLE_RECORDER
        /* 功能 5：每 10s 复扫一次，方便观察热插拔/接触不良。
         * 音频/录音启用时不做：实测 I2S 时钟运行后 I2C 会持续 timeout，
         * 复扫只会刷满错误日志，且对排查没有帮助。 */
        if (--i2c_rescan_ticks <= 0) {
            codec_bus_scan();
            i2c_rescan_ticks = 10;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS));
    }
}
