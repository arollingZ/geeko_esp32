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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_i2s.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "feature_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if APP_REC_DIAG_BOOT && APP_ENABLE_WIFI
#include "wifi_mgr.h"
#endif

#if APP_ENABLE_OPUS && OPUS_VERIFY_ON_RECORD
#include "opus_pipeline.h"
#endif

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

/* ==================== 录音前端处理（一阶高通 + 二阶低通） ==================== */

/*
 * 为什么要滤波：ES8388 的 MIC PGA 拉到 +24dB 之后，麦克风自噪声、
 * WiFi 射频耦合、电源纹波一起被放大进来，录进缓冲里就是一层嘶声。
 * 语音的有用能量主要在 300Hz~3.4kHz，所以两头都切：
 *   - 高通 100Hz：去掉 ADC 直流偏置和低频嗡声（直流偏置会让喇叭音圈偏移）
 *   - 低通 7kHz： 压掉语音带外的嘶声（24kHz 采样下 7k 以上对人声没用）
 * 都是线性相位无关的一/二阶 IIR，每个采样不到 20 条指令，24kHz 下
 * 占 ESP32-S3 的算力可以忽略。
 */
typedef struct {
    float hp_a;              /* 高通系数 */
    float hp_x1, hp_y1;      /* 高通的历史输入/输出 */
    float lp_b0, lp_b1, lp_b2, lp_a1, lp_a2;   /* 低通双二阶系数 */
    float lp_x1, lp_x2, lp_y1, lp_y2;          /* 低通状态 */
} rec_filter_t;

static rec_filter_t s_flt;

/* 初始化系数并清零历史状态。每次录音开始时调用，避免上一段的尾巴带过来。 */
static void filter_reset(void)
{
    const float fs = (float)AUDIO_SAMPLE_RATE;

    memset(&s_flt, 0, sizeof(s_flt));

    /* 一阶高通：y[n] = a * (y[n-1] + x[n] - x[n-1]) */
    s_flt.hp_a = 1.0f / (1.0f + 2.0f * (float)M_PI * (float)REC_HPF_HZ / fs);

    /* 二阶 Butterworth 低通（Q = 1/√2），双线性变换 */
    const float w0    = 2.0f * (float)M_PI * (float)REC_LPF_HZ / fs;
    const float cosw  = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * 0.70710678f);
    const float a0    = 1.0f + alpha;
    s_flt.lp_b0 = ((1.0f - cosw) * 0.5f) / a0;
    s_flt.lp_b1 = (1.0f - cosw) / a0;
    s_flt.lp_b2 = s_flt.lp_b0;
    s_flt.lp_a1 = (-2.0f * cosw) / a0;
    s_flt.lp_a2 = (1.0f - alpha) / a0;
}

/* 对单个采样做高通 -> 低通，带饱和保护 */
static inline int16_t filter_sample(int16_t in)
{
    const float x = (float)in;

    float hp = s_flt.hp_a * (s_flt.hp_y1 + x - s_flt.hp_x1);
    s_flt.hp_x1 = x;
    s_flt.hp_y1 = hp;

    float y = s_flt.lp_b0 * hp + s_flt.lp_b1 * s_flt.lp_x1 +
              s_flt.lp_b2 * s_flt.lp_x2 - s_flt.lp_a1 * s_flt.lp_y1 -
              s_flt.lp_a2 * s_flt.lp_y2;
    s_flt.lp_x2 = s_flt.lp_x1;
    s_flt.lp_x1 = hp;
    s_flt.lp_y2 = s_flt.lp_y1;
    s_flt.lp_y1 = y;

    if (y > 32767.0f) {
        y = 32767.0f;
    } else if (y < -32768.0f) {
        y = -32768.0f;
    }
    return (int16_t)(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

/* ==================== 录音电平统计（用于量化"杂音到底有多大"） ==================== */

typedef struct {
    double   sum;
    double   sumsq;
    int32_t  peak;
    uint32_t n;
    uint32_t clip;
} ch_stat_t;

static ch_stat_t s_st_l;
static ch_stat_t s_st_r;
static ch_stat_t s_st_out;

/* ---- 脉冲（丢采样台阶）检测 ---- */
#define CLICK_KEEP  8       /* 最多记录 8 个间隔，够看出规律了 */

static uint32_t s_click_count;                  /* 脉冲总数 */
static uint32_t s_click_prev_idx;               /* 上一次脉冲的采样序号 */
static uint32_t s_click_iv[CLICK_KEEP];         /* 相邻脉冲间隔（采样点） */
static uint32_t s_click_iv_n;
static int16_t  s_click_prev_sample;            /* 上一个采样的原始值 */

/* ---- 噪声门统计 ---- */
static uint32_t s_gate_opens;

static void stats_reset(void)
{
    memset(&s_st_l, 0, sizeof(s_st_l));
    memset(&s_st_r, 0, sizeof(s_st_r));
    memset(&s_st_out, 0, sizeof(s_st_out));
    s_click_count       = 0;
    s_click_prev_idx    = 0;
    s_click_iv_n        = 0;
    s_click_prev_sample = 0;
    memset(s_click_iv, 0, sizeof(s_click_iv));
    s_gate_opens        = 0;
}

/* 单点检测：相邻原始采样之间跳变过大 => 疑似丢采样造成的台阶。
 * 必须在滤波之前做：滤波器本身是低通，会把台阶抹平，就检测不出来了。 */
static inline void click_check(int16_t sample, uint32_t idx)
{
    int32_t d = (int32_t)sample - (int32_t)s_click_prev_sample;
    if (d < 0) {
        d = -d;
    }
    if (d > REC_CLICK_DIFF) {
        if (s_click_count > 0 && s_click_iv_n < CLICK_KEEP) {
            s_click_iv[s_click_iv_n++] = idx - s_click_prev_idx;
        }
        s_click_prev_idx = idx;
        s_click_count++;
    }
    s_click_prev_sample = sample;
}

static inline void stat_add(ch_stat_t *s, int32_t v)
{
    int32_t a = v < 0 ? -v : v;
    s->sum   += v;
    s->sumsq += (double)v * (double)v;
    if (a > s->peak) {
        s->peak = a;
    }
    if (a >= 32767) {
        s->clip++;
    }
    s->n++;
}

/* 归一化到 dBFS：0 dBFS = 满量程 32768 */
static double to_dbfs(double v)
{
    if (v <= 1.0) {
        return -120.0;
    }
    return 20.0 * log10(v / 32768.0);
}

static void stat_log(const char *name, const ch_stat_t *s)
{
    if (s->n == 0) {
        ESP_LOGW(TAG, "  %-3s: no samples", name);
        return;
    }
    double mean = s->sum / (double)s->n;
    double rms  = sqrt(s->sumsq / (double)s->n);
    ESP_LOGI(TAG, "  %-3s dc=%+7.0f  rms=%6.1f dBFS  peak=%6.1f dBFS  clip=%.2f%%",
             name, mean, to_dbfs(rms), to_dbfs((double)s->peak),
             100.0 * (double)s->clip / (double)s->n);
}

/* 录音结束时打印三路电平，用来判断噪声来自哪一环：
 *   L 和 R 都很大 -> 麦克风/增益问题（增益太高或环境噪声）
 *   L 和 R 差很多 -> 有一路是悬空输入，改 REC_MIC_CHANNEL 只取好的一路
 *   输出比输入小很多 -> 说明高通/低通确实滤掉了东西
 *   clip 大于 1% -> 削顶了，把 REC_ADC_PGA_DB 往下调
 */
static void stats_log_all(void)
{
#if REC_STATS_LOG
    ESP_LOGI(TAG, "record level (PGA +%d dB, HPF %d Hz, LPF %d Hz):",
             REC_ADC_PGA_DB, REC_HPF_HZ, REC_LPF_HZ);
    stat_log("L", &s_st_l);
    stat_log("R", &s_st_r);
    stat_log("out", &s_st_out);

    /* 脉冲间隔：有规律 => 是某个周期性任务在抢资源；无规律 => DMA 欠载 */
    if (s_click_count > 0) {
        char iv[96];
        int  pos = 0;
        for (uint32_t i = 0; i < s_click_iv_n && pos < (int)sizeof(iv) - 8; i++) {
            uint32_t ms = s_click_iv[i] * 1000 / AUDIO_SAMPLE_RATE;
            pos += snprintf(iv + pos, sizeof(iv) - pos, "%u ", (unsigned)ms);
        }
        ESP_LOGW(TAG, "  pulses: %u (jump>%d), intervals(ms): %s",
                 (unsigned)s_click_count, REC_CLICK_DIFF, iv);
    } else {
        ESP_LOGI(TAG, "  pulses: none (no sample jumps > %d)", REC_CLICK_DIFF);
    }
#endif
}

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

/* 定义在文件后半部分（跟回放/后处理放在一起），这里先声明：
 * 录音结束时要做噪声门和归一化，函数本身不依赖录音流程的中间状态。 */
static void postprocess_recording(void);
static void analyze_profile(void);

static void do_record_start(void)
{
    s_len_bytes = 0;
    s_state = RECORDER_RECORDING;

    /* 每段录音都从干净的滤波器状态和统计开始 */
    filter_reset();
    stats_reset();

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
    stats_log_all();

    /* 电平剖面分析放在后处理之前：看的是"录进来什么样"，
     * 噪声门把停顿抹成静音之后就看不出干扰的规律了。 */
    analyze_profile();

    /* 统计打的是"录进来什么样"，之后再做滤波外的整体处理 */
    postprocess_recording();

#if APP_ENABLE_OPUS && OPUS_VERIFY_ON_RECORD
    /* Opus 编解码验证：对"最终要发送的音频"（已过噪声门和归一化）做
     * 重采样 -> 编码 -> 打印 -> 解码 -> 比对。
     *
     * 注意：这一步在 recorder 任务里同步执行，期间无法响应按键。
     * 10 秒录音大约需要几百毫秒到 1~2 秒（打印十六进制占大头）。
     * 验证阶段可以接受；正式产品应改成独立任务或直接去掉。 */
    if (s_len_bytes >= (size_t)(AUDIO_SAMPLE_RATE / 2)) {   /* 至少 0.5 秒 */
        opus_verify_run((const int16_t *)s_buf, s_len_bytes / 2, NULL);
    } else {
        ESP_LOGW(TAG, "recording too short (%u ms), skip opus verify",
                 (unsigned)(s_len_bytes / 2 * 1000 / AUDIO_SAMPLE_RATE));
    }
#endif
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
        /* 两路原始数据先记统计，方便判断麦克风到底接在哪一路、
         * 以及噪声是两路共模还是某一路独有。 */
        int32_t l = s_stereo[i * 2];
        int32_t r = s_stereo[i * 2 + 1];
        stat_add(&s_st_l, l);
        stat_add(&s_st_r, r);

        /* 取单声道：默认平均。用平均而不是取单边，是因为板上麦克风接到
         * 哪一路不确定；平均值在两路相同时不丢幅度，只有一路有信号时
         * 最多损失 6dB（PGA 可补）。若日志显示某一路rms 明显更大且没有
         * 语音成分，用 REC_MIC_CHANNEL 切到干净的那一路。 */
#if REC_MIC_CHANNEL == 1
        int16_t mix = (int16_t)l;
#elif REC_MIC_CHANNEL == 2
        int16_t mix = (int16_t)r;
#else
        int16_t mix = (int16_t)((l + r) / 2);
#endif
        /* 前端滤波：去直流 + 压高频嘶声，再把结果写进缓冲 */
        int16_t out = filter_sample(mix);
        stat_add(&s_st_out, out);
        s_mono[i] = out;

        /* 原始信号（滤波前）的跳变检测，用来抓丢采样造成的"哒"声 */
        click_check(mix, (uint32_t)(s_len_bytes / 2) + (uint32_t)i);
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

/* ==================== 录音后处理：噪声门 + 峰值归一化 ==================== */

static inline float db_to_lin(float db)
{
    return powf(10.0f, db / 20.0f) * 32768.0f;
}

/* 噪声门：低于阈值把增益压到 0，说完立即安静。
 *
 * 三个要素缺一不可：
 *
 * 1) 保持时间（REC_GATE_HOLD_MS）：门打开后至少维持这么久才允许关。
 *    没有它，语音的音节间隙会让门反复开关，听感就是"哒哒"的断续。
 *
 * 2) 前瞻（REC_GATE_LOOKAHEAD_MS）：这是**避免字头被吃掉**的关键。
 *    门需要时间判断（包络爬升 + 增益平滑约 3~6ms），而"1、2、3"这类
 *    字头的前几毫秒恰恰能量最低。如果直接处理实时信号，等门反应过来
 *    字头已经播完了。做法是把音频延迟 N 毫秒再输出，而判断仍用未延迟
 *    的信号 —— 相当于"先看到未来"，门在声音到达时已经开好。
 *
 * 3) 增益平滑（2ms）：开关瞬间不留台阶，避免爆音。
 */
static void apply_noise_gate(int16_t *p, size_t n)
{
#if REC_GATE_DB != 0
    /* 前瞻缓冲：上限 20ms，够覆盖包络+平滑的全部延迟还留余量 */
#define GATE_LA_MAX   (AUDIO_SAMPLE_RATE / 1000 * 20)
    static int16_t s_delay[GATE_LA_MAX];

    size_t la = (size_t)AUDIO_SAMPLE_RATE * REC_GATE_LOOKAHEAD_MS / 1000;
    if (la > GATE_LA_MAX) {
        la = GATE_LA_MAX;
    }
    if (la < 1) {
        la = 1;
    }
    memset(s_delay, 0, la * sizeof(int16_t));

    const float open_th = db_to_lin((float)REC_GATE_DB);
    const float fs      = (float)AUDIO_SAMPLE_RATE;

    /* 包络跟随：1ms 快起（抓住字头）、100ms 慢落（不切尾音） */
    const float att    = 1.0f - expf(-1.0f / (0.001f * fs));
    const float rel    = 1.0f - expf(-1.0f / (0.100f * fs));
    /* 增益平滑：开 2ms、关 30ms。关得慢是关键，音节间隙和尾音才不会被剁碎 */
    const float smooth_att = 1.0f - expf(-1.0f / (0.002f * fs));
    const float smooth_rel = 1.0f - expf(-1.0f / (0.030f * fs));
    /* 保持时间换算成采样点数 */
    const uint32_t hold_n = (uint32_t)((float)REC_GATE_HOLD_MS * fs / 1000.0f);

    float    env  = 0.0f;
    float    g    = 0.0f;   /* 起始关闭：录音开头的底噪不该漏出来 */
    uint32_t hold = 0;
    size_t   wr   = 0;
    const float thr_db = (float)REC_GATE_DB;

    for (size_t i = 0; i < n; i++) {
        float x  = (float)p[i];          /* 未延迟的输入：只用于判断 */
        float ax = fabsf(x);

        env += (ax - env) * (ax > env ? att : rel);

        if (env > open_th) {
            if (hold == 0) {
                s_gate_opens++;   /* 记一次"开门"，用来观察门的活跃程度 */
            }
            hold = hold_n;
        } else if (hold > 0) {
            hold--;
        }

        /* 增益目标：保持期内满增益；保持期外不直接归零，而是按
         * REC_GATE_RATIO 做"向下扩展"——低于阈值多少 dB，就按比例的
         * 若干倍往下压。这样字头爬升段（低于阈值那几毫秒）只是变小，
         * 不会被整段删除，听感上就不会有"切断"的边界。 */
        float target;
        if (hold > 0) {
            target = 1.0f;
        } else {
            float env_db = 20.0f * log10f((env + 1.0f) / 32768.0f);
            float below  = thr_db - env_db;
            if (below <= 0.0f) {
                target = 1.0f;
            } else {
                float atten_db = below * ((float)REC_GATE_RATIO - 1.0f);
                if (atten_db > 96.0f) {
                    atten_db = 96.0f;       /* 最多压 96dB，等效静音 */
                }
                target = powf(10.0f, -atten_db / 20.0f);
            }
        }

        /* 非对称平滑：开得快（2ms，跟得上字头）、关得慢（30ms，
         * 音节间隙和尾音不会被剁碎） */
        float k = (target > g) ? smooth_att : smooth_rel;
        g += (target - g) * k;

        /* 取 N 毫秒之前的样本输出，用当前的增益 —— 门比声音先就位 */
        int16_t delayed = s_delay[wr];
        s_delay[wr] = p[i];
        if (++wr >= la) {
            wr = 0;
        }

        p[i] = (int16_t)((float)delayed * g);
    }

    ESP_LOGI(TAG, "noise gate: thr %.0f dBFS, ratio %d:1, hold %d ms, "
             "lookahead %d ms -> opened %u times",
             (float)REC_GATE_DB, REC_GATE_RATIO, REC_GATE_HOLD_MS,
             REC_GATE_LOOKAHEAD_MS, (unsigned)s_gate_opens);
#undef GATE_LA_MAX
#else
    (void)p;
    (void)n;
#endif
}

/* 峰值归一化：把整段峰值抬到目标电平，避免用户为了听清把音量开大、
 * 连带把功放底噪一起放大。只放大不衰减，且有最大提升量限制，
 * 否则一段几乎无声的录音会被放大成纯噪声。 */
static void apply_normalize(int16_t *p, size_t n)
{
#if REC_NORMALIZE_DB != 0
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = p[i] < 0 ? -(int32_t)p[i] : (int32_t)p[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak <= 0) {
        ESP_LOGW(TAG, "normalize skipped: silent recording");
        return;
    }

    const float target  = db_to_lin((float)REC_NORMALIZE_DB);
    const float max_gain = powf(10.0f, (float)REC_NORMALIZE_MAX_DB / 20.0f);
    float gain = target / (float)peak;
    if (gain > max_gain) {
        gain = max_gain;
    }
    if (gain <= 1.02f) {
        ESP_LOGI(TAG, "normalize skipped: peak already at %.1f dBFS",
                 20.0f * log10f((float)peak / 32768.0f));
        return;
    }

    for (size_t i = 0; i < n; i++) {
        float v = (float)p[i] * gain;
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }
        p[i] = (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }
    ESP_LOGI(TAG, "normalize: peak %.1f -> %.1f dBFS (x%.2f)",
             20.0f * log10f((float)peak / 32768.0f),
             20.0f * log10f((float)peak * gain / 32768.0f), gain);
#else
    (void)p;
    (void)n;
#endif
}

/* ==================== 周期性干扰分析（100ms 电平剖面） ====================
 *
 * 为什么需要：上一版用"相邻采样跳变"找丢采样，实测 pulses=none，
 * 说明缓冲里的采样是连续完整的。「哒哒」却还在，那它只能是
 * **幅度上的**周期性脉冲（射频突发、电源纹波耦合进麦克风），
 * 这类脉冲不会产生采样跳变，必须从包络上看。
 *
 * 做法：按 100ms 切窗算 RMS，得到一条电平曲线：
 *   - 打印成字符剖面（一格 = 100ms），形状直接能看出是不是规律脉冲
 *   - 用 25 分位数估计本底，超过本底 12dB 的局部峰记为一次突发
 *   - 打印突发次数和相邻间隔，间隔就是元凶的指纹
 *       ~100ms -> WiFi beacon（每秒 10 次）
 *       ~50ms  -> LCD 20fps
 *       无规律 -> 随机干扰或电源
 */
#define PROF_WIN_MS     100
#define PROF_MAX_WIN    400    /* 最多分析 40 秒（100ms/格） */
#define PROF_SPARK_LEN  120    /* 剖面最多打印 120 个字符 */

static float s_prof[PROF_MAX_WIN];

static char prof_char(float dbfs)
{
    if (dbfs < -70.0f) return '.';
    if (dbfs < -60.0f) return ':';
    if (dbfs < -50.0f) return '-';
    if (dbfs < -40.0f) return '=';
    if (dbfs < -30.0f) return '+';
    if (dbfs < -20.0f) return '#';
    return '@';
}

static void analyze_profile(void)
{
#if REC_STATS_LOG
    size_t n = s_len_bytes / 2;
    if (n == 0 || s_buf == NULL) {
        return;
    }
    const int16_t *p = (const int16_t *)s_buf;
    const size_t win_samples = (size_t)AUDIO_SAMPLE_RATE * PROF_WIN_MS / 1000;

    size_t windows = n / win_samples;
    if (windows == 0) {
        return;
    }
    if (windows > PROF_MAX_WIN) {
        windows = PROF_MAX_WIN;
    }

    /* 1) 逐窗 RMS */
    for (size_t w = 0; w < windows; w++) {
        const int16_t *q = p + w * win_samples;
        double sum = 0.0;
        for (size_t i = 0; i < win_samples; i++) {
            sum += (double)q[i] * (double)q[i];
        }
        s_prof[w] = (float)to_dbfs(sqrt(sum / (double)win_samples));
    }

    /* 2) 用 dB 直方图估本底（25 分位），避免排序大数组 */
    int hist[64];
    memset(hist, 0, sizeof(hist));
    for (size_t w = 0; w < windows; w++) {
        int idx = (int)((s_prof[w] + 100.0f) * 63.0f / 100.0f);
        if (idx < 0) idx = 0;
        if (idx > 63) idx = 63;
        hist[idx]++;
    }
    size_t need = windows / 4, acc = 0;
    int    floor_bin = 0;
    for (int b = 0; b < 64; b++) {
        acc += (size_t)hist[b];
        if (acc >= need) {
            floor_bin = b;
            break;
        }
    }
    const float floor_db = (float)floor_bin * 100.0f / 63.0f - 100.0f;
    const float thr_db   = floor_db + 12.0f;

    /* 3) 找突发：超过门限的局部峰，间隔至少两格 */
    uint32_t bursts = 0;
    int      last_w = -10;
    char     iv[96];
    int      pos = 0;
    for (size_t w = 1; w + 1 < windows; w++) {
        if (s_prof[w] < thr_db) {
            continue;
        }
        if (s_prof[w] < s_prof[w - 1] || s_prof[w] < s_prof[w + 1]) {
            continue;   /* 只要局部峰 */
        }
        if (last_w >= 0 && pos < (int)sizeof(iv) - 8) {
            pos += snprintf(iv + pos, sizeof(iv) - pos, "%d ",
                            (int)(w - (size_t)last_w) * PROF_WIN_MS);
        }
        last_w = (int)w;
        bursts++;
    }

    /* 4) 打印剖面（超长时只打前 PROF_SPARK_LEN 格） */
    char spark[PROF_SPARK_LEN + 1];
    size_t shown = windows > PROF_SPARK_LEN ? PROF_SPARK_LEN : windows;
    for (size_t w = 0; w < shown; w++) {
        spark[w] = prof_char(s_prof[w]);
    }
    spark[shown] = '\0';
    ESP_LOGI(TAG, "level profile (1 char = %d ms, max 0 dBFS):", PROF_WIN_MS);
    ESP_LOGI(TAG, "  |%s|", spark);

    if (bursts > 0) {
        ESP_LOGW(TAG, "  bursts: %u (floor %.0f dBFS, thr %.0f dBFS), "
                 "intervals(ms): %s", (unsigned)bursts, floor_db, thr_db, iv);
    } else {
        ESP_LOGI(TAG, "  bursts: none above %.0f dBFS (floor %.0f dBFS)",
                 thr_db, floor_db);
    }
#endif
}

/* 录音结束后的整体处理，顺序很重要：先关门再归一化，
 * 否则归一化会把门限以下的底噪一起抬上来。 */
static void postprocess_recording(void)
{
    size_t n = s_len_bytes / 2;
    if (n == 0 || s_buf == NULL) {
        return;
    }
    int16_t *p = (int16_t *)s_buf;

    apply_noise_gate(p, n);
    apply_normalize(p, n);
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

/* ==================== 开机录音自检（诊断模式） ==================== */
#if APP_REC_DIAG_BOOT

/* 采一段静音，打印电平统计和剖面。不复用状态机：诊断不需要 UI 联动，
 * 也不希望噪声门/归一化把剖面弄花。 */
static void diag_capture(uint32_t ms, const char *label)
{
    s_len_bytes = 0;
    filter_reset();
    stats_reset();

    ESP_LOGI(TAG, "=== diag capture: %s (%u ms) ===", label, (unsigned)ms);
    uint32_t blocks = ms / 20;
    for (uint32_t i = 0; i < blocks; i++) {
        if (!record_one_block()) {
            break;
        }
    }
    stats_log_all();
    analyze_profile();
}

/* 两段对照：先断射频录一段，再恢复射频录一段。
 * 两段剖面一比，"哒哒"是不是射频造成的立刻清楚。 */
static void run_boot_diag(void)
{
    ESP_LOGW(TAG, "boot diag: 2 x 6 s capture -- keep quiet, do not touch the board");

#if APP_ENABLE_WIFI
    wifi_mgr_radio_pause();
    vTaskDelay(pdMS_TO_TICKS(1500));   /* 等射频真的停下来 */
#endif
    diag_capture(6000, "radio OFF");

#if APP_ENABLE_WIFI
    wifi_mgr_radio_resume();
    vTaskDelay(pdMS_TO_TICKS(3000));   /* 等重连，顺便把重连流量也纳进来 */
#endif
    diag_capture(6000, "radio ON");

    /* 诊断录音不要留在缓冲里，否则短按会莫名其妙播出来一段噪声 */
    s_len_bytes = 0;
    s_state = RECORDER_IDLE;
    ESP_LOGW(TAG, "boot diag done -- set APP_REC_DIAG_BOOT to 0 before shipping");
}

#else
#define run_boot_diag()  do { } while (0)
#endif

static void recorder_task(void *arg)
{
    (void)arg;

    /* 音频通路初始化放这里而不是 init 里：约 200ms，不阻塞开机。
     * 任务一启动就做好，用户第一次按键时已就绪。 */
    esp_err_t err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_init failed: %s", esp_err_to_name(err));
    }

#if APP_REC_DIAG_BOOT
    if (err == ESP_OK) {
        run_boot_diag();
    }
#endif

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
