/*
 * LCD 显示界面 —— 两行：东八区时间 + 当前连接热点名，20fps 刷新。
 * 设计说明见 lcd_ui.h。
 */

#include "lcd_ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "feature_config.h"

#if APP_ENABLE_LCD

#include "esp_netif_sntp.h"
#include "font8x16.h"
#include "lcd_st7789.h"
#if APP_ENABLE_WIFI
#include "wifi_mgr.h"
#endif

static const char *TAG = "LCD_UI";

/* ---------------- 布局与配色 ---------------- */
#define UI_BG_COLOR         LCD_COLOR_BLACK
#define UI_TIME_COLOR       LCD_COLOR_WHITE
#define UI_SSID_COLOR       LCD_COLOR_CYAN

#define UI_LINE_H           FONT8X16_H          /* 16px */
#define UI_TIME_Y           18                  /* 第一行文字顶部 */
#define UI_SSID_Y           46                  /* 第二行文字顶部 */
#define UI_COLS             (LCD_H_RES / FONT8X16_W)   /* 每行字符数 = 20 */

#define UI_TASK_STACK       3072
#define UI_TASK_PRIO        4

/* 对时服务器：国内可达性较好；可按需替换 */
#define UI_NTP_SERVER       "ntp.aliyun.com"
#define UI_TZ               "CST-8"             /* 东八区（UTC+8） */

/* 整屏帧缓冲（DMA 可访问），绘制完一次性刷到屏幕 */
static uint16_t *s_fb = NULL;

/* ==================== 绘制 ==================== */

static inline void fb_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x < LCD_H_RES && (unsigned)y < LCD_V_RES) {
        s_fb[y * LCD_H_RES + x] = color;
    }
}

static void fb_fill(uint16_t color)
{
    size_t n = (size_t)LCD_H_RES * LCD_V_RES;
    for (size_t i = 0; i < n; i++) {
        s_fb[i] = color;
    }
}

/* 在 (x,y) 处绘制一个 8x16 字符，fg 为笔画色，bg 为底色 */
static void draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg)
{
    unsigned idx = (unsigned char)ch;
    if (idx < FONT8X16_FIRST || idx > FONT8X16_LAST) {
        idx = '?';
    }
    const uint8_t *rows = font8x16_data[idx - FONT8X16_FIRST];
    for (int r = 0; r < FONT8X16_H; r++) {
        uint8_t bits = rows[r];
        for (int c = 0; c < FONT8X16_W; c++) {
            fb_pixel(x + c, y + r, (bits & (0x80 >> c)) ? fg : bg);
        }
    }
}

/* 绘制字符串，水平居中；max_cols 为可用列数（超长截断） */
static void draw_text_centered(int y, const char *s, uint16_t fg, uint16_t bg)
{
    size_t len = strlen(s);
    if (len > UI_COLS) {
        len = UI_COLS;
    }
    int width = (int)len * FONT8X16_W;
    int x = (LCD_H_RES - width) / 2;
    for (size_t i = 0; i < len; i++) {
        draw_char(x + (int)i * FONT8X16_W, y, s[i], fg, bg);
    }
}

/* ==================== 时间与状态 ==================== */

/* SNTP 是否已同步：年份 < 2020 视为未同步（芯片默认时间从 1970 起） */
static bool time_is_valid(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return (tm_now.tm_year + 1900) >= 2020;
}

/* 第一行内容："2026-11-04 16:13:26"，未对时时给出提示 */
static void build_time_line(char *out, size_t out_sz)
{
    if (!time_is_valid()) {
        snprintf(out, out_sz, "NTP syncing...");   /* 不超过 20 列 */
        return;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(out, out_sz, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
}

/* 第二行内容：当前连接的热点名。超过屏幕列数时截断并加 ".."，
 * 让用户知道名字被省略了，而不是显示成一串残缺字符。 */
static void build_ssid_line(char *out, size_t out_sz)
{
#if APP_ENABLE_WIFI
    const char *ssid = wifi_mgr_get_ssid();
    if (ssid != NULL && ssid[0] != '\0') {
        size_t len = strlen(ssid);
        if (len <= UI_COLS) {
            snprintf(out, out_sz, "%s", ssid);
        } else {
            /* 留 2 列给 ".." */
            snprintf(out, out_sz, "%.*s..", UI_COLS - 2, ssid);
        }
        return;
    }
#endif
    snprintf(out, out_sz, "no wifi config");
}

/* ==================== 刷新任务 ==================== */

static void ui_render_once(void)
{
    char line1[32];
    char line2[40];

    build_time_line(line1, sizeof(line1));
    build_ssid_line(line2, sizeof(line2));

    fb_fill(UI_BG_COLOR);
    draw_text_centered(UI_TIME_Y, line1, UI_TIME_COLOR, UI_BG_COLOR);
    draw_text_centered(UI_SSID_Y, line2, UI_SSID_COLOR, UI_BG_COLOR);

    /* 整帧一次 DMA 送到屏幕，20fps 下写入窗口约 3.4ms */
    esp_err_t err = lcd_flush(0, 0, LCD_H_RES, LCD_V_RES, s_fb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t unsynced_frames = 0;
    bool synced_logged = false;
    const uint32_t frames_per_1p5s = 1500 / LCD_UI_REFRESH_MS;   /* 30 */
    const uint32_t frames_10min = 600000 / LCD_UI_REFRESH_MS;    /* 12000 */

    ESP_LOGI(TAG, "UI task started, %d ms per frame (%d fps)",
             LCD_UI_REFRESH_MS, 1000 / LCD_UI_REFRESH_MS);

    while (true) {
        ui_render_once();

        if (time_is_valid()) {
            if (!synced_logged) {
                synced_logged = true;
                char line[32];
                build_time_line(line, sizeof(line));
                ESP_LOGI(TAG, "NTP synced (CST-8): %s", line);
            }
        } else {
            /* 上电时 WiFi 往往还没连上，首次同步必然落空；这里周期性重新
             * 发起同步，保证网络就绪后能对上时间（只在前 10 分钟重试）。 */
            unsynced_frames++;
            if (unsynced_frames % frames_per_1p5s == 0) {
                if (unsynced_frames == frames_per_1p5s) {
                    ESP_LOGI(TAG, "waiting for NTP sync (no internet yet?)");
                }
                if (unsynced_frames <= frames_10min) {
                    esp_netif_sntp_start();   /* 已在运行则返回错误，忽略 */
                }
            }
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(LCD_UI_REFRESH_MS));
    }
}

/* ==================== 初始化 ==================== */

/* 启动 SNTP：东八区 + 阿里云 NTP，失败不影响显示（时间栏显示等待中） */
static void start_sntp(void)
{
    setenv("TZ", UI_TZ, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(UI_NTP_SERVER);
    cfg.start = true;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started, server=%s, timezone=%s", UI_NTP_SERVER, UI_TZ);
}

esp_err_t lcd_ui_init(void)
{
    if (s_fb == NULL) {
        s_fb = heap_caps_malloc((size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_fb == NULL) {
            ESP_LOGE(TAG, "framebuffer alloc failed (%u bytes)",
                     (unsigned)(LCD_H_RES * LCD_V_RES * sizeof(uint16_t)));
            return ESP_ERR_NO_MEM;
        }
    }

    start_sntp();

    if (xTaskCreate(ui_task, "lcd_ui", UI_TASK_STACK, NULL, UI_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "UI task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LCD UI: time (CST-8) + connected SSID, %dx%d @ %d fps",
             LCD_H_RES, LCD_V_RES, 1000 / LCD_UI_REFRESH_MS);
    return ESP_OK;
}

#else /* !APP_ENABLE_LCD */

esp_err_t lcd_ui_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* APP_ENABLE_LCD */
