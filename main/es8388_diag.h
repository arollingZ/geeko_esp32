/*
 * es8388_diag.h —— ES8388 寄存器读写诊断（语音翻译机 第 4 步）
 *
 * 三段式验证：
 *   1. 复位前 dump：查看芯片当前状态（可能残留上次固件的配置）
 *   2. 软复位：R0 bit7 SCPReset
 *   3. 复位后 dump：与数据手册默认值比对，证明复位真实生效
 *   4. 写-回读-还原测试：证明写方向可用（0xA5/0x5A 互补花样）
 *
 * 安全性：复位后 DAC 掉电、输出全关、且无 MCLK，物理上不会发声。
 * 测试寄存器为 MIC PGA(0x09) 与 LDAC 数字音量(0x1A)，均不影响输出。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 跑一次完整诊断，返回写-回读测试是否通过 */
esp_err_t es8388_diag_run(void);

#ifdef __cplusplus
}
#endif
