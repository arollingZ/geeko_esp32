/*
 * 功能开关配置 —— 各功能点通过宏 + 条件编译控制，方便硬件未就绪时
 * 暂时屏蔽某个功能，而不必改动功能代码本身。
 *
 * 规则：
 *   - 宏为 1 时该功能参与编译；为 0 时功能代码被屏蔽，对外接口
 *     保留桩实现（返回未启用/空值），调用方无需修改。
 */
#pragma once

/* ---------------- 功能 1：TF 卡（SPI 模式） ----------------
 * 2026-11-04：暂时屏蔽 —— 手头暂无 TF 卡，待找到后改回 1 调试。
 * 引脚：CS=IO2 / MOSI=IO11 / SCK=IO12 / MISO=IO13（板载 TF 卡座）
 */
#define APP_ENABLE_TF_CARD   0

/* ---------------- 功能 2：WiFi 配网 ----------------
 * SoftAP ESP32_DEMO + Web 配网（手机连上后浏览器访问任意网址弹出配网页），
 * 凭据优先存 TF 卡（APP_ENABLE_TF_CARD=0 时存 NVS），断线自动重连。
 */
#define APP_ENABLE_WIFI      1

/* ---------------- 功能 3：LED 状态指示（IO1） ----------------
 * 按 readme.txt 要求指示网络状态：
 *   NO_CRED      -> 熄灭
 *   CONNECTING   -> 快闪 2 下（亮100ms/灭100ms/亮100ms）后灭 1s，循环
 *   CONNECTED    -> 慢闪（亮500ms/灭500ms）
 *   INTERNET_OK  -> 常亮
 * 依赖功能 2 的 wifi_mgr_get_state()；APP_ENABLE_WIFI=0 时 LED 仅保持熄灭。
 */
#define APP_ENABLE_LED       1

/* ---------------- 功能 4：LCD 显示（ST7789，0.96 寸 80x160） ----------------
 * 引脚：CS=IO39 / DC=IO40 / RST=IO38 / BL=IO41，数据线 SCLK=IO12 / MOSI=IO11
 * 注意：数据线与 TF 卡共用同一条 SPI2 总线，靠各自 CS 分时复用。
 */
#define APP_ENABLE_LCD       1
