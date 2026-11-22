/* 模式状态机（MVP设计文档 §3.1 / §3.6）。
 *
 * 四个顶层模式：智能工牌 / 智能陪练 / 日常对话 / 设置。
 * 本模块只维护“当前模式”变量（纯状态），切换后的编排（UI、音频链路互斥、
 * WS 连接）全部由 MvpApp 负责，见 mvp_app.{h,cc}。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODE_BADGE = 0, /* 智能工牌 */
    MODE_PRACTICE,  /* 智能陪练 */
    MODE_CHAT,      /* 日常对话 */
    MODE_SETTINGS,  /* 设置 */
} MvpMode;

/* 初始化为默认模式（MODE_BADGE）。幂等可重入。 */
void ModeManager_Init(void);

MvpMode ModeManager_GetMode(void);

/* 切换模式。返回 0 成功；目标模式非法返回 -1（模式不变）。 */
int ModeManager_Switch(MvpMode mode);

#ifdef __cplusplus
}
#endif
