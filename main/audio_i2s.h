/*
 * audio_i2s.h —— I2S 音频通路（语音翻译机 第 2/3/5 步）
 *
 * 硬件接线（与 xiaozhi atk-dnesp32s3m-wifi 板一致）：
 *   MCLK=IO6  BCLK=IO7  WS=IO16  DOUT=IO15(播放)  DIN=IO17(录音)
 *   SPK_EN=IO42（高电平使能功放）
 *
 * 参数选择：
 *   24 kHz / 16 bit / 立体声，ESP32 作 I2S 主，ES8388 作从。
 *   MCLK = 24k x 256 = 6.144 MHz
 *   BCLK = 24k x 16 x 2 = 768 kHz
 *   WS   = 24 kHz
 *   立体声是必须的：I2S 标准模式下每个 WS 周期两个 slot，
 *   ES8388 也是按双声道取数，单声道配置会导致左右声道错位。
 *
 * 全双工：TX/RX 在同一 I2S 端口上创建，共享 BCLK/WS。
 * ES8388 的 ADC 和 DAC 共用同一对 LRCK（DACCONTROL21=0x80），
 * 所以录音和播放必须同采样率——这也是选 24k 全程统一的根本原因。
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE     24000
#define AUDIO_CHANNELS        2
#define AUDIO_BITS            16
#define AUDIO_MCLK_MULTIPLE   256

/* 单帧 = 左右各 16bit = 4 字节 */
#define AUDIO_FRAME_BYTES     (AUDIO_CHANNELS * AUDIO_BITS / 8)
/* 20ms 一帧的音量，与服务端协议的 960 字节帧长对齐 */
#define AUDIO_FRAME_20MS      (AUDIO_SAMPLE_RATE / 50)

/* 初始化 I2S（全双工）+ 配置 ES8388 播放通路 + 打开功放。
 * 幂等。 */
esp_err_t audio_init(void);

/* 播放 PCM：16bit 立体声交织，长度须为 4 的倍数。阻塞到写完。 */
esp_err_t audio_write(const void *pcm, size_t bytes);

/* 连续写静音，用于保活时钟（不需要发声时也要写，否则 DMA 空转）。 */
esp_err_t audio_write_silence(uint32_t ms);

/* 播放一段单频正弦（用于喇叭/功放验证）。振幅是满量程的比例，0~1。 */
esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t ms, float amplitude);

/* 打印 I2S 时钟实测配置（软件侧换算值，用于和示波器对照） */
void audio_log_clocks(void);

#ifdef __cplusplus
}
#endif
