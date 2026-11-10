/*
 * WiFi 管理模块 —— SoftAP Web 配网 + STA 连接与断线重连
 *
 * 实现要点：
 *  - APSTA 模式：SoftAP「ESP32_DEMO」常开（配网入口），STA 负责连接路由器。
 *  - HTTP 服务监听 80 端口，任意 GET URI（含 generate_204 等探测路径）都
 *    返回配网页，手机连上后浏览器输入任意网址即可进入配网界面。
 *  - 配网页实时扫描附近热点（节流 5s），下拉选择 + 手动输入 + 密码提交。
 *    扫描前若 STA 正在连接（换地方后旧热点不在），会临时暂停自动重连并断开，
 *    否则 IDF 不允许扫描，列表永远是空的。
 *  - 凭据保存：TF 卡可用时写 /sdcard/wifi_cfg.txt（key=value 两行），
 *    同时双写 NVS 兜底；TF 卡屏蔽期间仅写 NVS。加载优先 TF 卡。
 *  - 断线自动重连（事件驱动），SoftAP 常开支持随时重新配网。
 *  - 外网连通性验证：GET generate_204（小米探测地址，国内可达）。
 */

#include "wifi_mgr.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "feature_config.h"
#if APP_ENABLE_TF_CARD
#include "tf_card.h"
#endif

static const char *TAG = "WIFI_MGR";
static const char *TAG_P = "PORTAL";

/* ---------------- 配置 ---------------- */
#define WIFI_AP_SSID          WIFI_MGR_AP_SSID  /* 设备热点名，宏定义在 wifi_mgr.h */
#define WIFI_AP_CHANNEL       1
#define WIFI_AP_MAX_CONN      4
#define WIFI_STA_SSID_MAX     33                /* 802.11 SSID <= 32 字节 + NUL */
#define WIFI_STA_PASS_MAX     65                /* WPA 密码 <= 64 字节 + NUL */
#define WIFI_CRED_NVS_NS      "demo_wifi"
#define WIFI_CRED_NVS_SSID    "ssid"
#define WIFI_CRED_NVS_PASS    "pass"
#define WIFI_CRED_FILE        TFCARD_MOUNT_POINT "/wifi_cfg.txt"
/* 外网探测地址：返回 HTTP 204 即视为联网成功，可按需替换 */
#define NET_CHECK_URL         "http://connect.rom.miui.com/generate_204"
#define NET_CHECK_PERIOD_MS   10000
#define PORTAL_SCAN_MIN_MS    5000
#define PORTAL_SCAN_MAX_APS   20
/* 配网页扫描热点时暂停自动重连的时长：扫描期间 STA 必须空闲，否则 IDF 拒绝扫描。
 * 到期由 wifi_mgr_poll() 恢复重连，避免用户只开页面不提交时设备一直不联网。 */
#define PORTAL_SCAN_PAUSE_MS  10000
#define HEARTBEAT_PERIOD_MS   30000
#define PORTAL_DNS_PORT       53      /* DNS 劫持端口：所有域名解析到本机 */
#define PORTAL_DNS_TASK_STACK 4096
#define PORTAL_DNS_TASK_PRIO  3

/* ---------------- 状态 ---------------- */
static wifi_mgr_state_t s_state = WIFI_MGR_STATE_NO_CRED;
static char s_ssid[WIFI_STA_SSID_MAX] = { 0 };
static char s_pass[WIFI_STA_PASS_MAX] = { 0 };
static httpd_handle_t s_httpd = NULL;
static uint32_t s_retry_count = 0;

/* ---------------- 扫描 / 重连互斥 ----------------
 * STA 处于 connecting 时 esp_wifi_scan_start() 返回 ESP_ERR_WIFI_STATE。
 * 设备换了地方后旧热点不存在，断线事件会立刻触发重连，STA 几乎一直停在
 * connecting —— 配网页因此扫不到任何热点，用户也就无法重新配网。
 * 扫描前用下面的标志临时"按住"自动重连并主动断开，扫描结束后由 poll 恢复。
 * 访问来自 httpd 任务 / WiFi 事件任务 / 主循环任务，统一用临界区保护。 */
static portMUX_TYPE s_reconnect_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_reconnect_paused = false;
static int64_t s_reconnect_resume_us = 0;

/* AP / STA 网络接口（DNS 劫持、302 跳转、链路状态判断都要用到） */
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_ip4_addr_t s_ap_ip = { 0 };
static TaskHandle_t s_dns_task = NULL;

/* 配网页缓存（扫描结果拼接而成，按时间节流重建） */
static char *s_page_cache = NULL;
static int64_t s_page_cache_us = 0;

static int64_t s_last_netcheck_us = 0;
static int64_t s_last_heartbeat_us = 0;

/* ==================== 小工具 ==================== */

/* 有界字符串拼接缓冲 */
typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
} sbuf_t;

static void sbuf_append(sbuf_t *b, const char *fmt, ...)
{
    if (b->buf == NULL || b->pos >= b->cap - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->pos, b->cap - b->pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->pos += (size_t)n;
        if (b->pos > b->cap - 1) {
            b->pos = b->cap - 1;
        }
    }
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL 解码：%XX 与 '+' -> 空格（application/x-www-form-urlencoded） */
static bool url_decode(const char *src, size_t len, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < dst_sz; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= len) {
                return false;
            }
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return o < dst_sz;
}

/* 从 POST body 里取表单字段（带边界与解码校验） */
static bool form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == body || p[-1] == '&') && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = (end != NULL) ? (size_t)(end - v) : strlen(v);
            return url_decode(v, vlen, out, out_sz);
        }
        p += klen;
    }
    return false;
}

/* HTML 转义追加（& < > "），溢出截断保护 */
static void html_escape_append(sbuf_t *b, const char *src)
{
    for (; *src != '\0'; src++) {
        if (b->pos >= b->cap - 8) {
            return;
        }
        switch (*src) {
        case '&': sbuf_append(b, "&amp;");  break;
        case '<': sbuf_append(b, "&lt;");   break;
        case '>': sbuf_append(b, "&gt;");   break;
        case '"': sbuf_append(b, "&quot;"); break;
        default:  sbuf_append(b, "%c", *src); break;
        }
    }
}

static const char *state_name(wifi_mgr_state_t st)
{
    switch (st) {
    case WIFI_MGR_STATE_NO_CRED:     return "no_cred";
    case WIFI_MGR_STATE_CONNECTING:  return "connecting";
    case WIFI_MGR_STATE_CONNECTED:   return "connected";
    case WIFI_MGR_STATE_INTERNET_OK: return "internet_ok";
    default:                         return "unknown";
    }
}

/* ==================== 凭据存取 ==================== */

#if APP_ENABLE_TF_CARD
/* TF 卡凭据文件为两行 key=value：
 *   ssid=你的热点名
 *   pass=你的密码
 */
static bool cred_load_from_tf(void)
{
    char buf[WIFI_STA_SSID_MAX + WIFI_STA_PASS_MAX + 32];
    if (!tf_card_is_mounted()) {
        return false;
    }
    size_t rd = 0;
    if (tf_card_read_file(WIFI_CRED_FILE, buf, sizeof(buf) - 1, &rd) != ESP_OK || rd == 0) {
        return false;
    }
    buf[rd] = '\0';

    bool got_ssid = false;
    bool got_pass = false;
    char ssid[WIFI_STA_SSID_MAX] = { 0 };
    char pass[WIFI_STA_PASS_MAX] = { 0 };

    char *line = buf;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next = '\0';
            next++;
        }
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\r') {
            line[llen - 1] = '\0';
        }
        if (strncmp(line, "ssid=", 5) == 0) {
            snprintf(ssid, sizeof(ssid), "%s", line + 5);
            got_ssid = true;
        } else if (strncmp(line, "pass=", 5) == 0) {
            snprintf(pass, sizeof(pass), "%s", line + 5);
            got_pass = true;
        }
        line = next;
    }

    if (got_ssid && got_pass && ssid[0] != '\0') {
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        strlcpy(s_pass, pass, sizeof(s_pass));
        return true;
    }
    return false;
}
#endif /* APP_ENABLE_TF_CARD */

static bool cred_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_CRED_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char ssid[WIFI_STA_SSID_MAX] = { 0 };
    char pass[WIFI_STA_PASS_MAX] = { 0 };
    size_t len = sizeof(ssid);
    bool ok = false;
    if (nvs_get_str(h, WIFI_CRED_NVS_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0') {
        len = sizeof(pass);
        if (nvs_get_str(h, WIFI_CRED_NVS_PASS, pass, &len) == ESP_OK) {
            strlcpy(s_ssid, ssid, sizeof(s_ssid));
            strlcpy(s_pass, pass, sizeof(s_pass));
            ok = true;
        }
    }
    nvs_close(h);
    return ok;
}

static bool cred_load(void)
{
#if APP_ENABLE_TF_CARD
    if (cred_load_from_tf()) {
        ESP_LOGI(TAG, "Credentials loaded from TF card");
        return true;
    }
#endif
    if (cred_load_from_nvs()) {
        ESP_LOGI(TAG, "Credentials loaded from NVS");
        return true;
    }
    return false;
}

static esp_err_t cred_save_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_CRED_NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    nvs_set_str(h, WIFI_CRED_NVS_SSID, ssid);
    nvs_set_str(h, WIFI_CRED_NVS_PASS, pass);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t cred_save(const char *ssid, const char *pass)
{
#if APP_ENABLE_TF_CARD
    if (tf_card_is_mounted()) {
        char buf[WIFI_STA_SSID_MAX + WIFI_STA_PASS_MAX + 32];
        int n = snprintf(buf, sizeof(buf), "ssid=%s\npass=%s\n", ssid, pass);
        if (n > 0 && tf_card_write_file(WIFI_CRED_FILE, buf, (size_t)n) == ESP_OK) {
            ESP_LOGI(TAG, "Credentials saved to TF card: %s", WIFI_CRED_FILE);
            /* 同时双写 NVS，拔卡也有兜底 */
            return cred_save_nvs(ssid, pass);
        }
        ESP_LOGW(TAG, "Save to TF card failed, fallback to NVS");
    }
#endif
    return cred_save_nvs(ssid, pass);
}

/* ==================== STA 连接控制 ==================== */

/**
 * @brief STA 链路是否真的可用（WiFi 驱动已关联 + DHCP 已拿到 IP）
 *
 * s_state 是我们自己的软件状态机，只有 IP_EVENT_STA_GOT_IP 才能把它从
 * CONNECTING 推到 CONNECTED。一旦漏掉这个事件（例如重配网时
 * esp_wifi_connect() 被驱动拒绝，或扫描期间断开又自动恢复），状态就会
 * 永久卡在 CONNECTING：外网检测被状态门禁挡住、LED 一直快闪，尽管此时
 * 网络其实是通的（SNTP 已校时）。这里用驱动侧真实状态兜底校正。
 *
 * 只查 esp_wifi_sta_get_ap_info() 不够：实测在 auth/assoc 阶段、或驱动还
 * 保留着上次 AP 缓存时，它也会返回 ESP_OK（启动 1.3s 就误报 link recovered，
 * 而真正拿到 IP 是 6s）。因此必须再确认 STA netif 已有非零 IP。
 */
static bool sta_link_is_up(void)
{
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    if (s_sta_netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip = { 0 };
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

/* 把当前 s_ssid / s_pass 下发到 STA 配置（重连前调用，保证用最新凭据） */
static void sta_apply_config(void)
{
    if (s_ssid[0] == '\0') {
        return;
    }
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    if (s_pass[0] != '\0') {
        strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = WIFI_AUTH_WEP; /* 尽量兼容低加密热点 */
    } else {
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set STA config failed: %s", esp_err_to_name(err));
    }
}

/* 暂停自动重连 duration_ms 毫秒（配网页扫描热点期间用） */
static void reconnect_pause(int64_t duration_ms)
{
    int64_t until = esp_timer_get_time() + duration_ms * 1000;
    taskENTER_CRITICAL(&s_reconnect_lock);
    s_reconnect_paused = true;
    s_reconnect_resume_us = until;
    taskEXIT_CRITICAL(&s_reconnect_lock);
}

static bool reconnect_is_paused(void)
{
    bool paused;
    taskENTER_CRITICAL(&s_reconnect_lock);
    paused = s_reconnect_paused;
    taskEXIT_CRITICAL(&s_reconnect_lock);
    return paused;
}

static void reconnect_resume(void)
{
    taskENTER_CRITICAL(&s_reconnect_lock);
    s_reconnect_paused = false;
    taskEXIT_CRITICAL(&s_reconnect_lock);
}

/* 暂停已到期则清除标志并返回 true（由 wifi_mgr_poll 调用） */
static bool reconnect_resume_if_expired(int64_t now_us)
{
    bool expired = false;
    taskENTER_CRITICAL(&s_reconnect_lock);
    if (s_reconnect_paused && now_us >= s_reconnect_resume_us) {
        s_reconnect_paused = false;
        expired = true;
    }
    taskEXIT_CRITICAL(&s_reconnect_lock);
    return expired;
}

static void sta_apply_config_and_connect(const char *ssid, const char *pass)
{
    const char *safe_pass = (pass != NULL) ? pass : "";

    /* 凭据没变且链路本来就通：不要打断现有连接。否则 esp_wifi_connect() 会被
     * 驱动拒绝（日志 "sta is connected, disconnect before connecting to new ap"），
     * 状态却被我们降成 CONNECTING，之后又没有 GOT_IP 事件把它推回来。 */
    if (strcmp(s_ssid, ssid) == 0 && strcmp(s_pass, safe_pass) == 0 && sta_link_is_up()) {
        ESP_LOGI(TAG, "Credentials unchanged and STA already associated, keep current link");
        reconnect_resume();
        if (s_state == WIFI_MGR_STATE_CONNECTING) {
            s_state = WIFI_MGR_STATE_CONNECTED;
        }
        return;
    }

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, safe_pass, sizeof(s_pass));
    s_state = WIFI_MGR_STATE_CONNECTING;
    s_retry_count = 0;
    reconnect_resume();      /* 新凭据优先，清掉配网页扫描留下的暂停 */
    sta_apply_config();

    if (sta_link_is_up()) {
        /* 已连着旧热点：必须先断开，断线事件里会用新凭据自动重连。
         * 直接 connect 会被 IDF 拒绝，换不了热点。 */
        ESP_LOGI(TAG, "Disconnect current AP before switching to \"%s\"", ssid);
        esp_wifi_disconnect();
    } else {
        esp_wifi_connect();
    }
}

/**
 * @brief 扫描附近热点，写入 records（最多 max 个），*num_out 为实际数量
 *
 * STA 正在连接时 IDF 不允许扫描（ESP_ERR_WIFI_STATE）。此时先暂停自动重连
 * 并主动断开，等状态真正落到 idle 后再重试 —— 这是"设备换了地方、旧热点
 * 不在、却还能重新配网"的关键。连接正常时扫描本来就被允许，不会走到这里。
 */
static esp_err_t portal_scan_ap(wifi_ap_record_t *records, uint16_t max, uint16_t *num_out)
{
    *num_out = 0;
    wifi_scan_config_t cfg = { 0 };

    esp_err_t err = esp_wifi_scan_start(&cfg, true);

    /* 只有「确实在连接中、且链路并未真正建立（没拿到 IP）」时才需要断开 STA。
     * 若只是状态机卡住而链路其实是通的，绝不能断开 —— 那会把一个正常的
     * 连接打掉，反而让状态永远回不去（LED 一直快闪）。 */
    if (err == ESP_ERR_WIFI_STATE &&
        s_state == WIFI_MGR_STATE_CONNECTING && !sta_link_is_up()) {
        ESP_LOGW(TAG_P, "STA is connecting, scan not allowed -> pause reconnect and retry");
        reconnect_pause(PORTAL_SCAN_PAUSE_MS);
        esp_wifi_disconnect();          /* 先置暂停标志再断开，事件里就不会抢着重连 */
    }
    for (int i = 0; i < 10 && err == ESP_ERR_WIFI_STATE; i++) {
        /* 断开是异步的，最多等 500ms 让状态落下来 */
        vTaskDelay(pdMS_TO_TICKS(50));
        err = esp_wifi_scan_start(&cfg, true);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG_P, "scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
        return ESP_FAIL;
    }
    if (num > max) {
        num = max;
    }
    if (num > 0 && esp_wifi_scan_get_ap_records(&num, records) != ESP_OK) {
        return ESP_FAIL;
    }
    *num_out = num;
    ESP_LOGI(TAG_P, "scan found %u AP(s)", (unsigned)num);
    return ESP_OK;
}

/* ==================== DNS 劫持（captive portal 的前提） ====================
 *
 * 手机/电脑连上 SoftAP 后，浏览器里敲的是域名（www.baidu.com 之类）。
 * 此时设备没有外网，若不做 DNS 劫持，域名解析直接失败，HTTP 请求根本到不了
 * 本机的 80 端口 —— 表现就是「连上了热点但打不开任何网页、也不弹配网页」。
 *
 * 这里起一个最小 DNS 服务（UDP 53）：任何域名的 A 记录都回应成 SoftAP 的 IP，
 * 客户端于是把 HTTP 请求发给 ESP32，再由 302/配网页接管。
 */

/* 跳过 DNS 报文的域名部分，返回下一个字段（QTYPE）的偏移；报文异常返回 0 */
static size_t dns_skip_name(const uint8_t *buf, size_t len, size_t off)
{
    while (off < len) {
        uint8_t label = buf[off];
        if (label == 0) {
            return off + 1;                 /* 域名结束 */
        }
        if ((label & 0xC0) == 0xC0) {       /* 压缩指针，固定 2 字节 */
            return (off + 2 <= len) ? off + 2 : 0;
        }
        off += 1 + (size_t)label;           /* 长度字节 + label */
    }
    return 0;
}

static void dns_server_task(void *arg)
{
    (void)arg;
    uint8_t *in = malloc(512);
    uint8_t *out = malloc(640);             /* 查询 + 一条 A 记录 */
    int sock = -1;

    if (in == NULL || out == NULL) {
        ESP_LOGE(TAG, "DNS task: out of memory");
        goto cleanup;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed: errno %d", errno);
        goto cleanup;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind udp/%d failed: errno %d", PORTAL_DNS_PORT, errno);
        goto cleanup;
    }

    ESP_LOGI(TAG, "DNS hijack on udp/%d: every name -> " IPSTR, PORTAL_DNS_PORT, IP2STR(&s_ap_ip));

    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, in, 512, 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) {
            continue;                        /* 连 DNS 头都不完整 */
        }
        if ((in[2] & 0x80) != 0) {
            continue;                        /* 是响应，不是查询 */
        }
        if ((in[4] == 0 && in[5] == 0)) {
            continue;                        /* 没有 question */
        }
        size_t qtype_off = dns_skip_name(in, (size_t)n, 12);
        if (qtype_off == 0 || qtype_off + 4 > (size_t)n) {
            continue;
        }
        uint16_t qtype = (uint16_t)((in[qtype_off] << 8) | in[qtype_off + 1]);
        size_t qend = qtype_off + 4;         /* QTYPE + QCLASS */

        memcpy(out, in, qend);
        out[2] = 0x81;                       /* QR=1, RD=1 */
        out[3] = 0x80;                       /* RA=1, RCODE=0 */
        out[4] = 0x00; out[5] = 0x01;        /* QDCOUNT = 1 */
        out[8] = out[9] = out[10] = out[11] = 0;  /* NSCOUNT/ARCOUNT = 0 */

        size_t olen = qend;
        if (qtype == 1) {                    /* A 查询：所有域名都指向本机 */
            static const uint8_t ans_head[12] = {
                0xC0, 0x0C,                  /* 指向 question 里的域名 */
                0x00, 0x01,                  /* TYPE  = A */
                0x00, 0x01,                  /* CLASS = IN */
                0x00, 0x00, 0x00, 0x3C,      /* TTL   = 60s */
                0x00, 0x04,                  /* RDLENGTH = 4 */
            };
            out[6] = 0x00; out[7] = 0x01;    /* ANCOUNT = 1 */
            memcpy(out + olen, ans_head, sizeof(ans_head));
            olen += sizeof(ans_head);
            memcpy(out + olen, &s_ap_ip.addr, 4);   /* 已是网络字节序 */
            olen += 4;
        } else {                             /* AAAA 等：回空答案，客户端会退回 A */
            out[6] = 0x00; out[7] = 0x00;
        }

        sendto(sock, out, olen, 0, (struct sockaddr *)&from, from_len);
    }

cleanup:
    if (sock >= 0) {
        close(sock);
    }
    free(in);
    free(out);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* 通过 DHCP 把 DNS 和门户地址告诉客户端，否则客户端压根不知道来问我们。
 * 注意：esp_netif_dhcps_option() 在 DHCP 服务运行时会拒绝修改，
 * 必须先 dhcps_stop，改完再 dhcps_start（这也是 IDF 的既定用法）。*/
static void portal_dhcp_setup(void)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        s_ap_ip = ip_info.ip;
    }

    esp_netif_dhcps_stop(s_ap_netif);   /* 已停止时返回 ALREADY_STOPPED，忽略 */

    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = s_ap_ip;
    esp_err_t err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    /* OFFER_DNS(0x02)：让 DHCP 应答里真正带上 option 6 */
    uint8_t offer_dns = 0x02;
    esp_err_t err2 = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                            ESP_NETIF_DOMAIN_NAME_SERVER,
                                            &offer_dns, sizeof(offer_dns));

    /* option 114：部分系统（Windows/Android）据此直接打开门户页 */
    static char portal_uri[32];
    snprintf(portal_uri, sizeof(portal_uri), "http://" IPSTR "/", IP2STR(&s_ap_ip));
    esp_err_t err3 = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                            ESP_NETIF_CAPTIVEPORTAL_URI,
                                            portal_uri, strlen(portal_uri));

    esp_netif_dhcps_start(s_ap_netif);

    if (err != ESP_OK || err2 != ESP_OK) {
        ESP_LOGW(TAG, "DHCP DNS option failed (%s / %s), clients may need manual http://" IPSTR "/",
                 esp_err_to_name(err), esp_err_to_name(err2), IP2STR(&s_ap_ip));
    } else {
        ESP_LOGI(TAG, "DHCP serves DNS " IPSTR " and portal URI %s (option 114: %s)",
                 IP2STR(&s_ap_ip), portal_uri, esp_err_to_name(err3));
    }
}

static void portal_dns_start(void)
{
    if (s_dns_task != NULL) {
        return;
    }
    if (xTaskCreate(dns_server_task, "dns_hijack", PORTAL_DNS_TASK_STACK, NULL,
                    PORTAL_DNS_TASK_PRIO, &s_dns_task) != pdPASS) {
        ESP_LOGE(TAG, "DNS task create failed");
        s_dns_task = NULL;
    }
}

/* ==================== HTTP 配网服务 ==================== */

static char *portal_page_build(void)
{
    wifi_ap_record_t *records = NULL;
    size_t page_cap = 8192;
    char *page = malloc(page_cap);
    if (page == NULL) {
        return NULL;
    }
    sbuf_t b = { .buf = page, .cap = page_cap, .pos = 0 };

    sbuf_append(&b,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32 WiFi Setup</title>"
        "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px;"
        "color:#333}h2{margin:8px 0}label{display:block;margin:12px 0 4px;font-size:14px;"
        "color:#666}select,input{width:100%;box-sizing:border-box;padding:8px;font-size:15px;"
        "margin-bottom:4px}button{margin-top:16px;width:100%;padding:12px;font-size:16px;"
        "background:#1a73e8;color:#fff;border:0;border-radius:6px}p{font-size:13px;"
        "color:#888}</style></head><body><h2>ESP32 WiFi 配网</h2>");

    sbuf_append(&b, "<p>设备状态：%s", state_name(s_state));
    if (s_state == WIFI_MGR_STATE_CONNECTED || s_state == WIFI_MGR_STATE_INTERNET_OK) {
        sbuf_append(&b, "（已连接 ");
        html_escape_append(&b, s_ssid);
        sbuf_append(&b, "）");
    }
    sbuf_append(&b, "</p>");

    /* 扫描附近热点（阻塞约 1~2s，仅在重建页面时执行）。表单一定要先输出：
     * 扫描失败时如果连 <form> 都没有，手动输入框提交不了，用户就彻底没法配网了。 */
    uint16_t ap_num = 0;
    records = calloc(PORTAL_SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    esp_err_t scan_err = (records != NULL)
                             ? portal_scan_ap(records, PORTAL_SCAN_MAX_APS, &ap_num)
                             : ESP_ERR_NO_MEM;

    sbuf_append(&b, "<form method=\"POST\" action=\"/save\">");
    if (scan_err != ESP_OK) {
        sbuf_append(&b, "<p>未获取到热点列表（%s），请手动输入热点名，或点<a href=\"/?scan=1\">重新扫描</a>。</p>",
                    esp_err_to_name(scan_err));
    } else if (ap_num == 0) {
        sbuf_append(&b, "<p>没有扫描到热点，请点<a href=\"/?scan=1\">重新扫描</a>或手动输入。</p>");
    } else {
        sbuf_append(&b, "<label>附近热点（共 %u 个，按信号强度排序，<a href=\"/?scan=1\">重新扫描</a>）</label>"
                        "<select name=\"ssid\">", ap_num);
        bool seen[PORTAL_SCAN_MAX_APS] = { false };
        for (uint16_t i = 0; i < ap_num; i++) {
            const char *ssid = (const char *)records[i].ssid;
            if (ssid[0] == '\0') {
                continue; /* 隐藏网络无法通过选择配网 */
            }
            bool dup = false;
            for (uint16_t j = 0; j < i; j++) {
                if (seen[j] && strcmp((const char *)records[j].ssid, ssid) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            seen[i] = true;
            sbuf_append(&b, "<option value=\"");
            html_escape_append(&b, ssid);
            sbuf_append(&b, "\">");
            html_escape_append(&b, ssid);
            bool locked = (records[i].authmode != WIFI_AUTH_OPEN);
            sbuf_append(&b, " (RSSI %d dBm%s)</option>", records[i].rssi, locked ? ", 加密" : ", 开放");
        }
        sbuf_append(&b, "</select>");
    }
    free(records);

    sbuf_append(&b,
        "<label>或手动输入热点名（隐藏热点用这个）</label>"
        "<input name=\"ssid2\" placeholder=\"留空则使用上方选择的热点\">"
        "<label>密码（开放网络可留空）</label>"
        "<input type=\"password\" name=\"pass\" placeholder=\"WiFi 密码\">"
        "<button type=\"submit\">保存并连接</button></form>"
        "<p>提示：保存后设备将连接所选热点；若失败可重新连回 " WIFI_AP_SSID " 再次配网。"
        "热点列表刷新页面即可更新。</p>"
        "</body></html>");
    return page;
}

/* 常见的系统联网探测路径：必须回 302，系统才会判定「需要登录」并弹门户 */
static bool is_captive_probe(const char *uri)
{
    static const char *const probes[] = {
        "generate_204",     /* Android / Chrome */
        "gen_204",          /* Android */
        "hotspot-detect",   /* iOS / macOS */
        "connecttest",      /* Windows */
        "ncsi.txt",         /* Windows */
        "success.txt",      /* Firefox */
        "canonical.html",   /* Ubuntu */
        "captive",          /* 通用 */
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (strstr(uri, probes[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* 302 跳到配网页：手机/电脑看到跳转才会弹「登录网络」提示 */
static void portal_redirect(httpd_req_t *req)
{
    char location[32];
    snprintf(location, sizeof(location), "http://" IPSTR "/", IP2STR(&s_ap_ip));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_send(req, NULL, 0);
}

/* 请求的 Host 是否就是本机 IP（是则说明用户已经在访问配网页） */
static bool host_is_self(httpd_req_t *req)
{
    char host[64] = { 0 };
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    char self[16];
    snprintf(self, sizeof(self), IPSTR, IP2STR(&s_ap_ip));
    size_t n = strlen(self);
    if (strncmp(host, self, n) != 0) {
        return false;
    }
    return (host[n] == '\0' || host[n] == ':');
}

/* 任意 GET URI 都返回配网页：
 *  - 系统探测路径 / 访问的是别的域名（DNS 劫持过来的）-> 302 跳到配网页；
 *  - 已经是 http://<本机IP>/ -> 直接给页面。 */
static esp_err_t portal_get_handler(httpd_req_t *req)
{
    if (is_captive_probe(req->uri) || !host_is_self(req)) {
        portal_redirect(req);
        return ESP_OK;
    }

    int64_t now = esp_timer_get_time();
    /* ?scan=1：页面上的「重新扫描」链接，跳过 5s 缓存立即重扫 */
    bool force_scan = (strstr(req->uri, "scan=1") != NULL);
    if (force_scan || s_page_cache == NULL || (now - s_page_cache_us) > PORTAL_SCAN_MIN_MS * 1000LL) {
        char *fresh = portal_page_build();
        if (fresh != NULL) {
            free(s_page_cache);
            s_page_cache = fresh;
            s_page_cache_us = now;
        } else if (s_page_cache == NULL) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_send(req, s_page_cache, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_send_msg(httpd_req_t *req, const char *title, const char *msg)
{
    char body[512];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<meta http-equiv=\"refresh\" content=\"5;url=/\">"
             "<title>ESP32 WiFi Setup</title></head><body style=\"font-family:sans-serif;"
             "max-width:420px;margin:24px auto;padding:0 16px\">"
             "<h2>%s</h2><p>%s</p><p>5 秒后自动返回首页，也可关闭本页。</p></body></html>",
             title, msg);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[600] = { 0 };
    size_t total = req->content_len;
    if (total >= sizeof(body)) {
        return portal_send_msg(req, "请求过大", "提交内容过长，请重试。");
    }

    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            return ESP_FAIL; /* 连接中断 */
        }
        received += (size_t)r;
    }
    body[received] = '\0';

    char ssid[WIFI_STA_SSID_MAX] = { 0 };
    char ssid2[WIFI_STA_SSID_MAX] = { 0 };
    char pass[WIFI_STA_PASS_MAX] = { 0 };
    bool have_select = form_get(body, "ssid", ssid, sizeof(ssid));
    bool have_manual = form_get(body, "ssid2", ssid2, sizeof(ssid2));
    form_get(body, "pass", pass, sizeof(pass)); /* 密码缺失视为开放网络 */

    /* 手动输入优先于下拉选择 */
    const char *chosen = (have_manual && ssid2[0] != '\0') ? ssid2 : (have_select ? ssid : NULL);
    if (chosen == NULL || chosen[0] == '\0') {
        return portal_send_msg(req, "配网失败", "未选择或输入热点名，请返回重试。");
    }
    size_t plen = strlen(pass);
    if (plen > 0 && plen < 8) {
        return portal_send_msg(req, "配网失败", "WPA 密码至少 8 位，开放网络请留空。");
    }

    if (cred_save(chosen, pass) != ESP_OK) {
        return portal_send_msg(req, "配网失败", "凭据保存失败，请重试。");
    }
    ESP_LOGI(TAG_P, "New credentials: ssid=\"%s\" (pass len %u), connecting...",
             chosen, (unsigned)plen);

    sta_apply_config_and_connect(chosen, pass);

    char ok_msg[160];
    snprintf(ok_msg, sizeof(ok_msg), "已保存热点「%s」，设备正在连接，请留意串口日志。", chosen);
    return portal_send_msg(req, "配网成功", ok_msg);
}

static esp_err_t portal_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard; /* 支持 "*" 匹配任意 URI */

    esp_err_t ret = httpd_start(&s_httpd, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 先注册具体路径（精确匹配优先），再注册 "*" 兜底 */
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = portal_save_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &save_uri));

    httpd_uri_t catch_all = {
        .uri = "*",
        .method = HTTP_GET,
        .handler = portal_get_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &catch_all));

    ESP_LOGI(TAG, "Config portal ready: join \"" WIFI_AP_SSID "\", open any URL or http://" IPSTR "/",
             IP2STR(&s_ap_ip));
    return ESP_OK;
}

/* ==================== 事件与状态 ==================== */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_ssid[0] != '\0') {
                ESP_LOGI(TAG, "STA started, connecting to \"%s\"", s_ssid);
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            if (s_ssid[0] == '\0') {
                break; /* 未配网，无需重连 */
            }
            s_state = WIFI_MGR_STATE_CONNECTING;
            s_retry_count++;
            if (reconnect_is_paused()) {
                /* 配网页正在扫描热点：别抢回 STA，否则扫描又会被打断 */
                ESP_LOGI(TAG, "STA disconnected, auto-reconnect paused for portal scan");
                break;
            }
            if (s_retry_count % 5 == 1) {
                ESP_LOGW(TAG, "STA disconnected, retry #%u (ssid=\"%s\")",
                         (unsigned)s_retry_count, s_ssid);
            }
            sta_apply_config();  /* 用当前（可能是刚提交的新）凭据重连 */
            esp_wifi_connect(); /* 立即重连 */
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Device joined \"" WIFI_AP_SSID "\" for provisioning");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_retry_count = 0;
        s_state = WIFI_MGR_STATE_CONNECTED;
        ESP_LOGI(TAG, "Connected to \"%s\", got IP " IPSTR, s_ssid, IP2STR(&evt->ip_info.ip));
    }
}

static bool net_check_internet(void)
{
    esp_http_client_config_t cfg = {
        .url = NET_CHECK_URL,
        .timeout_ms = 5000,
        .method = HTTP_METHOD_GET,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }
    bool ok = false;
    if (esp_http_client_perform(client) == ESP_OK) {
        int code = esp_http_client_get_status_code(client);
        ok = (code >= 200 && code < 300);
    }
    esp_http_client_cleanup(client);
    return ok;
}

/* ==================== 对外接口 ==================== */

esp_err_t wifi_mgr_init(void)
{
    /* 1. NVS（凭据兜底存储） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 网络栈与事件循环 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));

    /* APSTA：AP 与 STA 各自需要默认 netif */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* 3. SoftAP 常开（配网入口，开放网络，readme 默认热点名）
     *
     * beacon_interval 单位是 TU（1 TU ≈ 1.024ms），必须是 100 的倍数。
     * 原来用 100（≈102ms，每秒 10 次），实测这个发射节奏会耦合进录音通路，
     * 表现为每隔约 1 秒一次的噪声爆发。改成 1000 TU ≈ 1.02s 后，
     * AP 自身发射活动降一个数量级；手机发现热点会慢一点，但能正常连。 */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = WIFI_AP_MAX_CONN,
            .beacon_interval = APP_WIFI_AP_BEACON_TU,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* 4. 读取凭据，有则恢复连接 */
    if (cred_load()) {
        ESP_LOGI(TAG, "Saved credentials found, will reconnect to \"%s\"", s_ssid);
        sta_apply_config();
        s_state = WIFI_MGR_STATE_CONNECTING;
    } else {
        ESP_LOGI(TAG, "No saved credentials, waiting for provisioning on \"" WIFI_AP_SSID "\"");
    }

    /* 5. 启动 WiFi（STA_START 事件中自动发起连接） */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 5.1 限制发射功率（必须在 start 之后调用）。
     * 录音 A/B 实测：射频开启时录音本底抬高 18~20dB，并伴随周期性爆发。
     * 本机 RSSI 约 -37dBm，远低于灵敏度门限，10dBm 足够维持连接，
     * 但耦合进麦克风/模拟通路的能量明显下降。 */
    esp_err_t perr = esp_wifi_set_max_tx_power(APP_WIFI_TX_POWER_QDBM);
    if (perr == ESP_OK) {
        ESP_LOGI(TAG, "max tx power limited to %d.%02d dBm", 
                 APP_WIFI_TX_POWER_QDBM / 4, (APP_WIFI_TX_POWER_QDBM % 4) * 25);
    } else {
        ESP_LOGW(TAG, "set max tx power failed: %s (keep default)",
                 esp_err_to_name(perr));
    }

    /* 6. 门户基础设施：DHCP 告知 DNS/门户地址 + DNS 劫持服务 */
    portal_dhcp_setup();
    portal_dns_start();

    /* 7. HTTP 配网服务 */
    ESP_ERROR_CHECK(portal_start());

    s_last_heartbeat_us = esp_timer_get_time();
    return ESP_OK;
}

void wifi_mgr_poll(void)
{
    int64_t now = esp_timer_get_time();

    /* 配网页扫描结束后恢复被暂停的自动重连（用户没提交新凭据的情况） */
    if (reconnect_resume_if_expired(now)) {
        if (s_ssid[0] != '\0' && s_state == WIFI_MGR_STATE_CONNECTING && !sta_link_is_up()) {
            ESP_LOGI(TAG, "portal scan done, resume reconnect to \"%s\"", s_ssid);
            esp_wifi_connect();
        }
    }

    /* 状态机自愈：STA 底层其实已关联（能校时、能上网），但状态还停在 CONNECTING。
     * 常见于「重配网时 esp_wifi_connect() 被驱动拒绝」或扫描期间的反复断开：
     * 没有新的 GOT_IP 事件就没人把状态推回来，外网检测也被门禁挡住，LED 便一直快闪。
     * 这里按驱动的真实关联状态把状态拉回 CONNECTED，随后外网检测会升到 INTERNET_OK。 */
    if (s_state == WIFI_MGR_STATE_CONNECTING && sta_link_is_up()) {
        s_state = WIFI_MGR_STATE_CONNECTED;
        s_last_netcheck_us = 0;   /* 下一轮立刻验证外网，尽快从慢闪恢复常亮 */
        ESP_LOGI(TAG, "STA link is up but state was CONNECTING -> CONNECTED (link recovered)");
    }

    /* 已连上：周期验证外网（含从 INTERNET_OK 回退） */
    if (s_state == WIFI_MGR_STATE_CONNECTED || s_state == WIFI_MGR_STATE_INTERNET_OK) {
        if ((now - s_last_netcheck_us) > NET_CHECK_PERIOD_MS * 1000LL) {
            s_last_netcheck_us = now;
            if (net_check_internet()) {
                if (s_state != WIFI_MGR_STATE_INTERNET_OK) {
                    s_state = WIFI_MGR_STATE_INTERNET_OK;
                    ESP_LOGI(TAG, "Internet check OK, state -> INTERNET_OK");
                }
            } else {
                if (s_state == WIFI_MGR_STATE_INTERNET_OK) {
                    s_state = WIFI_MGR_STATE_CONNECTED;
                    ESP_LOGW(TAG, "Internet check failed, state -> CONNECTED");
                } else {
                    ESP_LOGW(TAG, "Connected, but internet check failed");
                }
            }
        }
    }

    /* 心跳日志 */
    if ((now - s_last_heartbeat_us) > HEARTBEAT_PERIOD_MS * 1000LL) {
        s_last_heartbeat_us = now;
        ESP_LOGI(TAG, "heartbeat: state=%s, ssid=\"%s\"", state_name(s_state), s_ssid);
    }
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    return s_state;
}

/* ---- 诊断专用：射频开关（录音噪声 A/B 对照用）----
 * esp_wifi_stop() 会真正切断射频和关联，测完 esp_wifi_start() 由驱动
 * 按已保存凭据自行重连。这两个接口不进正常流程。 */
void wifi_mgr_radio_pause(void)
{
    esp_err_t err = esp_wifi_stop();
    ESP_LOGW(TAG, "radio paused for diagnostics (%s)", esp_err_to_name(err));
}

void wifi_mgr_radio_resume(void)
{
    esp_err_t err = esp_wifi_start();
    ESP_LOGW(TAG, "radio resumed (%s)", esp_err_to_name(err));
}

const char *wifi_mgr_get_ssid(void)
{
    return (s_ssid[0] != '\0') ? s_ssid : NULL;
}
