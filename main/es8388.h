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
#include <stdbool.h>
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

/*
 * 配置为「I2S 从机 + 16bit + 播放」状态。
 *
 * 前提：调用前必须已做过 es8388_soft_reset()，且 I2S 主时钟已在运行
 * （ES8388 的 ADC/DAC 依赖 MCLK，没有时钟时配置能写进去但不会工作）。
 *
 * 所有依赖的寄存器都显式写入，不依赖复位默认值——这是上一步 dump 出
 * 芯片残留旧固件配置后定下的规矩。
 */
esp_err_t es8388_config_playback(void);

/*
 * DAC 输出级开关（R4 DACPOWER）。
 * 配置阶段保持关闭（0xC0），等 I2S 时钟稳定且数据线上是静音后再打开，
 * 可以避免上电瞬间的爆音。
 */
esp_err_t es8388_output_enable(bool on);

/*
 * DAC 静音开关（R0x19 的 bit2）。
 * 配置阶段默认静音，避免配置过程中的杂音；开始播放在即再解除。
 */
esp_err_t es8388_mute(bool mute);

/* 打印全部 0x00~0x34 寄存器 + 关键值解读，用于定位"没声音" */
void es8388_dump_registers(void);

/*
 * 音量设置。dB 范围 -96 ~ 0，0 为最大。
 * 数字音量（0x1A/0x1B）与模拟输出音量（0x2E~0x31）一起调整。
 */
esp_err_t es8388_set_volume_db(int8_t db);

#ifdef __cplusplus
}
#endif
