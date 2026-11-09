/*
 * codec_bus.h —— ES8388 控制面 I2C 总线
 *
 * 硬件接线（与 xiaozhi atk-dnesp32s3m-wifi 板一致）：
 *   SDA = GPIO4, SCL = GPIO5
 *   ES8388 7 位地址 = 0x10（源码里写的 0x20 是 8 位写法，右移一位后才是 0x10）
 *
 * 本模块只负责"控制通路"：初始化 I2C 主机总线、扫描设备。
 * 后续 codec 寄存器配置（功能 6）会复用这里的总线句柄。
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ES8388 7 位从机地址（7-bit scanner 应扫到 0x10；扫到 0x11 说明 CE 脚被拉高） */
#define ES8388_I2C_ADDR_7BIT    0x10

/*
 * 初始化 I2C 主机总线（100 kHz，扫描用低速最稳）。
 * 重复调用直接返回 ESP_OK（幂等）。
 */
esp_err_t codec_bus_init(void);

/* 取总线句柄；未初始化返回 NULL */
i2c_master_bus_handle_t codec_bus_handle(void);

/*
 * 扫描 0x08~0x77 全部从机地址，日志打印 i2cdetect 风格网格，
 * 并对扫到的每个地址给出已知器件猜测（0x10 = ES8388）。
 * 返回扫到的设备数量；总线未初始化返回 -1。
 */
int codec_bus_scan(void);

#ifdef __cplusplus
}
#endif
