/*
 * opus_pipeline.h —— 录音 -> 重采样 -> Opus 编码 -> 打印 -> 解码 -> 比对
 *
 * 用途：在设备上闭环验证 Opus 编解码的准确性，不依赖服务器。
 *
 * 流程（全部在设备上完成）：
 *   1. 录音是 24 kHz 单声道（ES8388 的 ADC/DAC 共用 LRCK，无法改成 16k）
 *   2. 用多相 FIR 重采样到 16 kHz（Opus 的宽带档）
 *   3. Opus 编码，20 ms 一帧 = 320 采样
 *   4. 把编码后的字节以十六进制打印到串口（可配置打印多少个包）
 *   5. 再用 Opus 解码回 PCM
 *   6. 与第 2 步的 16 kHz 参考信号逐点比对，给出 SNR / 相关系数 / 峰值误差
 *
 * 比对为什么要先对齐：
 *   Opus 编码器有前瞻（lookahead），解码输出相对输入会有几毫秒延迟。
 *   直接逐点相减会得到一个巨大的"误差"，看起来像编解码坏了，其实是错位。
 *   所以先搜索最佳延迟，再在重叠区计算误差，并把延迟一并报出来。
 *
 * 注意：编码是有损的，SNR 不可能无限高。语音在 24 kbps 下大致
 * 10~20 dB，相关系数 0.99 以上属于正常。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 输入 */
    size_t   in_frames_24k;      /* 实际参与的 24k 帧数（可能被上限截断） */

    /* 重采样 */
    size_t   frames_16k;         /* 16k 帧数 */
    int      has_resampler_snr;  /* 保留 */

    /* 编码 */
    int      packets;            /* 包数 */
    size_t   opus_bytes;         /* 编码总字节 */
    size_t   pkt_min;
    size_t   pkt_max;
    double   kbps_actual;

    /* 解码与比对 */
    size_t   dec_frames;
    int      delay_samples;      /* 自动对齐求出的延迟（16k 采样点） */
    double   snr_db;
    double   corr;
    int32_t  max_abs_err;
    double   rms_ref_dbfs;
    double   rms_dec_dbfs;

    /* 耗时 */
    uint32_t resample_ms;
    uint32_t encode_ms;
    uint32_t decode_ms;
    uint32_t total_ms;
} opus_result_t;

/**
 * @brief 对一段 24 kHz 单声道 PCM 做完整的编解码验证
 *
 * @param pcm24k    24 kHz 单声道 int16 样本
 * @param frames24k 帧数（会被 OPUS_VERIFY_MAX_SECONDS 截断）
 * @param res       结果输出（可为 NULL，此时只打印不返回）
 *
 * @return ESP_OK 编码与解码都成功；比对指标无论如何都会打印
 */
esp_err_t opus_verify_run(const int16_t *pcm24k, size_t frames24k,
                          opus_result_t *res);

/**
 * @brief 编解码自检：不依赖麦克风，用合成语音包络信号走一遍
 *
 * 用于区分"麦克风/环境问题"和"Opus 链路问题"。
 * 同时会先跑 resampler_selftest()，把重采样和编码器的问题分开定位。
 */
esp_err_t opus_verify_selftest(void);

#ifdef __cplusplus
}
#endif
