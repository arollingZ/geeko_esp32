/*
 * resampler.h —— 有理数（分数倍）重采样器
 *
 * 用途：录音/播放都在 24 kHz（ES8388 的 ADC 与 DAC 共用一对 LRCK，
 * 不可能给两边设不同采样率），但 Opus 要 16 kHz。
 * 所以必须在 24k <-> 16k 之间做高质量转换。
 *
 * 为什么不用"隔点抽样"或线性插值：
 *   - 直接丢掉每 3 个里的 1 个（24k->16k）会把 8 kHz 以上的能量折叠回
 *     可听带内（混叠），听感是嘶哑的金属音，且无法用后级滤波挽回
 *   - 线性插值在高频段衰减明显，语音的齿音会发闷
 * 所以用标准的"升采样 -> 抗混叠低通 -> 降采样"结构，以多相形式实现。
 *
 * 数学形式（L = 升采样倍率，M = 降采样倍率，原型滤波器在 L*fs_in 网格上）：
 *
 *     y[m] = Σ_j  h[m*M + D - j*L] * x[j]      D = (taps-1)/2
 *
 * D 是线性相位滤波器的群延迟，加上它相当于把滤波器"居中对齐"到
 * 输入的时间轴上，因此输出与输入**零延迟、零相移**。
 * 这一点很关键：如果 D 取错（例如滤波器长度取偶数、D 只好取整），
 * 会引入半个采样的偏差，正弦测试的误差会从 90 dB 掉到 23 dB 左右。
 *
 * 24k <-> 16k 的比率为 3:2，故 LCM = 48 kHz，原型滤波器设计在 48k 上：
 *   24k -> 16k : L=2, M=3
 *   16k -> 24k : L=3, M=2
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 原型滤波器阶数（奇数！偶数会导致半个采样的群延迟，见上） */
#define RS_TAPS           97

/* 截止频率 = 该系数 × min(fs_in, fs_out)/2。
 * 0.9 给过渡带留出余量，保证 16k 的 8 kHz Nyquist 附近被充分压制。 */
#define RS_CUTOFF_RATIO   0.90f

/* 支持的倍率上限（3:2 只需要 3） */
#define RS_MAX_LM         8

typedef struct {
    uint32_t fs_in;
    uint32_t fs_out;
    int      L;                 /* 升采样倍率 */
    int      M;                 /* 降采样倍率 */
    int      taps;              /* 实际使用的阶数 */
    int      delay;             /* (taps-1)/2，原型网格上的群延迟 */
    float    h[RS_TAPS];
    bool     ready;
} resampler_t;

/**
 * @brief 初始化重采样器（设计滤波器系数）
 *
 * @param fs_in   输入采样率
 * @param fs_out  输出采样率
 * @return ESP_OK；比率无法用不超过 RS_MAX_LM 的整数表达时返回
 *         ESP_ERR_NOT_SUPPORTED
 */
esp_err_t resampler_init(resampler_t *rs, uint32_t fs_in, uint32_t fs_out);

/** 给定输入帧数，输出帧数（向上取整，与 resampler_process 一致） */
size_t resampler_out_frames(const resampler_t *rs, size_t in_frames);

/**
 * @brief 整段重采样（非流式）
 *
 * 首尾以 0 补齐，因此理论上两端各有约 (taps/2)/fs 的过渡区。
 * 做误差统计时应跳过首尾各 10~20 ms。
 *
 * @param out_cap      输出缓冲可容纳的帧数
 * @param out_frames   实际输出帧数（可为 NULL）
 */
esp_err_t resampler_process(const resampler_t *rs,
                            const int16_t *in, size_t in_frames,
                            int16_t *out, size_t out_cap, size_t *out_frames);

/**
 * @brief 自检：用 1 kHz 正弦走一遍，报告与理想正弦的偏差
 *
 * 用于把"重采样本身有问题"和"Opus 编解码有问题"这两类原因分开。
 * 正常应在 80 dB 以上。
 *
 * @param snr_db 输出实测信噪比（可为 NULL）
 */
esp_err_t resampler_selftest(float *snr_db);

#ifdef __cplusplus
}
#endif
