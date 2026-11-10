/*
 * button.h —— BOOT 按键（IO0）长按/短按识别
 *
 * 判定逻辑（按下时长以"松开时刻"为准反推）：
 *   按下                          -> 记下按下时刻
 *   按住达到 LONG_PRESS_MS        -> 立刻上报 BUTTON_EVT_LONG_START
 *                                    （此刻开始录音，不等松开）
 *   松开时已经上报过 LONG_START   -> 上报 BUTTON_EVT_LONG_END
 *                                    （此刻停止录音）
 *   松开但未满 LONG_PRESS_MS      -> 上报 BUTTON_EVT_SHORT_PRESS
 *                                    （触发回放）
 *
 * 为什么长按在"达到阈值那一刻"就上报，而不是等松开：
 * 录音要尽量早开始，否则等到松开才开始，整段话都录不到。
 *
 * 注意 IO0 是 strapping 脚：按住 BOOT 上电会进下载模式，这是硬件行为，
 * 无法在软件里规避。正常运行时读它没有副作用。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_EVT_SHORT_PRESS = 0,  /* 短按：按下时长 < 阈值 */
    BUTTON_EVT_LONG_START,       /* 长按开始：按住已满阈值 */
    BUTTON_EVT_LONG_END,         /* 长按结束：松开 */
} button_event_t;

typedef void (*button_event_cb_t)(button_event_t evt);

/**
 * @brief 启动按键扫描任务
 *
 * @param cb 事件回调，在按键任务上下文执行。**必须立刻返回**：
 *           内部不能做耗时操作（本项目的用法是只投递命令给录音任务）。
 * @return ESP_OK 成功
 */
esp_err_t button_init(button_event_cb_t cb);

#ifdef __cplusplus
}
#endif
