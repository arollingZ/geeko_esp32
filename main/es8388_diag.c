/*
 * es8388_diag.c —— ES8388 寄存器读写诊断实现
 *
 * 默认值表来源：ES8388 数据手册（Everest，rev 5.0/10.0/12.0 交叉核对）。
 * 各版本手册有冲突的寄存器（R1、R18/R19、R29、R32~R52）不参与比对。
 */
#include "es8388_diag.h"

#include <stdbool.h>
#include <stdio.h>

#include "es8388.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "es8388_diag";

#define ES8388_REG_MAX   0x34   /* R52 = DAC Control 30 */
#define DEFAULT_UNKNOWN  (-1)   /* 版本间冲突/未确认，只打印不比对 */

/* 数据手册复位默认值表：未列出的寄存器一律视为"不确定，不比对"。
 * 用键值表填充而不是逐个下标初始化，避免手数 53 个位置出错。 */
static int16_t s_defaults[ES8388_REG_MAX + 1];

static const struct { uint8_t reg; uint8_t val; } s_default_kv[] = {
    { 0x00, 0x06 },  /* Chip Control 1: EnRef=1, VMIDSEL=10 */
    { 0x02, 0xC3 },  /* Chip Power Management */
    { 0x03, 0xFC },  /* ADC Power Management */
    { 0x04, 0xC0 },  /* DAC Power Management */
    { 0x05, 0x00 },
    { 0x06, 0x00 },
    { 0x07, 0x7C },
    { 0x08, 0x80 },  /* Master Mode: 复位=主模式 */
    { 0x09, 0x00 },
    { 0x0A, 0x00 },
    { 0x0B, 0x02 },
    { 0x0C, 0x00 },
    { 0x0D, 0x06 },
    { 0x0E, 0x30 },
    { 0x0F, 0x20 },
    { 0x10, 0xC0 },  /* LADC volume: 0xC0 = 0dB（手册 TOC 写 0000 0000，
                      * 但 0x00 语义是 -96dB 静音，且实测复位后就是 0xC0，
                      * 与 R26/R27 的 DAC 音量编码一致，故按实测值） */
    { 0x11, 0xC0 },  /* RADC volume: 同上 */
    { 0x14, 0x32 },
    { 0x15, 0x06 },
    { 0x16, 0x00 },
    { 0x17, 0x00 },
    { 0x18, 0x06 },
    { 0x19, 0x22 },
    { 0x1A, 0xC0 },  /* LDAC volume */
    { 0x1B, 0xC0 },  /* RDAC volume */
    { 0x1C, 0x08 },
    { 0x1E, 0x1F },
    { 0x1F, 0xF7 },
    /* 未列出的 R1/R12~R13/R1D/R20~R34 各版本手册不一致，跳过比对 */
};

static void defaults_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }
    for (int i = 0; i <= ES8388_REG_MAX; i++) {
        s_defaults[i] = DEFAULT_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof(s_default_kv) / sizeof(s_default_kv[0]); i++) {
        s_defaults[s_default_kv[i].reg] = s_default_kv[i].val;
    }
    inited = true;
}

/* dump 全部寄存器到 vals[] */
static esp_err_t read_all(uint8_t *vals)
{
    for (int reg = 0; reg <= ES8388_REG_MAX; reg++) {
        ESP_RETURN_ON_ERROR(es8388_reg_read((uint8_t)reg, &vals[reg]), TAG,
                            "read R%d failed", reg);
    }
    return ESP_OK;
}

static void print_dump(const char *title, const uint8_t *vals)
{
    ESP_LOGI(TAG, "%s", title);
    for (int base = 0; base <= ES8388_REG_MAX; base += 8) {
        char line[96];
        int pos = snprintf(line, sizeof(line), "  R%02d: ", base);
        for (int i = 0; i < 8 && base + i <= ES8388_REG_MAX; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x ",
                            vals[base + i]);
        }
        ESP_LOGI(TAG, "%s", line);
    }
}

/* 与默认值比对，返回不匹配数量（跳过的不计） */
static int compare_defaults(const uint8_t *vals, bool verbose)
{
    int mismatch = 0, match = 0, skipped = 0;
    for (int reg = 0; reg <= ES8388_REG_MAX; reg++) {
        if (s_defaults[reg] == DEFAULT_UNKNOWN) {
            skipped++;
            continue;
        }
        if (vals[reg] == (uint8_t)s_defaults[reg]) {
            match++;
        } else {
            mismatch++;
            if (verbose) {
                ESP_LOGW(TAG, "  R%d (0x%02x): 0x%02x, default 0x%02x",
                         reg, reg, vals[reg], (uint8_t)s_defaults[reg]);
            }
        }
    }
    ESP_LOGI(TAG, "  vs defaults: %d match, %d mismatch, %d skipped",
             match, mismatch, skipped);
    return mismatch;
}

/* 单个寄存器 写->回读->还原 测试 */
static esp_err_t write_readback_test(uint8_t reg, const char *name)
{
    static const uint8_t patterns[] = { 0xA5, 0x5A };
    uint8_t orig = 0, rb = 0;

    ESP_RETURN_ON_ERROR(es8388_reg_read(reg, &orig), TAG, "R%d read failed", reg);

    for (size_t i = 0; i < sizeof(patterns); i++) {
        ESP_RETURN_ON_ERROR(es8388_reg_write(reg, patterns[i]), TAG,
                            "R%d write 0x%02x failed", reg, patterns[i]);
        ESP_RETURN_ON_ERROR(es8388_reg_read(reg, &rb), TAG, "R%d read failed", reg);
        if (rb != patterns[i]) {
            ESP_LOGE(TAG, "  R%d (%s): wrote 0x%02x, read 0x%02x -> FAIL",
                     reg, name, patterns[i], rb);
            es8388_reg_write(reg, orig);   /* 尽力还原 */
            return ESP_FAIL;
        }
    }

    ESP_RETURN_ON_ERROR(es8388_reg_write(reg, orig), TAG,
                        "R%d restore failed", reg);
    ESP_RETURN_ON_ERROR(es8388_reg_read(reg, &rb), TAG, "R%d read failed", reg);
    if (rb != orig) {
        ESP_LOGE(TAG, "  R%d (%s): restore failed (0x%02x != 0x%02x)",
                 reg, name, rb, orig);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "  R%d (%s): 0xA5/0x5A OK, restored 0x%02x", reg, name, orig);
    return ESP_OK;
}

/* 受控复位实验：故意把几个寄存器改成非默认值，再软复位，
 * 验证它们确实变回默认值。这是"复位生效"的直接证据——不依赖
 * 芯片开机时碰巧是什么状态。
 *
 * 选寄存器的原则（踩过坑）：
 *   1. 只选"所有 8 位都有定义"的寄存器。R25(0x19, DAC Control 3) 只定义
 *      bit7:6/5/3，bit4/2/1/0 是保留位；写 0x04 命中保留位 bit2 后，
 *      芯片进入未定义状态，回读变成 0xFF。**绝不要写保留位。**
 *   2. 选不影响输出通路的寄存器，保证全程无声。
 */
static esp_err_t reset_selftest(void)
{
    static const struct { uint8_t reg; uint8_t def; uint8_t dirty; const char *name; }
    cases[] = {
        { 0x09, 0x00, 0xBB, "MIC PGA gain" },      /* 8 位全定义：左/右 PGA 增益 */
        { 0x10, 0xC0, 0x50, "LADC volume" },       /* 8 位全定义：ADC 数字音量 */
        { 0x1A, 0xC0, 0x30, "LDAC volume" },       /* 8 位全定义：DAC 数字音量 */
    };

    /* 1) 弄脏 */
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t rb = 0;
        ESP_RETURN_ON_ERROR(es8388_reg_write(cases[i].reg, cases[i].dirty), TAG,
                            "dirty R0x%02x failed", cases[i].reg);
        ESP_RETURN_ON_ERROR(es8388_reg_read(cases[i].reg, &rb), TAG,
                            "read R0x%02x failed", cases[i].reg);
        if (rb != cases[i].dirty) {
            ESP_LOGE(TAG, "  R0x%02x: could not set dirty value", cases[i].reg);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "  chip deliberately dirtied (R9=0xBB, R16=0x50, R26=0x30)");

    /* 2) 复位 */
    ESP_RETURN_ON_ERROR(es8388_soft_reset(), TAG, "selftest reset failed");

    /* 3) 验证全部回到默认值 */
    int restored = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t rb = 0;
        ESP_RETURN_ON_ERROR(es8388_reg_read(cases[i].reg, &rb), TAG,
                            "read R0x%02x failed", cases[i].reg);
        if (rb == cases[i].def) {
            ESP_LOGI(TAG, "  R0x%02x (%s): 0x%02x -> reset -> 0x%02x  OK",
                     cases[i].reg, cases[i].name, cases[i].dirty, rb);
            restored++;
        } else {
            ESP_LOGE(TAG, "  R0x%02x (%s): dirty 0x%02x, after reset 0x%02x, "
                     "expected 0x%02x -> FAIL",
                     cases[i].reg, cases[i].name, cases[i].dirty, rb, cases[i].def);
        }
    }

    if (restored == 3) {
        ESP_LOGI(TAG, "  all 3 registers restored -> SCPReset CONFIRMED");
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t es8388_diag_run(void)
{
    ESP_RETURN_ON_ERROR(es8388_attach(), TAG, "attach failed");
    defaults_init();

    uint8_t before[ES8388_REG_MAX + 1];
    uint8_t after[ES8388_REG_MAX + 1];

    /* ---- 1. 复位前状态 ---- */
    ESP_LOGI(TAG, "=== step 1: dump BEFORE reset (chip keeps state across MCU reset) ===");
    ESP_RETURN_ON_ERROR(read_all(before), TAG, "pre-reset dump failed");
    print_dump("pre-reset:", before);
    int pre_mismatch = compare_defaults(before, false);

    /* ---- 2. 软复位 ---- */
    ESP_LOGI(TAG, "=== step 2: soft reset via R0 bit7 (SCPReset) ===");
    if (es8388_soft_reset() != ESP_OK) {
        ESP_LOGE(TAG, "RESULT: soft reset FAILED");
        return ESP_FAIL;
    }

    /* ---- 3. 复位后与默认值比对：这是复位生效的实证 ---- */
    ESP_LOGI(TAG, "=== step 3: dump AFTER reset, compare to datasheet defaults ===");
    ESP_RETURN_ON_ERROR(read_all(after), TAG, "post-reset dump failed");
    print_dump("post-reset:", after);
    int post_mismatch = compare_defaults(after, true);

    /* ---- 4. 写-回读-还原 ---- */
    ESP_LOGI(TAG, "=== step 4: write-readback-restore ===");
    esp_err_t r1 = write_readback_test(0x09, "MIC PGA gain");
    esp_err_t r2 = write_readback_test(0x1A, "LDAC digital vol");

    /* ---- 5. 受控复位实验：直接证明 SCPReset 生效 ---- */
    ESP_LOGI(TAG, "=== step 5: controlled reset test (dirty -> reset -> verify) ===");
    esp_err_t r3 = reset_selftest();

    /* ---- 结论 ---- */
    ESP_LOGI(TAG, "--- summary ---");
    ESP_LOGI(TAG, "  pre-reset mismatches : %d (leftover from previous firmware)",
             pre_mismatch);
    ESP_LOGI(TAG, "  post-reset mismatches: %d", post_mismatch);
    if (r1 == ESP_OK && r2 == ESP_OK) {
        ESP_LOGI(TAG, "  write/read path      : VERIFIED");
    } else {
        ESP_LOGE(TAG, "  write/read path      : FAILED");
        return ESP_FAIL;
    }
    if (r3 == ESP_OK) {
        ESP_LOGI(TAG, "  soft reset           : CONFIRMED");
    } else {
        ESP_LOGE(TAG, "  soft reset           : FAILED");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RESULT: register control path VERIFIED, chip in known state");
    return ESP_OK;
}
