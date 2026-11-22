/* MVP 工牌模式编排（M3）。
 *
 * 单例，串起模式状态机（mode_manager）、录音上传（record_client）、界面（mvp_ui）
 * 与音频链路（Application/AudioService）。三键事件从 ai_passport_board 汇入 HandleKey，
 * 音频分流从 application 主循环经 IsCapturing() 判断后走 RecordClient。
 *
 * 线程：HandleKey / StartCapture / StopCapture 由主任务调用（按键经 Application::Schedule）；
 *   capturing_ / capture_start_us_ 供 UI 定时器跨线程读取，用原子量。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_timer.h>

#include "record_client.h"  // BadgeProfile

class MvpApp {
public:
    static MvpApp& GetInstance();

    /* 初始化：模式状态机、RecordClient、工牌配置回调、按上次模式进入对话或主菜单。 */
    void Init();

    /* 三键事件入口（主任务调用）。
     * key: 0=UP 1=DOWN 2=OK；long_press: 长按。
     * 返回 true=已消费（调用方勿再走默认行为）；false=走默认（如音量、聊天开关）。 */
    bool HandleKey(int key, bool long_press);

    /* 工牌采集中（主循环音频分流据此把 Opus 走 RecordClient 而非聊天协议）。 */
    bool IsCapturing() const { return capturing_.load(); }

    /* 录音计时（毫秒）；未采集返回 0。 */
    uint32_t GetCaptureElapsedMs() const;

    /* ===== 智能陪练（《智能陪练详细设计.md》）===== */

    /* 陪练会话进行中（含场景选择/连接/对话/评分页），供 application.cc 音频与消息分流。 */
    bool IsPracticeActive() const { return practice_state_ != PracticeState::kNone; }

    /* 服务端消息入口（application.cc OnIncomingJson 转发 mcp/practice/stt/tts，主线程调用）。 */
    void OnServerMessage(const std::string& message_json);

private:
    MvpApp() = default;

    /* 录音开关（互斥占用音频链路） */
    void StartCapture();
    void StartCaptureNow();
    void StopCapture();
    void ToggleCapture();

    /* 菜单/模式编排 */
    void OpenMenu();
    void CloseMenuToChat();
    void EnterSelected();
    void MoveSelection(int delta);

    /* 工牌配置就绪（已在主线程）→ 刷新界面 */
    void OnBadgeProfile(const BadgeProfile& profile);
    void OnAvatarGif(const std::string& emoji_id, std::vector<uint8_t> gif);  // 头像动图就绪（主线程）

    /* 设置页编排（《MVP设计文档.md》设置页一节） */
    enum class SettingsPage { kMenu, kDeviceInfo, kVolume, kConfirmReset };
    void EnterSettings();
    void LeaveSettings();        // 回主菜单
    void RenderSettings();       // 组 SettingsView 快照 → ShowSettings
    void EnterSettingsItem();    // 设置菜单 OK：进子页 / 进配网 / 出确认页
    void MoveSettingsSelection(int delta);
    void SetSettingsVolume(int volume);  // 夹取 0-100 后写 Codec（内含 NVS 持久化）
    void DoFactoryReset();       // 擦 NVS + 重启

    /* 智能陪练编排 */
    enum class PracticeState {
        kNone,        // 非陪练
        kScenarios,   // 场景选择页（含加载中）
        kConnecting,  // 已切连接配置并发起连接，等通道就绪后发 set_scenario
        kDialog,      // 对话中
        kEnding,      // 已发 practice.end，等 practice.report
        kScore,       // 评分摘要页
    };
    struct ScenarioItem {
        std::string id;
        std::string name;
        int duration_minutes = 10;
    };

    void EnterPractice();
    void LeavePractice();                 // 恢复连接配置 + 关通道 + 回主菜单
    void FetchScenarios();                // 异步拉取场景列表（practice_api_url）
    void OnScenariosFetched(std::vector<ScenarioItem> items, std::string toast);
    void BeginDialog(const ScenarioItem& scenario);  // 连接切换 + 发起连接
    void OnPracticeTick();                // 250ms 定时器驱动（主线程）
    void SendSetScenario(const ScenarioItem& scenario);
    void EndDialog();                     // 发 practice.end → kEnding 等 report
    void ShowScore();
    void BackToScenarios();
    void RenderPractice();
    void SaveWsSettings();
    void RestoreWsSettings();
    void StartPracticeTimer();
    void StopPracticeTimer();
    static void PracticeTimerCallback(void* arg);
    static void PracticeFetchTask(void* arg);

    bool menu_open_ = false;
    int menu_selected_ = 0;

    // ---- 设置页状态 ----
    SettingsPage settings_page_ = SettingsPage::kMenu;
    int settings_selected_ = 0;
    std::vector<std::string> settings_info_lines_;

    std::atomic<bool> capturing_{false};
    std::atomic<int64_t> capture_start_us_{0};

    // ---- 头像动图 ----
    std::string badge_avatar_id_;  // 当前界面显示的头像 emoji_id（空=姓名首字占位）

    // ---- 智能陪练状态 ----
    PracticeState practice_state_ = PracticeState::kNone;
    std::vector<ScenarioItem> scenarios_;
    ScenarioItem dialog_scenario_;  // 当前陪练场景（连接后发 set_scenario 用）
    int scenario_selected_ = 0;
    bool set_scenario_sent_ = false;
    int pending_set_scenario_id_ = -1;
    int mcp_request_seq_ = 0;
    int connect_wait_ticks_ = 0;
    int report_wait_ticks_ = 0;
    int remaining_s_ = 0;
    int64_t dialog_deadline_us_ = 0;  // 对话倒计时截止时刻（本地递减，服务端 tick 校准）
    std::string dialog_scenario_name_;
    std::string dialog_role_;
    std::string user_line_;
    std::string role_line_;
    std::string practice_toast_;
    double overall_score_ = -1;  // 评分（服务端为浮点；<0 表示未收到）
    std::string feedback_text_;
    // 连接切换缓存（Settings("websocket") 原值，退出陪练时恢复）
    bool ws_settings_saved_ = false;
    std::string saved_ws_url_;
    std::string saved_ws_token_;
    int saved_ws_version_ = 3;
    esp_timer_handle_t practice_timer_ = nullptr;
};
