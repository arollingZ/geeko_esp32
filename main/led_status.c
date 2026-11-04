/*
 * LED 状态指示模块 —— 实现见 led_status.h 头部注释。
 */

#include "led_status.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "feature_config.h"

#if APP_ENABLE_LED
#include "driver/gpio.h"

#if APP_ENABLE_WIFI
#include "wifi_mgr.h"
#endif

static const char *TAG = "LED";

/* ---------------- 硬件配置 ---------------- */
#define LED_GPIO_NUM        1        /* readme.txt：LED pin 脚 IO1 */
#define LED_ACTIVE_LEVEL    0        /* 低电平点亮（本板灌电流驱动，2026-11-04 实测确认）；若换高电平驱动板改为 1 */
#define LED_TASK_STACK      2048
#define LED_TASK_PRIO       3

static void led_set(bool on)
{
    gpio_set_level(LED_GPIO_NUM, on ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);
}

static esp_err_t led_gpio_init(void)
{
    /* 先熄灭，避免上电到任务起来之间 LED 误亮 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO_NUM,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config failed");
    led_set(false);
    return ESP_OK;
}

#if APP_ENABLE_WIFI

/* 闪烁序列：以「亮」起头的交替时长表（ms），0 结尾。
 * 状态切换时从序列头重新播放。 */
typedef struct {
    const uint32_t *steps;   /* 亮/灭交替时长（亮起头），0 结尾 */
    bool solid;              /* true=恒定电平（不闪烁） */
    bool solid_level;        /* solid 时的电平 */
    const char *name;        /* 日志用 */
} led_pattern_t;

/* 未配网：熄灭 */
static const uint32_t pat_off[] = { 0 };
/* 快闪 2 下再灭 1s：亮100 灭100 亮100 灭1000 */
static const uint32_t pat_double[] = { 100, 100, 100, 1000, 0 };
/* 慢闪：亮500 灭500 */
static const uint32_t pat_slow[] = { 500, 500, 0 };

static led_pattern_t pattern_for(wifi_mgr_state_t st)
{
    switch (st) {
    case WIFI_MGR_STATE_NO_CRED:     /* a. 未配网：熄灭 */
        return (led_pattern_t){ pat_off, true, 0, "off (no cred)" };
    case WIFI_MGR_STATE_CONNECTING:   /* b. 有凭据未连上：快闪 2 下 */
        return (led_pattern_t){ pat_double, false, 0, "double blink (connecting)" };
    case WIFI_MGR_STATE_CONNECTED:    /* c. 已连热点未通外网：慢闪 */
        return (led_pattern_t){ pat_slow, false, 0, "slow blink (no internet)" };
    case WIFI_MGR_STATE_INTERNET_OK:  /* d. 网络通了：常亮 */
    default:
        return (led_pattern_t){ pat_off, true, 1, "solid on (internet ok)" };
    }
}

static void led_task(void *arg)
{
    (void)arg;
    wifi_mgr_state_t prev_state = WIFI_MGR_STATE_NO_CRED;
    led_pattern_t pat = pattern_for(prev_state);
    size_t idx = 0;
    bool output_on = false;

    ESP_LOGI(TAG, "state=%s", pat.name);

    while (true) {
        /* 状态变化：重选序列并从头播放，打一条日志方便对照验收现象 */
        wifi_mgr_state_t cur = wifi_mgr_get_state();
        if (cur != prev_state) {
            prev_state = cur;
            pat = pattern_for(cur);
            idx = 0;
            ESP_LOGI(TAG, "state=%s", pat.name);
        }

        if (pat.solid) {
            if (output_on != pat.solid_level) {
                output_on = pat.solid_level;
                led_set(output_on);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* 序列第 idx 步：偶数下标为亮，奇数下标为灭；0 表示循环播放 */
        if (pat.steps[idx] == 0) {
            idx = 0;
        }
        output_on = (idx % 2 == 0);
        led_set(output_on);
        vTaskDelay(pdMS_TO_TICKS(pat.steps[idx]));
        idx++;
    }
}

esp_err_t led_status_init(void)
{
    ESP_RETURN_ON_ERROR(led_gpio_init(), TAG, "gpio init failed");

    if (xTaskCreate(led_task, "led_status", LED_TASK_STACK, NULL,
                    LED_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "LED task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LED status indicator on GPIO %d (active %s)",
             LED_GPIO_NUM, LED_ACTIVE_LEVEL ? "high" : "low");
    return ESP_OK;
}

#else /* LED=1 但 WIFI=0：没有网络状态可指示，仅配置 GPIO 并保持熄灭 */

esp_err_t led_status_init(void)
{
    esp_err_t ret = led_gpio_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LED on GPIO %d initialized and kept off (wifi disabled, nothing to indicate)",
                 LED_GPIO_NUM);
    }
    return ESP_OK;
}

#endif /* APP_ENABLE_WIFI */

#else /* !APP_ENABLE_LED */

/* 功能屏蔽时不做任何事，保持接口不变 */

esp_err_t led_status_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* APP_ENABLE_LED */
