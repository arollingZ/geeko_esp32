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
#define PIN_SPK_EN  GPIO_NUM_42   /* 功放使能 */
#define PIN_PHONE_CK GPIO_NUM_3   /* 耳机检测：按 xiaozhi 板级代码 */
                                  /* 低=无耳机(功放该开)，高=有耳机(功放该关) */

/* 功放使能极性：1 = 低电平使能（active low）
 *
 * 实测依据（两个数据点互相印证）：
 *   第一版固件在 codec 配置失败后中断，GPIO42 停在初始化时的低电平，
 *   此时喇叭发出哒哒声 —— 说明低电平时功放是通的。
 *   后来把 GPIO42 拉高，喇叭完全无声 —— 说明高电平时功放关闭。
 * 所以这块板子是低电平使能。
 *
 * xiaozhi 板级代码在这点上自相矛盾：初始化里写 if(耳机高) 则拉高，
 * 但按键回调里写的是按下(不插耳机)拉高、松开拉低。后者才是对的。
 */
#define SPK_EN_ACTIVE_LOW   1

/* 便于阅读：把"开/关"翻译成实际电平 */
#if SPK_EN_ACTIVE_LOW
#  define SPK_LEVEL_ON   0
#  define SPK_LEVEL_OFF  1
#else
#  define SPK_LEVEL_ON   1
#  define SPK_LEVEL_OFF  0
#endif

/* 写超时：一次最多写 100ms 音频，给 1s 余量足够 */
#define I2S_WRITE_TIMEOUT_MS  1000

/* 100ms 的样本数，用于分块写（便于中途响应） */
#define CHUNK_FRAMES    (AUDIO_SAMPLE_RATE / 10)
#define CHUNK_BYTES     (CHUNK_FRAMES * AUDIO_FRAME_BYTES)

static i2s_chan_handle_t s_tx = NULL;
/* 录音通道，当前阶段（只验证播放）暂不创建，做录音时启用 */
static i2s_chan_handle_t s_rx = NULL;
static bool s_inited = false;

/* 复用的分块缓冲（立体声交织 int16） */
static int16_t s_buf[CHUNK_FRAMES * AUDIO_CHANNELS];

static esp_err_t spk_enable_init(void)
{
    /* 用 INPUT_OUTPUT 而不是纯 OUTPUT：ESP32 在纯输出模式下关闭输入通路，
     * gpio_get_level() 读回恒为 0，会误判成"引脚没拉高"。
     * 改成双向才能真实回读自己驱动的电平。 */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_SPK_EN),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "spk_en gpio config failed");
    /* 先保持关闭，等 I2S 和 codec 都配好再打开，避免上电爆音 */
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_SPK_EN, SPK_LEVEL_OFF), TAG,
                        "spk_en off failed");

    /* 耳机检测脚：只读，用上拉（与 xiaozhi 板级初始化一致） */
    gpio_config_t phone = {
        .pin_bit_mask = (1ULL << PIN_PHONE_CK),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&phone);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "phone detect gpio config failed: %s", esp_err_to_name(err));
    }
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

    /* ---- 2. 先配 codec，再开 I2S（顺序很关键，两个坑都要躲）----
     *
     * 坑 A（时钟争用）：R8 复位默认 0x80，bit7 MSC=1 = codec 作 I2S 主机。
     *   若此时 ESP32 已在驱动 BCLK/WS，两条线两个驱动源，时钟乱码，
     *   喇叭发出连续哒哒声，同时 codec 状态机错乱、I2C 开始 NAK。
     *   → 复位后立刻写 R8=0x00 强制从机。
     *
     * 坑 B（时钟干扰 I2C）：实测 I2S 时钟一旦跑起来，I2C 写会开始失败
     *   （ESP_ERR_INVALID_RESPONSE），而时钟未启动时连写 30 个寄存器零错误。
     *   → 所有 I2C 操作必须在 i2s_channel_enable() 之前做完。
     *
     * 结论：codec 控制口不依赖 MCLK，把"全部配置（含解除静音、打开输出级）"
     * 都放在开时钟之前完成，是唯一同时满足两个约束的顺序。
     * 开时钟时 DOUT 上是静音数据，所以不会有爆音。 */
    ESP_RETURN_ON_ERROR(es8388_soft_reset(), TAG, "codec soft reset failed");

    /* 复位后立刻改回从机，消除坑 A 的争用窗口 */
    ESP_RETURN_ON_ERROR(es8388_reg_write(0x08, 0x00), TAG,
                        "force slave mode failed");

    ESP_RETURN_ON_ERROR(es8388_config_playback(), TAG, "codec playback cfg failed");

    /* 在这里 dump：此时 I2S 时钟还没启动，I2C 稳定；等到时钟跑起来
     * 再 dump 会大量 timeout，读到的值不可信。 */
    es8388_dump_registers();

    /* ---- 3. I2S：先只开 TX 单工 ----
     * 这一阶段只验证播放。全双工在 ESP32-S3 上会把 RX 通道强制切成从机
     * 并与 TX 共享 BCLK/WS，多一层耦合就多一个排查变量。等做录音时
     * 再改成 tx+rx 同时创建。 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                            I2S_ROLE_MASTER);
    /* DMA 5 个描述符 x 240 帧 = 50ms 缓冲。太小会欠载（断续），
     * 太大会让"按下说话"到"数据送出"的延迟变长。 */
    chan_cfg.dma_desc_num  = 5;
    chan_cfg.dma_frame_num = 240;
    /* 没数据可发时自动补零：时钟一直跑，示波器好量，
     * 也不会因为来不及写而输出上一段的残留波形。 */
    chan_cfg.auto_clear    = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG,
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

    /* ---- 4. 使能 TX：MCLK/BCLK/WS 立刻开始输出 ----
     * 此时 codec 已是从机且已解除静音、输出级已打开，DOUT 上是 0。
     * 所以时钟一起就能正常出声，且不会有爆音。 */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx enable failed");

    /* 先送几帧静音，确保数据线是确定的低电平，而不是悬空 */
    ESP_RETURN_ON_ERROR(audio_write_silence(80), TAG, "silence failed");

    /* ---- 5. 最后开功放 ---- */
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_SPK_EN, SPK_LEVEL_ON), TAG,
                        "spk_en on failed");

    s_inited = true;
    ESP_LOGI(TAG, "audio up: %d Hz / %d bit / %d ch, codec slave, "
             "spk_en=IO%d ON (active %s, level=%d)",
             AUDIO_SAMPLE_RATE, AUDIO_BITS, AUDIO_CHANNELS, PIN_SPK_EN,
             SPK_EN_ACTIVE_LOW ? "LOW" : "HIGH", SPK_LEVEL_ON);
    ESP_LOGI(TAG, "spk_en reads %d (expect %d) | phone_ck=%d (%s)",
             gpio_get_level(PIN_SPK_EN), SPK_LEVEL_ON,
             gpio_get_level(PIN_PHONE_CK),
             gpio_get_level(PIN_PHONE_CK) ? "HEADPHONE" : "no headphone");
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
