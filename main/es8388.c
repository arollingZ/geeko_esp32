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

    /* 重试的理由：实测过 I2S 时钟在跑时偶发读失败（ES8388 内部状态机
     * 被干扰时会 NAK）。单次失败不代表总线坏了，重试即可；连续失败才
     * 做一次总线复位（发 9 个时钟把卡住的从机放出来）。 */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                          I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        i2c_master_bus_handle_t bus = codec_bus_handle();
        if (bus != NULL) {
            i2c_master_bus_reset(bus);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "read R0x%02x failed after 3 tries: %s",
             reg, esp_err_to_name(err));
    return err;
}

esp_err_t es8388_reg_write(uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "not attached");
    uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "write R0x%02x=0x%02x failed after 3 tries: %s",
             reg, val, esp_err_to_name(err));
    return err;
}

esp_err_t es8388_config_playback(void)
{
    ESP_RETURN_ON_ERROR(es8388_attach(), TAG, "attach failed");

    uint8_t r0 = 0;
    ESP_RETURN_ON_ERROR(es8388_reg_read(ES8388_REG_CONTROL1, &r0), TAG,
                        "read R0 failed");
    ESP_RETURN_ON_FALSE((r0 & ES8388_SCPRESET_BIT) == 0, ESP_ERR_INVALID_STATE,
                        TAG, "chip is in reset (R0=0x%02x), call soft_reset first",
                        r0);

    /* 每个依赖的寄存器都显式写，不假设当前状态。数值取自 esp_codec_dev
     * 在该板上的验证过的序列（xiaozhi 实测可用），仅把主从模式改成明确的
     * 从机（0x00），而不是原驱动的 bool 写法（实际落到 0x01，语义含糊）。 */
    static const struct { uint8_t reg; uint8_t val; } seq[] = {
        { 0x01, 0x50 },  /* CONTROL2: LPVrefBuf=1, PdnAna=0 模拟部分上电 */
        { 0x02, 0x00 },  /* CHIPPOWER: 全部正常工作（不复位、DLL 不关） */
        { 0x35, 0xA0 },  /* 关内部 DLL（驱动同款，改善低采样率） */
        { 0x37, 0xD0 },
        { 0x39, 0xD0 },
        { 0x08, 0x00 },  /* MASTERMODE: MSC=0 -> I2S 从机（ESP32 作主） */
        { 0x04, 0xC0 },  /* DACPOWER: 先全关，避免开输出瞬间的爆音 */
        { 0x00, 0x12 },  /* CONTROL1: SameFs=1(ADC/DAC 同 LRCK), VMIDSEL=10 */
        { 0x17, 0x18 },  /* DACCONTROL1: 16bit, I2S 格式 */
        { 0x18, 0x02 },  /* DACCONTROL2: 单速, DACFsRatio=256 (MCLK=256Fs) */
        { 0x26, 0x00 },  /* DACCONTROL16: DAC 输入选 LIN1/RIN1 */
        { 0x27, 0x90 },  /* DACCONTROL17: 左 DAC -> 左混音器, 0dB */
        { 0x2A, 0x90 },  /* DACCONTROL20: 右 DAC -> 右混音器, 0dB */
        { 0x2B, 0x80 },  /* DACCONTROL21: DAC 用 ADC 的 LRCK（共用一对） */
        { 0x2D, 0x00 },  /* DACCONTROL23: vroi=0 */
        { 0x1A, 0x00 },  /* LDACVOL: 0x00 = 0dB（该寄存器是反比编码） */
        { 0x1B, 0x00 },  /* RDACVOL: 0x00 = 0dB */
        /* 模拟输出音量（非反比编码）：0x1E=0dB, 0x00=-30dB。
         * 四路都开 0dB——喇叭功放接在哪一路目前只有原理图能确认，
         * 四路全开可避免"配了一路却没接对"导致没声音。 */
        { 0x2E, 0x1E },  /* LOUT1 */
        { 0x2F, 0x1E },  /* ROUT1 */
        { 0x30, 0x1E },  /* LOUT2 */
        { 0x31, 0x1E },  /* ROUT2 */
        /* 注意：这里不打开 DACPOWER。输出级由 es8388_output_enable()
         * 在时钟稳定、数据线已是静音之后再打开，避免爆音。 */
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_RETURN_ON_ERROR(es8388_reg_write(seq[i].reg, seq[i].val), TAG,
                            "write R0x%02x=0x%02x failed",
                            seq[i].reg, seq[i].val);
    }

    /* 回读关键寄存器，确认配置真的落地（写入不报错不等于生效） */
    uint8_t v8 = 0, v17 = 0;
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x08, &v8), TAG, "read R8 failed");
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x17, &v17), TAG, "read R0x17 failed");

    ESP_LOGI(TAG, "playback cfg: R8=0x%02x (%s) R0x17=0x%02x (16bit I2S), "
             "outputs still OFF", v8, (v8 & 0x80) ? "MASTER!" : "slave", v17);

    if (v8 & 0x80) {
        ESP_LOGE(TAG, "R8 bit7=1: codec is master, will fight ESP32 for BCLK/WS");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t es8388_output_enable(bool on)
{
    ESP_RETURN_ON_ERROR(es8388_attach(), TAG, "attach failed");
    /* R4: bit7/6=1 时 DAC 掉电，bit5~2 是 LOUT1/ROUT1/LOUT2/ROUT2 使能。
     * 0xC0 = DAC 掉电 + 四路输出全关；0x3C = DAC 上电 + 四路输出全开。 */
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x04, on ? 0x3C : 0xC0), TAG,
                        "write R4 failed");
    ESP_LOGI(TAG, "outputs %s (R4=0x%02x)", on ? "ON" : "OFF", on ? 0x3C : 0xC0);
    return ESP_OK;
}

esp_err_t es8388_set_volume_db(int8_t db)
{
    ESP_RETURN_ON_ERROR(es8388_attach(), TAG, "attach failed");
    if (db > 0) {
        db = 0;
    }
    if (db < -96) {
        db = -96;
    }

    /* 数字音量是反比编码：0x00=0dB，每 +1 约 -0.5dB，0xC0=-96dB */
    uint8_t vol = (uint8_t)((-db) * 2);
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x1A, vol), TAG, "LDAC vol failed");
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x1B, vol), TAG, "RDAC vol failed");

    /* 模拟输出音量：0x00=-30dB，0x1E=0dB，0x21=+3dB（线性步进 1dB） */
    int analog = 0x1E + db;
    if (analog < 0x00) {
        analog = 0x00;
    }
    for (uint8_t reg = 0x2E; reg <= 0x31; reg++) {
        ESP_RETURN_ON_ERROR(es8388_reg_write(reg, (uint8_t)analog), TAG,
                            "R0x%02x failed", reg);
    }
    ESP_LOGI(TAG, "volume %d dB: digital=0x%02x analog=0x%02x", db, vol, analog);
    return ESP_OK;
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
