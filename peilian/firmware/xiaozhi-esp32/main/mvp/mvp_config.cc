#include "mvp_config.h"

#include <cstdio>

#include "esp_mac.h"
#include "sdkconfig.h"
#include "settings.h"

static MvpConfig g_mvp_config;
static bool g_mvp_config_loaded = false;

static std::string ResolveDeviceId() {
    Settings settings("mvp", false);
    std::string device_id = settings.GetString("device_id", "");
    if (!device_id.empty()) {
        return device_id;
    }
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return buf;
    }
    return "unknown";
}

const MvpConfig& GetMvpConfig() {
    if (!g_mvp_config_loaded) {
        Settings settings("mvp", false);
        g_mvp_config.record_ws_url =
            settings.GetString("record_ws_url", CONFIG_MVP_RECORD_WS_URL);
        g_mvp_config.badge_api_url =
            settings.GetString("badge_api_url", CONFIG_MVP_BADGE_API_URL);
        g_mvp_config.device_token =
            settings.GetString("device_token", CONFIG_MVP_DEVICE_TOKEN);
        g_mvp_config.practice_ws_url =
            settings.GetString("practice_ws_url", CONFIG_MVP_PRACTICE_WS_URL);
        g_mvp_config.practice_api_url =
            settings.GetString("practice_api_url", CONFIG_MVP_PRACTICE_API_URL);
        g_mvp_config.device_id = ResolveDeviceId();
        g_mvp_config_loaded = true;
    }
    return g_mvp_config;
}
