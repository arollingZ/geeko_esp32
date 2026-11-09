/*
 * es8388_diag.h —— ES8388 寄存器读写诊断（语音翻译机 第 4 步）
 *
 * 目的：验证控制通路的"写"方向（第 1 步只验证了 ACK 应答）。
 *   1. 全寄存器 dump（0x00~0x34），与数据手册复位默认值比对；
 *   2. 写-回读-还原测试：挑两个确定可读写、且不会发声的寄存器，
 *      写入 0xA5/0x5A 花样并回读校验，最后还原原值。
 *
 * 安全性：测试用的寄存器是 MIC PGA 增益(0x09) 和 DAC 数字音量(0x1A)。
 * 复位状态下 DAC 已掉电（R4=0xC0）、输出全部关闭（R4 bit3:0=0），
 * 且没有 MCLK，无论写什么都不会出声。绝不写 R0（bit7 会触发寄存器复位）。
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读/写单个寄存器（供后续 codec 初始化代码复用） */
esp_err_t es8388_reg_read(uint8_t reg, uint8_t *val);
esp_err_t es8388_reg_write(uint8_t reg, uint8_t val);

/*
 * 跑一次完整诊断：dump + 默认值比对 + 写回读测试。
 * 返回写回读测试是否通过（ESP_OK = 全部通过）。
 */
esp_err_t es8388_diag_run(void);

#ifdef __cplusplus
}
#endif
