/* record 协议客户端（M1/M2，设计文档 §5.2 / §8）。
 *
 * WS 契约（权威=peilian/server/app/ws/record.py）：
 *   hello {type,device_id,token,resume_from_seq}（首条文本，token 在消息体）
 *     → hello_ack {session_id,acked_seq,server_time}（初始 -1）
 *   二进制帧：8B 头 struct "<II" seq+ts 小端 + Opus 载荷
 *   record.ack {acked_seq}（服务端每 50 帧/1 秒）、record.stop {session_id} → record.stopped
 *   错误信封 {error:{code,message}}。resume_from_seq>0 接管最近 uploading 会话，=0 新建。
 *
 * 线程模型：
 *   - OnAudioPacket 由主循环调用（不可阻塞）：写穿 flash_ring + 压入发送队列（深 8）；
 *     队列满只丢队列副本（ring 已有，补传按 seq 从 ring 读）。
 *   - 独立任务负责 Connect（阻塞）/发送/补传/record.stop 收尾；每 5s 重连。
 *   - WS 回调只更新原子状态/事件位，不做 Send。
 * seq 规则：会话内从 0 单调递增（与 ring 记录连续性一致，ring 写失败降级为仅在线直传，仍占号）；
 *   hello_ack 后 last_sent = acked_seq，之后按 seq 升序发送，断档从 ring 补。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_timer.h>

#include "flash_ring.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "protocols/protocol.h"

class WebSocket;

struct BadgeProfile {
    std::string name;
    std::string title;
    std::string employee_no;
    std::string qr_text;
    std::string avatar_id;
    std::string avatar_url;  /* 相对路径（如 /emojis/64px/<id>.png），头像静态图按需下载 */
    std::string updated_at;  /* 云端最后更新时间（ISO8601），15s 轮询比对是否变更 */
    bool valid = false;
};

/* 出厂预制工牌信息（未绑定/未拉到云端配置时的默认显示）：默认 😀（grinning_face）、Jack/销售顾问/No.1024。 */
BadgeProfile PresetBadgeProfile();

/* 最近一次工牌拉取结果（设备页"同步工牌"反馈用）：kNotConfigured=404 未在后台配置。 */
enum class BadgeFetchResult { kOk, kNotConfigured, kFailed };

class RecordClient {
public:
    static RecordClient& GetInstance();

    /* 创建后台任务与 flash_ring 初始化（幂等）。 */
    void Init();

    /* 开新会话（seq=0、清 ring、resume=0）；已有会话且在收尾中则取消收尾继续录（合并，MVP 容忍）。 */
    void StartRecording();
    /* 停录：补传发完后由任务补发 record.stop 收尾；离线时挂起等待重连。 */
    void StopRecording();

    bool IsSessionActive() const { return session_active_.load(); }
    bool IsStopping() const { return pending_stop_.load(); }

    /* 主循环音频分流入口（工牌捕获中调用）：写 ring + 入队。 */
    void OnAudioPacket(const AudioStreamPacket& packet);

    /* 工牌配置：HTTP GET /devices/{id}/badge（X-Device-Token），一次性任务执行，完成后回调。 */
    void FetchBadgeProfileAsync();
    /* 每 kBadgeRetrySec 秒轮询云端工牌配置并比对 updated_at：首次绑定与云端修改均自动
     * 同步刷新（无需重启设备、无需手动下发），网络恢复时立即重拉。 */
    void StartBadgeRetry();
    BadgeProfile GetBadgeProfile() const;
    void SetBadgeProfileCallback(std::function<void(const BadgeProfile&)> callback);
    /* 工牌拉取进行中（后台任务跑 HTTP，主线程可查避免重复触发）。 */
    bool IsBadgeFetching() const { return badge_fetch_running_.load(); }
    /* 最近一次拉取结果（kOk=200 且解析成功；kNotConfigured=404；kFailed=网络/服务/解析失败）。 */
    BadgeFetchResult GetBadgeLastResult() const {
        return static_cast<BadgeFetchResult>(badge_last_result_.load());
    }

    /* 头像静态图：badge 任务内按需下载 avatar_url（先查 mvp_emoji 缓存），完成后经 Schedule 回调
     * (emoji_id, PNG 字节, 长度)；data 为 heap_caps_malloc 块，所有权移交回调方（用后 heap_caps_free）；
     * 失败不回调（UI 保持默认 😀 头像）。 */
    void SetAvatarCallback(std::function<void(const std::string&, uint8_t* data, size_t len)> callback);

private:
    RecordClient() = default;

    static void TaskEntry(void* arg);
    void TaskLoop();
    bool TryConnect();
    void SendUpTo(int64_t target_seq); /* 从 ring 补发 last_sent+1..target，缺帧跳过 */
    bool SendBinaryFrame(uint32_t seq, uint32_t ts, const uint8_t* payload, uint16_t len);
    void OnMessage(const char* data, size_t len);
    void EndSession();

    static void BadgeTaskEntry(void* arg);
    static void BadgeRetryTimerCallback(void* arg);
    void OnBadgeRetryTick();
    bool FetchBadgeProfile(BadgeProfile* out);
    /* 头像静态图：badge 任务内按需下载 avatar_url（先查 mvp_emoji 缓存），完成后经 Schedule 回调
     * (emoji_id, PNG 字节, 长度)；成功返回 true 并记录 avatar_delivered_id_，失败返回 false（下 tick 重试）。 */
    bool DeliverAvatar(const BadgeProfile& profile);
    /* 把 avatar_url 内容下载进 buf（cap 容量）并回写 out_len；memcpy 直填，不做任何 vector 分配。 */
    bool DownloadAvatar(const std::string& url, uint8_t* buf, size_t cap, size_t* out_len);

    struct RecordFrame {
        uint32_t seq;
        uint32_t ts;
        uint16_t len;
        uint8_t payload[FLASH_RING_FRAME_MAX];
    };

    std::unique_ptr<WebSocket> ws_;
    TaskHandle_t task_ = nullptr;
    QueueHandle_t frame_queue_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    static constexpr int HELLO_ACK_BIT = BIT0;
    static constexpr int HELLO_ERR_BIT = BIT1;

    std::atomic<bool> session_active_{false};
    std::atomic<bool> pending_stop_{false};
    std::atomic<bool> hello_done_{false};       /* 本会话首个 hello_ack 已收到 */
    std::atomic<bool> stopped_by_server_{false};
    std::atomic<int64_t> acked_seq_{-1};        /* 服务端确认水位，-1=无 */
    std::atomic<int64_t> last_sent_seq_{-1};    /* 已发送的最大 seq，-1=无 */
    std::atomic<uint32_t> next_seq_{0};         /* 下一待分配 seq（仅主循环写） */
    int64_t session_start_us_ = 0;

    std::mutex session_mutex_;
    std::string session_id_;

    std::atomic<bool> badge_fetch_running_{false};
    std::atomic<int> badge_last_result_{static_cast<int>(BadgeFetchResult::kFailed)};
    esp_timer_handle_t badge_retry_timer_ = nullptr;
    mutable std::mutex badge_mutex_;
    BadgeProfile badge_;
    BadgeProfile shown_profile_;        /* 当前展示的工牌（含预制兜底），badge_mutex_ 保护 */
    std::string avatar_delivered_id_;   /* 已投递头像的 avatar_id（去重，防头像重复重载） */
    bool badge_preset_shown_ = false;   /* 未绑定已回调过预制信息（badge_mutex_ 保护），防每 tick 重绘 */
    std::function<void(const BadgeProfile&)> badge_callback_;
    std::function<void(const std::string&, uint8_t*, size_t)> avatar_callback_; /* data 所有权移交 UI */
};
