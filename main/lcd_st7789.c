/*
 * LCD 模块 —— 0.96 寸 80x160 ST7789 屏（SPI 接口），实现说明见 lcd_st7789.h。
 */

#include "lcd_st7789.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "feature_config.h"

#if APP_ENABLE_LCD

#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "LCD";

/* ---------------- 硬件配置（readme.txt 功能 4） ---------------- */
#define LCD_SPI_HOST        SPI2_HOST      /* 与 TF 卡共用 SPI2（IO11/12/13） */
#define LCD_PIN_CS          39
#define LCD_PIN_DC          40
#define LCD_PIN_RST         38
#define LCD_PIN_BL          41
#define LCD_PIN_SCLK        12
#define LCD_PIN_MOSI        11
#define LCD_PIN_MISO        13            /* LCD 不用，但 TF 卡要；一并纳入总线配置 */

/* ---------------- 面板参数（与正点原子该板的参考配置一致） ----------------
 * 若现象不符，按下面三个宏微调，改完重新编译即可：
 *   LCD_RGB_ORDER      红蓝颠倒 -> LCD_RGB_ELEMENT_ORDER_RGB
 *   LCD_INVERT_COLOR   黑变白/白变黑 -> true/false 互换
 *   LCD_OFFSET_X/Y     边框偏移 -> 按实际可见区域调整
 */
#define LCD_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_BGR
#define LCD_INVERT_COLOR    true
#define LCD_SWAP_XY         true            /* 横屏显示 160x80 */
#define LCD_MIRROR_X        false
#define LCD_MIRROR_Y        true
#define LCD_OFFSET_X        1
#define LCD_OFFSET_Y        26
#define LCD_PIXEL_CLOCK_HZ  (60 * 1000 * 1000)  /* 60MHz：缩短写帧窗口，对抗撕裂；不稳可降到 40MHz */
#define LCD_BL_ACTIVE_LEVEL 1               /* 背光高电平点亮 */

/* ---------------- 抗撕裂（tearing）说明 ----------------
 * ST7789 没有双缓冲，屏幕以约 60Hz 持续从显存扫描输出，而我们一边往里写。
 * 写入追上扫描线时，屏幕上就会出现「上半屏新色 / 下半屏旧色」的分界线
 * —— 这就是俗称的屏幕撕裂（写入与扫描在赛跑）。
 *
 * 缓解手段（按效果）：
 *   1. 整帧一次传完，而不是分多块：块与块之间的命令/等待间隙正是撕裂最明显的地方。
 *      整帧 160*80*2 = 25600 字节，@60MHz 约 3.4ms，远小于一个 16.7ms 的刷新周期。
 *   2. 提高 SPI 时钟（LCD_PIXEL_CLOCK_HZ），线性缩短写入窗口。
 *   3. 彻底消除：写帧前后关/开显示（LCD_FILL_DISPOFF_AROUND=1），但每帧会闪一下，
 *      适合纯色测试，不适合做 UI。
 * 更彻底的做法是接 TE 信号做垂直同步，但本板 TE 未引出，只能用上面这些软件手段。
 */
#define LCD_FILL_DISPOFF_AROUND  0   /* 1 = 写帧前后关/开显示（零撕裂但闪烁） */

/* 填充缓冲：整屏 25600 字节，一次分配反复复用（不再每帧 malloc） */
#define LCD_FILL_BUF_PIXELS  (LCD_H_RES * LCD_V_RES)

/* 周期性刷屏任务参数 */
#define LCD_TASK_STACK      3072
#define LCD_TASK_PRIO       3
#define LCD_SWEEP_PERIOD_MS 3000   /* 每 3 秒换一次颜色 */

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_inited = false;
static TaskHandle_t s_wait_task = NULL;
static uint16_t *s_fill_buf = NULL;

/* draw_bitmap 是异步的（队列深度 4），填色复用一个缓冲区，
 * 必须等上一笔传输完成才能改写数据，否则会出现花屏。 */
static bool IRAM_ATTR lcd_color_done_cb(esp_lcd_panel_io_handle_t io,
                                        esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    if (s_wait_task != NULL) {
        vTaskNotifyGiveFromISR(s_wait_task, &woken);
    }
    return woken == pdTRUE;
}

static esp_err_t lcd_wait_color_done(void)
{
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
        ESP_LOGE(TAG, "wait color transfer done timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 裁剪到屏幕范围内 */
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_H_RES) {
        w = LCD_H_RES - x;
    }
    if (y + h > LCD_V_RES) {
        h = LCD_V_RES - y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    /* 整帧一次传完：缓冲区一次性分配好，之后反复复用 */
    if (s_fill_buf == NULL) {
        s_fill_buf = heap_caps_malloc((size_t)LCD_FILL_BUF_PIXELS * sizeof(uint16_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_fill_buf == NULL) {
            ESP_LOGE(TAG, "fill buffer alloc failed (%u bytes)",
                     (unsigned)(LCD_FILL_BUF_PIXELS * sizeof(uint16_t)));
            return ESP_ERR_NO_MEM;
        }
    }

    size_t pixels = (size_t)w * h;
    for (size_t i = 0; i < pixels; i++) {
        s_fill_buf[i] = color;
    }

    return lcd_flush(x, y, w, h, s_fill_buf);
}

esp_err_t lcd_flush(int x, int y, int w, int h, const uint16_t *buf)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 裁剪到屏幕范围内 */
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_H_RES) {
        w = LCD_H_RES - x;
    }
    if (y + h > LCD_V_RES) {
        h = LCD_V_RES - y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    s_wait_task = xTaskGetCurrentTaskHandle();

#if LCD_FILL_DISPOFF_AROUND
    esp_lcd_panel_disp_on_off(s_panel, false);
#endif

    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, buf);
    if (ret == ESP_OK) {
        ret = lcd_wait_color_done();
    } else {
        ESP_LOGE(TAG, "flush draw_bitmap failed: %s", esp_err_to_name(ret));
    }

#if LCD_FILL_DISPOFF_AROUND
    esp_lcd_panel_disp_on_off(s_panel, true);
#endif

    s_wait_task = NULL;
    return ret;
}

esp_err_t lcd_fill_color(uint16_t color)
{
    return lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
}

void lcd_backlight(bool on)
{
    gpio_set_level(LCD_PIN_BL, on ? LCD_BL_ACTIVE_LEVEL : !LCD_BL_ACTIVE_LEVEL);
}

esp_err_t lcd_color_test(void)
{
    static const struct {
        uint16_t color;
        const char *name;
    } seq[] = {
        { LCD_COLOR_WHITE,   "white" },
        { LCD_COLOR_RED,     "red" },
        { LCD_COLOR_GREEN,   "green" },
        { LCD_COLOR_BLUE,    "blue" },
        { LCD_COLOR_BLACK,   "black" },
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_LOGI(TAG, "color test: %s", seq[i].name);
        ESP_RETURN_ON_ERROR(lcd_fill_color(seq[i].color), TAG, "fill %s failed", seq[i].name);
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    /* 黑底 + 2px 白框：位置/尺寸/偏移有问题时能一眼看出来 */
    ESP_LOGI(TAG, "borders: black background with white frame");
    ESP_RETURN_ON_ERROR(lcd_fill_color(LCD_COLOR_BLACK), TAG, "fill black failed");
    const int t = 2;
    ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 0, LCD_H_RES, t, LCD_COLOR_WHITE), TAG, "border top");
    ESP_RETURN_ON_ERROR(lcd_fill_rect(0, LCD_V_RES - t, LCD_H_RES, t, LCD_COLOR_WHITE), TAG, "border bottom");
    ESP_RETURN_ON_ERROR(lcd_fill_rect(0, 0, t, LCD_V_RES, LCD_COLOR_WHITE), TAG, "border left");
    ESP_RETURN_ON_ERROR(lcd_fill_rect(LCD_H_RES - t, 0, t, LCD_V_RES, LCD_COLOR_WHITE), TAG, "border right");

    return ESP_OK;
}

/* 周期刷屏测试：每 LCD_SWEEP_PERIOD_MS 换一种颜色整屏填充，循环往复。
 * 仅在 APP_LCD_COLOR_TEST=1 时编译，用于验证 LCD 通路稳定（长时间连续
 * 刷屏不花屏、不丢帧）。正常显示模式下由 lcd_ui 任务驱动屏幕。 */
#if APP_LCD_COLOR_TEST
typedef struct {
    uint16_t color;
    const char *name;
} lcd_sweep_color_t;

static const lcd_sweep_color_t s_sweep_seq[] = {
    { LCD_COLOR_RED,     "red" },
    { LCD_COLOR_GREEN,   "green" },
    { LCD_COLOR_BLUE,    "blue" },
    { LCD_COLOR_WHITE,   "white" },
    { LCD_COLOR_BLACK,   "black" },
    { LCD_COLOR_YELLOW,  "yellow" },
    { LCD_COLOR_CYAN,    "cyan" },
    { LCD_COLOR_MAGENTA, "magenta" },
};
#define LCD_SWEEP_COLOR_COUNT  (sizeof(s_sweep_seq) / sizeof(s_sweep_seq[0]))

static void lcd_sweep_task(void *arg)
{
    (void)arg;
    size_t idx = 0;
    uint32_t round = 0;

    while (true) {
        esp_err_t err = lcd_fill_color(s_sweep_seq[idx].color);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fill %s failed: %s", s_sweep_seq[idx].name, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "sweep #%u: %s", (unsigned)++round, s_sweep_seq[idx].name);
        }
        idx = (idx + 1) % LCD_SWEEP_COLOR_COUNT;
        vTaskDelay(pdMS_TO_TICKS(LCD_SWEEP_PERIOD_MS));
    }
}
#endif /* APP_LCD_COLOR_TEST */

esp_err_t lcd_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 1. SPI 总线（与 TF 卡共用，已被初始化过则直接复用） */
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI2 bus already in use, reuse it");
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 背光先关，避免初始化过程闪白屏 */
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio config failed");
    lcd_backlight(false);

    /* 3. panel IO：CS/DC/时钟/位宽 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = lcd_color_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "new panel io failed");

    /* 4. ST7789 面板对象（RGB565，数据小端，与 uint16_t 内存布局一致） */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ORDER,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel),
                        TAG, "new st7789 panel failed");

    /* 5. 复位 + 初始化 + 方向/偏移/反色 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLOR), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_OFFSET_X, LCD_OFFSET_Y), TAG, "set_gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on failed");

    s_inited = true;
    lcd_backlight(true);

    ESP_LOGI(TAG, "ST7789 ready: %dx%d, SPI2 CS=%d DC=%d RST=%d BL=%d, %d MHz",
             LCD_H_RES, LCD_V_RES, LCD_PIN_CS, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL,
             LCD_PIXEL_CLOCK_HZ / 1000000);

#if APP_LCD_COLOR_TEST
    /* 测试模式：起周期刷屏任务（与 wifi_mgr 一样，init 内部起任务，调用方不被阻塞） */
    if (xTaskCreate(lcd_sweep_task, "lcd_sweep", LCD_TASK_STACK, NULL,
                    LCD_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "LCD sweep task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "color sweep test started: %u colors, every %d ms",
             (unsigned)LCD_SWEEP_COLOR_COUNT, LCD_SWEEP_PERIOD_MS);
#else
    ESP_LOGI(TAG, "color test disabled (APP_LCD_COLOR_TEST=0), UI task will drive the panel");
#endif

    return ESP_OK;
}

#else /* !APP_ENABLE_LCD */

/* 功能屏蔽时保持接口不变，调用方无需修改 */

esp_err_t lcd_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void lcd_backlight(bool on)
{
    (void)on;
}

esp_err_t lcd_fill_color(uint16_t color)
{
    (void)color;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_flush(int x, int y, int w, int h, const uint16_t *buf)
{
    (void)x; (void)y; (void)w; (void)h; (void)buf;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    (void)x; (void)y; (void)w; (void)h; (void)color;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_color_test(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* APP_ENABLE_LCD */
