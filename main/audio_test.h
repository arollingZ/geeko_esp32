/*
 * audio_test.h —— 喇叭/功放验证任务（语音翻译机 第 5 步）
 *
 * 起一个独立任务：初始化 I2S + codec，然后周期性地
 * 「静音 2s -> 1kHz 正弦 400ms」，方便用耳朵确认通路，
 * 同时让 MCLK/BCLK/WS 一直在跑，方便用示波器量。
 *
 * 关掉蜂鸣（APP_AUDIO_BEEP=0）后只写静音，时钟照旧，
 * 可以安静地量时钟、不会打扰。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_test_start(void);

#ifdef __cplusplus
}
#endif
