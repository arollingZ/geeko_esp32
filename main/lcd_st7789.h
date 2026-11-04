/*
 * LCD 模块 —— 0.96 寸 80x160 ST7789 屏（SPI 接口）
 *
 * 硬件（正点原子 DNESP32S3M，见 readme.txt 功能 4）：
 *   CS=IO39  DC=IO40  RST=IO38  BL=IO41  数据线 SCLK=IO12 / MOSI=IO11
 *
 * 说明：
 *  - 驱动直接用 IDF v6.1 内置的 esp_lcd_panel_st7789，不再手写初始化序列；
 *  - 数据线与 TF 卡（功能 1）共用 IO11/12/13，靠各自 CS 分时复用同一条 SPI2 总线。
 *    总线上电谁先初始化都行，后初始化的一方会收到 ESP_ERR_INVALID_STATE，按复用处理；
 *  - 颜色格式 RGB565（16bpp），用 LCD_RGB565() 宏构造，传的是 MCU 原生 uint16_t。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 逻辑分辨率（横屏 160x80，见 LCD_SWAP_XY） */
#define LCD_H_RES   160
#define LCD_V_RES   80

/** RGB565 颜色构造：r/g/b 取 0~255 */
#define LCD_RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define LCD_COLOR_BLACK   0x0000
#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_YELLOW  0xFFE0
#define LCD_COLOR_CYAN    0x07FF
#define LCD_COLOR_MAGENTA 0xF81F

/**
 * @brief 初始化 LCD（SPI 总线 + panel IO + ST7789 + 背光）并启动刷屏任务
 *
 * 与 wifi_mgr_init() 一样是非阻塞的：初始化完成后内部创建 lcd_sweep 任务，
 * 每 3 秒整屏填充一种颜色（红/绿/蓝/白/黑/黄/青/品红循环），
 * 用于验证 LCD 通路持续稳定。后续接入状态界面时替换该任务的刷新逻辑即可。
 *
 * 功能被 APP_ENABLE_LCD=0 屏蔽时直接返回 ESP_ERR_NOT_SUPPORTED。
 *
 * @return ESP_OK 成功
 */
esp_err_t lcd_init(void);

/** 背光开关 */
void lcd_backlight(bool on);

/** 整屏填充单一颜色 */
esp_err_t lcd_fill_color(uint16_t color);

/** 填充矩形区域（x/y 为左上角，w/h 为尺寸，超界自动裁剪） */
esp_err_t lcd_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * @brief 一次性纯色自检：整屏刷 白/红/绿/蓝/黑，最后画一个白色边框
 *
 * 与刷屏任务区分：本函数用于上板联调时人工诊断（引脚、RGB/BGR 顺序、
 * 反色、偏移、方向），会占用约 2s，正常启动流程不调用。
 *  - 若「红」显示成蓝色 → 红蓝通道颠倒，改 LCD_RGB_ORDER 为 RGB；
 *  - 若整屏反色（黑变白）→ 改 LCD_INVERT_COLOR；
 *  - 若边框缺一边/不在屏幕边缘 → 调整 LCD_OFFSET_X/Y 或 MIRROR/SWAP。
 */
esp_err_t lcd_color_test(void);

#ifdef __cplusplus
}
#endif
