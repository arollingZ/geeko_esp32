/*
 * es8388.c —— ES8388 底层控制实现
 */
#include "es8388.h"

#include <stdio.h>

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

    /* 每个依赖的寄存器都显式写，不假设当前状态。数值严格复刻
     * esp_codec_dev 在该板上验证过的 open 序列（xiaozhi 实测能出声），
     * 仅把主从模式改成明确的从机（0x00），而不是原驱动的 bool 写法
     * （实际落到 0x01，语义含糊）。
     *
     * 三个容易漏、漏了就完全没声音的点：
     *   1. 先写 0x19=0x04 主动静音，配置过程中不出杂音，最后再解除
     *   2. DACCONTROL21(0x2B) 变化后必须重启状态机（0xF0 -> 0x00），
     *      否则 DAC 状态机停在 power-down 状态，时钟有、数据有、就是没声
     *   3. ADC 部分也要配（0x03=0x09 上电）。虽然这一阶段只播放，
     *      但模拟参考是共用的，ADC 全掉电时 DAC 参考可能不稳。 */
    static const struct { uint8_t reg; uint8_t val; } seq[] = {
        { 0x19, 0x04 },  /* DACCONTROL3: 主动静音（bit2=1），配置期间无杂音 */
        { 0x01, 0x50 },  /* CONTROL2: 模拟部分上电（PdnAna=0） */
        { 0x02, 0x00 },  /* CHIPPOWER: 全部正常工作 */
        { 0x35, 0xA0 },  /* 关内部 DLL（驱动同款，改善低采样率时钟） */
        { 0x37, 0xD0 },
        { 0x39, 0xD0 },
        { 0x08, 0x00 },  /* MASTERMODE: MSC=0 -> I2S 从机（ESP32 作主） */
        { 0x04, 0xC0 },  /* DACPOWER: 先全关，避免开输出瞬间的爆音 */
        /* R0 = 0x16: SameFs=1(ADC/DAC 共用 LRCK), EnRef=1(参考电压使能),
         * VMIDSEL=10(500kΩ)。
         * 注意不是 0x12：0x12 的 bit2=0 会关闭参考电压（手冊 R0 bit2
         * EnRef，复位默认 1）。参考被禁时模拟部分整体不工作，
         * 表现就是"寄存器全对、时钟全对、一点声音都没有"。 */
        { 0x00, 0x16 },
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
         * 四路都给 0dB——喇叭功放接在哪一路只能看原理图，四路全开
         * 可避免"配了一路却没接对"导致没声音。 */
        { 0x2E, 0x1E },  /* LOUT1 */
        { 0x2F, 0x1E },  /* ROUT1 */
        { 0x30, 0x1E },  /* LOUT2 */
        { 0x31, 0x1E },  /* ROUT2 */
        /* ADC 侧（虽然暂不录音，但保证与可用配置一致）
         *
         * R9 = 0x88: MIC PGA 左右都是档位 8 = +24dB（数据手册上限）。
         * 原来照抄厂商驱动的 0xBB，但 0xBB 的低/高 4 位都是 0xB，
         * 而合法档位只有 0~8，0xB 属于未定义值——录音底噪异常大很可能
         * 跟它有关。真正生效的值随后由 es8388_set_mic_pga() 按
         * feature_config.h 的 REC_ADC_PGA_DB 覆盖，这里只是给个安全默认。 */
        { 0x03, 0xFF },  /* ADCPOWER: 先全掉电 */
        { 0x09, 0x88 },  /* ADCCONTROL1: MIC PGA 双声道 +24dB（合法档位） */
        { 0x0A, 0x00 },  /* ADCCONTROL2: LIN1/RIN1 作 ADC 输入 */
        { 0x0B, 0x02 },  /* ADCCONTROL3 */
        { 0x0C, 0x0C },  /* ADCCONTROL4: 16bit, I2S 格式 */
        { 0x0D, 0x02 },  /* ADCCONTROL5: 单速, 256Fs */
        { 0x10, 0x00 },  /* ADCCONTROL8: LADC 音量 0dB */
        { 0x11, 0x00 },  /* ADCCONTROL9: RADC 音量 0dB */
        { 0x03, 0x09 },  /* ADCPOWER: 上电 ADC */
        /* 解除静音 + 打开输出级，都放在这里（无时钟时）完成。
         * 原因：实测 I2S 时钟一旦运行，I2C 写就会开始 NAK
         * （ESP_ERR_INVALID_RESPONSE），配置阶段连写 30 个寄存器却零错误。
         * 所以全部 I2C 操作必须在时钟启动前做完。
         * 顺序上先解除静音再开输出：此时模拟输出还没打开，不会出声，
         * 开时钟时 DOUT 上又是静音数据，所以也不会有爆音。 */
        { 0x19, 0x00 },  /* DACCONTROL3: 解除静音 */
        { 0x04, 0x3C },  /* DACPOWER: DAC 上电 + 四路输出使能 */
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_RETURN_ON_ERROR(es8388_reg_write(seq[i].reg, seq[i].val), TAG,
                            "write R0x%02x=0x%02x failed",
                            seq[i].reg, seq[i].val);
    }

    /* 状态机重启：DACCONTROL21 改过之后必须做，否则状态机停在
     * power-down，表现为时钟/数据都对但完全没声音。 */
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x02, 0xF0), TAG, "stm reset failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x02, 0x00), TAG, "stm start failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 回读关键寄存器，确认配置真的落地（写入不报错不等于生效） */
    uint8_t v8 = 0, v17 = 0, v19 = 0, v2b = 0, v3 = 0, v4 = 0;
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x08, &v8), TAG, "read R8 failed");
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x17, &v17), TAG, "read R0x17 failed");
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x19, &v19), TAG, "read R0x19 failed");
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x2B, &v2b), TAG, "read R0x2B failed");
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x03, &v3), TAG, "read R0x03 failed");
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x04, &v4), TAG, "read R0x04 failed");

    ESP_LOGI(TAG, "playback cfg: R8=0x%02x(%s) R17=0x%02x R19=0x%02x(mute=%d) "
             "R2B=0x%02x R3=0x%02x R4=0x%02x",
             v8, (v8 & 0x80) ? "MASTER!" : "slave", v17, v19, !!(v19 & 0x04),
             v2b, v3, v4);

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

esp_err_t es8388_mute(bool mute)
{
    ESP_RETURN_ON_ERROR(es8388_attach(), TAG, "attach failed");
    /* R0x19 bit2 = DACMU。只改这一位，保留其他配置。 */
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(es8388_reg_read(0x19, &v), TAG, "read R0x19 failed");
    v = (uint8_t)((v & ~0x04) | (mute ? 0x04 : 0x00));
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x19, v), TAG, "write R0x19 failed");
    ESP_LOGI(TAG, "%s (R0x19=0x%02x)", mute ? "DAC muted" : "DAC unmuted", v);
    return ESP_OK;
}

void es8388_dump_registers(void)
{
    if (s_dev == NULL) {
        ESP_LOGW(TAG, "not attached, cannot dump");
        return;
    }
    ESP_LOGI(TAG, "--- register dump ---");
    for (int base = 0x00; base <= 0x30; base += 8) {
        char line[112];
        int pos = snprintf(line, sizeof(line), "  %02x: ", base);
        for (int i = 0; i < 8; i++) {
            uint8_t v = 0;
            if (es8388_reg_read((uint8_t)(base + i), &v) == ESP_OK) {
                pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", v);
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "?? ");
            }
        }
        ESP_LOGI(TAG, "%s", line);
    }

    /* 关键值解读：没声音时这几项是最先要看的 */
    uint8_t r0 = 0, r2 = 0, r4 = 0, r8 = 0, r17 = 0, r19 = 0, r1a = 0, r2b = 0;
    uint8_t r2e = 0, r2f = 0, r30 = 0, r31 = 0;
    es8388_reg_read(0x00, &r0);   es8388_reg_read(0x02, &r2);
    es8388_reg_read(0x04, &r4);   es8388_reg_read(0x08, &r8);
    es8388_reg_read(0x17, &r17);  es8388_reg_read(0x19, &r19);
    es8388_reg_read(0x1A, &r1a);  es8388_reg_read(0x2B, &r2b);
    es8388_reg_read(0x2E, &r2e);  es8388_reg_read(0x2F, &r2f);
    es8388_reg_read(0x30, &r30);  es8388_reg_read(0x31, &r31);

    ESP_LOGI(TAG, "--- key values ---");
    ESP_LOGI(TAG, "  R0 =0x%02x  SCPReset=%d(power-on reset trigger!) "
             "EnRef=%d(ref enable) VMIDSEL=%d",
             r0, !!(r0 & 0x80), !!(r0 & 0x04), r0 & 0x03);
    ESP_LOGI(TAG, "  R2 =0x%02x  DAC stm_rst=%d(1=power-down) DLL_PDN=%d "
             "Vref_PDN=%d", r2, !!(r2 & 0x10), !!(r2 & 0x04), !!(r2 & 0x01));
    ESP_LOGI(TAG, "  R4 =0x%02x  DACL_PDN=%d DACR_PDN=%d | LOUT1=%d ROUT1=%d "
             "LOUT2=%d ROUT2=%d", r4, !!(r4 & 0x80), !!(r4 & 0x40),
             !!(r4 & 0x20), !!(r4 & 0x10), !!(r4 & 0x08), !!(r4 & 0x04));
    ESP_LOGI(TAG, "  R8 =0x%02x  MSC=%d(0=slave,1=master!)", r8, !!(r8 & 0x80));
    ESP_LOGI(TAG, "  R17=0x%02x  DACWL=%d(011=16bit) FMT=%d(00=I2S)",
             r17, (r17 >> 3) & 0x07, (r17 >> 1) & 0x03);
    ESP_LOGI(TAG, "  R19=0x%02x  DACMU=%d(1=MUTED!)", r19, !!(r19 & 0x04));
    ESP_LOGI(TAG, "  R1A=0x%02x  LDAC vol(%s)", r1a,
             r1a == 0 ? "0dB max" : "attenuated");
    ESP_LOGI(TAG, "  R2B=0x%02x  DAC LRCK sel=%d", r2b, (r2b >> 7) & 0x03);
    ESP_LOGI(TAG, "  analog vol: LOUT1=0x%02x ROUT1=0x%02x LOUT2=0x%02x "
             "ROUT2=0x%02x (0x1E=0dB, 0x00=-30dB)", r2e, r2f, r30, r31);
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

esp_err_t es8388_set_mic_pga(int db)
{
    ESP_RETURN_ON_ERROR(es8388_attach(), TAG, "attach failed");

    if (db < 0) {
        db = 0;
    }
    if (db > 24) {
        db = 24;
    }
    /* 数据手册：MIC PGA 只有 0000~1000 共 9 档，0~+24dB 步进 3dB。
     * 高 4 位是左声道、低 4 位是右声道，两路同值。 */
    uint8_t code = (uint8_t)(db / 3);
    uint8_t v = (uint8_t)((code << 4) | code);

    ESP_RETURN_ON_ERROR(es8388_reg_write(ES8388_REG_ADCCONTROL1, v), TAG,
                        "write R9 failed");
    ESP_LOGI(TAG, "mic PGA %d dB (R9=0x%02x)", code * 3, v);
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
