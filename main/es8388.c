/*
 * es8388.c —— ES8388 底层控制实现
 */
#include "es8388.h"

#include "codec_bus.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8388";

#define I2C_XFER_TIMEOUT_MS   100
#define I2C_SCL_HZ            100000

static i2c_master_dev_handle_t s_dev = NULL;

esp_err_t es8388_attach(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = codec_bus_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "codec bus not initialized");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8388_I2C_ADDR_7BIT,
        .scl_speed_hz    = I2C_SCL_HZ,
        .scl_wait_us     = 0,
        .flags = { .disable_ack_check = false },
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG,
                        "add device failed");
    ESP_LOGI(TAG, "attached at 0x%02x", ES8388_I2C_ADDR_7BIT);
    return ESP_OK;
}

esp_err_t es8388_reg_read(uint8_t reg, uint8_t *val)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "not attached");
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       I2C_XFER_TIMEOUT_MS);
}

esp_err_t es8388_reg_write(uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "not attached");
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
}

esp_err_t es8388_soft_reset(void)
{
    ESP_RETURN_ON_ERROR(es8388_attach(), TAG, "attach failed");

    /* 1) 置 SCPReset，控制端口寄存器恢复默认值 */
    ESP_RETURN_ON_ERROR(es8388_reg_write(ES8388_REG_CONTROL1,
                                         ES8388_SCPRESET_BIT), TAG,
                        "assert SCPReset failed");
    vTaskDelay(pdMS_TO_TICKS(ES8388_RESET_DELAY_MS));

    /* 2) SCPReset 不自清零，必须显式写回默认态解除复位 */
    ESP_RETURN_ON_ERROR(es8388_reg_write(ES8388_REG_CONTROL1,
                                         ES8388_R0_DEFAULT), TAG,
                        "clear SCPReset failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 3) 回读确认 bit7 已清零，否则后续所有写入都不会生效 */
    uint8_t r0 = 0;
    ESP_RETURN_ON_ERROR(es8388_reg_read(ES8388_REG_CONTROL1, &r0), TAG,
                        "readback R0 failed");
    if (r0 & ES8388_SCPRESET_BIT) {
        ESP_LOGE(TAG, "R0=0x%02x: SCPReset still set, chip stuck in reset", r0);
        return ESP_FAIL;
    }
    if (r0 != ES8388_R0_DEFAULT) {
        ESP_LOGW(TAG, "R0 readback 0x%02x (wrote 0x%02x)", r0, ES8388_R0_DEFAULT);
    }
    ESP_LOGI(TAG, "soft reset done: R0=0x%02x (SCPReset cleared)", r0);
    return ESP_OK;
}
