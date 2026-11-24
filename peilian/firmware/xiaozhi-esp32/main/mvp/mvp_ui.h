/* MVP 工牌模式 LVGL 界面（M3）。
 *
 * 负责渲染（只管表现，不管业务编排）：
 *   - 主页面（lv_layer_top 全屏覆盖层）：顶部状态栏（时间 / 日期 / Wi-Fi / 电量）+
 *     大头像居中（动图 / 首字占位）+ 姓名、岗位·工号居中 + 底部暗提示"按 OK 进入菜单"
 *     或录音状态区（工牌录音态）。
 *   - 导航页（独立整页）：主菜单 / 设置 / 工牌 三行列表
 *     （UP/DOWN 选择、OK 进入、长按 OK 回首页）。
 *   - 工牌绑定页（独立整页）：设备码 + 当前工牌 + "同步工牌"（OK 立即拉取云端配置并反馈结果）。
 *   - 主菜单页（独立整页）：录音 / 智能陪练 / 小智对话 / 返回
 *     （UP/DOWN 选择、OK 进入、长按 OK 进小智对话）。
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

/* 配网页来源：开机未配网 / 设置“重新配网” / 联网功能触发（无网络拦截）。 */
enum class ProvOrigin { kBoot, kSettings, kFeature };

class MvpUi {
public:
    static MvpUi& GetInstance();

    /* 创建覆盖层与刷新定时器（幂等）。 */
    void Init();

    /* 主页面·首页态（纯工牌：大头像居中 + 姓名/岗位·工号 + 底部暗提示"按 OK 进入菜单"） */
    void ShowHome();

    /* 导航页（独立整页：主菜单 / 设置 / 工牌 三行列表，UP/DOWN 选择、OK 进入、长按 OK 回首页） */
    void ShowNav(int selected);
    void SetNavSelected(int selected);

    /* 工牌绑定页（独立整页）：设备码 + 当前工牌 + "同步工牌"项（OK 立即拉取云端配置）。
     * 同步状态由 provider 每秒回询（返回非空则刷新状态行），结果支持"同步成功/未绑定/同步失败"。 */
    void ShowBadgeInfo(const std::string& device_id, const BadgeProfile& profile, int selected);
    void SetBadgeInfoStatus(const char* text);
    void SetBadgeInfoCurrent(const std::string& name_or_unbound);
    void SetBadgeSyncProvider(std::function<const char*()> provider);

    /* 主菜单页（独立整页：录音 / 智能陪练 / 小智对话 / 返回） */
    void ShowMenu(int selected);
    void SetMenuSelected(int selected);

    /* 主页面·工牌录音态（底部录音状态/计时） */
    void ShowBadge();
    void SetBadgeProfile(const BadgeProfile& profile);

    /* 回小智对话：隐藏主页面/主菜单页（露出小智既有对话页）。 */
    void ShowChat();

    /* 配网页（Wi-Fi 配网）：热点名 + 配网地址。仅 设置→“重新配网” 进入；
     * AP 未启动时 ssid/url 传空，页面显示“正在开启配网热点…”，热点起来后 SetProvisioningInfo 回填。 */
    void ShowProvisioning(const std::string& ap_ssid, const std::string& ap_url, ProvOrigin origin);
    void SetProvisioningInfo(const std::string& ap_ssid, const std::string& ap_url);
    void HideProvisioning();

    /* 离线一次性提醒：顶层 toast 提示“未联网，可在设置重新配网”，约 3 秒自动消失。 */
    void ShowOfflineNotice();

    /* 头像静态图：PNG 覆盖头像位（盖住默认 😀）；data 为 heap_caps_malloc 块，所有权移交本类
     * （内部 heap_caps_free）；空数据/解析失败回落默认 😀。 */
    void SetAvatarImage(uint8_t* data, size_t len);
    void ClearAvatarImage();

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
