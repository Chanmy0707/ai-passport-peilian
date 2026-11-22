/* MVP 工牌模式 LVGL 界面（M3）。
 *
 * 负责渲染（只管表现，不管业务编排）：
 *   - 主页面（lv_layer_top 全屏覆盖层）：顶部状态栏（Wi-Fi 标志 + 时间）+
 *     工牌卡（动图/姓名/职位/工号/二维码）+ 底部区域两态之一。
 *     底部两态：主菜单（工牌 / 智能陪练 / 小智对话 / 设置，UP/DOWN 选择、OK 进入）
 *     或工牌录音态（录音状态/计时 + 按键提示）。
 *   - 配网页：Wi-Fi 配网提示（热点名 + 配网地址），开机未配网或设置里重新配网时显示。
 *   - 智能陪练页：场景选择 / 对话 / 评分三页面。
 *   - 设置页：设置菜单 + 设备信息 / 音量 / 恢复出厂确认子页。
 * 所有 LVGL 操作在 DisplayLockGuard 内进行；内部 1s esp_timer 刷新状态栏与录音计时。
 * 业务逻辑（模式切换、录音开关、按键分发）在 MvpApp。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "record_client.h"  // BadgeProfile

/* 智能陪练页面视图数据（只读快照，ShowPractice 时整体重建内容）。 */
struct PracticeView {
    enum class Page { kScenarios, kDialog, kScore };
    Page page = Page::kScenarios;
    std::vector<std::string> scenario_items;  // 场景名列表（最多显示 4 项）
    int selected = 0;                         // 选中场景下标
    std::string scenario_detail;              // 选中场景的时长+简介（列表下方）
    std::string dialog_title;                 // 对话页标题（场景名）
    std::string countdown;                    // 倒计时文本（如 "09:32"）
    std::string status;                       // 状态文本（连接中/聆听中/角色说话中…）
    std::string user_line;                    // 用户最近一句
    std::string role_line;                    // 角色最近一句
    std::string score;                        // 评分页大分数文本
    std::string feedback;                     // 评分页反馈文本
    std::string toast;                        // 临时提示（红字，空=无）
};

/* 设置页视图数据（只读快照，ShowSettings 时整体重建内容）。 */
struct SettingsView {
    enum class Page { kMenu, kDeviceInfo, kVolume, kConfirmReset };
    Page page = Page::kMenu;
    int selected = 0;                      // kMenu 选中项下标（0..3）
    int volume = 0;                        // kVolume 当前音量（0-100）
    std::vector<std::string> info_lines;   // kDeviceInfo 信息行
};

class MvpUi {
public:
    static MvpUi& GetInstance();

    /* 创建覆盖层与刷新定时器（幂等）。 */
    void Init();

    /* 主页面·主菜单态 */
    void ShowMenu(int selected);
    void SetMenuSelected(int selected);
    void HideMenu();

    /* 主页面·工牌录音态 */
    void ShowBadge();
    void HideBadge();
    void SetBadgeProfile(const BadgeProfile& profile);

    /* 配网页（Wi-Fi 配网）：热点名 + 配网地址。AP 未启动时 ssid/url 传空，
     * 页面显示“正在开启配网热点…”，热点起来后 SetProvisioningInfo 回填。
     * allow_exit=显示“长按 OK 返回”提示（从设置进入时可退出）。 */
    void ShowProvisioning(const std::string& ap_ssid, const std::string& ap_url, bool allow_exit);
    void SetProvisioningInfo(const std::string& ap_ssid, const std::string& ap_url);
    void HideProvisioning();

    /* 头像动图：GIF 覆盖头像位（盖住姓名首字占位）；空数据/解析失败回落首字占位。 */
    void SetAvatarGif(std::vector<uint8_t> gif_data);
    void ClearAvatarGif();

    /* 智能陪练页（场景/对话/评分三页面，内容随 view 整体重建） */
    void ShowPractice(const PracticeView& view);
    void HidePractice();

    /* 设置页（设置菜单 + 设备信息/音量/恢复出厂子页，内容随 view 整体重建） */
    void ShowSettings(const SettingsView& view);
    void HideSettings();

    /* 录音状态/计时提供者：UI 定时器或立即刷新时回填 recording / elapsed_ms。 */
    void SetRecordStatusProvider(std::function<void(bool&, uint32_t&)> provider);

    /* 立即刷新录音状态区（定时器每秒也调用一次）。 */
    void Update();

private:
    MvpUi() = default;

    struct Impl;
    Impl* impl_ = nullptr;
};
