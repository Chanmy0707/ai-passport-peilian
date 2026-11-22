#include "record_client.h"

#include <cstring>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "board.h"
#include "emoji_cache.h"
#include "flash_ring.h"
#include "http.h"
#include "mvp_config.h"
#include "settings.h"
#include "web_socket.h"

#define TAG "mvp_record"

static constexpr size_t kFrameQueueDepth = 8;
static constexpr int kReconnectIntervalSec = 5;
static constexpr int kHelloTimeoutMs = 10000;

RecordClient& RecordClient::GetInstance() {
    static RecordClient instance;
    return instance;
}

void RecordClient::Init() {
    if (task_ != nullptr) {
        return;
    }
    frame_queue_ = xQueueCreate(kFrameQueueDepth, sizeof(RecordFrame));
    events_ = xEventGroupCreate();
    FlashRing_Init();
    xTaskCreate(&RecordClient::TaskEntry, "mvp_record", 10240, this, 5, &task_);
}

void RecordClient::StartRecording() {
    if (session_active_.load()) {
        if (pending_stop_.exchange(false)) {
            ESP_LOGI(TAG, "取消收尾，继续当前会话");
        }
        return;
    }
    Init();
    next_seq_ = 0;
    acked_seq_ = -1;
    last_sent_seq_ = -1;
    hello_done_ = false;
    stopped_by_server_ = false;
    session_start_us_ = esp_timer_get_time();
    FlashRing_Clear();
    if (frame_queue_ != nullptr) {
        xQueueReset(frame_queue_);
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_.clear();
    }
    session_active_ = true;
    ESP_LOGI(TAG, "开始新会话 device=%s", GetMvpConfig().device_id.c_str());
}

void RecordClient::StopRecording() {
    if (!session_active_.load()) {
        return;
    }
    pending_stop_ = true;
    ESP_LOGI(TAG, "停录请求，等待补传收尾");
}

void RecordClient::OnAudioPacket(const AudioStreamPacket& packet) {
    if (!session_active_.load() || packet.payload.empty() ||
        packet.payload.size() > FLASH_RING_FRAME_MAX) {
        return;
    }
    /* 仅主循环单线程写 next_seq_；ring 写失败降级为仅在线直传（补传跳过缺帧），仍占号 */
    uint32_t seq = next_seq_.load();
    uint32_t ts = (uint32_t)((esp_timer_get_time() - session_start_us_) / 1000);
    if (!FlashRing_Write(seq, ts, packet.payload.data(), (uint16_t)packet.payload.size())) {
        ESP_LOGW(TAG, "ring 写失败 seq=%u，降级为仅在线直传", (unsigned)seq);
    }
    next_seq_ = seq + 1;

    if (frame_queue_ != nullptr) {
        static RecordFrame f; /* 仅主循环单线程调用，放 BSS 避免超大栈帧 */
        f.seq = seq;
        f.ts = ts;
        f.len = (uint16_t)packet.payload.size();
        memcpy(f.payload, packet.payload.data(), f.len);
        xQueueSend(frame_queue_, &f, 0); /* 满则只丢队列副本，ring 已有 */
    }
}

/* ---------------- 后台任务 ---------------- */

void RecordClient::TaskEntry(void* arg) {
    static_cast<RecordClient*>(arg)->TaskLoop();
}

void RecordClient::TaskLoop() {
    int64_t next_connect_us = 0;
    while (true) {
        if (!session_active_.load()) {
            if (ws_) {
                ws_->Close();
                ws_.reset();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (ws_ == nullptr || !ws_->IsConnected()) {
            ws_.reset();
            int64_t now = esp_timer_get_time();
            if (now >= next_connect_us) {
                if (TryConnect()) {
                    /* hello_ack 后补传 ring 中 acked+1 .. 最新已写帧 */
                    SendUpTo((int64_t)next_seq_.load() - 1);
                } else {
                    next_connect_us = now + (int64_t)kReconnectIntervalSec * 1000000;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        static RecordFrame f; /* 仅 TaskLoop 单线程消费，放 BSS 避免超大栈帧 */
        if (xQueueReceive(frame_queue_, &f, pdMS_TO_TICKS(200)) == pdTRUE) {
            if ((int64_t)f.seq > last_sent_seq_.load()) {
                /* 先从 ring 补上断档（队列副本丢失的帧） */
                SendUpTo((int64_t)f.seq - 1);
                if ((int64_t)f.seq == last_sent_seq_.load() + 1) {
                    SendBinaryFrame(f.seq, f.ts, f.payload, f.len);
                }
            }
        }

        /* 收尾前先把 ring 中队列副本丢失的帧补完（队列空但仍有未发帧时无人触发断档补传） */
        if (pending_stop_.load() && uxQueueMessagesWaiting(frame_queue_) == 0 &&
            (int64_t)next_seq_.load() - 1 > last_sent_seq_.load()) {
            SendUpTo((int64_t)next_seq_.load() - 1);
        }

        /* 收尾：无待发帧且全部已发送 → record.stop */
        if (pending_stop_.load() && uxQueueMessagesWaiting(frame_queue_) == 0 &&
            (int64_t)next_seq_.load() - 1 <= last_sent_seq_.load()) {
            if (!pending_stop_.load()) {
                continue; /* 期间被 StartRecording 取消，继续录 */
            }
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "record.stop");
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
            }
            char* json = cJSON_PrintUnformatted(root);
            std::string stop_msg(json);
            cJSON_free(json);
            cJSON_Delete(root);
            if (ws_->Send(stop_msg)) {
                ESP_LOGI(TAG, "已发 record.stop");
                int64_t deadline = esp_timer_get_time() + 3000000;
                while (!stopped_by_server_.load() && esp_timer_get_time() < deadline) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
            EndSession();
        }
    }
}

bool RecordClient::TryConnect() {
    const auto& cfg = GetMvpConfig();
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    ws_ = network->CreateWebSocket(2);
    if (ws_ == nullptr) {
        ESP_LOGE(TAG, "创建 WebSocket 失败");
        return false;
    }

    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary) {
            OnMessage(data, len);
        }
    });
    ws_->OnDisconnected([this]() { ESP_LOGW(TAG, "record WS 断开"); });
    ws_->OnError([this](const NetworkError& error) {
        ESP_LOGW(TAG, "record WS 错误: %s", error.ToString().c_str());
    });

    if (auto connected = ws_->Connect(cfg.record_ws_url.c_str()); !connected) {
        ESP_LOGW(TAG, "连接 %s 失败: %s", cfg.record_ws_url.c_str(),
                 connected.error().ToString().c_str());
        ws_.reset();
        return false;
    }

    /* 首个 hello 用 resume=0 新建会话；此后断线重连用 acked+1（acked=-1 时用 1 接管会话） */
    int64_t acked = acked_seq_.load();
    uint32_t resume = 0;
    if (hello_done_.load()) {
        resume = (acked >= 0) ? (uint32_t)(acked + 1) : 1;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", cfg.device_id.c_str());
    cJSON_AddStringToObject(root, "token", cfg.device_token.c_str());
    cJSON_AddNumberToObject(root, "resume_from_seq", (double)resume);
    char* json = cJSON_PrintUnformatted(root);
    std::string hello(json);
    cJSON_free(json);
    cJSON_Delete(root);

    xEventGroupClearBits(events_, HELLO_ACK_BIT | HELLO_ERR_BIT);
    if (!ws_->Send(hello)) {
        ESP_LOGW(TAG, "hello 发送失败");
        ws_.reset();
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(events_, HELLO_ACK_BIT | HELLO_ERR_BIT, pdTRUE,
                                           pdFALSE, pdMS_TO_TICKS(kHelloTimeoutMs));
    if (!(bits & HELLO_ACK_BIT)) {
        ESP_LOGW(TAG, "hello_ack 未收到（超时或被拒）");
        ws_.reset();
        return false;
    }
    last_sent_seq_ = acked_seq_.load();
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        ESP_LOGI(TAG, "会话就绪 session=%s acked=%lld resume=%u", session_id_.c_str(),
                 (long long)last_sent_seq_.load(), (unsigned)resume);
    }
    return true;
}

void RecordClient::SendUpTo(int64_t target_seq) {
    static uint8_t payload[FLASH_RING_FRAME_MAX]; /* 仅 TaskLoop 调用，放 BSS 避免超大栈帧 */
    while (last_sent_seq_.load() < target_seq && ws_ != nullptr && ws_->IsConnected()) {
        uint32_t seq = (uint32_t)(last_sent_seq_.load() + 1);
        uint32_t ts = 0;
        uint16_t len = 0;
        if (FlashRing_Read(seq, payload, sizeof(payload), &ts, &len)) {
            if (!SendBinaryFrame(seq, ts, payload, len)) {
                return; /* 发送失败，连接已断 */
            }
        } else {
            ESP_LOGW(TAG, "ring 缺帧 seq=%u，跳过", (unsigned)seq);
            last_sent_seq_ = (int64_t)seq;
        }
    }
}

bool RecordClient::SendBinaryFrame(uint32_t seq, uint32_t ts, const uint8_t* payload,
                                   uint16_t len) {
    if (ws_ == nullptr || !ws_->IsConnected()) {
        return false;
    }
    /* 8B 头 struct "<II" 小端（ESP32-C3 小端，直接拷贝）+ Opus 载荷 */
    static uint8_t buf[8 + FLASH_RING_FRAME_MAX]; /* 仅 TaskLoop 调用，放 BSS 避免超大栈帧 */
    memcpy(buf, &seq, 4);
    memcpy(buf + 4, &ts, 4);
    memcpy(buf + 8, payload, len);
    if (!ws_->Send(buf, 8 + (size_t)len, true)) {
        ESP_LOGW(TAG, "发送帧失败 seq=%u", (unsigned)seq);
        return false;
    }
    last_sent_seq_ = (int64_t)seq;
    return true;
}

void RecordClient::OnMessage(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        return;
    }
    cJSON* err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(err)) {
        cJSON* code = cJSON_GetObjectItem(err, "code");
        cJSON* message = cJSON_GetObjectItem(err, "message");
        ESP_LOGE(TAG, "服务端错误 %s: %s", cJSON_IsString(code) ? code->valuestring : "?",
                 cJSON_IsString(message) ? message->valuestring : "?");
        xEventGroupSetBits(events_, HELLO_ERR_BIT);
        cJSON_Delete(root);
        return;
    }
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type->valuestring, "hello_ack") == 0) {
        cJSON* sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid)) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_id_ = sid->valuestring;
        }
        cJSON* acked = cJSON_GetObjectItem(root, "acked_seq");
        if (cJSON_IsNumber(acked)) {
            acked_seq_ = (int64_t)acked->valuedouble;
        }
        hello_done_ = true;
        xEventGroupSetBits(events_, HELLO_ACK_BIT);
    } else if (strcmp(type->valuestring, "record.ack") == 0) {
        cJSON* acked = cJSON_GetObjectItem(root, "acked_seq");
        if (cJSON_IsNumber(acked)) {
            int64_t v = (int64_t)acked->valuedouble;
            if (v > acked_seq_.load()) {
                acked_seq_ = v;
            }
        }
    } else if (strcmp(type->valuestring, "record.stopped") == 0) {
        stopped_by_server_ = true;
    }
    cJSON_Delete(root);
}

void RecordClient::EndSession() {
    if (ws_) {
        ws_->Close();
        ws_.reset();
    }
    session_active_ = false;
    pending_stop_ = false;
    hello_done_ = false;
    stopped_by_server_ = false;
    acked_seq_ = -1;
    last_sent_seq_ = -1;
    if (frame_queue_ != nullptr) {
        xQueueReset(frame_queue_);
    }
    ESP_LOGI(TAG, "会话结束");
}

/* ---------------- 工牌配置（HTTP） ---------------- */

static bool ParseBadgeJson(const std::string& body, BadgeProfile* out) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    auto get = [root](const char* key) -> std::string {
        cJSON* item = cJSON_GetObjectItem(root, key);
        return cJSON_IsString(item) ? item->valuestring : "";
    };
    out->name = get("name");
    out->title = get("title");
    out->employee_no = get("employee_no");
    out->qr_text = get("qr_text");
    out->avatar_id = get("avatar_id");
    out->avatar_url = get("avatar_url");
    out->valid = true;
    cJSON_Delete(root);
    return true;
}

bool RecordClient::FetchBadgeProfile(BadgeProfile* out) {
    const auto& cfg = GetMvpConfig();
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(1);
    if (http == nullptr) {
        return false;
    }
    http->SetTimeout(10000);
    http->SetHeader("X-Device-Token", cfg.device_token);
    std::string url = cfg.badge_api_url + "/devices/" + cfg.device_id + "/badge";
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGW(TAG, "badge 拉取失败: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        return false;
    }
    if (*status == 404) {
        ESP_LOGI(TAG, "设备未配置工牌");
        http->Close();
        return false;
    }
    if (*status != 200) {
        ESP_LOGW(TAG, "badge HTTP 状态码 %d", *status);
        http->Close();
        return false;
    }
    std::string body = http->ReadAll();
    http->Close();
    if (!ParseBadgeJson(body, out)) {
        ESP_LOGW(TAG, "badge 响应解析失败");
        return false;
    }
    Settings settings("mvp", true);
    settings.SetString("badge_json", body);
    return true;
}

void RecordClient::BadgeTaskEntry(void* arg) {
    auto* self = static_cast<RecordClient*>(arg);
    BadgeProfile profile;
    if (!self->FetchBadgeProfile(&profile)) {
        /* 兜底：NVS 缓存的上次配置 */
        Settings settings("mvp", false);
        std::string cached = settings.GetString("badge_json", "");
        if (!cached.empty()) {
            ParseBadgeJson(cached, &profile);
        }
    }
    if (profile.valid) {
        std::function<void(const BadgeProfile&)> callback;
        {
            std::lock_guard<std::mutex> lock(self->badge_mutex_);
            self->badge_ = profile;
            callback = self->badge_callback_;
        }
        if (callback) {
            BadgeProfile copy = profile;
            Application::GetInstance().Schedule([callback, copy]() { callback(copy); });
        }
        self->DeliverAvatar(profile);
    }
    self->badge_fetch_running_ = false;
    vTaskDelete(nullptr);
}

void RecordClient::FetchBadgeProfileAsync() {
    if (badge_fetch_running_.exchange(true)) {
        return;
    }
    if (xTaskCreate(&RecordClient::BadgeTaskEntry, "mvp_badge", 8192, this, 4, nullptr) !=
        pdPASS) {
        badge_fetch_running_ = false;
        ESP_LOGE(TAG, "badge 任务创建失败");
    }
}

BadgeProfile RecordClient::GetBadgeProfile() const {
    std::lock_guard<std::mutex> lock(badge_mutex_);
    return badge_;
}

void RecordClient::SetBadgeProfileCallback(std::function<void(const BadgeProfile&)> callback) {
    std::lock_guard<std::mutex> lock(badge_mutex_);
    badge_callback_ = std::move(callback);
}

void RecordClient::SetAvatarCallback(
    std::function<void(const std::string&, std::vector<uint8_t>)> callback) {
    std::lock_guard<std::mutex> lock(badge_mutex_);
    avatar_callback_ = std::move(callback);
}

void RecordClient::DeliverAvatar(const BadgeProfile& profile) {
    if (profile.avatar_id.empty() || profile.avatar_url.empty()) {
        return;
    }
    std::function<void(const std::string&, std::vector<uint8_t>)> callback;
    {
        std::lock_guard<std::mutex> lock(badge_mutex_);
        callback = avatar_callback_;
    }
    if (!callback) {
        return;
    }
    EmojiCache_Init();
    std::vector<uint8_t> gif(EMOJI_CACHE_DATA_MAX);
    size_t len = 0;
    if (EmojiCache_Load(profile.avatar_id.c_str(), gif.data(), gif.size(), &len)) {
        gif.resize(len);
        ESP_LOGI(TAG, "头像缓存命中 id=%s len=%u", profile.avatar_id.c_str(), (unsigned)len);
    } else {
        if (!DownloadAvatar(profile.avatar_url, &gif)) {
            ESP_LOGW(TAG, "头像获取失败 url=%s，回落姓名首字", profile.avatar_url.c_str());
            return;
        }
        ESP_LOGI(TAG, "头像下载完成 id=%s len=%u", profile.avatar_id.c_str(),
                 (unsigned)gif.size());
        EmojiCache_Save(profile.avatar_id.c_str(), gif.data(), gif.size());
    }
    Application::GetInstance().Schedule([callback, id = profile.avatar_id,
                                         gif = std::move(gif)]() mutable {
        callback(id, std::move(gif));
    });
}

bool RecordClient::DownloadAvatar(const std::string& url, std::vector<uint8_t>* out) {
    const auto& cfg = GetMvpConfig();
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr || out == nullptr) {
        return false;
    }
    std::string full = url;
    if (full.rfind("http", 0) != 0) {
        /* 相对路径（如 /emojis/64px/<id>.gif）拼到 badge_api_url 的 scheme://host:port */
        size_t scheme = cfg.badge_api_url.find("://");
        if (scheme == std::string::npos) {
            return false;
        }
        size_t path = cfg.badge_api_url.find('/', scheme + 3);
        std::string base = (path == std::string::npos)
                               ? cfg.badge_api_url
                               : cfg.badge_api_url.substr(0, path);
        full = base + (url.empty() || url[0] != '/' ? "/" : "") + url;
    }
    auto http = network->CreateHttp(1);
    if (http == nullptr) {
        return false;
    }
    http->SetTimeout(15000);
    if (auto opened = http->Open("GET", full); !opened) {
        ESP_LOGW(TAG, "头像下载失败 %s: %s", full.c_str(),
                 opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGW(TAG, "头像 HTTP 状态码 %d", status ? *status : -1);
        http->Close();
        return false;
    }
    size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > EMOJI_CACHE_DATA_MAX) {
        ESP_LOGW(TAG, "头像长度非法 %u", (unsigned)content_length);
        http->Close();
        return false;
    }
    out->clear();
    out->reserve(content_length);
    static char chunk[1024]; /* 仅 BadgeTaskEntry 单线程调用，放 BSS 避免超大栈帧 */
    while (true) {
        auto ret = http->Read(chunk, sizeof(chunk));
        if (!ret) {
            http->Close();
            return false;
        }
        size_t n = (size_t)*ret;
        if (n == 0) {
            break;
        }
        if (out->size() + n > EMOJI_CACHE_DATA_MAX) {
            ESP_LOGW(TAG, "头像超长，拒收");
            http->Close();
            return false;
        }
        out->insert(out->end(), chunk, chunk + n);
    }
    http->Close();
    return !out->empty();
}
