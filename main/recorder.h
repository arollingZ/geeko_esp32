/*
 * recorder.h —— 录音与回放（内存缓冲，不落盘）
 *
 * 状态机：
 *   IDLE ──长按开始──> RECORDING ──松开/录满──> IDLE
 *   IDLE ──短按────> PLAYING ────播完/再按───> IDLE
 *
 * 存储：单声道 16bit / 24kHz，约 48 KB/s。
 *   优先放 PSRAM（这块板 N16R8 有 8MB），失败则退回内部 RAM 并降级时长。
 *   选单声道而不是立体声的原因：
 *     1. 板载麦克风只有一路，立体声的另一路是重复/空数据，白占一半
 *     2. 服务端协议本来就是 24k 单声道（20ms = 960 字节），格式直接对齐
 *
 * 为什么必须 24kHz：ES8388 的 ADC 和 DAC 共用同一对 LRCK
 * （DACCONTROL21=0x80），录音和播放不可能用不同采样率。
 *
 * 线程模型：内部一个任务负责所有阻塞 I/O（I2S 读写），
 * 对外接口只投递命令、立即返回，所以按钮任务和 UI 任务都不会被阻塞。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RECORDER_IDLE = 0,      /* 空闲：LCD 显示 WiFi 名 */
    RECORDER_RECORDING,     /* 录音中：LCD 显示录音时长 */
    RECORDER_PLAYING,       /* 回放中：LCD 显示总时长 + 剩余 */
    RECORDER_NO_DATA,       /* 短按但没有录音：短暂提示，自动回 IDLE */
} recorder_state_t;

/* 初始化：分配缓冲 + 启动任务。任务内部会异步初始化音频通路
 * （audio_init 约需 200ms），不阻塞开机。 */
esp_err_t recorder_init(void);

/* 以下四个接口都是"投递命令后立即返回"，不会阻塞调用方 */
void recorder_record_start(void);   /* 长按触发 */
void recorder_record_stop(void);    /* 长按松开触发 */
void recorder_play_start(void);     /* 短按触发 */
void recorder_play_stop(void);      /* 需要中断回放时用 */

/* ---- 供 UI 轮询的状态查询（都是无锁读取，允许瞬时不一致）---- */
recorder_state_t recorder_get_state(void);

/* 已录时长（录音中实时增长） */
uint32_t recorder_recorded_ms(void);

/* 当前录音总时长（回放时用于显示"总共多少秒"） */
uint32_t recorder_total_ms(void);

/* 回放剩余时长（回放时用于显示"还剩多少秒"） */
uint32_t recorder_remaining_ms(void);

bool recorder_has_data(void);

#ifdef __cplusplus
}
#endif
