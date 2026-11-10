/*
 * recorder.c —— 录音与回放实现
 *
 * 关键设计取舍见 recorder.h 顶部注释。这里补充三点实现细节：
 *
 * 1. 录音存单声道：I2S 读到的是立体声交织数据，逐帧做 (L+R)/2 降混。
 *    用平均而不是取单边，是因为板上麦克风接到哪一路不确定；平均值
 *    在两路相同时不丢幅度，只有一路有信号时最多损失 6dB（PGA 可补）。
 *
 * 2. 回放要扩回立体声：单声道样本复制到左右两路。
 *
 * 3. 录音/空闲时关功放：否则喇叭的底噪会被麦克风收进去形成正反馈。
 *    只在回放期间打开。
 */
#include "recorder.h"

#include <string.h>

#include "audio_i2s.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "feature_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "recorder";

/* 20ms 一块：够粗以保证效率，够细以及时响应停止请求 */
#define BLOCK_FRAMES     (AUDIO_SAMPLE_RATE / 50)              /* 480 */
#define BLOCK_STEREO_BYTES (BLOCK_FRAMES * AUDIO_FRAME_BYTES)  /* 1920 */
#define BLOCK_MONO_BYTES   (BLOCK_FRAMES * 2)                  /* 960 */

/* 录音上限：PSRAM 可用时按 REC_MAX_SECONDS，否则降级 */
#define BYTES_PER_SEC   (AUDIO_SAMPLE_RATE * 2)

/* 内部 RAM 降级时的上限（秒）：给 WiFi/LCD 留足空间 */
#define FALLBACK_SEC    4

/* "无录音"提示的停留时间 */
#define NO_DATA_MSG_MS  1500

typedef enum {
    CMD_REC_START = 0,
    CMD_REC_STOP,
    CMD_PLAY_START,
    CMD_PLAY_STOP,
} rec_cmd_t;

static QueueHandle_t     s_queue = NULL;
static TaskHandle_t      s_task  = NULL;

/* 录音缓冲 */
static int16_t          *s_buf = NULL;
static size_t            s_cap_bytes = 0;   /* 缓冲容量（字节） */
static size_t            s_len_bytes = 0;   /* 已录字节 */
static size_t            s_play_pos  = 0;   /* 回放读位置（字节） */
static size_t            s_play_total = 0;  /* 本次回放总字节 */

/* 状态：跨任务读取，用 volatile 保证不被优化掉 */
static volatile recorder_state_t s_state = RECORDER_IDLE;
static volatile TickType_t       s_no_data_until = 0;

/* I2S 读写的中间缓冲（静态，避免每块都 malloc） */
static int16_t s_stereo[BLOCK_FRAMES * AUDIO_CHANNELS];
static int16_t s_mono[BLOCK_FRAMES];
static int16_t s_play_stereo[BLOCK_FRAMES * AUDIO_CHANNELS];

/* ==================== 缓冲分配 ==================== */

static esp_err_t alloc_buffer(void)
{
    size_t want = (size_t)REC_MAX_SECONDS * BYTES_PER_SEC;

    /* 优先 PSRAM：录音是纯数据，放外部内存不影响实时性，
     * 而且内部 RAM 要留给 WiFi 协议栈和 LCD 帧缓冲。 */
    s_buf = heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    if (s_buf != NULL) {
        s_cap_bytes = want;
        ESP_LOGI(TAG, "record buffer in PSRAM: %u KB (%d s max)",
                 (unsigned)(want / 1024), REC_MAX_SECONDS);
        return ESP_OK;
    }

    /* 退回内部 RAM：容量大幅缩水，但功能仍可验证 */
    size_t fallback = (size_t)FALLBACK_SEC * BYTES_PER_SEC;
    s_buf = heap_caps_malloc(fallback, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "record buffer alloc failed (PSRAM %u KB / internal %u KB)",
                 (unsigned)(want / 1024), (unsigned)(fallback / 1024));
        return ESP_ERR_NO_MEM;
    }
    s_cap_bytes = fallback;
    ESP_LOGW(TAG, "PSRAM unavailable -> internal RAM fallback, only %d s! "
             "enable CONFIG_SPIRAM for full length", FALLBACK_SEC);
    return ESP_OK;
}

/* ==================== 录音 ==================== */

static void do_record_start(void)
{
    s_len_bytes = 0;
    s_state = RECORDER_RECORDING;

    /* 关功放：录音时喇叭不能出声，否则麦克风会收进去 */
    audio_spk_enable(false);

    ESP_LOGI(TAG, "recording start (max %u s)",
             (unsigned)(s_cap_bytes / BYTES_PER_SEC));
}

static void do_record_stop(void)
{
    s_state = RECORDER_IDLE;
    audio_spk_enable(false);
    ESP_LOGI(TAG, "recording stop: %u bytes = %u ms",
             (unsigned)s_len_bytes,
             (unsigned)(s_len_bytes / 2 * 1000 / AUDIO_SAMPLE_RATE));
}

/* 读一块并降混追加。返回 false 表示应该结束录音。 */
static bool record_one_block(void)
{
    /* 缓冲满：自动结束，不覆盖旧数据 */
    if (s_len_bytes + BLOCK_MONO_BYTES > s_cap_bytes) {
        ESP_LOGW(TAG, "buffer full, stop recording");
        do_record_stop();
        return false;
    }

    size_t got = 0;
    esp_err_t err = audio_read(s_stereo, BLOCK_STEREO_BYTES, &got,
                               AUDIO_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s read failed: %s", esp_err_to_name(err));
        do_record_stop();
        return false;
    }
    if (got == 0) {
        /* 暂时没数据（DMA 还没填满），继续循环即可，不丢状态 */
        return true;
    }

    size_t frames = got / AUDIO_FRAME_BYTES;
    for (size_t i = 0; i < frames; i++) {
        /* (L+R)/2，用 int32 避免两路接近满量程时相加溢出 */
        int32_t l = s_stereo[i * 2];
        int32_t r = s_stereo[i * 2 + 1];
        s_mono[i] = (int16_t)((l + r) / 2);
    }
    size_t mono_bytes = frames * 2;
    memcpy((uint8_t *)s_buf + s_len_bytes, s_mono, mono_bytes);
    s_len_bytes += mono_bytes;
    return true;
}

/* ==================== 回放 ==================== */

static void do_play_start(void)
{
    if (s_len_bytes < (size_t)(AUDIO_SAMPLE_RATE / 10)) {
        /* 不到 100ms 视为没有录音 */
        ESP_LOGW(TAG, "no recording to play (%u bytes)", (unsigned)s_len_bytes);
        s_no_data_until = xTaskGetTickCount() +
                          pdMS_TO_TICKS(NO_DATA_MSG_MS);
        s_state = RECORDER_NO_DATA;
        return;
    }
    s_play_pos = 0;
    s_play_total = s_len_bytes;
    s_state = RECORDER_PLAYING;
    /* 先关再开：确保是"从静音进入有声"，减少爆音 */
    audio_spk_enable(false);
    audio_spk_enable(true);
    ESP_LOGI(TAG, "playback start: %u ms", (unsigned)recorder_total_ms());
}

static void do_play_stop(void)
{
    s_state = RECORDER_IDLE;
    audio_spk_enable(false);
    ESP_LOGI(TAG, "playback stop at %u ms", (unsigned)recorder_total_ms());
}

/* 播一块。返回 false 表示播完。 */
static bool play_one_block(void)
{
    size_t left = s_play_total - s_play_pos;
    if (left == 0) {
        do_play_stop();
        return false;
    }

    size_t mono_bytes = left > BLOCK_MONO_BYTES ? BLOCK_MONO_BYTES : left;
    size_t frames = mono_bytes / 2;
    const int16_t *src = (const int16_t *)((const uint8_t *)s_buf + s_play_pos);

    /* 单声道 -> 立体声（左右相同） */
    for (size_t i = 0; i < frames; i++) {
        s_play_stereo[i * 2]     = src[i];
        s_play_stereo[i * 2 + 1] = src[i];
    }
    size_t stereo_bytes = frames * AUDIO_FRAME_BYTES;

    esp_err_t err = audio_write(s_play_stereo, stereo_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s write failed: %s", esp_err_to_name(err));
        do_play_stop();
        return false;
    }
    s_play_pos += mono_bytes;
    return true;
}

/* ==================== 任务 ==================== */

static void handle_cmd(rec_cmd_t cmd)
{
    switch (cmd) {
    case CMD_REC_START:
        /* 录音优先：正在回放就打断它 */
        if (s_state == RECORDER_PLAYING) {
            do_play_stop();
        }
        do_record_start();
        break;
    case CMD_REC_STOP:
        if (s_state == RECORDER_RECORDING) {
            do_record_stop();
        }
        break;
    case CMD_PLAY_START:
        if (s_state == RECORDER_RECORDING) {
            do_record_stop();
        }
        do_play_start();
        break;
    case CMD_PLAY_STOP:
        if (s_state == RECORDER_PLAYING) {
            do_play_stop();
        }
        break;
    }
}

static void recorder_task(void *arg)
{
    (void)arg;

    /* 音频通路初始化放这里而不是 init 里：约 200ms，不阻塞开机。
     * 任务一启动就做好，用户第一次按键时已就绪。 */
    esp_err_t err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_init failed: %s", esp_err_to_name(err));
    }

    for (;;) {
        rec_cmd_t cmd;

        switch (s_state) {
        case RECORDER_IDLE:
            /* 空闲：阻塞等命令，不消耗 CPU */
            if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {
                handle_cmd(cmd);
            }
            break;

        case RECORDER_RECORDING:
            /* 先非阻塞看一眼有没有停止请求，保证松开就停 */
            if (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
                handle_cmd(cmd);
                break;
            }
            if (!record_one_block()) {
                break;
            }
            /* 让出一拍，避免长时间独占 CPU（I2S 读本身已是阻塞的） */
            taskYIELD();
            break;

        case RECORDER_PLAYING:
            if (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
                handle_cmd(cmd);
                break;
            }
            play_one_block();
            break;

        case RECORDER_NO_DATA:
            if (xTaskGetTickCount() >= s_no_data_until) {
                s_state = RECORDER_IDLE;
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            break;
        }
    }
}

/* ==================== 对外接口 ==================== */

esp_err_t recorder_init(void)
{
    ESP_RETURN_ON_ERROR(alloc_buffer(), TAG, "buffer alloc failed");

    s_queue = xQueueCreate(4, sizeof(rec_cmd_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }

    /* 4KB 栈：有 memcpy 和 I2S 调用，别给太紧 */
    if (xTaskCreate(recorder_task, "recorder", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void post(rec_cmd_t cmd)
{
    if (s_queue == NULL) {
        return;
    }
    /* 队列满就丢弃：命令是可合并的，丢一条不影响最终状态 */
    xQueueSend(s_queue, &cmd, 0);
}

void recorder_record_start(void) { post(CMD_REC_START); }
void recorder_record_stop(void)  { post(CMD_REC_STOP); }
void recorder_play_start(void)   { post(CMD_PLAY_START); }
void recorder_play_stop(void)    { post(CMD_PLAY_STOP); }

recorder_state_t recorder_get_state(void)
{
    return s_state;
}

uint32_t recorder_recorded_ms(void)
{
    return (uint32_t)(s_len_bytes / 2 * 1000 / AUDIO_SAMPLE_RATE);
}

uint32_t recorder_total_ms(void)
{
    return (uint32_t)(s_play_total / 2 * 1000 / AUDIO_SAMPLE_RATE);
}

uint32_t recorder_remaining_ms(void)
{
    size_t left = s_play_total > s_play_pos ? s_play_total - s_play_pos : 0;
    return (uint32_t)(left / 2 * 1000 / AUDIO_SAMPLE_RATE);
}

bool recorder_has_data(void)
{
    return s_len_bytes > (size_t)(AUDIO_SAMPLE_RATE / 10);
}
