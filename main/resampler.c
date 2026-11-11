/*
 * resampler.c —— 有理数重采样实现
 *
 * 系数在运行时算（97 个 tap，几毫秒），不用预生成表：
 * 这样改 RS_TAPS / RS_CUTOFF_RATIO 后行为立刻跟着变，
 * 也便于对照注释理解每一项的来历。
 */
#include "resampler.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "resampler";

/* ==================== 滤波器设计 ==================== */

/* 零阶修正贝塞尔函数 I0(x)，Kaiser 窗需要。
 * 用级数展开，beta <= 10 时 25 项收敛到 1e-12 以下。 */
static double bessel_i0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k <= 25; k++) {
        double t = x / (2.0 * k);
        term *= t * t;
        sum += term;
        if (term < 1e-12 * sum) {
            break;
        }
    }
    return sum;
}

/* Kaiser 窗：旁瓣衰减由 beta 决定，7.0 约 -60 dB，对语音足够。
 * denom = I0(beta) 由调用方算一次传入（同一 beta 下是常数）。 */
static double kaiser_win(int i, int n, double beta, double denom)
{
    if (n <= 1) {
        return 1.0;
    }
    double r = 2.0 * (double)i / (double)(n - 1) - 1.0;
    double v = 1.0 - r * r;
    if (v < 0.0) {
        v = 0.0;
    }
    return bessel_i0(beta * sqrt(v)) / denom;
}

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

esp_err_t resampler_init(resampler_t *rs, uint32_t fs_in, uint32_t fs_out)
{
    ESP_RETURN_ON_FALSE(rs != NULL, ESP_ERR_INVALID_ARG, TAG, "rs NULL");
    ESP_RETURN_ON_FALSE(fs_in > 0 && fs_out > 0, ESP_ERR_INVALID_ARG, TAG,
                        "bad rates");

    memset(rs, 0, sizeof(*rs));
    rs->fs_in = fs_in;
    rs->fs_out = fs_out;

    uint32_t g = gcd_u32(fs_in, fs_out);
    int L = (int)(fs_out / g);
    int M = (int)(fs_in / g);
    ESP_RETURN_ON_FALSE(L <= RS_MAX_LM && M <= RS_MAX_LM,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "ratio %d:%d too large (max %d)", L, M, RS_MAX_LM);

    rs->L = L;
    rs->M = M;
    rs->taps = RS_TAPS;
    rs->delay = (RS_TAPS - 1) / 2;

    /* 原型滤波器采样率（L 与 M 的最小公倍数对应的网格） */
    const double proto_fs = (double)fs_in * (double)L;

    /* 截止频率：取两者 Nyquist 中较小的那个再打 0.9 折。
     * 24k<->16k 两个方向都是 0.9 * 8000 = 7200 Hz，
     * 因此正反两个滤波器形状相同，只是 L/M 互换。 */
    uint32_t nyq = (fs_in < fs_out ? fs_in : fs_out) / 2;
    double cutoff = RS_CUTOFF_RATIO * (double)nyq;
    double fc = cutoff / proto_fs;          /* 归一化频率（周期/采样） */

    const double beta = 7.0;
    const double w_denom = bessel_i0(beta);
    double sum = 0.0;
    for (int i = 0; i < RS_TAPS; i++) {
        double n = (double)i - (double)rs->delay;
        double s;
        if (n == 0.0) {
            s = 1.0;
        } else {
            double arg = 2.0 * M_PI * fc * n;
            s = sin(arg) / arg;
        }
        double v = 2.0 * fc * s * kaiser_win(i, RS_TAPS, beta, w_denom);
        rs->h[i] = (float)v;
        sum += v;
    }

    /* 关键归一化：sum(h) 必须等于 L，而不是 1。
     * 升采样插了 L-1 个零，若不乘回 L，输出幅度会小 L 倍
     * （实测 24k->16k 时幅度正好减半）。 */
    if (fabs(sum) < 1e-12) {
        ESP_LOGE(TAG, "filter sum ~ 0, design failed");
        return ESP_FAIL;
    }
    float k = (float)((double)L / sum);
    for (int i = 0; i < RS_TAPS; i++) {
        rs->h[i] *= k;
    }

    rs->ready = true;
    ESP_LOGI(TAG, "%u -> %u Hz: L=%d M=%d, %d taps (prototype %.0f Hz, "
             "cutoff %.0f Hz, beta %.1f)",
             (unsigned)fs_in, (unsigned)fs_out, L, M, RS_TAPS, proto_fs, cutoff,
             beta);
    return ESP_OK;
}

size_t resampler_out_frames(const resampler_t *rs, size_t in_frames)
{
    if (rs == NULL || !rs->ready) {
        return 0;
    }
    /* 向上取整：in*L/M */
    return (in_frames * (size_t)rs->L + (size_t)rs->M - 1) / (size_t)rs->M;
}

esp_err_t resampler_process(const resampler_t *rs,
                            const int16_t *in, size_t in_frames,
                            int16_t *out, size_t out_cap, size_t *out_frames)
{
    ESP_RETURN_ON_FALSE(rs != NULL && rs->ready, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");
    ESP_RETURN_ON_FALSE(in != NULL && out != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "NULL buffer");

    size_t out_len = resampler_out_frames(rs, in_frames);
    ESP_RETURN_ON_FALSE(out_len <= out_cap, ESP_ERR_INVALID_SIZE, TAG,
                        "out buffer too small: need %u, have %u",
                        (unsigned)out_len, (unsigned)out_cap);

    const int L = rs->L;
    const int M = rs->M;
    const int taps = rs->taps;
    const int D = rs->delay;
    const float *h = rs->h;

    for (size_t m = 0; m < out_len; m++) {
        /* 原型网格上的中心位置：m*M 是时间轴，+D 抵消滤波器群延迟 */
        long c = (long)m * M + D;

        /* 最大合法输入下标：c - j*L >= 0  =>  j <= c/L */
        long j = c / L;
        if (j > (long)in_frames - 1) {
            j = (long)in_frames - 1;
        }

        float acc = 0.0f;
        /* j 递减时索引 idx 递增，超出 taps-1 就没贡献了 */
        for (; j >= 0; j--) {
            long idx = c - j * L;
            if (idx > taps - 1) {
                break;
            }
            acc += h[idx] * (float)in[j];
        }

        /* 饱和到 int16 */
        if (acc > 32767.0f) {
            acc = 32767.0f;
        } else if (acc < -32768.0f) {
            acc = -32768.0f;
        }
        out[m] = (int16_t)(acc >= 0.0f ? acc + 0.5f : acc - 0.5f);
    }

    if (out_frames != NULL) {
        *out_frames = out_len;
    }
    return ESP_OK;
}

esp_err_t resampler_selftest(float *snr_db)
{
    /* 0.5 秒 1 kHz 正弦，24k -> 16k，与解析式逐点比对。
     * 跳过首尾各 20 ms 避开零填充过渡区。 */
    const size_t n_in = 24000 / 2;
    const float  amp = 0.5f;
    const float  freq = 1000.0f;

    /* 优先 PSRAM，退回内部 RAM：自检不该因为内存类型而失败 */
    int16_t *x = heap_caps_malloc(n_in * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (x == NULL) {
        x = heap_caps_malloc(n_in * sizeof(int16_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (x == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < n_in; i++) {
        x[i] = (int16_t)(amp * 32767.0f *
                         sinf(2.0f * (float)M_PI * freq * (float)i / 24000.0f));
    }

    resampler_t rs;
    esp_err_t err = resampler_init(&rs, 24000, 16000);
    if (err != ESP_OK) {
        heap_caps_free(x);
        return err;
    }

    size_t n_out = resampler_out_frames(&rs, n_in);
    int16_t *y = heap_caps_malloc(n_out * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (y == NULL) {
        y = heap_caps_malloc(n_out * sizeof(int16_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (y == NULL) {
        heap_caps_free(x);
        return ESP_ERR_NO_MEM;
    }

    err = resampler_process(&rs, x, n_in, y, n_out, &n_out);
    if (err == ESP_OK) {
        size_t skip = 16000 / 50;       /* 20 ms */
        double err_e = 0.0, sig_e = 0.0;
        for (size_t i = skip; i + skip < n_out; i++) {
            double ideal = (double)amp * 32767.0 *
                           sin(2.0 * M_PI * (double)freq * (double)i / 16000.0);
            double e = (double)y[i] - ideal;
            err_e += e * e;
            sig_e += ideal * ideal;
        }
        double snr = 10.0 * log10(sig_e / (err_e > 0.0 ? err_e : 1e-30));
        if (snr_db != NULL) {
            *snr_db = (float)snr;
        }
        ESP_LOGI(TAG, "selftest 24k->16k 1 kHz: SNR = %.1f dB (expect > 80)", snr);
        if (snr < 60.0) {
            ESP_LOGE(TAG, "selftest FAILED -- resampler is broken");
            err = ESP_FAIL;
        }
    }

    heap_caps_free(y);
    heap_caps_free(x);
    return err;
}
