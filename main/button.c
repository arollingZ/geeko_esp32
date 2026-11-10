/*
 * button.c —— BOOT 按键识别实现
 *
 * 用轮询而不是 GPIO 中断，原因是：
 *   - 只需区分 300ms 这种粗粒度时长，10ms 轮询精度绰绰有余
 *   - 中断方式要处理抖动、边沿抖动、长按计时，代码更复杂且更容易出错
 *   - 一个 10ms 周期的低优先级任务几乎不占 CPU
 */
#include "button.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "feature_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

/* BOOT 按键：IO0，按下接地（低有效） */
#define BTN_GPIO        GPIO_NUM_0
#define BTN_ACTIVE_LOW  1

#define BTN_POLL_MS     10
#define BTN_TASK_STACK  2560
#define BTN_TASK_PRIO   3

static button_event_cb_t s_cb = NULL;
static bool s_inited = false;

static inline bool btn_is_pressed(void)
{
#if BTN_ACTIVE_LOW
    return gpio_get_level(BTN_GPIO) == 0;
#else
    return gpio_get_level(BTN_GPIO) != 0;
#endif
}

static void button_task(void *arg)
{
    (void)arg;
    bool prev_pressed = false;
    bool long_fired   = false;
    TickType_t press_tick = 0;

    /* 上电时如果按键已经按着（比如刚烧完固件手还没松），
     * 先等它松开再开始，避免误触发一次录音。 */
    while (btn_is_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
    prev_pressed = false;

    while (true) {
        bool pressed = btn_is_pressed();

        if (pressed && !prev_pressed) {
            /* 按下沿 */
            press_tick = xTaskGetTickCount();
            long_fired = false;
        } else if (pressed && !long_fired) {
            /* 按住中：到阈值立刻上报，让录音尽早开始 */
            uint32_t held = (uint32_t)((xTaskGetTickCount() - press_tick) *
                                       portTICK_PERIOD_MS);
            if (held >= BTN_LONG_PRESS_MS) {
                long_fired = true;
                ESP_LOGI(TAG, "long press (%u ms) -> start", (unsigned)held);
                if (s_cb != NULL) {
                    s_cb(BUTTON_EVT_LONG_START);
                }
            }
        } else if (!pressed && prev_pressed) {
            /* 松开沿 */
            uint32_t held = (uint32_t)((xTaskGetTickCount() - press_tick) *
                                       portTICK_PERIOD_MS);
            if (long_fired) {
                ESP_LOGI(TAG, "released after %u ms -> stop", (unsigned)held);
                if (s_cb != NULL) {
                    s_cb(BUTTON_EVT_LONG_END);
                }
            } else {
                ESP_LOGI(TAG, "short press (%u ms) -> playback", (unsigned)held);
                if (s_cb != NULL) {
                    s_cb(BUTTON_EVT_SHORT_PRESS);
                }
            }
        }

        prev_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

esp_err_t button_init(button_event_cb_t cb)
{
    if (s_inited) {
        s_cb = cb;
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 板上有外部上拉，这里兜底 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio config failed");

    s_cb = cb;
    if (xTaskCreate(button_task, "button", BTN_TASK_STACK, NULL,
                    BTN_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "BOOT button on IO%d: long >= %d ms = record, "
             "short = playback (now level=%d)", BTN_GPIO, BTN_LONG_PRESS_MS,
             gpio_get_level(BTN_GPIO));
    return ESP_OK;
}
