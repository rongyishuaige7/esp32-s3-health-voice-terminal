#include "wake_sr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "sdkconfig.h"

static const char *TAG = "wake_sr";

static const esp_wn_iface_t *s_iface;
static model_iface_data_t *s_model;
static int s_chunk = 0;

#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32)
#if CONFIG_MODEL_IN_FLASH
static uint32_t read_le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static bool name_has_index_subtag(const char *name32)
{
    /* 固定 32 字节槽，如 "wn9_index\0..." */
    for (int i = 0; i <= SRMODEL_STRING_LENGTH - 6; i++) {
        if (memcmp(name32 + i, "_index", 6) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t read_model_part(const esp_partition_t *p, size_t off, void *dst, size_t len, const char *what)
{
    if (off + len > p->size) {
        ESP_LOGE(TAG, "读取 %s 越界：off=%u len=%u size=%lu", what,
                 (unsigned)off, (unsigned)len, (unsigned long)p->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_partition_read(p, off, dst, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 %s 失败: %s", what, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t read_model_part_u32_le(const esp_partition_t *p, size_t off, uint32_t *out, const char *what)
{
    uint8_t raw[4];
    esp_err_t err = read_model_part(p, off, raw, sizeof(raw), what);
    if (err != ESP_OK) {
        return err;
    }
    *out = read_le32(raw);
    return ESP_OK;
}

static esp_err_t read_model_name_slot(const esp_partition_t *p, size_t off,
                                      char out[SRMODEL_STRING_LENGTH + 1], const char *what)
{
    memset(out, 0, SRMODEL_STRING_LENGTH + 1);
    return read_model_part(p, off, out, SRMODEL_STRING_LENGTH, what);
}

static bool srmodel_name_equals(const char *slot_name, const char *wanted)
{
    size_t slot_len = strnlen(slot_name, SRMODEL_STRING_LENGTH);
    size_t wanted_len = strlen(wanted);
    return slot_len == wanted_len && memcmp(slot_name, wanted, wanted_len) == 0;
}

static esp_err_t model_flash_ready(void)
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!p) {
        ESP_LOGE(TAG, "无 model 分区：请在 partitions.csv 添加 model 并重新烧录分区表");
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t hdr[64];
    esp_err_t err = read_model_part(p, 0, hdr, sizeof(hdr), "model 分区头");
    if (err != ESP_OK) {
        return err;
    }
    size_t ff = 0;
    for (size_t i = 0; i < 64; i++) {
        if (hdr[i] == 0xff) {
            ff++;
        }
    }
    if (ff == 64) {
        ESP_LOGE(TAG, "model 分区为空白(全0xFF)：请 idf.py flash 写入 0x%lx 的 srmodels.bin",
                 (unsigned long)p->address);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t nmodels = read_le32(hdr);
    if (nmodels < 1 || nmodels > 8) {
        ESP_LOGE(TAG, "srmodels 头无效(model 数=%lu)，分区非 srmodels.bin 或损坏，请 erase-flash 后重烧",
                 (unsigned long)nmodels);
        return ESP_ERR_NOT_FOUND;
    }
    size_t off = 4;
    bool found_model = false;
    bool has_index = false;
    for (uint32_t i = 0; i < nmodels; i++) {
        char model_name[SRMODEL_STRING_LENGTH + 1];
        uint32_t nfiles = 0;

        err = read_model_name_slot(p, off, model_name, "模型名");
        if (err != ESP_OK) {
            return err;
        }
        err = read_model_part_u32_le(p, off + SRMODEL_STRING_LENGTH, &nfiles, "模型文件数");
        if (err != ESP_OK) {
            return err;
        }
        if (nfiles < 2 || nfiles > 32) {
            ESP_LOGE(TAG, "模型 %s 的文件数异常(%lu)，请重新生成并烧录 srmodels.bin",
                     model_name, (unsigned long)nfiles);
            return ESP_ERR_NOT_FOUND;
        }

        bool is_target_model = srmodel_name_equals(model_name, CONFIG_WAKENET_MODEL_NAME);
        if (is_target_model) {
            found_model = true;
        }

        off += SRMODEL_STRING_LENGTH + 4;
        for (uint32_t j = 0; j < nfiles; j++) {
            char file_name[SRMODEL_STRING_LENGTH + 1];
            err = read_model_name_slot(p, off, file_name, "模型文件名");
            if (err != ESP_OK) {
                return err;
            }
            if (is_target_model && name_has_index_subtag(file_name)) {
                has_index = true;
            }
            off += SRMODEL_STRING_LENGTH + 4 + 4;
        }
    }

    if (!found_model) {
        ESP_LOGE(TAG, "srmodels 中未找到 CONFIG_WAKENET_MODEL_NAME(%s)，请检查 menuconfig 与 srmodels 是否一致",
                 CONFIG_WAKENET_MODEL_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    if (!has_index) {
        ESP_LOGE(TAG, "目标模型 %s 缺少 *_index（如 wn9_index），与 /srmodel/.../wn9_index 报错一致：重烧 build/srmodels/srmodels.bin",
                 CONFIG_WAKENET_MODEL_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
#endif
#endif

esp_err_t wake_sr_init(void)
{
#if CONFIG_MODEL_IN_FLASH && (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32)
    esp_err_t merr = model_flash_ready();
    if (merr != ESP_OK) {
        return merr;
    }
#endif

    /* 必须先 mmap 加载 model 分区中的 srmodels.bin，否则 WakeNet 会错误地走 /srmodel/... 文件路径 */
#if CONFIG_MODEL_IN_SDCARD
    /* SD 模式：路径需与挂载点一致，见 esp-sr 文档 */
    srmodel_list_t *models = esp_srmodel_init("/sdcard/srmodel");
#else
    srmodel_list_t *models = esp_srmodel_init("model");
#endif
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init 失败：分区未挂载 srmodels 或标签非 \"model\"");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "srmodels ready, num=%d", models->num);

    const char *name = CONFIG_WAKENET_MODEL_NAME;
    s_iface = esp_wn_handle_from_name(name);
    if (s_iface == NULL) {
        ESP_LOGE(TAG, "unknown wakenet model: %s", name);
        esp_srmodel_deinit(models);
        return ESP_ERR_NOT_FOUND;
    }
    esp_task_wdt_reset();
    s_model = s_iface->create(name, DET_MODE_90);
    esp_task_wdt_reset();
    if (s_model == NULL) {
        ESP_LOGE(TAG, "WakeNet create 失败: %s", name);
        esp_srmodel_deinit(models);
        return ESP_ERR_NO_MEM;
    }
    s_chunk = s_iface->get_samp_chunksize(s_model);
    int rate = s_iface->get_samp_rate(s_model);
    ESP_LOGI(TAG, "WakeNet model=%s chunk=%d rate=%d", name, s_chunk, rate);
    return ESP_OK;
}

void wake_sr_deinit(void)
{
    if (s_iface && s_model) {
        s_iface->destroy(s_model);
    }
    s_model = NULL;
    s_iface = NULL;
    s_chunk = 0;

    srmodel_list_t *models = get_static_srmodels();
    if (models != NULL) {
        esp_srmodel_deinit(models);
    }
}

int wake_sr_feed_chunk_samples(void)
{
    return s_chunk > 0 ? s_chunk : 512;
}

bool wake_sr_process(int16_t *samples)
{
    if (!s_iface || !s_model || !samples) {
        return false;
    }
    wakenet_state_t st = s_iface->detect(s_model, samples);
    return (st == WAKENET_DETECTED);
}
