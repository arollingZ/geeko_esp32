/*
 * LCD 显示界面 —— 两行文字：东八区时间 + 当前连接的热点名称
 *
 * 刷新率：50ms/次（20fps），由独立任务驱动。
 * 内容：
 *   第一行  2026-11-04 16:13:26   （8x16 点阵，19 字符 = 152px，居中）
 *   第二行  热点名称              （超长自动截断加 ".."）
 *
 * 时间来源：SNTP（上电自动同步，时区固定东八区 CST-8）。
 * 未同步前第一行显示 "-- 未对时 --"，第二行照常显示 SSID。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 界面刷新周期（20fps） */
#define LCD_UI_REFRESH_MS   50

/**
 * @brief 启动 LCD 界面任务（需先调用 lcd_init()）
 *
 * 内部会启动 SNTP 对时（东八区）并创建 ui 任务，50ms 刷新一次屏幕。
 * 功能被 APP_ENABLE_LCD=0 屏蔽时返回 ESP_ERR_NOT_SUPPORTED。
 *
 * @return ESP_OK 成功
 */
esp_err_t lcd_ui_init(void);

#ifdef __cplusplus
}
#endif
