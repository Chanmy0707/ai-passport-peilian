/* MVP 工牌模式运行时配置（M1）。
 *
 * 配置项读取顺序：NVS 命名空间 "mvp" 同名键 > Kconfig 编译期默认值。
 * NVS 键（持久化 API，改动需迁移）：record_ws_url / badge_api_url / device_token / device_id
 *   / practice_ws_url / practice_api_url
 *   / badge_json（工牌配置缓存，record_client 写入）。
 */
#pragma once

#include <string>

struct MvpConfig {
    std::string record_ws_url;  // wss://…/record/v1/
    std::string badge_api_url;  // https://…/api/v1
    std::string device_token;
    std::string device_id;      // 保证非空：NVS 为空时取 Wi-Fi STA MAC
    std::string practice_ws_url;  // wss://…/xiaozhi/v1/（智能陪练对话）
    std::string practice_api_url; // https://…（智能陪练场景拉取基地址）
};

/* 进程内一次性加载，之后返回同一实例。 */
const MvpConfig& GetMvpConfig();
