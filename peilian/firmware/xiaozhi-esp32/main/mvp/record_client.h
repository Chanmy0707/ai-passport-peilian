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
    std::string avatar_url;  /* 相对路径（如 /emojis/64px/<id>.gif），头像 GIF 按需下载 */
    bool valid = false;
};

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
    BadgeProfile GetBadgeProfile() const;
    void SetBadgeProfileCallback(std::function<void(const BadgeProfile&)> callback);

    /* 头像动图：badge 任务内按需下载 avatar_url（先查 mvp_emoji 缓存），完成后经 Schedule 回调
     * (emoji_id, GIF 字节)；失败不回调（UI 保持姓名首字占位）。 */
    void SetAvatarCallback(std::function<void(const std::string&, std::vector<uint8_t>)> callback);

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
    bool FetchBadgeProfile(BadgeProfile* out);
    void DeliverAvatar(const BadgeProfile& profile);
    bool DownloadAvatar(const std::string& url, std::vector<uint8_t>* out);

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
    mutable std::mutex badge_mutex_;
    BadgeProfile badge_;
    std::function<void(const BadgeProfile&)> badge_callback_;
    std::function<void(const std::string&, std::vector<uint8_t>)> avatar_callback_;
};
