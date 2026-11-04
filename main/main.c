/*
 * ESP32-S3M-N16R8（正点原子 DNESP32S3M WiFi 版）功能演示 demo
 *
 * 功能清单（各功能由 main/feature_config.h 的宏开关控制）：
 *   1. TF 卡数据读写 + 读写准确性验证（SPI 模式）   —— 当前屏蔽：暂无 TF 卡
 *   2. WiFi 配网（SoftAP ESP32_DEMO + Web 配网 + 断线重连）—— 已实现
 *   3. LED 指示灯（IO1，指示网络状态）              —— 待实现
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

static const char *TAG = "APP";

#define MAIN_LOOP_PERIOD_MS     1000  /* 主循环周期 */
#define TFCARD_RETRY_PERIOD_MS 5000  /* TF 卡挂载失败重试间隔 */

void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ESP32-S3M-N16R8 demo project");
    ESP_LOGI(TAG, " ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, " features: TF_CARD=%d WIFI=%d", APP_ENABLE_TF_CARD, APP_ENABLE_WIFI);
    ESP_LOGI(TAG, "==================================================");

#if APP_ENABLE_WIFI
    /* 功能 2：WiFi 管理（SoftAP 常开 + Web 配网 + 断线重连），非阻塞启动 */
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_LOGI(TAG, " feature 2: WiFi provisioning started (AP: " WIFI_MGR_AP_SSID ")");
#endif

#if APP_ENABLE_TF_CARD
    bool self_test_done = false;
    int tf_wait_ticks = 0; /* 距下次 TF 卡重试的主循环轮数 */
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

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS));
    }
}
