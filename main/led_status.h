/*
 * LED 状态指示模块 —— 按 WiFi 网络状态驱动 LED（readme.txt 功能点 3）
 *
 * 引脚：IO1（见 LED_GPIO_NUM 宏，如硬件改动改这里）。
 * 亮灭规则（与 wifi_mgr 状态机一一对应）：
 *   NO_CRED      -> 熄灭（未配网）
 *   CONNECTING   -> 快闪 2 下（亮100ms/灭100ms/亮100ms）后熄灭 1s，循环
 *   CONNECTED    -> 慢闪（亮500ms/灭500ms）
 *   INTERNET_OK  -> 常亮
 *
 * 实现方式：独立的低优先级 FreeRTOS 任务按状态查表播放闪烁序列，
 * 状态切换时序列从头开始。LED 亮灭电平由 LED_ACTIVE_LEVEL 配置。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LED 状态指示（配置 GPIO 并启动指示任务，非阻塞）
 *
 * 功能被 APP_ENABLE_LED=0 屏蔽时不做任何事；APP_ENABLE_WIFI=0 时
 * 仅配置 GPIO 并保持熄灭（没有网络状态可指示）。
 *
 * @return ESP_OK 成功
 */
esp_err_t led_status_init(void);

#ifdef __cplusplus
}
#endif
