/*
 * es8388.h —— ES8388 底层控制接口（寄存器读写 + 软复位）
 *
 * 这是 codec 控制的最小单元，供诊断代码和后续 codec 初始化共用。
 * 上层不应直接操作 I2C 句柄。
 *
 * 关于软复位（重要）：
 *   R0(0x00) 的 bit7 = SCPReset，写 1 让控制端口寄存器恢复默认值。
 *   该位**不自清零**，写 0x80 之后必须显式写回 0x00（或默认态 0x06），
 *   否则寄存器会一直停留在复位状态，后续写入都不生效。
 *   ES8388 没有硬件复位引脚，且断电前一直保持寄存器内容，
 *   所以每次初始化都应该先软复位，才能得到确定的状态。
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 关键寄存器地址（完整表见数据手册第 6 章） */
#define ES8388_REG_CONTROL1     0x00   /* Chip Control 1: SCPReset(bit7), EnRef(bit2), VMIDSEL(1:0) */
#define ES8388_REG_CONTROL2     0x01   /* Chip Control 2: PdnAna(bit3) */
#define ES8388_REG_CHIPPOWER    0x02   /* Chip Power Management */
#define ES8388_REG_ADCPOWER     0x03
#define ES8388_REG_DACPOWER     0x04
#define ES8388_REG_MASTERMODE   0x08   /* Master Mode: MSC(bit7) 0=slave */
#define ES8388_REG_ADCCONTROL1  0x09   /* MIC PGA gain */
#define ES8388_REG_DACCONTROL1  0x17
#define ES8388_REG_DACCONTROL24 0x2e   /* LOUT1 volume */
#define ES8388_REG_DACCONTROL26 0x30   /* LOUT2 volume */

/* 软复位后写入 R0 的值：数据手册默认态
 * bit2 EnRef=1（使能参考）、bit1:0 VMIDSEL=10（500kΩ 分压） */
#define ES8388_R0_DEFAULT       0x06

/* 软复位位 */
#define ES8388_SCPRESET_BIT     0x80

#define ES8388_RESET_DELAY_MS   100

/* 在已初始化的 codec_bus 上挂载 ES8388 设备（幂等） */
esp_err_t es8388_attach(void);

/* 单寄存器读写 */
esp_err_t es8388_reg_read(uint8_t reg, uint8_t *val);
esp_err_t es8388_reg_write(uint8_t reg, uint8_t val);

/*
 * 软复位：R0 写 0x80 -> 等待 100ms -> R0 写 0x06 解除复位。
 * 解除后回读校验：bit7 必须为 0。
 */
esp_err_t es8388_soft_reset(void);

#ifdef __cplusplus
}
#endif
