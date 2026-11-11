/*
 * opus_pipeline.c —— Opus 编解码验证流水线实现
 *
 * 设计说明见 opus_pipeline.h。这里补充实现层面的取舍：
 *
 * 1. 中间缓冲全部走 PSRAM：10 秒 16 kHz 单声道就有 320 KB，
 *    内部 RAM 装不下（还要留给 WiFi / LCD / I2S DMA）。
 *    PSRAM 不可用时退回内部 RAM，但会限制处理时长。
 *
 * 2. 编码是一帧一帧喂的：esp_opus_enc_process 要求输入长度是
 *    帧长的整数倍，逐帧调用最直观，也便于统计每包大小。
 *
 * 3. 对齐搜索先粗后细：先每 4 个点采样算互相关定位大致延迟，
 *    再在 ±4 点内逐点精修。10 秒音频的暴力全搜索是 4 亿次乘加，
 *    在 160 MHz 上要好几秒，粗搜索只要 1/4。
 */
#include "opus_pipeline.h"

#include <math.h>
#include <string.h>

#include "audio_i2s.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "feature_config.h"

#include "esp_audio_types.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#include "resampler.h"

static const char *TAG = "opus";

/* ==================== 小工具 ==================== */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 优先 PSRAM，失败退回内部 RAM */
static void *buf_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p != NULL) {
            ESP_LOGW(TAG, "PSRAM alloc failed (%u B), used internal RAM",
                     (unsigned)bytes);
        }
    }
    return p;
}

static void buf_free(void *p)
{
    if (p != NULL) {
        heap_caps_free(p);
    }
}

static double to_dbfs_i16(double rms)
{
    if (rms < 1.0) {
        return -120.0;
    }
    return 20.0 * log10(rms / 32768.0);
}

/* 十六进制打印，每行 16 字节，带包编号 */
static void hexdump_packet(int idx, const uint8_t *d, size_t n)
{
    char line[16 * 3 + 8];
    for (size_t off = 0; off < n; off += 16) {
        size_t cnt = n - off > 16 ? 16 : n - off;
        int pos = 0;
        for (size_t i = 0; i < cnt; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", d[off + i]);
        }
        if (off == 0) {
            ESP_LOGI(TAG, "  #%-3d %3uB | %s", idx, (unsigned)n, line);
        } else {
            ESP_LOGI(TAG, "              | %s", line);
        }
    }
}

/* ==================== 对齐与误差统计 ==================== */

/* 在 dec 相对 ref 的延迟上搜索：找使 Σref[i]*dec[i+lag] 最大的 lag。
 * 用抽样粗搜 + 局部精修，兼顾速度与精度。 */
static int find_delay(const int16_t *ref, size_t nref,
                      const int16_t *dec, size_t ndec, int max_lag)
{
    if (ndec <= (size_t)max_lag + 64) {
        return 0;
    }
    size_t n = nref < ndec ? nref : ndec;
    /* 只在中段搜索，避开首尾的零填充/尾帧缺失 */
    size_t lo = n / 10;
    size_t hi = n - n / 10;

    int best_lag = 0;
    double best = -1e300;

    /* 粗搜：步长 4 */
    for (int lag = 0; lag <= max_lag; lag += 4) {
        double acc = 0.0;
        for (size_t i = lo; i < hi; i += 4) {
            if (i + (size_t)lag >= ndec) {
                break;
            }
            acc += (double)ref[i] * (double)dec[i + lag];
        }
        if (acc > best) {
            best = acc;
            best_lag = lag;
        }
    }

    /* 精修：±4 内逐点 */
    int lo2 = best_lag - 4 < 0 ? 0 : best_lag - 4;
    int hi2 = best_lag + 4 > max_lag ? max_lag : best_lag + 4;
    for (int lag = lo2; lag <= hi2; lag++) {
        double acc = 0.0;
        for (size_t i = lo; i < hi; i += 2) {
            if (i + (size_t)lag >= ndec) {
                break;
            }
            acc += (double)ref[i] * (double)dec[i + lag];
        }
        if (acc > best) {
            best = acc;
            best_lag = lag;
        }
    }
    return best_lag;
}

/* ==================== 主流程 ==================== */

esp_err_t opus_verify_run(const int16_t *pcm24k, size_t frames24k,
                          opus_result_t *res)
{
    ESP_RETURN_ON_FALSE(pcm24k != NULL && frames24k > 0, ESP_ERR_INVALID_ARG,
                        TAG, "no input");

    opus_result_t r;
    memset(&r, 0, sizeof(r));
    uint32_t t_start = now_ms();

    /* ---- 0. 限制处理时长 ---- */
    size_t max_frames = (size_t)OPUS_VERIFY_MAX_SECONDS * AUDIO_SAMPLE_RATE;
    if (frames24k > max_frames) {
        ESP_LOGW(TAG, "input %u frames > %d s limit, truncating",
                 (unsigned)frames24k, OPUS_VERIFY_MAX_SECONDS);
        frames24k = max_frames;
    }
    r.in_frames_24k = frames24k;

    ESP_LOGI(TAG, "==== Opus verify start ====");
    ESP_LOGI(TAG, "input : %u frames @ %d Hz = %u ms",
             (unsigned)frames24k, AUDIO_SAMPLE_RATE,
             (unsigned)(frames24k * 1000 / AUDIO_SAMPLE_RATE));

    /* ---- 1. 重采样 24k -> 16k ---- */
    uint32_t t0 = now_ms();
    resampler_t rs;
    ESP_RETURN_ON_ERROR(resampler_init(&rs, AUDIO_SAMPLE_RATE, OPUS_SAMPLE_RATE),
                        TAG, "resampler init failed");

    size_t n16 = resampler_out_frames(&rs, frames24k);
    int16_t *pcm16 = buf_alloc(n16 * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(pcm16 != NULL, ESP_ERR_NO_MEM, TAG,
                        "pcm16 alloc %u B failed", (unsigned)(n16 * 2));
    esp_err_t err = resampler_process(&rs, pcm24k, frames24k, pcm16, n16, &n16);
    if (err != ESP_OK) {
        buf_free(pcm16);
        return err;
    }
    r.frames_16k = n16;
    r.resample_ms = now_ms() - t0;
    ESP_LOGI(TAG, "resample: %u -> %u frames (%u ms), %u ms elapsed",
             (unsigned)frames24k, (unsigned)n16,
             (unsigned)(n16 * 1000 / OPUS_SAMPLE_RATE), (unsigned)r.resample_ms);

    /* ---- 2. 打开编码器 ---- */
    esp_opus_enc_config_t ecfg = {0};
    ecfg.sample_rate      = OPUS_SAMPLE_RATE;
    ecfg.channel          = ESP_AUDIO_MONO;
    ecfg.bits_per_sample  = ESP_AUDIO_BIT16;
    ecfg.bitrate          = OPUS_BITRATE;
    ecfg.frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    ecfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    ecfg.complexity       = OPUS_COMPLEXITY;
    ecfg.enable_fec       = false;
    ecfg.enable_dtx       = false;      /* 关掉 DTX：否则停顿处不发包，比对会错位 */
    ecfg.enable_vbr       = OPUS_ENABLE_VBR;

    void *enc = NULL;
    int ret = esp_opus_enc_open(&ecfg, sizeof(ecfg), &enc);
    if (ret != ESP_AUDIO_ERR_OK || enc == NULL) {
        ESP_LOGE(TAG, "encoder open failed: %d", ret);
        buf_free(pcm16);
        return ESP_FAIL;
    }

    int in_bytes = 0, out_bytes = 0;
    ret = esp_opus_enc_get_frame_size(enc, &in_bytes, &out_bytes);
    if (ret != ESP_AUDIO_ERR_OK || in_bytes <= 0 || out_bytes <= 0) {
        ESP_LOGE(TAG, "get_frame_size failed: %d (%d/%d)", ret, in_bytes,
                 out_bytes);
        esp_opus_enc_close(enc);
        buf_free(pcm16);
        return ESP_FAIL;
    }
    int frame_samples = in_bytes / 2;
    /* 防御：get_frame_size 返回的是字节数，帧长应为 20ms。
     * 万一库的约定和我们理解不同（比如返回样本数），这里会明显报出来，
     * 否则后面按错误的帧长切分，会得到"能跑但结果诡异"的输出。 */
    int expect_samples = OPUS_SAMPLE_RATE * 20 / 1000;
    if (frame_samples != expect_samples) {
        ESP_LOGW(TAG, "unexpected frame size: %d samples (in_bytes=%d), "
                 "expected %d for 20 ms @ %d Hz",
                 frame_samples, in_bytes, expect_samples, OPUS_SAMPLE_RATE);
    }
    ESP_LOGI(TAG, "encoder: %d Hz mono, %d ms/frame (%d samples), %d bps, "
             "complexity %d, vbr %d", OPUS_SAMPLE_RATE,
             (int)(frame_samples * 1000 / OPUS_SAMPLE_RATE), frame_samples,
             OPUS_BITRATE, OPUS_COMPLEXITY, (int)OPUS_ENABLE_VBR);

    /* ---- 3. 逐帧编码 ---- */
    t0 = now_ms();
    int nframes = (int)(n16 / (size_t)frame_samples);   /* 只编码完整帧 */
    if (nframes < 1) {
        ESP_LOGE(TAG, "too short: %u samples < one frame (%d)", (unsigned)n16,
                 frame_samples);
        esp_opus_enc_close(enc);
        buf_free(pcm16);
        return ESP_FAIL;
    }
    size_t obuf_size = (size_t)nframes * (size_t)out_bytes;
    uint8_t *obuf = buf_alloc(obuf_size);
    if (obuf == NULL) {
        ESP_LOGE(TAG, "opus buffer alloc %u B failed", (unsigned)obuf_size);
        esp_opus_enc_close(enc);
        buf_free(pcm16);
        return ESP_ERR_NO_MEM;
    }
    size_t *pkt_len = buf_alloc((size_t)nframes * sizeof(size_t));
    size_t *pkt_off = buf_alloc((size_t)nframes * sizeof(size_t));
    if (pkt_len == NULL || pkt_off == NULL) {
        ESP_LOGE(TAG, "packet index alloc failed");
        buf_free(obuf); buf_free(pkt_len); buf_free(pkt_off);
        esp_opus_enc_close(enc);
        buf_free(pcm16);
        return ESP_ERR_NO_MEM;
    }

    int encoded = 0;
    size_t used = 0;
    size_t pmin = (size_t)-1, pmax = 0;
    for (int i = 0; i < nframes; i++) {
        esp_audio_enc_in_frame_t in = {
            .buffer = (uint8_t *)pcm16 + (size_t)i * (size_t)in_bytes,
            .len    = (uint32_t)in_bytes,
        };
        esp_audio_enc_out_frame_t out = {
            .buffer = obuf + used,
            .len    = (uint32_t)(obuf_size - used),
            .encoded_bytes = 0,
        };
        ret = esp_opus_enc_process(enc, &in, &out);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "encode frame %d failed: %d", i, ret);
            break;
        }
        pkt_len[encoded] = out.encoded_bytes;
        pkt_off[encoded] = used;
        used += out.encoded_bytes;
        if (out.encoded_bytes < pmin) {
            pmin = out.encoded_bytes;
        }
        if (out.encoded_bytes > pmax) {
            pmax = out.encoded_bytes;
        }
        encoded++;
    }
    esp_opus_enc_close(enc);
    r.encode_ms = now_ms() - t0;

    if (encoded <= 0) {
        ESP_LOGE(TAG, "no packet encoded");
        buf_free(obuf); buf_free(pkt_len); buf_free(pkt_off); buf_free(pcm16);
        return ESP_FAIL;
    }

    r.packets    = encoded;
    r.opus_bytes = used;
    r.pkt_min    = pmin;
    r.pkt_max    = pmax;
    double dur_s = (double)encoded * frame_samples / (double)OPUS_SAMPLE_RATE;
    r.kbps_actual = dur_s > 0.0 ? (double)used * 8.0 / dur_s / 1000.0 : 0.0;

    /* ---- 4. 打印编码结果 ---- */
    ESP_LOGI(TAG, "encoded: %d packets, %u bytes in %u ms", encoded,
             (unsigned)used, (unsigned)r.encode_ms);
    ESP_LOGI(TAG, "  actual bitrate %.1f kbps (target %d), pkt %u~%u B (avg %.1f)",
             r.kbps_actual, OPUS_BITRATE, (unsigned)pmin, (unsigned)pmax,
             (double)used / encoded);
    ESP_LOGI(TAG, "  compression: %u B PCM(16k) -> %u B = %.1f%%",
             (unsigned)(encoded * frame_samples * 2), (unsigned)used,
             100.0 * (double)used / (double)(encoded * frame_samples * 2));

#if OPUS_PRINT_PACKETS > 0
    int show = encoded < OPUS_PRINT_PACKETS ? encoded : OPUS_PRINT_PACKETS;
    ESP_LOGI(TAG, "first %d of %d packets (hex):", show, encoded);
    for (int i = 0; i < show; i++) {
        hexdump_packet(i, obuf + pkt_off[i], pkt_len[i]);
    }
#endif

    /* ---- 5. 解码 ---- */
    t0 = now_ms();
    esp_opus_dec_cfg_t dcfg = {0};
    dcfg.sample_rate    = OPUS_SAMPLE_RATE;
    dcfg.channel        = ESP_AUDIO_MONO;
    dcfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;  /* 自适应 */
    dcfg.self_delimited = false;

    void *dec = NULL;
    ret = esp_opus_dec_open(&dcfg, sizeof(dcfg), &dec);
    if (ret != ESP_AUDIO_ERR_OK || dec == NULL) {
        ESP_LOGE(TAG, "decoder open failed: %d", ret);
        buf_free(obuf); buf_free(pkt_len); buf_free(pkt_off); buf_free(pcm16);
        return ESP_FAIL;
    }

    /* 解码输出缓冲：每包最多 frame_samples 点 */
    size_t dcap = (size_t)encoded * (size_t)frame_samples;
    int16_t *pcm_dec = buf_alloc(dcap * sizeof(int16_t));
    if (pcm_dec == NULL) {
        ESP_LOGE(TAG, "decode buffer alloc %u B failed", (unsigned)(dcap * 2));
        esp_opus_dec_close(dec);
        buf_free(obuf); buf_free(pkt_len); buf_free(pkt_off); buf_free(pcm16);
        return ESP_ERR_NO_MEM;
    }

    size_t dec_used = 0;      /* 已解码的样本数 */
    int dec_ok = 0;
    for (int i = 0; i < encoded; i++) {
        esp_audio_dec_in_raw_t raw = {
            .buffer = obuf + pkt_off[i],
            .len    = (uint32_t)pkt_len[i],
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t out = {
            .buffer = (uint8_t *)(pcm_dec + dec_used),
            .len    = (uint32_t)((dcap - dec_used) * sizeof(int16_t)),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t dinfo = {0};
        ret = esp_opus_dec_decode(dec, &raw, &out, &dinfo);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "decode packet %d failed: %d (skip)", i, ret);
            continue;
        }
        dec_used += out.decoded_size / sizeof(int16_t);
        dec_ok++;
    }
    esp_opus_dec_close(dec);
    r.decode_ms = now_ms() - t0;
    r.dec_frames = dec_used;

    ESP_LOGI(TAG, "decoded: %d/%d packets -> %u frames @ %d Hz (%u ms), %u ms",
             dec_ok, encoded, (unsigned)dec_used, OPUS_SAMPLE_RATE,
             (unsigned)(dec_used * 1000 / OPUS_SAMPLE_RATE),
             (unsigned)r.decode_ms);

    if (dec_used == 0) {
        ESP_LOGE(TAG, "nothing decoded");
        buf_free(pcm_dec); buf_free(obuf);
        buf_free(pkt_len); buf_free(pkt_off); buf_free(pcm16);
        return ESP_FAIL;
    }

    /* ---- 6. 对齐 + 误差统计 ---- */
    /* Opus 前瞻在 16 kHz 下约 6.5 ms（104 点），留 25 ms 搜索范围 */
    int max_lag = OPUS_SAMPLE_RATE * 25 / 1000;
    int lag = find_delay(pcm16, n16, pcm_dec, dec_used, max_lag);
    r.delay_samples = lag;

    size_t skip = (size_t)(OPUS_SAMPLE_RATE / 50);      /* 首尾各跳 20 ms */
    double sig_e = 0.0, err_e = 0.0;
    double sum_r = 0.0, sum_d = 0.0, sum_rr = 0.0, sum_dd = 0.0, sum_rd = 0.0;
    int32_t max_err = 0;
    size_t cnt = 0;

    /* 有效区间：起点跳 skip，终点再跳 skip，且不超过解码长度 */
    size_t i_end = n16;
    if (i_end > skip) {
        i_end -= skip;
    }
    for (size_t i = skip; i < i_end; i++) {
        size_t j = i + (size_t)lag;
        if (j >= dec_used) {
            break;
        }
        double a = (double)pcm16[i];
        double b = (double)pcm_dec[j];
        double e = a - b;
        sig_e += a * a;
        err_e += e * e;
        double ae = fabs(e);
        if (ae > (double)max_err) {
            max_err = (int32_t)ae;
        }
        sum_r += a;  sum_d += b;
        sum_rr += a * a;  sum_dd += b * b;  sum_rd += a * b;
        cnt++;
    }

    if (cnt > 0) {
        r.snr_db = err_e > 0.0 ? 10.0 * log10(sig_e / err_e) : 999.0;
        r.max_abs_err = max_err;
        /* 两路 rms 都从同一批样本算，避免范围不一致导致"电平差"虚假 */
        r.rms_ref_dbfs = to_dbfs_i16(sqrt(sum_rr / (double)cnt));
        r.rms_dec_dbfs = to_dbfs_i16(sqrt(sum_dd / (double)cnt));

        /* Pearson 相关系数 */
        double cov = sum_rd / cnt - (sum_r / cnt) * (sum_d / cnt);
        double vr = sum_rr / cnt - (sum_r / cnt) * (sum_r / cnt);
        double vd = sum_dd / cnt - (sum_d / cnt) * (sum_d / cnt);
        r.corr = (vr > 0.0 && vd > 0.0) ? cov / sqrt(vr * vd) : 0.0;
    }

    ESP_LOGI(TAG, "---- accuracy (aligned, edges trimmed 20 ms) ----");
    ESP_LOGI(TAG, "  delay    : %d samples (%.2f ms)  <- Opus lookahead + resampler",
             lag, lag * 1000.0 / OPUS_SAMPLE_RATE);
    ESP_LOGI(TAG, "  SNR      : %.1f dB", r.snr_db);
    ESP_LOGI(TAG, "  corr     : %.5f", r.corr);
    ESP_LOGI(TAG, "  max |err|: %d (%.1f%% FS)", (int)r.max_abs_err,
             100.0 * r.max_abs_err / 32768.0);
    ESP_LOGI(TAG, "  rms      : ref %.1f dBFS, dec %.1f dBFS (delta %+.1f dB)",
             r.rms_ref_dbfs, r.rms_dec_dbfs, r.rms_dec_dbfs - r.rms_ref_dbfs);

    /* 判据：语音在 24 kbps 下正常应 corr > 0.95、SNR > 8 dB。
     * 达不到就明确报出来，不要让人误以为"能播就是对的"。 */
    bool ok = (cnt > 0) && (r.corr > 0.95) && (r.snr_db > 8.0);
    if (ok) {
        ESP_LOGI(TAG, "RESULT: encode/decode OK");
    } else {
        ESP_LOGW(TAG, "RESULT: metrics below expectation "
                 "(corr>0.95, SNR>8 dB) -- check bitrate/input level");
    }

#if OPUS_PLAY_DECODED
    /* 把解码结果重采样回 24k 播一遍，用人耳做最终确认 */
    resampler_t rs_up;
    if (resampler_init(&rs_up, OPUS_SAMPLE_RATE, AUDIO_SAMPLE_RATE) == ESP_OK) {
        size_t n24 = resampler_out_frames(&rs_up, dec_used);
        int16_t *play = buf_alloc(n24 * sizeof(int16_t));
        if (play != NULL &&
            resampler_process(&rs_up, pcm_dec, dec_used, play, n24, &n24) == ESP_OK) {
            audio_spk_enable(true);
            size_t chunk = (size_t)AUDIO_SAMPLE_RATE / 10;   /* 100 ms */
            size_t off = 0;
            int16_t *stereo = buf_alloc(chunk * AUDIO_FRAME_BYTES);
            if (stereo != NULL) {
                while (off < n24) {
                    size_t f = n24 - off > chunk ? chunk : n24 - off;
                    for (size_t i = 0; i < f; i++) {
                        stereo[i * 2] = play[off + i];
                        stereo[i * 2 + 1] = play[off + i];
                    }
                    if (audio_write(stereo, f * AUDIO_FRAME_BYTES) != ESP_OK) {
                        break;
                    }
                    off += f;
                }
                buf_free(stereo);
            }
            audio_spk_enable(false);
            ESP_LOGI(TAG, "played decoded audio (%u ms)", (unsigned)(n24 * 1000 / AUDIO_SAMPLE_RATE));
        }
        buf_free(play);
    }
#endif

    r.total_ms = now_ms() - t_start;
    ESP_LOGI(TAG, "==== Opus verify done in %u ms ====", (unsigned)r.total_ms);

    buf_free(pcm_dec);
    buf_free(obuf);
    buf_free(pkt_len);
    buf_free(pkt_off);
    buf_free(pcm16);

    if (res != NULL) {
        *res = r;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

/* ==================== 自检 ==================== */

esp_err_t opus_verify_selftest(void)
{
    ESP_LOGI(TAG, "---- Opus selftest (synthetic input, no mic) ----");

    /* 先单独验重采样：这一步失败说明问题不在 Opus */
    float rs_snr = 0.0f;
    esp_err_t err = resampler_selftest(&rs_snr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resampler selftest failed -- fix this first");
        return err;
    }

    /* 合成一段"类语音"信号：基频 + 谐波 + 幅度包络，
     * 比纯正弦更能反映 Opus 对语音的行为 */
    const size_t n = AUDIO_SAMPLE_RATE;      /* 1 秒 */
    int16_t *pcm = buf_alloc(n * sizeof(int16_t));
    if (pcm == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / (double)AUDIO_SAMPLE_RATE;
        double f0 = 180.0 + 40.0 * sin(2.0 * M_PI * 0.7 * t);   /* 轻微变化的基频 */
        double v = 0.0;
        for (int h = 1; h <= 5; h++) {
            v += sin(2.0 * M_PI * f0 * h * t) / h;
        }
        /* 4 Hz 的幅度包络，模拟音节 */
        double env = 0.5 + 0.5 * sin(2.0 * M_PI * 4.0 * t);
        v = v * env * 0.15;
        pcm[i] = (int16_t)(v * 32767.0);
    }

    opus_result_t r;
    err = opus_verify_run(pcm, n, &r);
    buf_free(pcm);
    return err;
}
