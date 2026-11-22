#include "emoji_cache.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define EC_MAGIC 0x314F4D45u /* 'EMO1' 小端 */
#define EC_HDR_SIZE 48
#define EC_ID_OFF 4
#define EC_LEN_OFF 36
#define EC_CRC_OFF 40

static const char *TAG = "emoji_cache";

static const esp_partition_t *s_part = NULL;
static SemaphoreHandle_t s_mutex = NULL;

bool EmojiCache_Init(void) {
    if (s_part != NULL) {
        return true;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      "mvp_emoji");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "mvp_emoji 分区不存在，头像缓存禁用");
        return false;
    }
    if (s_part->size < EC_HDR_SIZE + 4096) {
        ESP_LOGW(TAG, "mvp_emoji 分区过小（%uB），头像缓存禁用", (unsigned)s_part->size);
        s_part = NULL;
        return false;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "就绪：单槽 %uB", (unsigned)s_part->size);
    return true;
}

bool EmojiCache_Load(const char *emoji_id, uint8_t *out, size_t cap, size_t *out_len) {
    if (s_part == NULL || emoji_id == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    bool ok = false;
    uint8_t hdr[EC_HDR_SIZE];
    if (esp_partition_read(s_part, 0, hdr, EC_HDR_SIZE) == ESP_OK) {
        uint32_t magic, len, crc;
        memcpy(&magic, hdr, 4);
        memcpy(&len, hdr + EC_LEN_OFF, 4);
        memcpy(&crc, hdr + EC_CRC_OFF, 4);
        hdr[EC_ID_OFF + EMOJI_CACHE_ID_MAX - 1] = '\0'; /* 兜底，保证 id 可作 C 串比较 */
        if (magic == EC_MAGIC && strcmp((const char *)hdr + EC_ID_OFF, emoji_id) == 0 &&
            len > 0 && len <= EMOJI_CACHE_DATA_MAX && len <= cap &&
            len + EC_HDR_SIZE <= s_part->size) {
            if (esp_partition_read(s_part, EC_HDR_SIZE, out, len) == ESP_OK &&
                esp_rom_crc32_le(0, out, len) == crc) {
                *out_len = len;
                ok = true;
            } else {
                ESP_LOGW(TAG, "缓存校验失败 id=%s，将重下载", emoji_id);
            }
        }
    }
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

bool EmojiCache_Save(const char *emoji_id, const uint8_t *data, size_t len) {
    if (s_part == NULL || emoji_id == NULL || data == NULL) {
        return false;
    }
    size_t id_len = strlen(emoji_id);
    if (id_len == 0 || id_len >= EMOJI_CACHE_ID_MAX || len == 0 ||
        len > EMOJI_CACHE_DATA_MAX || len + EC_HDR_SIZE > s_part->size) {
        return false;
    }
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    bool ok = false;
    if (esp_partition_erase_range(s_part, 0, s_part->size) == ESP_OK) {
        uint8_t hdr[EC_HDR_SIZE];
        memset(hdr, 0, sizeof(hdr));
        uint32_t magic = EC_MAGIC;
        uint32_t len32 = (uint32_t)len;
        uint32_t crc = esp_rom_crc32_le(0, data, (uint32_t)len);
        memcpy(hdr, &magic, 4);
        memcpy(hdr + EC_ID_OFF, emoji_id, id_len);
        memcpy(hdr + EC_LEN_OFF, &len32, 4);
        memcpy(hdr + EC_CRC_OFF, &crc, 4);
        if (esp_partition_write(s_part, 0, hdr, EC_HDR_SIZE) == ESP_OK &&
            esp_partition_write(s_part, EC_HDR_SIZE, data, len) == ESP_OK) {
            ok = true;
        } else {
            ESP_LOGE(TAG, "缓存写失败 id=%s", emoji_id);
        }
    } else {
        ESP_LOGE(TAG, "缓存擦除失败 id=%s", emoji_id);
    }
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
    return ok;
}
