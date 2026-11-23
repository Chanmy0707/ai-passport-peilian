/* mvp_avatar_default.h — 内置默认头像（😀 grinning_face，Twemoji 72px→64px PNG，CC-BY 4.0）
 * 静态常量存 flash，不占 RAM；数据来源见 server/static/emojis/manifest.json。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t kMvpDefaultAvatarPng[];
extern const size_t kMvpDefaultAvatarPngSize;

#ifdef __cplusplus
}
#endif
