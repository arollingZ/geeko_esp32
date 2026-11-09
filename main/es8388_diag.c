/*
 * es8388_diag.c —— ES8388 寄存器读写诊断实现
 *
 * 默认值表来源：ES8388 数据手册（Everest，rev 5.0/10.0/12.0 交叉核对）。
 * 各版本手册有冲突的寄存器（R1、R29、R32~R52）不参与比对，仅打印实测值。
 */
#include "es8388_diag.h"

#include "codec_bus.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "es8388_diag";

#define ES8388_REG_MAX     0x34   /* R52 = DAC Control 30 */
#define I2C_XFER_TIMEOUT_MS 100
#define DEFAULT_UNKNOWN    (-1)   /* 该寄存器默认值不确定，跳过比对 */

static i2c_master_dev_handle_t s_dev = NULL;

/* 复位默认值表（DEFAULT_UNKNOWN = 版本间冲突/未确认，只打印不比对） */
static const int16_t s_defaults[ES8388_REG_MAX + 1] = {
    /* R0  */ 0x06,   /* Chip Control 1 */
    /* R1  */ DEFAULT_UNKNOWN, /* Chip Control 2: rev10=0x5C, UserGuide=0x1C */
    /* R2  */ 0xC3,   /* Chip Power Management */
    /* R3  */ 0xFC,   /* ADC Power Management */
    /* R4  */ 0xC0,   /* DAC Power Management */
    /* R5  */ 0x00,   /* Chip Low Power 1 */
    /* R6  */ 0x00,   /* Chip Low Power 2 */
    /* R7  */ 0x7C,   /* Analog Voltage Management */
    /* R8  */ 0x80,   /* Master Mode Control（复位=主模式，需软件改为从） */
    /* R9  */ 0x00,   /* ADC Control 1 (MIC PGA gain) */
    /* R10 */ 0x00,   /* ADC Control 2 */
    /* R11 */ 0x02,   /* ADC Control 3 */
    /* R12 */ 0x00,   /* ADC Control 4 */
    /* R13 */ 0x06,   /* ADC Control 5 */
    /* R14 */ 0x30,   /* ADC Control 6 */
    /* R15 */ 0x20,   /* ADC Control 7 */
    /* R16 */ 0x00,   /* ADC Control 8  (LADC volume, 0dB) */
    /* R17 */ 0x00,   /* ADC Control 9  (RADC volume, 0dB) */
    /* R18 */ DEFAULT_UNKNOWN,
    /* R19 */ DEFAULT_UNKNOWN,
    /* R20 */ 0x32,   /* ADC Control 12 */
    /* R21 */ 0x06,   /* ADC Control 13 */
    /* R22 */ 0x00,   /* ADC Control 14 */
    /* R23 */ 0x00,   /* DAC Control 1 */
    /* R24 */ 0x06,   /* DAC Control 2 (FsRatio=768) */
    /* R25 */ 0x22,   /* DAC Control 3 (soft ramp) */
    /* R26 */ 0xC0,   /* DAC Control 4 (LDAC volume) */
    /* R27 */ 0xC0,   /* DAC Control 5 (RDAC volume) */
    /* R28 */ 0x08,   /* DAC Control 6 */
    /* R29 */ DEFAULT_UNKNOWN, /* DAC Control 7: rev12=0x00, rev2=0x06 */
    /* R30 */ 0x1F,   /* DAC Control 8 */
    /* R31 */ 0xF7,   /* DAC Control 9 */
    /* R32~R52: 各版本手册不一致，跳过 */
    DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN,
    DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN,
    DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN,
    DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN,
    DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN, DEFAULT_UNKNOWN,
    DEFAULT_UNKNOWN,
};

static esp_err_t diag_attach_device(void)
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
        .scl_speed_hz    = 100000,
        .scl_wait_us     = 0,
        .flags = { .disable_ack_check = false },
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG,
                        "add device failed");
    return ESP_OK;
}

esp_err_t es8388_reg_read(uint8_t reg, uint8_t *val)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "device not attached");
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       I2C_XFER_TIMEOUT_MS);
}

esp_err_t es8388_reg_write(uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "device not attached");
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
}

/* 全部寄存器 dump（i2cdump 风格）+ 与默认值比对。返回不匹配数（不含跳过的） */
static int dump_and_compare(void)
{
    uint8_t vals[ES8388_REG_MAX + 1];

    for (int reg = 0; reg <= ES8388_REG_MAX; reg++) {
        esp_err_t err = es8388_reg_read(reg, &vals[reg]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read reg 0x%02x failed: %s", reg,
                     esp_err_to_name(err));
            return -1;
        }
    }

    /* i2cdump 风格打印 */
    ESP_LOGI(TAG, "register dump (R0~R52):");
    for (int base = 0; base <= ES8388_REG_MAX; base += 8) {
        char line[96];
        int pos = snprintf(line, sizeof(line), "  R%02d: ", base);
        for (int i = 0; i < 8 && base + i <= ES8388_REG_MAX; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x ",
                            vals[base + i]);
        }
        ESP_LOGI(TAG, "%s", line);
    }

    /* 与默认值比对 */
    int match = 0, mismatch = 0, skipped = 0;
    for (int reg = 0; reg <= ES8388_REG_MAX; reg++) {
        if (s_defaults[reg] == DEFAULT_UNKNOWN) {
            skipped++;
            continue;
        }
        if (vals[reg] == (uint8_t)s_defaults[reg]) {
            match++;
        } else {
            mismatch++;
            ESP_LOGW(TAG, "  R%d (0x%02x): read 0x%02x, datasheet default 0x%02x",
                     reg, reg, vals[reg], (uint8_t)s_defaults[reg]);
        }
    }
    ESP_LOGI(TAG, "default check: %d match, %d mismatch, %d skipped",
             match, mismatch, skipped);
    return mismatch;
}

/* 单个寄存器的 写->回读->还原 测试 */
static esp_err_t write_readback_test(uint8_t reg, const char *name)
{
    static const uint8_t patterns[] = { 0xA5, 0x5A };
    uint8_t orig, rb;

    ESP_RETURN_ON_ERROR(es8388_reg_read(reg, &orig), TAG,
                        "R%d read orig failed", reg);

    for (size_t i = 0; i < sizeof(patterns); i++) {
        ESP_RETURN_ON_ERROR(es8388_reg_write(reg, patterns[i]), TAG,
                            "R%d write 0x%02x failed", reg, patterns[i]);
        ESP_RETURN_ON_ERROR(es8388_reg_read(reg, &rb), TAG,
                            "R%d readback failed", reg);
        if (rb != patterns[i]) {
            ESP_LOGE(TAG, "  R%d (%s): wrote 0x%02x but read 0x%02x -> FAIL",
                     reg, name, patterns[i], rb);
            /* 尽力还原后再返回 */
            es8388_reg_write(reg, orig);
            return ESP_FAIL;
        }
    }

    /* 还原原值并确认 */
    ESP_RETURN_ON_ERROR(es8388_reg_write(reg, orig), TAG,
                        "R%d restore failed", reg);
    ESP_RETURN_ON_ERROR(es8388_reg_read(reg, &rb), TAG,
                        "R%d restore readback failed", reg);
    if (rb != orig) {
        ESP_LOGE(TAG, "  R%d (%s): restore failed (0x%02x != 0x%02x)",
                 reg, name, rb, orig);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "  R%d (%s): 0xA5/0x5A write+readback OK, restored 0x%02x",
             reg, name, orig);
    return ESP_OK;
}

esp_err_t es8388_diag_run(void)
{
    ESP_RETURN_ON_ERROR(diag_attach_device(), TAG, "attach failed");

    ESP_LOGI(TAG, "=== step 1: full register dump vs datasheet defaults ===");
    int mismatch = dump_and_compare();
    if (mismatch < 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "=== step 2: write-readback-restore test ===");
    /* R9  = ADC Control 1，MIC PGA 增益，纯输入侧，安全
     * R26 = DAC Control 4，DAC 左声道数字音量；复位时 DAC 已掉电，安全 */
    esp_err_t r1 = write_readback_test(9,  "MIC PGA gain");
    esp_err_t r2 = write_readback_test(26, "LDAC digital vol");

    if (r1 == ESP_OK && r2 == ESP_OK) {
        ESP_LOGI(TAG, "RESULT: register write/read path VERIFIED");
        if (mismatch > 0) {
            ESP_LOGW(TAG, "note: %d default mismatch(es) above -- informational "
                     "only, could be chip revision or bootloader touch", mismatch);
        }
        return ESP_OK;
    }
    ESP_LOGE(TAG, "RESULT: write/read path FAILED");
    return ESP_FAIL;
}
