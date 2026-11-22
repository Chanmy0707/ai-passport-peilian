/* 头像动态 emoji 本地缓存（设计文档 §3.2 / §5.2：素材云端按需下载，不预置打包）。
 *
 * 分区 "mvp_emoji"（data，64KB 单槽）：
 *   头 48B：magic u32 'EMO1' + emoji_id[32]（NUL 结尾）+ len u32 + crc32 u32 + 保留
 *   载荷：GIF 原始字节（64px 调色板 GIF，20-55KB）
 * 单槽设计：换头像即整体覆盖，magic/id/crc 校验任一不符视为未缓存（重下载）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMOJI_CACHE_ID_MAX 32   /* emoji_id 存储上限（含 NUL） */
#define EMOJI_CACHE_DATA_MAX 60000  /* 载荷上限（分区 64KB - 头 - 裕量） */

/* 挂载分区。分区缺失/初始化失败返回 false（缓存禁用，仅在线下载）。 */
bool EmojiCache_Init(void);

/* 读取缓存的 emoji 载荷到 out（cap 字节）。命中且校验通过返回 true。 */
bool EmojiCache_Load(const char *emoji_id, uint8_t *out, size_t cap, size_t *out_len);

/* 覆盖写入缓存（先擦后写）。成功返回 true。 */
bool EmojiCache_Save(const char *emoji_id, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
