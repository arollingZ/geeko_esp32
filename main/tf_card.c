/*
 * TF 卡（SD 卡）读写模块 —— SPI 模式实现
 *
 * 说明：
 *  - 使用 SPI2_HOST 作为 SDSPI 主机，引脚见下方宏定义（对应 readme 中的接线）。
 *  - 初始化流程参考 xiaozhi-esp32 工程 xingzhi-abs-2.0 板的 InitializeSDcardSpi()：
 *    SPI 总线 + sdspi_device_config + esp_vfs_fat_sdspi_mount 一步挂载。
 *  - 挂载后可用标准 POSIX 接口访问 /sdcard 下的文件。
 *  - 若使用杜邦线连接，建议把 TFCARD_MAX_FREQ_KHZ 降到 10000 提高稳定性。
 */

#include "tf_card.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "soc/gpio_num.h"

#include "feature_config.h"

#if APP_ENABLE_TF_CARD

static const char *TAG = "TFCARD";

/* ---------------- 硬件配置 ----------------
 * 正点原子 DNESP32S3M（WiFi 版）板载 TF 卡座的官方 SPI 接线：
 *   TF_CS=IO2 / SPI_MOSI=IO11 / SPI_SCK=IO12 / SPI_MISO=IO13
 * 即下方宏定义，与 demo README 接线表一致，无需改动。
 *
 * 注意：该板 TF 卡接口与 SPI LCD 接口共用同一条 SPI 总线（IO11/12/13），
 * 靠 CS 分时复用（xiaozhi 中 LCD_CS=IO39、LCD_DC=IO40、LCD_RST=IO38）。
 * 本 demo 不初始化 LCD，但测试时请确认 LCD 未选中/未接线，避免干扰 MISO。
 * 若 LCD 驱动已在同一固件里初始化过 SPI2_HOST，则不要重复 spi_bus_initialize，
 * 直接复用该总线挂载 SDSPI 设备即可。
 */
/* 正点原子 DNESP32S3M 板载 TF 卡座官方接线（勿改成其他板的引脚：
 * IO38/39/40 在这块板上是 LCD 的 RST/CS/DC！）
 */
#define TFCARD_SPI_HOST      SPI2_HOST
#define TFCARD_PIN_CS        GPIO_NUM_2
#define TFCARD_PIN_MOSI      GPIO_NUM_11
#define TFCARD_PIN_SCLK      GPIO_NUM_12
#define TFCARD_PIN_MISO      GPIO_NUM_13

/* xiaozhi 实现不覆盖 max_freq_khz（SDSPI 默认 20MHz），探针阶段固定 400kHz；
 * 若挂载成功后读写不稳，可恢复 host.max_freq_khz 赋值并降到 10000。
 */
#define TFCARD_MAX_FREQ_KHZ  20000

/*
 * 挂载失败时是否自动格式化 TF 卡。
 * 注意：格式化会清空卡内所有数据，默认关闭；确认是空白卡时可临时改为 1。
 */
#define TFCARD_FORMAT_IF_MOUNT_FAILED  0

/* ---------------- 自检参数 ---------------- */
#define SELFTEST_FILE        TFCARD_MOUNT_POINT "/rw_test.bin"
#define SELFTEST_TOTAL_SIZE  (64 * 1024)
#define SELFTEST_CHUNK_SIZE  (4 * 1024)
#define SELFTEST_TAIL_SIZE   512

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

/* 生成可复现的测试图案：写入与读取校验使用同一函数 */
static inline uint8_t selftest_pattern(uint32_t index)
{
    return (uint8_t)((index * 31u) ^ (index >> 8) ^ 0xA5u);
}

esp_err_t tf_card_init(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "TF card already mounted at %s", TFCARD_MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card over SPI (CS=%d MOSI=%d SCK=%d MISO=%d)",
             TFCARD_PIN_CS, TFCARD_PIN_MOSI, TFCARD_PIN_SCLK, TFCARD_PIN_MISO);

    /* 以下初始化流程与 xiaozhi-esp32 xingzhi-abs-2.0 的 InitializeSDcardSpi() 一致 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TFCARD_PIN_MOSI,
        .miso_io_num = TFCARD_PIN_MISO,
        .sclk_io_num = TFCARD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 400000,
    };

    esp_err_t ret = spi_bus_initialize(TFCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    static sdspi_device_config_t slot_config = {
        .host_id = TFCARD_SPI_HOST,
        .gpio_cs = TFCARD_PIN_CS,
        .gpio_cd = SDSPI_SLOT_NO_CD,
        .gpio_wp = GPIO_NUM_NC,
        .gpio_int = GPIO_NUM_NC,
    };

    esp_vfs_fat_mount_config_t mount_config = {
        /* 空白卡/未格式化时是否自动格式化，见 TFCARD_FORMAT_IF_MOUNT_FAILED */
        .format_if_mount_failed = TFCARD_FORMAT_IF_MOUNT_FAILED,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    /* 与 xiaozhi 相同：不覆盖 max_freq_khz，使用 SDSPI_HOST_DEFAULT 默认值 */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = TFCARD_SPI_HOST;

    ret = esp_vfs_fat_sdspi_mount(TFCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        s_card = NULL;
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount FAT filesystem. Card must be formatted as FAT16/FAT32 "
                     "(use a PC to format it, or set TFCARD_FORMAT_IF_MOUNT_FAILED to 1)");
        } else {
            ESP_LOGE(TAG, "Card init failed (%s). Check wiring, 3.3V supply and pull-ups on CMD/DAT lines",
                     esp_err_to_name(ret));
        }
        spi_bus_free(TFCARD_SPI_HOST);
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "TF card mounted at %s", TFCARD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t tf_card_deinit(void)
{
    if (!s_mounted || s_card == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(TFCARD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;

    esp_err_t bus_ret = spi_bus_free(TFCARD_SPI_HOST);
    if (ret == ESP_OK) {
        ret = bus_ret;
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TF card unmounted");
    } else {
        ESP_LOGE(TAG, "Unmount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool tf_card_is_mounted(void)
{
    return s_mounted;
}

sdmmc_card_t *tf_card_handle(void)
{
    return s_card;
}

void tf_card_print_info(void)
{
    if (!s_mounted || s_card == NULL) {
        ESP_LOGW(TAG, "TF card not mounted");
        return;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t card_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

    ESP_LOGI(TAG, "---- TF card info ----");
    ESP_LOGI(TAG, "name        : %s", s_card->cid.name);
    ESP_LOGI(TAG, "type        : %s", s_card->is_mmc ? "MMC" : (s_card->is_sdio ? "SDIO" : "SD"));
    ESP_LOGI(TAG, "capacity    : %llu MB", (unsigned long long)(card_bytes / (1024ULL * 1024ULL)));
    ESP_LOGI(TAG, "max freq    : %lu kHz (working %d kHz)",
             (unsigned long)s_card->max_freq_khz, s_card->real_freq_khz);
    ESP_LOGI(TAG, "bus width   : %d line(s)", 1 << s_card->log_bus_width);

    if (esp_vfs_fat_info(TFCARD_MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "filesystem  : %llu KB total, %llu KB free",
                 (unsigned long long)(total_bytes / 1024ULL),
                 (unsigned long long)(free_bytes / 1024ULL));
    }
    ESP_LOGI(TAG, "----------------------");
}

/* ---------------- 通用文件读写 ---------------- */

esp_err_t tf_card_write_file(const char *path, const void *data, size_t len)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    size_t written = (len > 0) ? fwrite(data, 1, len, f) : 0;
    int flush_ret = fflush(f);
    int close_ret = fclose(f);

    if (written != len || flush_ret != 0 || close_ret != 0) {
        ESP_LOGE(TAG, "Write %s failed (%u/%u bytes)", path, (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wrote %u bytes to %s", (unsigned)len, path);
    return ESP_OK;
}

esp_err_t tf_card_read_file(const char *path, void *buf, size_t len, size_t *out_len)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len != NULL) {
        *out_len = 0;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for reading", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t rd = fread(buf, 1, len, f);
    int err = ferror(f);
    fclose(f);

    if (err) {
        ESP_LOGE(TAG, "Read %s failed", path);
        return ESP_FAIL;
    }

    if (out_len != NULL) {
        *out_len = rd;
    }
    return ESP_OK;
}

esp_err_t tf_card_list_dir(const char *path)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Directory listing of %s:", path);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char full[300];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            ESP_LOGI(TAG, "  %-24s %8llu bytes%s", ent->d_name,
                     (unsigned long long)st.st_size,
                     S_ISDIR(st.st_mode) ? "  <DIR>" : "");
        } else {
            ESP_LOGI(TAG, "  %-24s (stat failed)", ent->d_name);
        }
    }
    closedir(dir);

    return ESP_OK;
}

/* ---------------- 读写准确性自检 ---------------- */

static esp_err_t selftest_write(const char *path, size_t total, uint32_t *out_crc)
{
    uint8_t *buf = malloc(SELFTEST_CHUNK_SIZE);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        free(buf);
        return ESP_FAIL;
    }

    uint32_t crc = 0;
    size_t written = 0;
    int64_t t_start = esp_timer_get_time();

    for (size_t off = 0; off < total; off += SELFTEST_CHUNK_SIZE) {
        size_t n = (total - off < SELFTEST_CHUNK_SIZE) ? (total - off) : SELFTEST_CHUNK_SIZE;
        for (size_t i = 0; i < n; i++) {
            buf[i] = selftest_pattern((uint32_t)(off + i));
        }

        if (fwrite(buf, 1, n, f) != n) {
            ESP_LOGE(TAG, "fwrite failed at offset %u", (unsigned)off);
            fclose(f);
            free(buf);
            return ESP_FAIL;
        }
        crc = esp_rom_crc32_le(crc, buf, n);
        written += n;
    }

    int flush_ret = fflush(f);
    int close_ret = fclose(f);
    free(buf);

    if (flush_ret != 0 || close_ret != 0 || written != total) {
        ESP_LOGE(TAG, "Write test data failed (%u/%u bytes)", (unsigned)written, (unsigned)total);
        return ESP_FAIL;
    }

    int64_t elapsed_us = esp_timer_get_time() - t_start;
    if (elapsed_us <= 0) {
        elapsed_us = 1;
    }
    ESP_LOGI(TAG, "write: %u bytes in %lld ms (%lld KB/s)", (unsigned)total,
             (long long)(elapsed_us / 1000),
             (long long)((uint64_t)total * 1000ULL / (uint64_t)elapsed_us));

    *out_crc = crc;
    return ESP_OK;
}

static esp_err_t selftest_verify(const char *path, size_t total, uint32_t expect_crc)
{
    uint8_t *buf = malloc(SELFTEST_CHUNK_SIZE);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for reading", path);
        free(buf);
        return ESP_FAIL;
    }

    /* 先校验文件长度 */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size != (long)total) {
        ESP_LOGE(TAG, "size mismatch: expect %u, got %ld", (unsigned)total, file_size);
        fclose(f);
        free(buf);
        return ESP_FAIL;
    }

    uint32_t crc = 0;
    size_t off = 0;
    int mismatch = 0;
    int64_t t_start = esp_timer_get_time();

    while (off < total) {
        size_t n = (total - off < SELFTEST_CHUNK_SIZE) ? (total - off) : SELFTEST_CHUNK_SIZE;
        size_t rd = fread(buf, 1, n, f);
        if (rd != n) {
            ESP_LOGE(TAG, "fread short at offset %u (%u/%u)", (unsigned)off, (unsigned)rd, (unsigned)n);
            fclose(f);
            free(buf);
            return ESP_FAIL;
        }

        for (size_t i = 0; i < n; i++) {
            uint8_t expect = selftest_pattern((uint32_t)(off + i));
            if (buf[i] != expect) {
                if (mismatch < 5) {
                    ESP_LOGE(TAG, "data mismatch at offset %u: expect 0x%02X, got 0x%02X",
                             (unsigned)(off + i), expect, buf[i]);
                }
                mismatch++;
            }
        }

        crc = esp_rom_crc32_le(crc, buf, n);
        off += n;
    }

    fclose(f);
    free(buf);

    int64_t elapsed_us = esp_timer_get_time() - t_start;
    if (elapsed_us <= 0) {
        elapsed_us = 1;
    }
    ESP_LOGI(TAG, "read : %u bytes in %lld ms (%lld KB/s)", (unsigned)total,
             (long long)(elapsed_us / 1000),
             (long long)((uint64_t)total * 1000ULL / (uint64_t)elapsed_us));

    if (mismatch != 0) {
        ESP_LOGE(TAG, "byte compare FAILED, %d byte(s) differ", mismatch);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "byte compare OK");

    if (crc != expect_crc) {
        ESP_LOGE(TAG, "CRC32 mismatch: expect 0x%08lX, got 0x%08lX",
                 (unsigned long)expect_crc, (unsigned long)crc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CRC32 OK (0x%08lX)", (unsigned long)crc);

    return ESP_OK;
}

/* 指定偏移读取校验：覆盖 FATFS 簇/扇区寻址能力 */
static esp_err_t selftest_read_at(const char *path, size_t offset, size_t len)
{
    uint8_t *buf = malloc(len);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        free(buf);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek to %u failed", (unsigned)offset);
        ret = ESP_FAIL;
    } else if (fread(buf, 1, len, f) != len) {
        ESP_LOGE(TAG, "read at offset %u failed", (unsigned)offset);
        ret = ESP_FAIL;
    } else {
        for (size_t i = 0; i < len; i++) {
            if (buf[i] != selftest_pattern((uint32_t)(offset + i))) {
                ESP_LOGE(TAG, "random access mismatch at offset %u", (unsigned)(offset + i));
                ret = ESP_FAIL;
                break;
            }
        }
    }

    fclose(f);
    free(buf);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "random access read OK (offset %u, %u bytes)", (unsigned)offset, (unsigned)len);
    }
    return ret;
}

esp_err_t tf_card_selftest(void)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "TF card not mounted, skip self test");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "===== TF card read/write self test =====");

    uint32_t write_crc = 0;
    esp_err_t ret = selftest_write(SELFTEST_FILE, SELFTEST_TOTAL_SIZE, &write_crc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "self test FAILED at write stage");
        return ret;
    }

    ret = selftest_verify(SELFTEST_FILE, SELFTEST_TOTAL_SIZE, write_crc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "self test FAILED at verify stage");
        return ret;
    }

    /* 追加写入 + 重新读取校验 */
    uint8_t tail[SELFTEST_TAIL_SIZE];
    for (size_t i = 0; i < sizeof(tail); i++) {
        tail[i] = selftest_pattern((uint32_t)(SELFTEST_TOTAL_SIZE + i));
    }

    FILE *f = fopen(SELFTEST_FILE, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for appending", SELFTEST_FILE);
        return ESP_FAIL;
    }
    size_t appended = fwrite(tail, 1, sizeof(tail), f);
    int append_close = fclose(f);
    if (appended != sizeof(tail) || append_close != 0) {
        ESP_LOGE(TAG, "append test FAILED");
        return ESP_FAIL;
    }

    /* 追加的数据位于文件末尾，用偏移读核对 */
    ret = selftest_read_at(SELFTEST_FILE, SELFTEST_TOTAL_SIZE, sizeof(tail));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "append test FAILED");
        return ret;
    }
    ESP_LOGI(TAG, "append OK (file size = %u bytes)",
             (unsigned)(SELFTEST_TOTAL_SIZE + SELFTEST_TAIL_SIZE));

    /* 文件中部随机位置读取 */
    ret = selftest_read_at(SELFTEST_FILE, 12345, SELFTEST_TAIL_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "random access test FAILED");
        return ret;
    }

    /* 打印测试文件大小并列出卡内文件 */
    struct stat st;
    if (stat(SELFTEST_FILE, &st) == 0) {
        ESP_LOGI(TAG, "test file: %s, %llu bytes", SELFTEST_FILE, (unsigned long long)st.st_size);
    }

    tf_card_list_dir(TFCARD_MOUNT_POINT);

    ESP_LOGI(TAG, "===== TF card self test PASSED =====");
    return ESP_OK;
}

#else /* !APP_ENABLE_TF_CARD：功能暂时屏蔽，仅提供桩实现 */

static const char *TAG = "TFCARD";

esp_err_t tf_card_init(void)
{
    ESP_LOGW(TAG, "TF card feature disabled (APP_ENABLE_TF_CARD=0), see feature_config.h");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tf_card_deinit(void)
{
    return ESP_OK;
}

bool tf_card_is_mounted(void)
{
    return false;
}

sdmmc_card_t *tf_card_handle(void)
{
    return NULL;
}

void tf_card_print_info(void)
{
    ESP_LOGW(TAG, "TF card feature disabled");
}

esp_err_t tf_card_selftest(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tf_card_write_file(const char *path, const void *data, size_t len)
{
    (void)path; (void)data; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tf_card_read_file(const char *path, void *buf, size_t len, size_t *out_len)
{
    (void)path; (void)buf; (void)len;
    if (out_len != NULL) {
        *out_len = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tf_card_list_dir(const char *path)
{
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* APP_ENABLE_TF_CARD */
