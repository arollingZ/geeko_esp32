/*
 * TF 卡（SD 卡）读写模块 —— SPI 模式
 *
 * 硬件连接（ESP32-S3M-N16R8）:
 *   CS   -> IO2
 *   MOSI -> IO11
 *   SCK  -> IO12
 *   MISO -> IO13
 *
 * 卡内文件系统为 FAT，挂载到 /sdcard，可直接使用 fopen/fread/fwrite 等 POSIX 接口。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载点，例如 "/sdcard/wifi_cfg.txt" */
#define TFCARD_MOUNT_POINT  "/sdcard"

/**
 * @brief 初始化 SPI 总线并挂载 TF 卡
 *
 * 卡需要是 FAT16/FAT32 分区；默认不会自动格式化（避免误删卡内数据）。
 * 重复调用是安全的（已挂载则直接返回 ESP_OK）。
 *
 * @return ESP_OK 成功；否则返回底层错误码（ESP_ERR_TIMEOUT 多为接线/供电问题）
 */
esp_err_t tf_card_init(void);

/**
 * @brief 卸载文件系统并释放 SPI 总线
 */
esp_err_t tf_card_deinit(void);

/**
 * @brief TF 卡是否已经挂载成功
 */
bool tf_card_is_mounted(void);

/**
 * @brief 获取 sdmmc 卡句柄（未挂载时返回 NULL）
 */
sdmmc_card_t *tf_card_handle(void);

/**
 * @brief 打印卡信息（名称、类型、容量、总线频率等）与剩余空间
 */
void tf_card_print_info(void);

/**
 * @brief 读写准确性自检
 *
 * 流程：写入 64KB 递增图案数据 -> 读回逐字节比对 + CRC32 校验 ->
 *       追加写入校验 -> 指定偏移读取校验 -> 列出根目录文件。
 *
 * @return ESP_OK 全部校验通过；否则返回失败原因
 */
esp_err_t tf_card_selftest(void);

/**
 * @brief 便捷写入：一次性把 data 写入 path（覆盖写）
 */
esp_err_t tf_card_write_file(const char *path, const void *data, size_t len);

/**
 * @brief 便捷读取：从 path 读取最多 len 字节到 buf
 *
 * @param out_len 实际读取到的字节数，可为 NULL
 */
esp_err_t tf_card_read_file(const char *path, void *buf, size_t len, size_t *out_len);

/**
 * @brief 列出目录内容（默认用于查看卡内文件）
 */
esp_err_t tf_card_list_dir(const char *path);

#ifdef __cplusplus
}
#endif
