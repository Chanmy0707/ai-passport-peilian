/* Flash 环形帧缓冲（M2，设计文档 §6.2 / §8）。
 *
 * 分区 "mvp_ring"（data，1MB）按 4KB sector 环形存放录音帧：
 *   sector 头 16B：magic u32 + first_seq u32 + 保留 8B（0xFF）
 *   记录（4B 对齐顺序写）：[seq u32][ts u32][len u16][rsv u16][payload ≤240B][pad]
 * 空槽判定：seq == 0xFFFFFFFF（擦除态）。写满自动覆盖最旧 sector（懒擦除）。
 * ack 不落盘：服务端 acked_seq 为真相，hello resume 幂等去重。
 * 写入要求：seq 单调递增（记录连续，读取按 seq 定位）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLASH_RING_FRAME_MAX 2048  /* Opus 60ms 帧上限（AUTO 码率 <=256kbps -> <=1920B，留裕量） */

/* 挂载分区并扫描索引。分区缺失/初始化失败返回 false（降级为仅在线直传）。 */
bool FlashRing_Init(void);

/* 逻辑清空（懒擦除：sector 在下次写入复用时才真正擦除）。 */
void FlashRing_Clear(void);

/* 写入一帧。成功返回 true；帧超长/写失败返回 false。 */
bool FlashRing_Write(uint32_t seq, uint32_t ts, const uint8_t *payload, uint16_t len);

/* 读取 seq 帧载荷到 out（cap 字节）。成功返回 true；seq 不在环内返回 false。 */
bool FlashRing_Read(uint32_t seq, uint8_t *out, uint16_t cap, uint32_t *ts, uint16_t *len);

/* 环内最旧 / 最新可用 seq；空环返回 UINT32_MAX。 */
uint32_t FlashRing_FirstSeq(void);
uint32_t FlashRing_LastSeq(void);

#ifdef __cplusplus
}
#endif
