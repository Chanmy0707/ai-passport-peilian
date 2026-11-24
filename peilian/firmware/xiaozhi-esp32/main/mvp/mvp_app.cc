#include "mvp_app.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#include <cstdio>
#include <string>

#include <cJSON.h>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "http.h"
#include "mvp/mode_manager.h"
#include "mvp/mvp_config.h"
#include "mvp/mvp_ui.h"
#include "mvp/record_client.h"
#include "settings.h"
#include "system_info.h"
#include "wifi_board.h"

#include <ssid_manager.h>
#include <wifi_manager.h>

#define TAG "mvp_app"

namespace {
/* 导航页三项：0=主菜单 1=设置 2=工牌。 */
constexpr int kNavItemMenu = 0;
constexpr int kNavItemSettings = 1;
constexpr int kNavItemBadge = 2;

/* 主菜单页项：0=录音 1=智能陪练 2=小智对话 3=返回。 */
constexpr int kMenuItemRecord = 0;
constexpr int kMenuItemPractice = 1;
constexpr int kMenuItemChat = 2;
constexpr int kMenuItemBack = 3;
bool MenuItemEnabled(int index) { return index >= 0 && index <= 3; }

/* 设置子页项：0=设备信息 1=音量调节 2=Wi-Fi 重新配网 3=恢复出厂设置。 */
constexpr int kSettingsItemDeviceInfo = 0;
constexpr int kSettingsItemVolume = 1;
constexpr int kSettingsItemWifi = 2;
constexpr int kSettingsItemReset = 3;

int CurrentOutputVolume() {
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec != nullptr ? codec->output_volume() : 0;
}

/* 陪练连接/评分等待超时（250ms 一定时器 tick）：连接 100=25s，评分 20=5s。
 * 连接超时放宽到 25s：frpc 穿透链路抖动时 10s 常误判，配合超时自动重试。 */
constexpr int kConnectTimeoutTicks = 100;
constexpr int kReportTimeoutTicks = 20;

std::atomic<bool> g_practice_fetch_running{false};
}  // namespace

MvpApp& MvpApp::GetInstance() {
    static MvpApp instance;
    return instance;
}

void MvpApp::Init() {
    ModeManager_Init();
    RecordClient::GetInstance().Init();
    MvpUi::GetInstance().Init();

    // 录音状态/计时提供者（UI 定时器回填 recording / elapsed_ms）
    MvpUi::GetInstance().SetRecordStatusProvider([this](bool& recording, uint32_t& elapsed_ms) {
        recording = capturing_.load();
        elapsed_ms = GetCaptureElapsedMs();
    });

    // 工牌配置在后台任务返回，调度到主线程刷新界面
    RecordClient::GetInstance().SetBadgeProfileCallback([this](const BadgeProfile& profile) {
        Application::GetInstance().Schedule([this, profile]() { OnBadgeProfile(profile); });
    });

    // 工牌绑定页手动同步状态回询（UI tick 任务调用，内部读原子量/互斥快照）
    MvpUi::GetInstance().SetBadgeSyncProvider([this]() { return PollBadgeSyncStatus(); });

    // 头像静态图按需下载/缓存命中后回调（badge 任务内）→ 调度到主线程覆盖默认 😀 头像
    RecordClient::GetInstance().SetAvatarCallback(
        [this](const std::string& emoji_id, uint8_t* png, size_t len) {
            Application::GetInstance().Schedule([this, emoji_id, png, len]() {
                OnAvatarImage(emoji_id, png, len);
            });
        });

    // 开机立即拉取工牌配置/头像（原首次拉取在 15s 定时器，短周期/重启场景拿不到头像）
    RecordClient::GetInstance().FetchBadgeProfileAsync();

    // 开机未配网：离线进首页显示工牌，仅一次性提醒；配网入口收敛到 设置→重新配网
    if (SsidManager::GetInstance().GetSsidList().empty()) {
        GoToHome();
        NotifyOfflineOnce();
        return;
    }

    // 按上次模式进入：上次为对话则直进对话，否则进首页
    Settings settings("mvp", true);
    if (settings.GetString("last_mode", "") == "chat") {
        ModeManager_Switch(MODE_CHAT);
        home_open_ = false;
    } else {
        GoToHome();
    }
}

bool MvpApp::HandleKey(int key, bool long_press) {
    if (provisioning_) {
        // 配网页：长按 OK 取消（kSettings 回退原网络并返回设置；kBoot/kFeature 回主页面）
        if (key == 2) {
            if (long_press) {
                CancelProvisioning();
            }
            return true;
        }
        return false;  // UP/DOWN 走默认音量
    }

    // 主菜单页：UP/DOWN 选择、OK 进入（长按=小智对话）
    if (menu_open_) {
        if (key == 2) {  // OK
            if (long_press) {
                GoToChat();
            } else {
                EnterSelected();
            }
            return true;
        }
        if (!long_press && (key == 0 || key == 1)) {  // UP/DOWN 短按导航
            MoveSelection(key == 0 ? -1 : +1);
            return true;
        }
        return false;  // UP/DOWN 长按走默认音量
    }

    // 导航页：主菜单 / 设置 / 工牌 三项，UP/DOWN 选择、OK 进入（长按=返回首页）
    if (nav_open_) {
        if (key == 2) {  // OK
            if (long_press) {
                GoToHome();
            } else if (nav_selected_ == kNavItemMenu) {
                OpenMenu();
            } else if (nav_selected_ == kNavItemBadge) {
                OpenBadgeInfo();
            } else {
                ModeManager_Switch(MODE_SETTINGS);
                nav_open_ = false;
                badge_info_open_ = false;
                home_open_ = false;
                EnterSettings();
            }
            return true;
        }
        if (!long_press && (key == 0 || key == 1)) {  // UP/DOWN 短按三项循环
            nav_selected_ = (nav_selected_ + (key == 0 ? -1 : +1) + 3) % 3;
            MvpUi::GetInstance().SetNavSelected(nav_selected_);
            return true;
        }
        return false;  // UP/DOWN 长按走默认音量
    }

    // 工牌绑定页：OK 短按=立即同步（长按=回首页）
    if (badge_info_open_) {
        if (key == 2) {
            if (long_press) {
                GoToHome();
            } else {
                StartBadgeSync();
            }
            return true;
        }
        return false;  // UP/DOWN 走默认音量
    }

    // 主页面·首页态：纯工牌展示，OK 进导航页（长按=小智对话）
    if (home_open_) {
        if (key == 2) {  // OK
            if (long_press) {
                GoToChat();
            } else {
                OpenNav();
            }
            return true;
        }
        return false;  // UP/DOWN 走默认音量
    }

    // 工牌录音态：OK 录音/停止（长按回首页）
    if (ModeManager_GetMode() == MODE_BADGE) {
        if (key == 2) {  // OK
            if (long_press) {
                StopCapture();
                GoToHome();
            } else {
                ToggleCapture();
            }
            return true;
        }
        return false;  // UP/DOWN 走默认音量
    }

    // 智能陪练：三键按《智能陪练详细设计.md》§1.2 分流
    if (ModeManager_GetMode() == MODE_PRACTICE) {
        if (key == 2) {  // OK
            if (long_press) {
                if (practice_state_ == PracticeState::kScenarios) {
                    LeavePractice();
                } else if (practice_state_ == PracticeState::kConnecting ||
                           practice_state_ == PracticeState::kDialog) {
                    EndDialog();
                } else if (practice_state_ == PracticeState::kScore) {
                    BackToScenarios();
                }
            } else if (practice_state_ == PracticeState::kScenarios) {
                if (!scenarios_.empty()) {
                    BeginDialog(scenarios_[scenario_selected_]);
                }
            } else if (practice_state_ == PracticeState::kDialog) {
                // 说话 / 打断：按设备态分流
                auto& app = Application::GetInstance();
                DeviceState state = app.GetDeviceState();
                if (state == kDeviceStateListening) {
                    app.StopListening();
                } else if (state == kDeviceStateSpeaking) {
                    app.AbortSpeaking(kAbortReasonNone);
                } else {
                    app.StartListening();
                }
            } else if (practice_state_ == PracticeState::kScore) {
                BackToScenarios();
            }
            return true;
        }
        if (!long_press && (key == 0 || key == 1) &&
            practice_state_ == PracticeState::kScenarios && !scenarios_.empty()) {
            int sel = scenario_selected_ + (key == 0 ? -1 : 1);
            if (sel < 0) {
                sel = static_cast<int>(scenarios_.size()) - 1;
            }
            if (sel >= static_cast<int>(scenarios_.size())) {
                sel = 0;
            }
            scenario_selected_ = sel;
            RenderPractice();
            return true;
        }
        return false;  // 其余走默认（如音量）
    }

    // 设置页：三键按《MVP设计文档.md》设置页一节分流
    if (ModeManager_GetMode() == MODE_SETTINGS) {
        if (key == 2) {  // OK
            if (settings_page_ == SettingsPage::kMenu) {
                if (long_press) {
                    LeaveSettings();
                } else {
                    EnterSettingsItem();
                }
            } else if (settings_page_ == SettingsPage::kConfirmReset && !long_press) {
                DoFactoryReset();
            } else {
                // 子页：OK 返回设置菜单（恢复出厂确认页长按=取消）
                settings_page_ = SettingsPage::kMenu;
                RenderSettings();
            }
            return true;
        }
        if (key == 0 || key == 1) {  // UP/DOWN
            if (settings_page_ == SettingsPage::kMenu) {
                if (!long_press) {
                    MoveSettingsSelection(key == 0 ? -1 : +1);
                    return true;
                }
            } else if (settings_page_ == SettingsPage::kVolume) {
                // 短按 ±10，长按到最大/最小（与板级默认音量键一致）
                int volume =
                    long_press ? (key == 0 ? 100 : 0) : CurrentOutputVolume() + (key == 0 ? 10 : -10);
                SetSettingsVolume(volume);
                return true;
            }
        }
        return false;
    }

    // 对话模式：仅 OK 长按回首页，其余走默认聊天行为
    if (key == 2 && long_press) {
        GoToHome();
        return true;
    }
    // 离线时 OK 短按不走默认聊天开关（默认的“连接中”提示在 mvp 界面下不可见），改为一次性提醒
    if (key == 2 && !long_press && !WifiManager::GetInstance().IsConnected()) {
        NotifyOfflineOnce();
        return true;
    }
    return false;
}

uint32_t MvpApp::GetCaptureElapsedMs() const {
    if (!capturing_.load()) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - capture_start_us_.load()) / 1000);
}

/* ---------------- 录音开关（互斥占用音频链路） ---------------- */

void MvpApp::ToggleCapture() {
    if (capturing_.load()) {
        StopCapture();
    } else {
        StartCapture();
    }
}

void MvpApp::StartCapture() {
    if (!RequireNetwork()) {
        return;  // 未联网一次性提醒，不离线录音
    }
    if (capturing_.load()) {
        return;
    }
    auto& app = Application::GetInstance();
    DeviceState state = app.GetDeviceState();
    if (state == kDeviceStateListening) {
        app.StopListening();
    } else if (state == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
    }
    // 推迟一拍执行：状态机切到 Idle 时会关语音处理，须等它稳定后再开采集
    app.Schedule([this]() { StartCaptureNow(); });
}

void MvpApp::StartCaptureNow() {
    if (capturing_.load()) {
        return;
    }
    auto& app = Application::GetInstance();
    RecordClient::GetInstance().StartRecording();
    app.GetAudioService().EnableVoiceProcessing(true);
    app.GetAudioService().EnableWakeWordDetection(false);
    capture_start_us_ = esp_timer_get_time();
    capturing_ = true;
    MvpUi::GetInstance().Update();
}

void MvpApp::StopCapture() {
    if (!capturing_.load()) {
        return;
    }
    capturing_ = false;
    auto& app = Application::GetInstance();
    app.GetAudioService().EnableVoiceProcessing(false);
    app.GetAudioService().EnableWakeWordDetection(app.GetAudioService().IsAfeWakeWord());
    RecordClient::GetInstance().StopRecording();
    MvpUi::GetInstance().Update();
}

/* ---------------- 首页/导航页/菜单/模式编排 ---------------- */

void MvpApp::GoToHome() {
    StopCapture();  // 首页态不录音
    ModeManager_Switch(MODE_BADGE);
    home_open_ = true;
    nav_open_ = false;
    menu_open_ = false;
    badge_info_open_ = false;
    MvpUi::GetInstance().ShowHome();
}

void MvpApp::OpenNav() {
    StopCapture();  // 导航页不录音
    home_open_ = false;
    nav_open_ = true;
    menu_open_ = false;
    badge_info_open_ = false;
    MvpUi::GetInstance().ShowNav(nav_selected_);
}

void MvpApp::GoToChat() {
    if (!RequireNetwork()) {
        return;  // 未联网一次性提醒，不离线对话
    }
    StopCapture();
    ModeManager_Switch(MODE_CHAT);
    home_open_ = false;
    nav_open_ = false;
    menu_open_ = false;
    badge_info_open_ = false;
    Settings settings("mvp", true);
    settings.SetString("last_mode", "chat");
    MvpUi::GetInstance().ShowChat();
}

void MvpApp::OpenMenu() {
    StopCapture();  // 菜单态不录音
    nav_open_ = false;
    menu_open_ = true;
    badge_info_open_ = false;
    menu_selected_ = kMenuItemRecord;
    MvpUi::GetInstance().ShowMenu(menu_selected_);
}

void MvpApp::OpenBadgeInfo() {
    StopCapture();  // 信息页不录音
    nav_open_ = false;
    menu_open_ = false;
    home_open_ = false;
    badge_info_open_ = true;
    badge_sync_pending_ = false;
    MvpUi::GetInstance().ShowBadgeInfo(GetMvpConfig().device_id,
                                       RecordClient::GetInstance().GetBadgeProfile(), 0);
}

void MvpApp::StartBadgeSync() {
    if (badge_sync_pending_.load()) {
        return;  // 上一次同步还在等结果
    }
    auto& rc = RecordClient::GetInstance();
    badge_sync_start_us_ = esp_timer_get_time();
    badge_sync_pending_ = true;
    rc.FetchBadgeProfileAsync();  // 与 15s 自动轮询共用任务（进行中则复用本次结果）
    MvpUi::GetInstance().SetBadgeInfoStatus("拉取中");
}

const char* MvpApp::PollBadgeSyncStatus() {
    if (!badge_sync_pending_.load()) {
        return nullptr;
    }
    auto& rc = RecordClient::GetInstance();
    int64_t elapsed_us = esp_timer_get_time() - badge_sync_start_us_.load();
    if (elapsed_us < 1500000 || rc.IsBadgeFetching()) {
        return nullptr;  // 仍在拉取
    }
    badge_sync_pending_ = false;
    const char* status = "同步失败";
    switch (rc.GetBadgeLastResult()) {
        case BadgeFetchResult::kOk:
            status = "同步成功";
            break;
        case BadgeFetchResult::kNotConfigured:
            status = "未绑定";
            break;
        default:
            break;
    }
    BadgeProfile p = rc.GetBadgeProfile();
    std::string cur = (p.valid && !p.name.empty()) ? p.name : "未绑定";
    Application::GetInstance().Schedule([cur]() {
        MvpUi::GetInstance().SetBadgeInfoCurrent(cur);
    });
    return status;
}

void MvpApp::EnterSelected() {
    if (menu_selected_ == kMenuItemRecord) {
        ModeManager_Switch(MODE_BADGE);
        menu_open_ = false;
        home_open_ = false;
        nav_open_ = false;
        Settings settings("mvp", true);
        settings.SetString("last_mode", "badge");
        MvpUi::GetInstance().ShowBadge();
        OnBadgeProfile(RecordClient::GetInstance().GetBadgeProfile());  // 先显示缓存/未绑定态
        RecordClient::GetInstance().FetchBadgeProfileAsync();  // 刷新工牌配置
    } else if (menu_selected_ == kMenuItemPractice) {
        if (!RequireNetwork()) {
            return;  // 未联网一次性提醒
        }
        ModeManager_Switch(MODE_PRACTICE);
        menu_open_ = false;
        home_open_ = false;
        nav_open_ = false;
        EnterPractice();
    } else if (menu_selected_ == kMenuItemChat) {
        GoToChat();
    } else if (menu_selected_ == kMenuItemBack) {
        GoToHome();
    }
}

void MvpApp::MoveSelection(int delta) {
    int sel = menu_selected_;
    for (int i = 0; i < 4; i++) {
        sel += delta;
        if (sel < 0) {
            sel = 3;
        }
        if (sel > 3) {
            sel = 0;
        }
        if (MenuItemEnabled(sel)) {
            break;
        }
    }
    if (sel != menu_selected_) {
        menu_selected_ = sel;
        MvpUi::GetInstance().SetMenuSelected(sel);
    }
}

/* ================= 设置页（《MVP设计文档.md》设置页一节） ================= */

void MvpApp::EnterSettings() {
    settings_page_ = SettingsPage::kMenu;
    settings_selected_ = 0;
    settings_info_lines_.clear();
    RenderSettings();
}

void MvpApp::LeaveSettings() {
    settings_info_lines_.clear();
    MvpUi::GetInstance().HideSettings();
    GoToHome();
}

void MvpApp::RenderSettings() {
    SettingsView view;
    switch (settings_page_) {
        case SettingsPage::kMenu:
            view.page = SettingsView::Page::kMenu;
            view.selected = settings_selected_;
            break;
        case SettingsPage::kDeviceInfo:
            view.page = SettingsView::Page::kDeviceInfo;
            view.info_lines = settings_info_lines_;
            break;
        case SettingsPage::kVolume:
            view.page = SettingsView::Page::kVolume;
            view.volume = CurrentOutputVolume();
            break;
        case SettingsPage::kConfirmReset:
            view.page = SettingsView::Page::kConfirmReset;
            break;
    }
    MvpUi::GetInstance().ShowSettings(view);
}

void MvpApp::EnterSettingsItem() {
    switch (settings_selected_) {
        case kSettingsItemDeviceInfo: {
            settings_page_ = SettingsPage::kDeviceInfo;
            const auto& cfg = GetMvpConfig();
            const esp_app_desc_t* desc = esp_app_get_description();
            settings_info_lines_.clear();
            settings_info_lines_.push_back("设备ID  " + cfg.device_id);
            settings_info_lines_.push_back(std::string("固件版本  ") +
                                           (desc != nullptr ? desc->version : "未知"));
            settings_info_lines_.push_back("MAC  " + SystemInfo::GetMacAddress());
            break;
        }
        case kSettingsItemVolume:
            settings_page_ = SettingsPage::kVolume;
            break;
        case kSettingsItemWifi:
            // 重新配网：跳转配网页 + 板级进 AP 配网模式
            EnterProvisioning(ProvOrigin::kSettings);
            return;  // 配网页接管界面，勿再 RenderSettings
        case kSettingsItemReset:
            settings_page_ = SettingsPage::kConfirmReset;
            break;
        default:
            return;
    }
    RenderSettings();
}

void MvpApp::MoveSettingsSelection(int delta) {
    int sel = settings_selected_ + delta;
    if (sel < 0) {
        sel = 3;
    }
    if (sel > 3) {
        sel = 0;
    }
    settings_selected_ = sel;
    RenderSettings();
}

void MvpApp::SetSettingsVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetOutputVolume(volume);  // 内部写 NVS 持久化
    }
    RenderSettings();
}

void MvpApp::DoFactoryReset() {
    ESP_LOGW(TAG, "恢复出厂设置：擦除 NVS 并重启");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
    }
    nvs_flash_init();
    esp_restart();
}

/* ============= 配网页（开机未配网 / 设置里“Wi-Fi 重新配网” / 联网功能触发） ============= */

void MvpApp::EnterProvisioning(ProvOrigin origin) {
    if (provisioning_) {
        return;
    }
    provisioning_ = true;
    provisioning_origin_ = origin;
    auto& wifi = WifiManager::GetInstance();
    // 配网热点已在运行（如开机未配网由板级流程开启）则直接复用，勿重复开启
    if (!wifi.IsConfigMode()) {
        static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
    }
    MvpUi::GetInstance().ShowProvisioning(wifi.GetApSsid(), wifi.GetApWebUrl(), origin);
}

bool MvpApp::RequireNetwork() {
    if (WifiManager::GetInstance().IsConnected()) {
        return true;
    }
    // 离线：仅一次性提醒，不跳配网页、不动网络（配网入口在 设置→重新配网）
    NotifyOfflineOnce();
    return false;
}

void MvpApp::NotifyOfflineOnce() {
    if (offline_notice_shown_) {
        return;
    }
    offline_notice_shown_ = true;
    MvpUi::GetInstance().ShowOfflineNotice();
}

void MvpApp::ExitProvisioning(bool to_settings) {
    if (!provisioning_) {
        return;
    }
    provisioning_ = false;
    MvpUi::GetInstance().HideProvisioning();
    if (to_settings) {
        settings_page_ = SettingsPage::kMenu;
        RenderSettings();
    } else {
        GoToHome();
    }
}

void MvpApp::CancelProvisioning() {
    auto& wifi = WifiManager::GetInstance();
    wifi.StopConfigAp();
    if (!SsidManager::GetInstance().GetSsidList().empty()) {
        wifi.StartStation();
    }
    // kSettings 取消回设置菜单（已回退原网络）；kBoot/kFeature 取消回主页面离线显示工牌
    ExitProvisioning(provisioning_origin_ == ProvOrigin::kSettings);
}

void MvpApp::OnNetworkEvent(NetworkEvent event) {
    Application::GetInstance().Schedule([this, event]() {
        if (event == NetworkEvent::WifiConfigModeEnter) {
            // 配网热点就绪：仅在配网页已打开时回填热点名/配网地址，不自动弹页
            if (provisioning_) {
                auto& wifi = WifiManager::GetInstance();
                MvpUi::GetInstance().SetProvisioningInfo(wifi.GetApSsid(), wifi.GetApWebUrl());
            }
        } else if (event == NetworkEvent::Connected) {
            // 新的联网周期：重新允许一次性离线提醒
            offline_notice_shown_ = false;
            // 配网成功：从设置进入的回设置菜单，否则进主页面
            if (provisioning_) {
                ExitProvisioning(provisioning_origin_ == ProvOrigin::kSettings &&
                                 ModeManager_GetMode() == MODE_SETTINGS);
            }
        }
    });
}

void MvpApp::OnBadgeProfile(const BadgeProfile& profile) {
    MvpUi::GetInstance().SetBadgeProfile(profile);
    /* 头像变更（含取消头像）：先回落默认 😀 头像，新头像随后由 OnAvatarImage 盖上；获取失败保持默认。 */
    if (profile.avatar_id != badge_avatar_id_) {
        badge_avatar_id_.clear();
        MvpUi::GetInstance().ClearAvatarImage();
    }
}

void MvpApp::OnAvatarImage(const std::string& emoji_id, uint8_t* data, size_t len) {
    badge_avatar_id_ = emoji_id;
    MvpUi::GetInstance().SetAvatarImage(data, len); /* 所有权移交 UI */
}

/* ================= 智能陪练（《智能陪练详细设计.md》） ================= */

void MvpApp::EnterPractice() {
    practice_state_ = PracticeState::kScenarios;
    scenario_selected_ = 0;
    practice_toast_.clear();
    overall_score_ = -1;
    feedback_text_.clear();
    user_line_.clear();
    role_line_.clear();
    dialog_role_.clear();
    RenderPractice();
    FetchScenarios();
}

void MvpApp::LeavePractice() {
    StopPracticeTimer();
    RestoreWsSettings();
    Application::GetInstance().CloseAudioChannel();
    practice_state_ = PracticeState::kNone;
    set_scenario_sent_ = false;
    pending_set_scenario_id_ = -1;
    MvpUi::GetInstance().HidePractice();
    GoToHome();
}

void MvpApp::FetchScenarios() {
    if (g_practice_fetch_running.exchange(true)) {
        return;
    }
    if (xTaskCreate(&MvpApp::PracticeFetchTask, "mvp_practice_fetch", 8192, this, 4, nullptr) !=
        pdPASS) {
        g_practice_fetch_running = false;
        practice_toast_ = "场景获取失败";
        RenderPractice();
    }
}

void MvpApp::PracticeFetchTask(void* arg) {
    auto* self = static_cast<MvpApp*>(arg);
    std::vector<ScenarioItem> items;
    std::string toast;
    const auto& cfg = GetMvpConfig();
    auto network = Board::GetInstance().GetNetwork();
    bool ok = false;
    if (network != nullptr) {
        auto http = network->CreateHttp(1);
        if (http != nullptr) {
            http->SetTimeout(10000);
            http->SetHeader("X-Device-Token", cfg.device_token);
            std::string url =
                cfg.practice_api_url + "/api/v1/devices/" + cfg.device_id + "/scenarios";
            if (auto opened = http->Open("GET", url); opened) {
                auto status = http->GetStatusCode();
                if (status && status.value() == 200) {
                    std::string body = http->ReadAll();
                    cJSON* root = cJSON_Parse(body.c_str());
                    if (cJSON_IsArray(root)) {
                        cJSON* it = nullptr;
                        cJSON_ArrayForEach (it, root) {
                            cJSON* sid = cJSON_GetObjectItem(it, "scenario_id");
                            cJSON* name = cJSON_GetObjectItem(it, "name");
                            cJSON* desc = cJSON_GetObjectItem(it, "description");
                            cJSON* dur = cJSON_GetObjectItem(it, "duration_minutes");
                            if (!cJSON_IsString(sid) || !cJSON_IsString(name)) {
                                continue;
                            }
                            ScenarioItem s;
                            s.id = sid->valuestring;
                            s.name = name->valuestring;
                            if (cJSON_IsString(desc)) {
                                s.description = desc->valuestring;
                            }
                            if (cJSON_IsNumber(dur) && dur->valueint > 0) {
                                s.duration_minutes = dur->valueint;
                            }
                            items.push_back(std::move(s));
                        }
                        ok = true;
                    }
                    cJSON_Delete(root);
                }
            }
            http->Close();
        }
    }
    if (!ok) {
        toast = "场景获取失败";
    }
    g_practice_fetch_running = false;
    Application::GetInstance().Schedule([self, items, toast]() {
        self->OnScenariosFetched(items, toast);
    });
    vTaskDelete(nullptr);
}

void MvpApp::OnScenariosFetched(std::vector<ScenarioItem> items, std::string toast) {
    if (practice_state_ != PracticeState::kScenarios) {
        return;  // 用户已离开陪练
    }
    if (!items.empty()) {
        scenarios_ = std::move(items);
        scenario_selected_ = 0;
    } else if (practice_toast_.empty()) {
        practice_toast_ = toast.empty() ? "暂无可选场景" : toast;
    }
    RenderPractice();
}

void MvpApp::BeginDialog(const ScenarioItem& scenario) {
    dialog_scenario_ = scenario;
    dialog_scenario_name_ = scenario.name;
    user_line_.clear();
    role_line_.clear();
    dialog_role_.clear();
    overall_score_ = -1;
    feedback_text_.clear();
    remaining_s_ = scenario.duration_minutes * 60;
    dialog_deadline_us_ = 0;
    set_scenario_sent_ = false;
    pending_set_scenario_id_ = -1;
    connect_wait_ticks_ = 0;
    connect_retried_ = false;
    practice_toast_.clear();

    // 连接切换：缓存原连接配置 → 写陪练端点（设计 §1.4，version 必须为 3）
    SaveWsSettings();
    Settings ws("websocket", false);
    ws.SetString("url", GetMvpConfig().practice_ws_url);
    ws.SetString("token", GetMvpConfig().device_token);
    ws.SetInt("version", 3);

    practice_state_ = PracticeState::kConnecting;
    RenderPractice();
    StartPracticeTimer();
    // 先关旧通道再发起连接，强制按新配置重连
    Application::GetInstance().CloseAudioChannel();
    Application::GetInstance().StartListening();
}

void MvpApp::OnPracticeTick() {
    if (practice_state_ == PracticeState::kConnecting) {
        connect_wait_ticks_++;
        DeviceState state = Application::GetInstance().GetDeviceState();
        if (!set_scenario_sent_ && state == kDeviceStateListening) {
            // 通道就绪：发 set_scenario，随后停听等角色开场
            practice_toast_.clear();
            SendSetScenario(dialog_scenario_);
            set_scenario_sent_ = true;
            Application::GetInstance().StopListening();
        } else if (connect_wait_ticks_ >= kConnectTimeoutTicks) {
            if (connect_retried_) {
                practice_toast_ = "连接陪练服务超时";
                BackToScenarios();
                return;
            }
            // 首次超时自动重连重试一次（链路抖动时二次握手常可恢复）
            connect_retried_ = true;
            connect_wait_ticks_ = 0;
            set_scenario_sent_ = false;
            practice_toast_ = "网络不稳定，正在重试";
            Application::GetInstance().CloseAudioChannel();
            Application::GetInstance().StartListening();
        }
        RenderPractice();
    } else if (practice_state_ == PracticeState::kDialog) {
        // 本地倒计时递减（服务端 practice.tick 会校准）
        if (dialog_deadline_us_ > 0) {
            int64_t remain_us = dialog_deadline_us_ - esp_timer_get_time();
            remaining_s_ = remain_us > 0 ? (int)(remain_us / 1000000) : 0;
        }
        RenderPractice();
    } else if (practice_state_ == PracticeState::kEnding) {
        report_wait_ticks_++;
        if (report_wait_ticks_ >= kReportTimeoutTicks) {
            if (overall_score_ < 0) {
                feedback_text_ = "评分生成超时，请稍后在管理后台查看";
            }
            ShowScore();
        }
    }
}

void MvpApp::SendSetScenario(const ScenarioItem& scenario) {
    pending_set_scenario_id_ = ++mcp_request_seq_;
    std::string payload =
        std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(pending_set_scenario_id_) +
        R"(,"method":"tools/call","params":{"name":"set_scenario","arguments":{"scenario_id":")" +
        scenario.id + R"("}}})";
    Application::GetInstance().SendMcpMessage(payload);
}

void MvpApp::EndDialog() {
    if (practice_state_ == PracticeState::kDialog) {
        Application::GetInstance().SendJsonMessage(R"({"type":"practice","event":"end"})");
        practice_state_ = PracticeState::kEnding;
        report_wait_ticks_ = 0;
        RenderPractice();
    } else if (practice_state_ == PracticeState::kConnecting) {
        // 连接阶段取消
        BackToScenarios();
    }
}

void MvpApp::ShowScore() {
    practice_state_ = PracticeState::kScore;
    StopPracticeTimer();
    RenderPractice();
}

void MvpApp::BackToScenarios() {
    StopPracticeTimer();
    RestoreWsSettings();
    Application::GetInstance().CloseAudioChannel();
    set_scenario_sent_ = false;
    pending_set_scenario_id_ = -1;
    remaining_s_ = 0;
    dialog_deadline_us_ = 0;
    overall_score_ = -1;
    feedback_text_.clear();
    user_line_.clear();
    role_line_.clear();
    dialog_role_.clear();
    practice_state_ = PracticeState::kScenarios;
    RenderPractice();
}

void MvpApp::RenderPractice() {
    PracticeView v;
    if (practice_state_ == PracticeState::kScenarios) {
        v.page = PracticeView::Page::kScenarios;
        for (const auto& s : scenarios_) {
            v.scenario_items.push_back(s.name);
        }
        v.selected = scenario_selected_;
        // 选中场景的时长+简介（场景列表下方显示）
        if (scenario_selected_ >= 0 && scenario_selected_ < static_cast<int>(scenarios_.size())) {
            const ScenarioItem& s = scenarios_[scenario_selected_];
            char dur[16];
            std::snprintf(dur, sizeof(dur), "%d 分钟", s.duration_minutes);
            v.scenario_detail =
                s.description.empty() ? dur : std::string(dur) + " · " + s.description;
        }
    } else if (practice_state_ == PracticeState::kScore) {
        v.page = PracticeView::Page::kScore;
        if (overall_score_ >= 0) {
            char buf[16];
            if (overall_score_ == (double)(int)overall_score_) {
                std::snprintf(buf, sizeof(buf), "%d 分", (int)overall_score_);
            } else {
                std::snprintf(buf, sizeof(buf), "%.1f 分", overall_score_);
            }
            v.score = buf;
        } else {
            v.score = "--";
        }
        v.feedback = feedback_text_;
    } else {
        v.page = PracticeView::Page::kDialog;
        v.dialog_title = dialog_scenario_name_;
        int remain = remaining_s_ > 0 ? remaining_s_ : 0;
        char t[12];
        std::snprintf(t, sizeof(t), "%02d:%02d", remain / 60, remain % 60);
        v.countdown = t;
        if (practice_state_ == PracticeState::kConnecting) {
            v.status = set_scenario_sent_ ? "正在开启陪练…" : "连接中…";
        } else if (practice_state_ == PracticeState::kEnding) {
            v.status = "生成评分中…";
        } else {
            switch (Application::GetInstance().GetDeviceState()) {
                case kDeviceStateListening:
                    v.status = "聆听中 · 请说话";
                    break;
                case kDeviceStateSpeaking:
                    v.status = "角色说话中 · OK 打断";
                    break;
                case kDeviceStateConnecting:
                    v.status = "连接中…";
                    break;
                default:
                    v.status = "待命 · OK 开始说话";
                    break;
            }
        }
        v.user_line = user_line_;
        if (!role_line_.empty()) {
            v.role_line = role_line_;
        } else if (!dialog_role_.empty()) {
            v.role_line = dialog_role_ + "（等待开场）";
        } else {
            v.role_line = "（等待角色开场）";
        }
    }
    v.toast = practice_toast_;
    MvpUi::GetInstance().ShowPractice(v);
}

void MvpApp::SaveWsSettings() {
    if (ws_settings_saved_) {
        return;
    }
    Settings ws("websocket", false);
    saved_ws_url_ = ws.GetString("url");
    saved_ws_token_ = ws.GetString("token");
    saved_ws_version_ = ws.GetInt("version", 3);
    ws_settings_saved_ = true;
}

void MvpApp::RestoreWsSettings() {
    if (!ws_settings_saved_) {
        return;
    }
    Settings ws("websocket", false);
    ws.SetString("url", saved_ws_url_);
    ws.SetString("token", saved_ws_token_);
    ws.SetInt("version", saved_ws_version_);
    ws_settings_saved_ = false;
}

void MvpApp::StartPracticeTimer() {
    if (practice_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &MvpApp::PracticeTimerCallback;
        args.arg = this;
        args.name = "mvp_practice";
        if (esp_timer_create(&args, &practice_timer_) != ESP_OK) {
            practice_timer_ = nullptr;
            return;
        }
    }
    esp_timer_stop(practice_timer_);
    esp_timer_start_periodic(practice_timer_, 250000);  // 250ms
}

void MvpApp::StopPracticeTimer() {
    if (practice_timer_ != nullptr) {
        esp_timer_stop(practice_timer_);
    }
}

void MvpApp::PracticeTimerCallback(void* arg) {
    auto* self = static_cast<MvpApp*>(arg);
    Application::GetInstance().Schedule([self]() { self->OnPracticeTick(); });
}

void MvpApp::OnServerMessage(const std::string& message_json) {
    cJSON* root = cJSON_Parse(message_json.c_str());
    if (root == nullptr) {
        return;
    }
    auto get_str = [root](const char* key) -> std::string {
        cJSON* item = cJSON_GetObjectItem(root, key);
        return cJSON_IsString(item) ? item->valuestring : "";
    };
    std::string type = get_str("type");
    if (type == "mcp") {
        // set_scenario 的 JSON-RPC 响应（设计 §2.3）
        cJSON* payload = cJSON_GetObjectItem(root, "payload");
        cJSON* id = cJSON_IsObject(payload) ? cJSON_GetObjectItem(payload, "id") : nullptr;
        if (cJSON_IsNumber(id) && id->valueint == pending_set_scenario_id_ &&
            practice_state_ == PracticeState::kConnecting) {
            if (cJSON_GetObjectItem(payload, "error") != nullptr) {
                practice_toast_ = "场景不存在或服务错误";
                BackToScenarios();
            } else {
                // result.content[0].text 为内嵌 JSON 字符串 {"role_name": ...}
                cJSON* result = cJSON_GetObjectItem(payload, "result");
                cJSON* content = cJSON_IsObject(result)
                                     ? cJSON_GetObjectItem(result, "content")
                                     : nullptr;
                cJSON* first = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : nullptr;
                cJSON* text = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "text") : nullptr;
                if (cJSON_IsString(text)) {
                    cJSON* inner = cJSON_Parse(text->valuestring);
                    if (inner != nullptr) {
                        cJSON* rn = cJSON_GetObjectItem(inner, "role_name");
                        if (cJSON_IsString(rn)) {
                            dialog_role_ = rn->valuestring;
                        }
                        cJSON_Delete(inner);
                    }
                }
                practice_state_ = PracticeState::kDialog;
                practice_toast_.clear();
                dialog_deadline_us_ = esp_timer_get_time() + (int64_t)remaining_s_ * 1000000;
                RenderPractice();
            }
        }
    } else if (type == "practice") {
        std::string event = get_str("event");
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (event == "tick" && cJSON_IsObject(data)) {
            cJSON* rem = cJSON_GetObjectItem(data, "remaining_s");
            if (cJSON_IsNumber(rem)) {
                remaining_s_ = rem->valueint;
                dialog_deadline_us_ = esp_timer_get_time() + (int64_t)remaining_s_ * 1000000;
                if (practice_state_ == PracticeState::kDialog) {
                    RenderPractice();
                }
            }
        } else if (event == "report" && cJSON_IsObject(data)) {
            cJSON* score = cJSON_GetObjectItem(data, "overall_score");
            cJSON* fb = cJSON_GetObjectItem(data, "feedback");
            overall_score_ = cJSON_IsNumber(score) ? score->valuedouble : -1;
            if (cJSON_IsString(fb)) {
                feedback_text_ = fb->valuestring;
            }
            if (practice_state_ == PracticeState::kDialog ||
                practice_state_ == PracticeState::kEnding) {
                ShowScore();
            }
        }
    } else if (type == "stt") {
        std::string text = get_str("text");
        if (!text.empty()) {
            user_line_ = text;
            if (practice_state_ == PracticeState::kDialog) {
                RenderPractice();
            }
        }
    } else if (type == "tts") {
        if (get_str("state") == "sentence_start") {
            std::string text = get_str("text");
            if (!text.empty()) {
                role_line_ = text;
                if (practice_state_ == PracticeState::kDialog) {
                    RenderPractice();
                }
            }
        }
    }
    cJSON_Delete(root);
}
