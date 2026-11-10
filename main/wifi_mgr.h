/*
 * WiFi 管理模块 —— SoftAP Web 配网 + STA 连接与断线重连
 *
 * 功能（对应 readme.txt 功能点 2）：
 *   1. 设备常开 SoftAP「ESP32_DEMO」，手机连接后浏览器访问任意网址
 *      即弹出配网页（HTTP 80 端口，所有 GET 都返回配网界面）。
 *   2. 配网页实时显示设备扫描到的附近热点，选择热点并填入密码提交。
 *   3. 凭据保存：TF 卡可用（APP_ENABLE_TF_CARD=1 且挂载成功）时写入
 *      /sdcard/wifi_cfg.txt；否则写入 NVS。重启后自动读取并恢复连接。
 *   4. STA 断线自动重连；SoftAP 常开，随时可连回设备重新配网。
 *   5. wifi_mgr_get_state() 提供状态查询，供后续 LED(功能3)/LCD(功能4) 复用：
 *      NO_CRED -> CONNECTING -> CONNECTED -> INTERNET_OK。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 设备默认热点名（readme 要求），手机连上后浏览器访问任意网址弹出配网页 */
#define WIFI_MGR_AP_SSID  "ESP32_DEMO"

/** WiFi 管理状态机 */
typedef enum {
    WIFI_MGR_STATE_NO_CRED = 0,  /* 无凭据，等待配网 */
    WIFI_MGR_STATE_CONNECTING,   /* 有凭据，正在连接 */
    WIFI_MGR_STATE_CONNECTED,    /* 已连接热点并获取 IP，外网未验证 */
    WIFI_MGR_STATE_INTERNET_OK,  /* 外网验证通过 */
} wifi_mgr_state_t;

/**
 * @brief 初始化并启动 WiFi 管理模块（非阻塞，内部起 AP + HTTP 配网服务）
 *
 * 流程：NVS/网络栈初始化 -> 读取凭据 -> 启动 SoftAP ESP32_DEMO ->
 *       有凭据则自动发起 STA 连接，无凭据则等待配网。
 *
 * @return ESP_OK 成功；否则返回错误码
 */
esp_err_t wifi_mgr_init(void);

/**
 * @brief 主循环周期调用（建议 1s 一次）
 *
 * 内部做节流的外网连通性验证与状态变化日志，不阻塞。
 */
void wifi_mgr_poll(void);

/** 当前状态（LED/LCD 指示用） */
wifi_mgr_state_t wifi_mgr_get_state(void);

/** 当前连接/已保存的 SSID（未配网时返回 NULL） */
const char *wifi_mgr_get_ssid(void);

/**
 * @brief 临时关闭/恢复射频（仅用于录音噪声诊断）
 *
 * 用来做 A/B 对照：关掉射频后录音若变干净，说明干扰来自 WiFi。
 * 不是正常运行路径；resume 之后由 WiFi 驱动自行重连。
 */
void wifi_mgr_radio_pause(void);
void wifi_mgr_radio_resume(void);

#ifdef __cplusplus
}
#endif
