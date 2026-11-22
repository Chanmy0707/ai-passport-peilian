#include "flash_ring.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FR_SECTOR_SIZE 4096
#define FR_HDR_SIZE 16
#define FR_REC_HDR 12
#define FR_MAGIC 0x4D565052u /* 'MVPR' */
#define FR_FREE_SEQ 0xFFFFFFFFu

static const char *TAG = "flash_ring";

typedef struct {
    uint32_t first_seq;  /* FR_FREE_SEQ = 空闲 sector */
    uint16_t used_bytes; /* 下一记录写入偏移 */
    uint16_t rec_count;
} fr_sector_t;

static const esp_partition_t *s_part = NULL;
static fr_sector_t *s_sec = NULL;
static uint32_t s_count = 0;
static int32_t s_cur = -1; /* 当前写入 sector 下标 */
static SemaphoreHandle_t s_mutex = NULL;

static uint16_t rec_size(uint16_t len) {
    return (uint16_t)(FR_REC_HDR + ((len + 3u) & ~3u));
}

static void scan_sector(uint32_t i) {
    uint8_t hdr[FR_HDR_SIZE];
    esp_partition_read(s_part, i * FR_SECTOR_SIZE, hdr, FR_HDR_SIZE);
    uint32_t magic, first;
    memcpy(&magic, hdr, 4);
    memcpy(&first, hdr + 4, 4);
    if (magic != FR_MAGIC) {
        s_sec[i].first_seq = FR_FREE_SEQ;
        s_sec[i].used_bytes = 0;
        s_sec[i].rec_count = 0;
        return;
    }
    s_sec[i].first_seq = first;
    uint16_t off = FR_HDR_SIZE;
    uint16_t count = 0;
    while (off + FR_REC_HDR <= FR_SECTOR_SIZE) {
        uint8_t rh[FR_REC_HDR];
        esp_partition_read(s_part, i * FR_SECTOR_SIZE + off, rh, FR_REC_HDR);
        uint32_t seq;
        uint16_t len;
        memcpy(&seq, rh, 4);
        memcpy(&len, rh + 8, 2);
        if (seq == FR_FREE_SEQ || len == 0 || len > FLASH_RING_FRAME_MAX) {
            break;
        }
        off = (uint16_t)(off + rec_size(len));
        count++;
    }
    s_sec[i].used_bytes = off;
    s_sec[i].rec_count = count;
}

bool FlashRing_Init(void) {
    if (s_part != NULL) {
        return true;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      "mvp_ring");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "mvp_ring 分区不存在，离线补传禁用");
        return false;
    }
    s_count = s_part->size / FR_SECTOR_SIZE;
    s_sec = (fr_sector_t *)calloc(s_count, sizeof(fr_sector_t));
    if (s_sec == NULL) {
        s_part = NULL;
        return false;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    for (uint32_t i = 0; i < s_count; i++) {
        scan_sector(i);
    }
    s_cur = -1;
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_sec[i].first_seq == FR_FREE_SEQ) {
            continue;
        }
        if (s_cur < 0 || s_sec[i].first_seq > s_sec[s_cur].first_seq) {
            s_cur = (int32_t)i;
        }
    }
    ESP_LOGI(TAG, "就绪：%u 个 sector，first=%u last=%u", (unsigned)s_count,
             (unsigned)FlashRing_FirstSeq(), (unsigned)FlashRing_LastSeq());
    return true;
}

void FlashRing_Clear(void) {
    if (s_sec == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint32_t i = 0; i < s_count; i++) {
        s_sec[i].first_seq = FR_FREE_SEQ;
        s_sec[i].used_bytes = 0;
        s_sec[i].rec_count = 0;
    }
    s_cur = -1;
    xSemaphoreGive(s_mutex);
}

bool FlashRing_Write(uint32_t seq, uint32_t ts, const uint8_t *payload, uint16_t len) {
    if (s_part == NULL || payload == NULL || len == 0 || len > FLASH_RING_FRAME_MAX) {
        return false;
    }
    uint16_t size = rec_size(len);
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int32_t target = s_cur;
    if (target >= 0 && (uint16_t)(s_sec[target].used_bytes + size) > FR_SECTOR_SIZE) {
        target = -1; /* 当前 sector 放不下，换 sector */
    }
    bool fresh_sector = false;
    if (target < 0) {
        for (uint32_t i = 0; i < s_count; i++) {
            if (s_sec[i].first_seq == FR_FREE_SEQ) {
                target = (int32_t)i;
                break;
            }
        }
        if (target < 0) {
            /* 环满：覆盖 first_seq 最小（最旧）的 sector */
            target = 0;
            for (uint32_t i = 1; i < s_count; i++) {
                if (s_sec[i].first_seq < s_sec[target].first_seq) {
                    target = (int32_t)i;
                }
            }
        }
        if (esp_partition_erase_range(s_part, (size_t)target * FR_SECTOR_SIZE,
                                      FR_SECTOR_SIZE) != ESP_OK) {
            xSemaphoreGive(s_mutex);
            return false;
        }
        s_sec[target].first_seq = seq;
        s_sec[target].used_bytes = FR_HDR_SIZE;
        s_sec[target].rec_count = 0;
        fresh_sector = true;
    }

    static uint8_t buf[FR_HDR_SIZE + FR_REC_HDR + FLASH_RING_FRAME_MAX + 4]; /* 大缓冲放 BSS（持 s_mutex） */
    size_t off = 0;
    if (fresh_sector) {
        uint32_t magic = FR_MAGIC;
        memcpy(buf, &magic, 4);
        memcpy(buf + 4, &seq, 4);
        memset(buf + 8, 0xFF, 8);
        off = FR_HDR_SIZE;
    }
    memcpy(buf + off, &seq, 4);
    memcpy(buf + off + 4, &ts, 4);
    memcpy(buf + off + 8, &len, 2);
    uint16_t rsv = 0xFFFF;
    memcpy(buf + off + 10, &rsv, 2);
    memcpy(buf + off + FR_REC_HDR, payload, len);
    size_t pad = size - FR_REC_HDR - len;
    if (pad > 0) {
        memset(buf + off + FR_REC_HDR + len, 0xFF, pad);
    }
    esp_err_t err = esp_partition_write(s_part, (size_t)target * FR_SECTOR_SIZE +
                                                   s_sec[target].used_bytes,
                                        buf, off + size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写帧失败 seq=%u err=%d", (unsigned)seq, err);
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_sec[target].used_bytes = (uint16_t)(s_sec[target].used_bytes + size);
    s_sec[target].rec_count++;
    s_cur = target;
    xSemaphoreGive(s_mutex);
    return true;
}

bool FlashRing_Read(uint32_t seq, uint8_t *out, uint16_t cap, uint32_t *ts, uint16_t *len) {
    if (s_part == NULL || out == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = false;
    for (uint32_t i = 0; i < s_count && !found; i++) {
        if (s_sec[i].first_seq == FR_FREE_SEQ ||
            seq - s_sec[i].first_seq >= s_sec[i].rec_count) {
            continue;
        }
        uint16_t off = FR_HDR_SIZE;
        for (uint16_t n = 0; n < s_sec[i].rec_count; n++) {
            uint8_t rh[FR_REC_HDR];
            esp_partition_read(s_part, i * FR_SECTOR_SIZE + off, rh, FR_REC_HDR);
            uint32_t rseq, rts;
            uint16_t rlen;
            memcpy(&rseq, rh, 4);
            memcpy(&rts, rh + 4, 4);
            memcpy(&rlen, rh + 8, 2);
            if (rseq == seq) {
                if (rlen > cap) {
                    break;
                }
                esp_partition_read(s_part, i * FR_SECTOR_SIZE + off + FR_REC_HDR, out, rlen);
                if (ts != NULL) {
                    *ts = rts;
                }
                if (len != NULL) {
                    *len = rlen;
                }
                found = true;
                break;
            }
            off = (uint16_t)(off + rec_size(rlen));
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

uint32_t FlashRing_FirstSeq(void) {
    if (s_sec == NULL) {
        return FR_FREE_SEQ;
    }
    uint32_t best = FR_FREE_SEQ;
    bool found = false;
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_sec[i].first_seq == FR_FREE_SEQ) {
            continue;
        }
        if (!found || s_sec[i].first_seq < best) {
            best = s_sec[i].first_seq;
            found = true;
        }
    }
    return found ? best : FR_FREE_SEQ;
}

uint32_t FlashRing_LastSeq(void) {
    if (s_sec == NULL || s_cur < 0 || s_sec[s_cur].rec_count == 0) {
        return FR_FREE_SEQ;
    }
    return s_sec[s_cur].first_seq + s_sec[s_cur].rec_count - 1;
}
