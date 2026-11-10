/*
 * audio_i2s.c —— I2S 音频通路实现
 */
#include "audio_i2s.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "es8388.h"

static const char *TAG = "audio";

/* 引脚（与 xiaozhi atk-dnesp32s3m-wifi 板一致） */
#define PIN_MCLK    GPIO_NUM_6
#define PIN_BCLK    GPIO_NUM_7
#define PIN_WS      GPIO_NUM_16
#define PIN_DOUT    GPIO_NUM_15   /* ESP32 -> codec，播放数据 */
#define PIN_DIN     GPIO_NUM_17   /* codec -> ESP32，录音数据 */
#define PIN_SPK_EN  GPIO_NUM_42   /* 高电平使能功放 */

/* 写超时：一次最多写 100ms 音频，给 1s 余量足够 */
#define I2S_WRITE_TIMEOUT_MS  1000

/* 100ms 的样本数，用于分块写（便于中途响应） */
#define CHUNK_FRAMES    (AUDIO_SAMPLE_RATE / 10)
#define CHUNK_BYTES     (CHUNK_FRAMES * AUDIO_FRAME_BYTES)

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static bool s_inited = false;

/* 复用的分块缓冲（立体声交织 int16） */
static int16_t s_buf[CHUNK_FRAMES * AUDIO_CHANNELS];

static esp_err_t spk_enable_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_SPK_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "spk_en gpio config failed");
    /* 先保持关闭，等 I2S 和 codec 都配好再打开，避免上电爆音 */
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_SPK_EN, 0), TAG, "spk_en low failed");
    return ESP_OK;
}

esp_err_t audio_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* ---- 1. 功放先强制关掉，排在所有事情最前面 ----
     * 上电时 GPIO42 是浮空输入，如果板上有外部上拉，功放可能已经开着。
     * 先驱动成低电平，避免后面任何意外噪声被放大出来。 */
    ESP_RETURN_ON_ERROR(spk_enable_init(), TAG, "spk_en init failed");

    /* ---- 2. 先配 codec，再开 I2S（顺序很关键）----
     * ES8388 的控制口（I2C）不依赖 MCLK，所以完全可以先配好。
     * 反过来做会踩坑：软复位把 R8 恢复成默认 0x80 —— bit7 MSC=1
     * 即「codec 作 I2S 主机」。此时 ESP32 也在驱动 BCLK/WS，两边同时
     * 驱动同一条线，结果是时钟被判成乱码、喇叭持续发出哒哒声，
     * 同时 codec 内部状态机错乱、I2C 读寄存器开始 NAK。
     * 实测踩过：read R0 failed / ESP_ERR_INVALID_RESPONSE。 */
    ESP_RETURN_ON_ERROR(es8388_soft_reset(), TAG, "codec soft reset failed");

    /* 复位后立刻改回从机，消除上面说的时钟争用窗口 */
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x08, 0x00), TAG,
                        "force slave mode failed");

    ESP_RETURN_ON_ERROR(es8388_config_playback(), TAG, "codec playback cfg failed");

    /* ---- 3. I2S：全双工，ESP32 作主 ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                            I2S_ROLE_MASTER);
    /* DMA 5 个描述符 x 240 帧 = 50ms 缓冲。太小会欠载（断续），
     * 太大会让"按下说话"到"数据送出"的延迟变长。 */
    chan_cfg.dma_desc_num  = 5;
    chan_cfg.dma_frame_num = 240;
    /* 没数据可发时自动补零：时钟一直跑，示波器好量，
     * 也不会因为来不及写而输出上一段的残留波形。 */
    chan_cfg.auto_clear    = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG,
                        "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = PIN_DOUT,
            .din  = PIN_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false,
                              .ws_inv = false },
        },
    };
    /* MCLK 必须是采样率的 256 倍，否则 ES8388 的 DACFsRatio=256 对不上，
     * 表现为音调变快/变慢或干脆没声。 */
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG,
                        "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG,
                        "i2s rx init failed");

    /* ---- 4. 使能 TX：MCLK/BCLK/WS 立刻开始输出 ---- */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx enable failed");

    /* 先送几帧静音，确保数据线是确定的低电平，而不是悬空 */
    ESP_RETURN_ON_ERROR(audio_write_silence(80), TAG, "silence failed");

    /* ---- 5. 时钟稳定、数据线上已是静音，这时才开 DAC 输出级 ---- */
    ESP_RETURN_ON_ERROR(es8388_output_enable(true), TAG, "output enable failed");

    /* ---- 6. 最后开功放 ---- */
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_SPK_EN, 1), TAG, "spk_en high failed");

    s_inited = true;
    ESP_LOGI(TAG, "audio up: %d Hz / %d bit / %d ch, codec slave, spk_en=IO%d ON",
             AUDIO_SAMPLE_RATE, AUDIO_BITS, AUDIO_CHANNELS, PIN_SPK_EN);
    audio_log_clocks();
    return ESP_OK;
}

esp_err_t audio_write(const void *pcm, size_t bytes)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = bytes;
    while (left > 0) {
        size_t n = left > CHUNK_BYTES ? CHUNK_BYTES : left;
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, p, n, &written,
                                          I2S_WRITE_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_FAIL;
        }
        p += written;
        left -= written;
    }
    return ESP_OK;
}

esp_err_t audio_write_silence(uint32_t ms)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_buf, 0, sizeof(s_buf));
    uint32_t chunks = (ms + 99) / 100;
    for (uint32_t i = 0; i < chunks; i++) {
        ESP_RETURN_ON_ERROR(audio_write(s_buf, sizeof(s_buf)), TAG,
                            "silence write failed");
    }
    return ESP_OK;
}

esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t ms, float amplitude)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (amplitude > 1.0f) {
        amplitude = 1.0f;
    }
    int16_t peak = (int16_t)(amplitude * 32767.0f);

    /* 相位累加器：跨块连续，避免每块边界出现相位跳变（会听到"咔"声） */
    double phase = 0.0;
    double phase_step = 2.0 * M_PI * (double)freq_hz / (double)AUDIO_SAMPLE_RATE;

    uint32_t total_frames = AUDIO_SAMPLE_RATE / 1000 * ms;
    uint32_t done = 0;
    while (done < total_frames) {
        uint32_t frames = total_frames - done;
        if (frames > CHUNK_FRAMES) {
            frames = CHUNK_FRAMES;
        }
        for (uint32_t i = 0; i < frames; i++) {
            int16_t v = (int16_t)(peak * sin(phase));
            phase += phase_step;
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
            s_buf[i * 2]     = v;   /* 左 */
            s_buf[i * 2 + 1] = v;   /* 右 */
        }
        ESP_RETURN_ON_ERROR(audio_write(s_buf, frames * AUDIO_FRAME_BYTES), TAG,
                            "tone write failed");
        done += frames;
    }
    return ESP_OK;
}

void audio_log_clocks(void)
{
    if (s_tx == NULL) {
        return;
    }
    i2s_chan_info_t info = {0};
    if (i2s_channel_get_info(s_tx, &info) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "clocks (driver report): MCLK=%.3f MHz  BCLK=%u Hz  "
             "SCLK=%.3f MHz", info.mclk_hz / 1e6, (unsigned)info.bclk_hz,
             info.sclk_hz / 1e6);
    ESP_LOGI(TAG, "clocks (expected):      MCLK=%.3f MHz  BCLK=%u Hz  "
             "WS=%d Hz", (AUDIO_SAMPLE_RATE * AUDIO_MCLK_MULTIPLE) / 1e6,
             (unsigned)(AUDIO_SAMPLE_RATE * AUDIO_BITS * AUDIO_CHANNELS),
             AUDIO_SAMPLE_RATE);
}
