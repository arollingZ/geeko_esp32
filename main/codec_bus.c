/*
 * codec_bus.c —— ES8388 控制面 I2C 总线实现
 *
 * 第一步（当前）：只做总线初始化 + 地址扫描，不触碰 codec 任何寄存器，
 * 因此不会发出任何声音。
 */
#include "codec_bus.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "codec_bus";

/* 引脚定义（与 xiaozhi atk-dnesp32s3m-wifi 板一致） */
#define CODEC_I2C_SDA_GPIO    4
#define CODEC_I2C_SCL_GPIO    5

/* 扫描用 100 kHz：最稳，任何器件都能响应 */
#define CODEC_I2C_FREQ_HZ     100000

/* 单个地址探测超时：100 ms 足够，卡住也不拖累整体扫描 */
#define PROBE_TIMEOUT_MS      100

static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t codec_bus_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK; /* 幂等：重复调用不重复建总线 */
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = -1, /* 自动选择 I2C 端口号 */
        .sda_io_num        = CODEC_I2C_SDA_GPIO,
        .scl_io_num        = CODEC_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority     = 0,
        .trans_queue_depth = 0,
        .flags = {
            /* 板子一般有外部上拉；内部上拉作为兜底，开启无害 */
            .enable_internal_pullup = true,
            .allow_pd               = false,
        },
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG,
                        "i2c_new_master_bus failed");
    ESP_LOGI(TAG, "I2C bus up: SDA=IO%d SCL=IO%d @ %d Hz",
             CODEC_I2C_SDA_GPIO, CODEC_I2C_SCL_GPIO, CODEC_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t codec_bus_handle(void)
{
    return s_bus;
}

/* 已知器件猜测表（仅为日志可读性，不影响扫描逻辑） */
static const char *guess_device(int addr)
{
    switch (addr) {
    case 0x10: return "ES8388 codec (expected!)";
    case 0x11: return "ES8388 with CE pulled high";
    case 0x1A: return "maybe WM8978";
    case 0x3C: case 0x3D: return "maybe SSD1306 OLED";
    case 0x68: return "maybe MPU6050 / DS3231";
    default:   return "unknown";
    }
}

int codec_bus_scan(void)
{
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "scan before init");
        return -1;
    }

    ESP_LOGI(TAG, "scanning 0x08..0x77 ...");

    int found = 0;
    /* i2cdetect 风格网格：每行 16 个地址 */
    char line[80];
    for (int base = 0x00; base < 0x80; base += 16) {
        int pos = snprintf(line, sizeof(line), "  %02x: ", base);
        for (int i = 0; i < 16; i++) {
            int addr = base + i;
            if (addr < 0x08 || addr > 0x77) {
                /* 0x00~0x07 与 0x78~0x7F 是保留地址，不探测 */
                pos += snprintf(line + pos, sizeof(line) - pos, "   ");
                continue;
            }
            if (i2c_master_probe(s_bus, addr, PROBE_TIMEOUT_MS) == ESP_OK) {
                pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", addr);
                found++;
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "-- ");
            }
        }
        ESP_LOGI(TAG, "%s", line);
    }

    /* 汇总 + 已知器件猜测 */
    ESP_LOGI(TAG, "scan done: %d device(s) found", found);
    if (found > 0) {
        for (int addr = 0x08; addr <= 0x77; addr++) {
            if (i2c_master_probe(s_bus, addr, PROBE_TIMEOUT_MS) == ESP_OK) {
                ESP_LOGI(TAG, "  @0x%02x -> %s", addr, guess_device(addr));
            }
        }
    }

    /* 结论性提示：这一步的判断标准 */
    if (i2c_master_probe(s_bus, ES8388_I2C_ADDR_7BIT, PROBE_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGI(TAG, "OK: ES8388 responded at 0x%02x, control path is alive",
                 ES8388_I2C_ADDR_7BIT);
    } else {
        ESP_LOGW(TAG, "ES8388 NOT found at 0x%02x!", ES8388_I2C_ADDR_7BIT);
        ESP_LOGW(TAG, "check: codec power, SDA/SCL wiring (IO%d/IO%d), pull-ups",
                 CODEC_I2C_SDA_GPIO, CODEC_I2C_SCL_GPIO);
    }
    return found;
}
