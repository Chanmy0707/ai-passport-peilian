/* MVP 工牌模式 LVGL 界面实现（M3）。
 *
 * 只做表现：主页面 + 导航页 + 主菜单页 + 配网页 + 陪练页 + 设置页（均挂在 lv_layer_top，不改小智既有对话页）。
 * 主页面＝顶部状态栏（时间 / 日期 / Wi-Fi / 电量）＋ 大头像居中（静态表情 / 默认 😀）＋
 * 姓名、岗位·工号居中 ＋ 底部暗提示（按 OK 进入菜单）或录音状态区；
 * 导航页为独立整页（主菜单 / 设置 两行）；主菜单为独立整页（录音/智能陪练/小智对话/返回）。
 * 所有 LVGL 调用都在 DisplayLockGuard 内；状态栏与录音计时用独立 esp_timer 周期刷新，
 * 回调内先取显示锁再触碰 LVGL（与 lvgl_display 的 notification_timer_ 同一套路，
 * 避免与 LVGL 定时器的加锁顺序冲突）。
 */
#include "mvp_ui.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <material_symbols.h>
#include <wifi_manager.h>

#include "board.h"
#include "display.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "mvp_default_avatar.h"
#include "src/draw/lv_image_decoder.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace {

// 深色风格：黑底 / 白字 / 绿色强调 / 红色录音。
lv_color_t ClrBg()     { return lv_color_black(); }
lv_color_t ClrAccent() { return lv_color_make(0, 190, 95); }
lv_color_t ClrText()   { return lv_color_white(); }
lv_color_t ClrDim()    { return lv_color_make(125, 125, 125); }
lv_color_t ClrRec()    { return lv_color_make(225, 60, 50); }
lv_color_t ClrCard()   { return lv_color_make(24, 24, 30); }

const char* kNavLabels[2] = {
    "主菜单",
    "设置",
};

const char* kMenuLabels[4] = {
    "录音",
    "智能陪练",
    "小智对话",
    "返回",
};

bool MenuEnabled(int i) { return i >= 0 && i < 4; }

const char* kSettingsLabels[4] = {
    "设备信息",
    "音量调节",
    "重新配网",
    "恢复出厂",
};

// 取首个 UTF-8 字符，用于“姓名首字占位”头像。
void FirstUtf8Char(const std::string& s, char* out, size_t outsz) {
    out[0] = '\0';
    if (s.empty() || outsz == 0) {
        return;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t len = 1;
    if ((p[0] & 0x80u) == 0x00u) {
        len = 1;
    } else if ((p[0] & 0xE0u) == 0xC0u) {
        len = 2;
    } else if ((p[0] & 0xF0u) == 0xE0u) {
        len = 3;
    } else if ((p[0] & 0xF8u) == 0xF0u) {
        len = 4;
    }
    if (len >= outsz) {
        len = outsz - 1;
    }
    std::memcpy(out, s.data(), len);
    out[len] = '\0';
}

void FormatMMSS(uint32_t elapsed_ms, char* out, size_t outsz) {
    uint32_t total_sec = elapsed_ms / 1000u;
    std::snprintf(out, outsz, "%02u:%02u",
                  static_cast<unsigned>(total_sec / 60u),
                  static_cast<unsigned>(total_sec % 60u));
}

// 姓名下方一行：岗位 · 工号（任一为空时只显示另一段）。
std::string TitleLine(const BadgeProfile& p) {
    if (p.title.empty()) {
        return p.employee_no;
    }
    if (p.employee_no.empty()) {
        return p.title;
    }
    return p.title + " · " + p.employee_no;
}

// 当前活动文本字体（中文渲染依赖它；常规构建加载的是宽 CJK common 字体）。
const lv_font_t* ActiveFont() {
    const lv_font_t* font = nullptr;
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        Theme* theme = display->GetTheme();
        if (theme != nullptr) {
            auto tf = theme->GetTextFont();
            if (tf) {
                font = tf->font();
            }
        }
    }
    return font;
}

}  // namespace

struct MvpUi::Impl {
    int w = 240;
    int h = 320;
    const lv_font_t* font = nullptr;

    // 主页面（全屏覆盖层）：顶部状态栏 + 大头像居中 + 底部（暗提示 / 录音态）
    lv_obj_t* home_root = nullptr;
    lv_obj_t* time_label = nullptr;    // 顶栏时间 HH:MM（最左）
    lv_obj_t* date_label = nullptr;    // 顶栏日期 MM.DD
    lv_obj_t* wifi_label = nullptr;    // 顶栏 Wi-Fi 标志（icon 字体）
    lv_obj_t* battery_label = nullptr; // 顶栏电量图标（icon 字体，最右）
    lv_obj_t* home_hint = nullptr;     // 首页底部暗提示：按 OK 进入菜单

    // 导航页（独立整页）：主菜单 / 设置 两行列表
    lv_obj_t* nav_root = nullptr;
    lv_obj_t* nav_items[2] = {nullptr, nullptr};
    lv_obj_t* nav_labels[2] = {nullptr, nullptr};
    int nav_selected = 0;

    // 主菜单页（独立整页）：录音 / 智能陪练 / 小智对话 / 返回
    lv_obj_t* menu_root = nullptr;
    lv_obj_t* menu_items[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* menu_labels[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* menu_hint = nullptr;
    int menu_selected = 0;

    // 大头像 + 姓名/岗位·工号（居中）+ 底部录音状态（工牌录音态）
    lv_obj_t* avatar = nullptr;
    lv_obj_t* avatar_label = nullptr;
    lv_obj_t* avatar_img = nullptr;
    uint8_t* avatar_png = nullptr;              // 下载的 PNG 源字节（heap_caps_malloc，所有权在本类），须比 avatar_dsc 活得久
    size_t avatar_png_len = 0;
    lv_img_dsc_t avatar_dsc = {};               // data 指向 avatar_png 或内置 flash 常量
    lv_obj_t* name_label = nullptr;
    lv_obj_t* title_label = nullptr;
    lv_obj_t* record_box = nullptr;
    lv_obj_t* status_bar = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* badge_hint = nullptr;
    bool badge_visible = false;

    // 配网页（Wi-Fi 配网提示）
    lv_obj_t* prov_root = nullptr;
    lv_obj_t* prov_ssid_label = nullptr;
    lv_obj_t* prov_url_label = nullptr;
    lv_obj_t* prov_tip = nullptr;
    lv_obj_t* prov_exit_hint = nullptr;
    std::string prov_ssid;
    std::string prov_url;

    // 陪练页（内容随 PracticeView 整体重建）
    lv_obj_t* practice_root = nullptr;
    PracticeView practice_view;

    // 设置页（内容随 SettingsView 整体重建）
    lv_obj_t* settings_root = nullptr;
    SettingsView settings_view;

    esp_timer_handle_t tick = nullptr;
    std::function<void(bool&, uint32_t&)> provider;

    // 离线一次性提醒 toast（layer_top 顶层，3s 后自动隐藏）
    lv_obj_t* toast_label = nullptr;
    esp_timer_handle_t toast_timer = nullptr;

    void EnsureObjects();
    void ApplyNavStyle();
    void ApplyMenuStyle();
    void RefreshStatus();
    void RefreshTopbar();
    void RefreshProvisioning();
    void ApplyAvatarImage(uint8_t* data, size_t len);  // 取得 data 所有权（内部 heap_caps_free）
    bool SetAvatarPng(const void* png, size_t len);  // PNG 直引 + lodepng 解码显示
    void ShowDefaultAvatar();                        // 内置默认 😀（flash 常量）
    void BuildPractice();
    void BuildSettings();
    void StartTick();
    void StopTick();
    void Tick();
    void ShowOfflineToast();  // 调用方已持显示锁
    void HideToast();         // 定时器任务调用，内部自取显示锁

    static void TickCallback(void* arg) { static_cast<Impl*>(arg)->Tick(); }
    static void ToastCallback(void* arg) { static_cast<Impl*>(arg)->HideToast(); }

    lv_obj_t* MakeBox(lv_obj_t* parent, int x, int y, int width, int height);
    lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, int x, int y, int width);
    lv_obj_t* MakeCenteredLabel(lv_obj_t* parent, const char* text);
};

lv_obj_t* MvpUi::Impl::MakeBox(lv_obj_t* parent, int x, int y, int width, int height) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, width, height);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

lv_obj_t* MvpUi::Impl::MakeLabel(lv_obj_t* parent, const char* text, int x, int y, int width) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    if (font != nullptr) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, ClrText(), 0);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, width);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t* MvpUi::Impl::MakeCenteredLabel(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    if (font != nullptr) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, ClrText(), 0);
    lv_label_set_text(l, text);
    return l;
}

void MvpUi::Impl::EnsureObjects() {
    if (home_root != nullptr) {
        return;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        if (display->width() > 0) w = display->width();
        if (display->height() > 0) h = display->height();
    }
    font = ActiveFont();

    lv_obj_t* top = lv_layer_top();

    // ---- 主页面（全屏覆盖层）----
    home_root = lv_obj_create(top);
    lv_obj_remove_flag(home_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(home_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(home_root, 0, 0);
    lv_obj_set_size(home_root, w, h);
    lv_obj_set_style_bg_opa(home_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(home_root, ClrBg(), 0);
    lv_obj_set_style_border_width(home_root, 0, 0);
    lv_obj_set_style_radius(home_root, 0, 0);
    lv_obj_set_style_pad_all(home_root, 0, 0);

    // 顶部状态栏：时间 / 日期 / Wi-Fi / 电量（左→右，电量最右；《MVP设计文档.md》主页面）。
    // 各项为自适应宽度标签，布局用对齐关系排布（RefreshTopbar 设文本后重对齐，不写死坐标）。
    lv_obj_t* topbar = MakeBox(home_root, 0, 0, w, 30);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_TRANSP, 0);
    time_label = lv_label_create(topbar);
    if (font != nullptr) {
        lv_obj_set_style_text_font(time_label, font, 0);
    }
    lv_obj_set_style_text_color(time_label, ClrText(), 0);
    lv_label_set_text(time_label, "--:--");
    date_label = lv_label_create(topbar);
    if (font != nullptr) {
        lv_obj_set_style_text_font(date_label, font, 0);
    }
    lv_obj_set_style_text_color(date_label, ClrText(), 0);
    lv_label_set_text(date_label, "--.--");
    wifi_label = lv_label_create(topbar);
    lv_obj_set_style_text_color(wifi_label, ClrText(), 0);
    lv_label_set_text(wifi_label, "");
    battery_label = lv_label_create(topbar);
    lv_obj_set_style_text_color(battery_label, ClrText(), 0);
    lv_label_set_text(battery_label, "");

    // ---- 大头像居中 + 姓名 / 岗位·工号居中（方案C：首页纯工牌）----
    avatar = MakeBox(home_root, (w - 88) / 2, 36, 88, 88);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(avatar, ClrAccent(), 0);
    lv_obj_set_flex_flow(avatar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(avatar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 预制工牌信息先行显示（云端配置到位后覆盖） */
    const BadgeProfile preset = PresetBadgeProfile();
    char first[8];
    FirstUtf8Char(preset.name, first, sizeof(first));
    avatar_label = MakeCenteredLabel(avatar, first);
    lv_obj_set_style_text_color(avatar_label, ClrBg(), 0);
    /* 初始化表情头像：内置默认 😀（flash 常量）先行显示，云端头像到位后覆盖 */
    ShowDefaultAvatar();

    name_label = MakeLabel(home_root, preset.name.c_str(), 12, 130, w - 24);
    title_label = MakeLabel(home_root, TitleLine(preset).c_str(), 12, 166, w - 24);
    lv_obj_set_style_text_color(title_label, ClrDim(), 0);

    // ---- 首页底部暗提示：按 OK 进入菜单（工牌录音态显示录音状态区，两者互斥）----
    home_hint = MakeLabel(home_root, "按 OK 进入菜单", 0, h - 32, w);
    lv_obj_set_style_text_color(home_hint, ClrDim(), 0);

    // ---- 导航页（独立整页）：主菜单 / 设置 两行列表（UP/DOWN 选择、OK 进入、长按 OK 回首页）----
    nav_root = lv_obj_create(top);
    lv_obj_remove_flag(nav_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(nav_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(nav_root, 0, 0);
    lv_obj_set_size(nav_root, w, h);
    lv_obj_set_style_bg_opa(nav_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(nav_root, ClrBg(), 0);
    lv_obj_set_style_border_width(nav_root, 0, 0);
    lv_obj_set_style_radius(nav_root, 0, 0);
    lv_obj_set_style_pad_all(nav_root, 0, 0);
    lv_obj_add_flag(nav_root, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) {
        nav_items[i] = MakeBox(nav_root, 20, 88 + i * 60, w - 40, 48);
        lv_obj_set_flex_flow(nav_items[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(nav_items[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        nav_labels[i] = MakeCenteredLabel(nav_items[i], kNavLabels[i]);
    }
    lv_obj_t* nav_hint = MakeLabel(nav_root, "OK 进入  长按返回", 0, h - 32, w);
    lv_obj_set_style_text_color(nav_hint, ClrDim(), 0);

    // ---- 主菜单页（独立整页，照陪练/设置页范式）：录音 / 智能陪练 / 小智对话 / 返回 ----
    menu_root = lv_obj_create(top);
    lv_obj_remove_flag(menu_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(menu_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(menu_root, 0, 0);
    lv_obj_set_size(menu_root, w, h);
    lv_obj_set_style_bg_opa(menu_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(menu_root, ClrBg(), 0);
    lv_obj_set_style_border_width(menu_root, 0, 0);
    lv_obj_set_style_radius(menu_root, 0, 0);
    lv_obj_set_style_pad_all(menu_root, 0, 0);
    lv_obj_add_flag(menu_root, LV_OBJ_FLAG_HIDDEN);

    MakeLabel(menu_root, "主菜单", 0, 18, w);
    for (int i = 0; i < 4; i++) {
        menu_items[i] = MakeBox(menu_root, 20, 56 + i * 50, w - 40, 44);
        lv_obj_set_flex_flow(menu_items[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(menu_items[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        menu_labels[i] = MakeCenteredLabel(menu_items[i], kMenuLabels[i]);
    }
    menu_hint = MakeLabel(menu_root, "OK 进入  长按对话", 0, h - 32, w);
    lv_obj_set_style_text_color(menu_hint, ClrDim(), 0);

    // ---- 底部录音状态（工牌录音态）----
    record_box = MakeBox(home_root, 0, h - 128, w, 128);
    lv_obj_set_style_bg_opa(record_box, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(record_box, LV_OBJ_FLAG_HIDDEN);
    status_bar = MakeBox(record_box, 16, 8, w - 32, 56);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, ClrCard(), 0);
    status_label = MakeCenteredLabel(status_bar, "未录音");
    lv_obj_set_style_text_color(status_label, ClrAccent(), 0);

    badge_hint = MakeLabel(record_box, "OK 录音  长按主页", 0, 104, w);
    lv_obj_set_style_text_color(badge_hint, ClrDim(), 0);

    // ---- 陪练页（内容由 BuildPractice 动态重建）----
    practice_root = lv_obj_create(top);
    lv_obj_remove_flag(practice_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(practice_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(practice_root, 0, 0);
    lv_obj_set_size(practice_root, w, h);
    lv_obj_set_style_bg_opa(practice_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(practice_root, ClrBg(), 0);
    lv_obj_set_style_border_width(practice_root, 0, 0);
    lv_obj_set_style_radius(practice_root, 0, 0);
    lv_obj_set_style_pad_all(practice_root, 0, 0);
    lv_obj_add_flag(practice_root, LV_OBJ_FLAG_HIDDEN);

    // ---- 设置页（内容由 BuildSettings 动态重建）----
    settings_root = lv_obj_create(top);
    lv_obj_remove_flag(settings_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(settings_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(settings_root, 0, 0);
    lv_obj_set_size(settings_root, w, h);
    lv_obj_set_style_bg_opa(settings_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(settings_root, ClrBg(), 0);
    lv_obj_set_style_border_width(settings_root, 0, 0);
    lv_obj_set_style_radius(settings_root, 0, 0);
    lv_obj_set_style_pad_all(settings_root, 0, 0);
    lv_obj_add_flag(settings_root, LV_OBJ_FLAG_HIDDEN);

    // ---- 配网页（Wi-Fi 配网提示）----
    prov_root = lv_obj_create(top);
    lv_obj_remove_flag(prov_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(prov_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(prov_root, 0, 0);
    lv_obj_set_size(prov_root, w, h);
    lv_obj_set_style_bg_opa(prov_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(prov_root, ClrBg(), 0);
    lv_obj_set_style_border_width(prov_root, 0, 0);
    lv_obj_set_style_radius(prov_root, 0, 0);
    lv_obj_set_style_pad_all(prov_root, 0, 0);
    lv_obj_add_flag(prov_root, LV_OBJ_FLAG_HIDDEN);

    MakeLabel(prov_root, "Wi-Fi 配网", 0, 18, w);
    lv_obj_t* prov_step1 = MakeLabel(prov_root, "手机连接热点", 16, 64, w - 32);
    lv_obj_set_style_text_align(prov_step1, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(prov_step1, ClrDim(), 0);
    prov_ssid_label = MakeLabel(prov_root, "", 16, 92, w - 32);
    lv_obj_set_style_text_color(prov_ssid_label, ClrAccent(), 0);
    lv_obj_t* prov_step2 = MakeLabel(prov_root, "浏览器打开配置页", 16, 140, w - 32);
    lv_obj_set_style_text_align(prov_step2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(prov_step2, ClrDim(), 0);
    prov_url_label = MakeLabel(prov_root, "", 16, 168, w - 32);
    lv_obj_set_style_text_color(prov_url_label, ClrAccent(), 0);
    prov_tip = MakeLabel(prov_root, "配网成功后自动进入主页面", 0, 232, w);
    lv_obj_set_style_text_color(prov_tip, ClrDim(), 0);
    prov_exit_hint = MakeLabel(prov_root, "长按 OK 返回设置", 0, h - 32, w);
    lv_obj_set_style_text_color(prov_exit_hint, ClrDim(), 0);

    // ---- 离线一次性提醒 toast（最后创建保证层级最前，显示时再 move_foreground）----
    toast_label = MakeLabel(top, "未联网，可在设置重新配网", 16, h / 2 - 24, w - 32);
    lv_obj_set_style_bg_opa(toast_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(toast_label, ClrCard(), 0);
    lv_obj_set_style_radius(toast_label, 12, 0);
    lv_obj_set_style_pad_all(toast_label, 12, 0);
    lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);

    ApplyNavStyle();
    ApplyMenuStyle();
}

void MvpUi::Impl::ApplyMenuStyle() {
    for (int i = 0; i < 4; i++) {
        if (menu_items[i] == nullptr) {
            continue;
        }
        bool sel = (i == menu_selected);
        bool en = MenuEnabled(i);
        if (!en) {
            lv_obj_set_style_bg_opa(menu_items[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(menu_labels[i], ClrDim(), 0);
        } else if (sel) {
            lv_obj_set_style_bg_opa(menu_items[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(menu_items[i], ClrAccent(), 0);
            lv_obj_set_style_text_color(menu_labels[i], ClrText(), 0);
        } else {
            lv_obj_set_style_bg_opa(menu_items[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(menu_items[i], ClrCard(), 0);
            lv_obj_set_style_text_color(menu_labels[i], ClrText(), 0);
        }
    }
}

// 导航页选项样式：选中项绿色高亮（文字 ≤4 字、不加前缀标记，防止按钮溢出）。
void MvpUi::Impl::ApplyNavStyle() {
    for (int i = 0; i < 2; i++) {
        if (nav_items[i] == nullptr || nav_labels[i] == nullptr) {
            continue;
        }
        bool sel = (i == nav_selected);
        lv_obj_set_style_bg_opa(nav_items[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(nav_items[i], sel ? ClrAccent() : ClrCard(), 0);
        lv_obj_set_style_text_color(nav_labels[i], sel ? ClrBg() : ClrText(), 0);
    }
}

// 陪练页内容整体重建（《智能陪练详细设计.md》§1.2 三页面布局）。
void MvpUi::Impl::BuildPractice() {
    if (practice_root == nullptr) {
        return;
    }
    lv_obj_clean(practice_root);
    const PracticeView& v = practice_view;

    if (v.page == PracticeView::Page::kScenarios) {
        MakeLabel(practice_root, "智能陪练 · 选择场景", 0, 18, w);
        if (v.scenario_items.empty()) {
            MakeLabel(practice_root, "场景加载中…", 0, 130, w);
        }
        size_t count = v.scenario_items.size() < 4 ? v.scenario_items.size() : 4;
        for (size_t i = 0; i < count; i++) {
            int idx = static_cast<int>(i);
            lv_obj_t* row = MakeBox(practice_root, 20, 56 + idx * 50, w - 40, 44);
            bool sel = (idx == v.selected);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, sel ? ClrAccent() : ClrCard(), 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_t* label = MakeCenteredLabel(row, v.scenario_items[i].c_str());
            if (sel) {
                lv_obj_set_style_text_color(label, ClrBg(), 0);
            }
        }
        if (!v.scenario_detail.empty()) {
            lv_obj_t* detail =
                MakeLabel(practice_root, v.scenario_detail.c_str(), 20, 56 + count * 50 + 2, w - 40);
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);  // 超长省略号，不挤占提示行
            lv_obj_set_style_text_color(detail, ClrDim(), 0);
        }
        MakeLabel(practice_root, "OK 开始陪练  长按返回", 0, h - 32, w);
    } else if (v.page == PracticeView::Page::kDialog) {
        std::string title = "陪练 · " + v.dialog_title;
        MakeLabel(practice_root, title.c_str(), 0, 14, w);
        std::string head = v.countdown + "  ·  " + v.status;
        lv_obj_t* head_label = MakeLabel(practice_root, head.c_str(), 0, 48, w);
        lv_obj_set_style_text_color(head_label, ClrAccent(), 0);
        lv_obj_t* user = MakeLabel(practice_root, ("你说：" + v.user_line).c_str(), 16, 88, w - 32);
        lv_obj_set_style_text_align(user, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_t* role = MakeLabel(practice_root, ("角色：" + v.role_line).c_str(), 16, 158, w - 32);
        lv_obj_set_style_text_align(role, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(role, ClrAccent(), 0);
        MakeLabel(practice_root, "OK 说话/打断  长按结束", 0, h - 32, w);
    } else {  // kScore
        MakeLabel(practice_root, "陪练评分", 0, 24, w);
        lv_obj_t* score_label = MakeLabel(practice_root, v.score.c_str(), 0, 62, w);
        lv_obj_set_style_text_color(score_label, ClrAccent(), 0);
        lv_obj_t* fb = MakeLabel(practice_root, v.feedback.c_str(), 16, 110, w - 32);
        lv_obj_set_style_text_align(fb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(fb, ClrDim(), 0);
        MakeLabel(practice_root, "OK 返回", 0, h - 32, w);
    }

    if (!v.toast.empty()) {
        lv_obj_t* toast = MakeLabel(practice_root, v.toast.c_str(), 12, h - 64, w - 24);
        lv_obj_set_style_text_color(toast, ClrRec(), 0);
    }
}

// 设置页内容整体重建（设置菜单 + 设备信息/音量/恢复出厂确认子页）。
void MvpUi::Impl::BuildSettings() {
    if (settings_root == nullptr) {
        return;
    }
    lv_obj_clean(settings_root);
    const SettingsView& v = settings_view;

    if (v.page == SettingsView::Page::kMenu) {
        MakeLabel(settings_root, "设置", 0, 18, w);
        for (int i = 0; i < 4; i++) {
            lv_obj_t* row = MakeBox(settings_root, 20, 56 + i * 50, w - 40, 44);
            bool sel = (i == v.selected);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, sel ? ClrAccent() : ClrCard(), 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_t* label = MakeCenteredLabel(row, kSettingsLabels[i]);
            if (sel) {
                lv_obj_set_style_text_color(label, ClrBg(), 0);
            }
        }
        MakeLabel(settings_root, "OK 进入  长按返回", 0, h - 32, w);
    } else if (v.page == SettingsView::Page::kDeviceInfo) {
        MakeLabel(settings_root, "设备信息", 0, 18, w);
        int y = 60;
        for (const auto& line : v.info_lines) {
            lv_obj_t* l = MakeLabel(settings_root, line.c_str(), 16, y, w - 32);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
            y += 36;
        }
        MakeLabel(settings_root, "OK 返回", 0, h - 32, w);
    } else if (v.page == SettingsView::Page::kVolume) {
        MakeLabel(settings_root, "音量调节", 0, 18, w);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", v.volume);
        lv_obj_t* num = MakeLabel(settings_root, buf, 0, 70, w);
        lv_obj_set_style_text_color(num, ClrAccent(), 0);
        lv_obj_t* track = MakeBox(settings_root, 20, 150, w - 40, 16);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(track, ClrCard(), 0);
        lv_obj_set_style_radius(track, 8, 0);
        int fill = (w - 40) * v.volume / 100;
        if (fill > 0) {
            lv_obj_t* bar = MakeBox(settings_root, 20, 150, fill, 16);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(bar, ClrAccent(), 0);
            lv_obj_set_style_radius(bar, 8, 0);
        }
        MakeLabel(settings_root, "UP/DOWN 调节    OK 返回", 0, h - 32, w);
    } else {  // kConfirmReset
        MakeLabel(settings_root, "恢复出厂设置", 0, 30, w);
        lv_obj_t* warn =
            MakeLabel(settings_root, "将清除全部配置（Wi-Fi、绑定等）并重启，确定继续？", 16, 100, w - 32);
        lv_obj_set_style_text_color(warn, ClrRec(), 0);
        MakeLabel(settings_root, "OK 确认恢复    长按取消", 0, h - 32, w);
    }
}

void MvpUi::Impl::RefreshStatus() {
    if (!badge_visible || status_label == nullptr) {
        return;
    }
    bool recording = false;
    uint32_t elapsed = 0;
    if (provider) {
        provider(recording, elapsed);
    }
    if (recording) {
        char t[12];
        FormatMMSS(elapsed, t, sizeof(t));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "录音中  %s", t);
        lv_label_set_text(status_label, buf);
        lv_obj_set_style_text_color(status_label, ClrText(), 0);
        if (status_bar != nullptr) {
            lv_obj_set_style_bg_color(status_bar, ClrRec(), 0);
        }
        if (badge_hint != nullptr) {
            lv_label_set_text(badge_hint, "OK 停止  长按主页");
        }
    } else {
        lv_label_set_text(status_label, "未录音");
        lv_obj_set_style_text_color(status_label, ClrText(), 0);
        if (status_bar != nullptr) {
            lv_obj_set_style_bg_color(status_bar, ClrCard(), 0);
        }
        if (badge_hint != nullptr) {
            lv_label_set_text(badge_hint, "OK 录音  长按主页");
        }
    }
}

void MvpUi::Impl::StartTick() {
    if (tick == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &MvpUi::Impl::TickCallback;
        args.arg = this;
        args.name = "mvp_ui_tick";
        if (esp_timer_create(&args, &tick) != ESP_OK) {
            tick = nullptr;
            return;
        }
    }
    esp_timer_stop(tick);
    esp_timer_start_periodic(tick, 1000000);  // 1s
}

void MvpUi::Impl::StopTick() {
    if (tick != nullptr) {
        esp_timer_stop(tick);
    }
}

void MvpUi::Impl::Tick() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    RefreshTopbar();
    RefreshStatus();
}

MvpUi& MvpUi::GetInstance() {
    static MvpUi instance;
    return instance;
}

void MvpUi::Init() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
}

void MvpUi::ShowHome() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->prov_root != nullptr) {
        lv_obj_add_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
    // 首页态：纯工牌展示（大头像居中 + 底部暗提示），隐藏导航页/主菜单页/录音区
    if (impl_->nav_root != nullptr) {
        lv_obj_add_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->home_hint != nullptr) {
        lv_obj_remove_flag(impl_->home_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->record_box != nullptr) {
        lv_obj_add_flag(impl_->record_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->home_root != nullptr) {
        lv_obj_remove_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    impl_->RefreshTopbar();
    impl_->StartTick();
}

void MvpUi::ShowNav(int selected) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    impl_->nav_selected = (selected < 0 || selected > 1) ? 0 : selected;
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->prov_root != nullptr) {
        lv_obj_add_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
    // 导航页（独立整页）：隐藏主页面与主菜单页
    if (impl_->home_root != nullptr) {
        lv_obj_add_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->nav_root != nullptr) {
        lv_obj_remove_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    impl_->ApplyNavStyle();
    impl_->StopTick();
}

void MvpUi::SetNavSelected(int selected) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    impl_->nav_selected = (selected < 0 || selected > 1) ? 0 : selected;
    impl_->ApplyNavStyle();
}

void MvpUi::ShowMenu(int selected) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    impl_->menu_selected = (selected < 0 || selected > 3) ? 0 : selected;
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->prov_root != nullptr) {
        lv_obj_add_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
    // 主菜单页（独立整页）：隐藏主页面与导航页，显示主菜单页
    if (impl_->home_root != nullptr) {
        lv_obj_add_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->nav_root != nullptr) {
        lv_obj_add_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_remove_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    impl_->ApplyMenuStyle();
    impl_->StopTick();
}

void MvpUi::SetMenuSelected(int selected) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    impl_->menu_selected = (selected < 0 || selected > 3) ? 0 : selected;
    impl_->ApplyMenuStyle();
}

void MvpUi::ShowBadge() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->prov_root != nullptr) {
        lv_obj_add_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
    // 工牌录音态：底部显示录音状态区，隐藏导航页/主菜单页/首页暗提示
    if (impl_->nav_root != nullptr) {
        lv_obj_add_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->home_hint != nullptr) {
        lv_obj_add_flag(impl_->home_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->record_box != nullptr) {
        lv_obj_remove_flag(impl_->record_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->home_root != nullptr) {
        lv_obj_remove_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = true;
    impl_->StartTick();
    impl_->RefreshTopbar();
    impl_->RefreshStatus();
}

void MvpUi::ShowChat() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        return;
    }
    impl_->StopTick();
    impl_->badge_visible = false;
    if (impl_->home_root != nullptr) {
        lv_obj_add_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->nav_root != nullptr) {
        lv_obj_add_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void MvpUi::SetBadgeProfile(const BadgeProfile& profile) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();

    /* 无有效姓名回落出厂预制信息（默认 😀、Jack/销售顾问/No.1024） */
    const BadgeProfile p = (profile.valid && !profile.name.empty()) ? profile : PresetBadgeProfile();
    if (impl_->avatar_label != nullptr) {
        char first[8];
        FirstUtf8Char(p.name, first, sizeof(first));
        lv_label_set_text(impl_->avatar_label, first);
    }
    if (impl_->name_label != nullptr) {
        lv_label_set_text(impl_->name_label, p.name.c_str());
    }
    if (impl_->title_label != nullptr) {
        lv_label_set_text(impl_->title_label, TitleLine(p).c_str());
    }
}

/* 头像 PNG 直引显示（须在 DisplayLockGuard 内调用）：lv_img_dsc_t 的 data 直指源字节
 * （下载缓冲或内置 flash 常量），由 LVGL lodepng 解码；源字节更换前作废解码缓存。 */
bool MvpUi::Impl::SetAvatarPng(const void* png, size_t len) {
    lv_image_cache_drop(&avatar_dsc);
    avatar_dsc = lv_img_dsc_t{};
    avatar_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    avatar_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    avatar_dsc.data_size = len;
    avatar_dsc.data = static_cast<const uint8_t*>(png);
    lv_image_header_t info;
    if (lv_image_decoder_get_info(&avatar_dsc, &info) != LV_RESULT_OK) {
        return false;
    }
    if (avatar_img == nullptr) {
        avatar_img = lv_image_create(avatar);
        lv_obj_set_size(avatar_img, 64, 64);  // PNG 原生 64px，居中于 88px 头像圆
        lv_obj_center(avatar_img);
    }
    lv_image_set_src(avatar_img, &avatar_dsc);
    lv_obj_clear_flag(avatar_img, LV_OBJ_FLAG_HIDDEN);
    if (avatar_label != nullptr) {
        lv_obj_add_flag(avatar_label, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

/* 内置默认 😀（grinning_face 静态 PNG，flash 常量）；内置图异常时回落姓名首字占位。 */
void MvpUi::Impl::ShowDefaultAvatar() {
    uint8_t* old = avatar_png;
    avatar_png = nullptr;
    avatar_png_len = 0;
    bool ok = SetAvatarPng(kMvpDefaultAvatarPng, kMvpDefaultAvatarPngSize);
    heap_caps_free(old); /* SetAvatarPng 已丢弃旧解码缓存，旧下载图源可安全释放 */
    if (ok) {
        return;
    }
    ESP_LOGW("mvp_ui", "内置默认头像显示失败，回落首字占位");
    if (avatar_img != nullptr) {
        lv_obj_add_flag(avatar_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (avatar_label != nullptr) {
        lv_obj_clear_flag(avatar_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 头像静态图应用（须在 DisplayLockGuard 内调用）：PNG 源字节存 Impl::avatar_png
 * （heap_caps_malloc 块，比 avatar_dsc 活得久）；空数据/解析失败释放入参并回落默认 😀 头像。 */
void MvpUi::Impl::ApplyAvatarImage(uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        heap_caps_free(data);
        ShowDefaultAvatar();
        return;
    }
    uint8_t* old = avatar_png;
    avatar_png = data;
    avatar_png_len = len;
    bool ok = SetAvatarPng(avatar_png, avatar_png_len);
    if (!ok) {
        ESP_LOGW("mvp_ui", "头像 PNG 解析失败 len=%u，回落默认头像", (unsigned)avatar_png_len);
        avatar_png = nullptr;
        avatar_png_len = 0;
    }
    /* SetAvatarPng 已丢弃旧解码缓存，旧下载图源此时才可安全释放 */
    if (old != nullptr && old != data) {
        heap_caps_free(old);
    }
    if (!ok) {
        heap_caps_free(data);
        ShowDefaultAvatar();
    }
}

void MvpUi::SetAvatarImage(uint8_t* data, size_t len) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        heap_caps_free(data);
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        heap_caps_free(data);
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    impl_->ApplyAvatarImage(data, len);
}

void MvpUi::ClearAvatarImage() {
    SetAvatarImage(nullptr, 0);
}

void MvpUi::SetRecordStatusProvider(std::function<void(bool&, uint32_t&)> provider) {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->provider = std::move(provider);
}

void MvpUi::Update() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        return;
    }
    impl_->RefreshTopbar();
    impl_->RefreshStatus();
}

void MvpUi::ShowPractice(const PracticeView& view) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    if (impl_->home_root != nullptr) {
        lv_obj_add_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->nav_root != nullptr) {
        lv_obj_add_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    impl_->StopTick();
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->prov_root != nullptr) {
        lv_obj_add_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->practice_view = view;
    impl_->BuildPractice();
    if (impl_->practice_root != nullptr) {
        lv_obj_remove_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void MvpUi::HidePractice() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ != nullptr && impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void MvpUi::ShowSettings(const SettingsView& view) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    if (impl_->home_root != nullptr) {
        lv_obj_add_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->nav_root != nullptr) {
        lv_obj_add_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->prov_root != nullptr) {
        lv_obj_add_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->StopTick();
    impl_->settings_view = view;
    impl_->BuildSettings();
    if (impl_->settings_root != nullptr) {
        lv_obj_remove_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void MvpUi::HideSettings() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ != nullptr && impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void MvpUi::ShowProvisioning(const std::string& ap_ssid, const std::string& ap_url, ProvOrigin origin) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    impl_->prov_ssid = ap_ssid;
    impl_->prov_url = ap_url;
    // 配网页全屏覆盖其余页面
    if (impl_->home_root != nullptr) {
        lv_obj_add_flag(impl_->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->nav_root != nullptr) {
        lv_obj_add_flag(impl_->nav_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    impl_->StopTick();
    // 提示文案随来源变化：kSettings 取消回设置，其余回主页面；kFeature 补充“未联网”引导
    if (impl_->prov_tip != nullptr) {
        lv_label_set_text(impl_->prov_tip,
                          origin == ProvOrigin::kFeature ? "未联网，请先配网使用" : "配网成功后自动进入主页面");
    }
    if (impl_->prov_exit_hint != nullptr) {
        lv_label_set_text(impl_->prov_exit_hint,
                          origin == ProvOrigin::kSettings ? "长按 OK 返回设置" : "长按 OK 返回主页");
        lv_obj_remove_flag(impl_->prov_exit_hint, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->RefreshProvisioning();
    if (impl_->prov_root != nullptr) {
        lv_obj_remove_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void MvpUi::SetProvisioningInfo(const std::string& ap_ssid, const std::string& ap_url) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        return;
    }
    impl_->prov_ssid = ap_ssid;
    impl_->prov_url = ap_url;
    impl_->RefreshProvisioning();
}

void MvpUi::HideProvisioning() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ != nullptr && impl_->prov_root != nullptr) {
        lv_obj_add_flag(impl_->prov_root, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 离线一次性提醒：toast 覆盖在当前页面顶层，3 秒后自动隐藏。 */
void MvpUi::ShowOfflineNotice() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->EnsureObjects();
    impl_->ShowOfflineToast();
}

void MvpUi::Impl::ShowOfflineToast() {
    if (toast_label == nullptr) {
        return;
    }
    lv_label_set_text(toast_label, "未联网，可在设置重新配网");
    lv_obj_move_foreground(toast_label);
    lv_obj_remove_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    if (toast_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &MvpUi::Impl::ToastCallback;
        args.arg = this;
        args.name = "mvp_ui_toast";
        if (esp_timer_create(&args, &toast_timer) != ESP_OK) {
            toast_timer = nullptr;
            return;
        }
    }
    esp_timer_stop(toast_timer);
    esp_timer_start_once(toast_timer, 3000000);  // 3s
}

void MvpUi::Impl::HideToast() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (toast_label != nullptr) {
        lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 配网页内容回填：AP 未起来时显示“正在开启配网热点…”，起来后显示热点名 + 配网地址。 */
void MvpUi::Impl::RefreshProvisioning() {
    if (prov_ssid_label == nullptr) {
        return;
    }
    if (prov_ssid.empty()) {
        lv_label_set_text(prov_ssid_label, "正在开启配网热点…");
        lv_obj_set_style_text_color(prov_ssid_label, ClrDim(), 0);
        lv_label_set_text(prov_url_label, "");
    } else {
        lv_label_set_text(prov_ssid_label, prov_ssid.c_str());
        lv_obj_set_style_text_color(prov_ssid_label, ClrAccent(), 0);
        lv_label_set_text(prov_url_label, prov_url.c_str());
    }
}

/* 顶部状态栏刷新：时间 HH:MM + 日期 MM.DD + Wi-Fi 标志 + 电量图标（左→右，电量最右）。
 * 各项自适应宽度，设文本后按对齐关系重排（时间最左、电量最右，未同步显示 --:-- / --.--）。 */
void MvpUi::Impl::RefreshTopbar() {
    if (time_label == nullptr) {
        return;
    }
    const lv_font_t* icon_font = nullptr;
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        auto* theme = static_cast<LvglTheme*>(display->GetTheme());
        if (theme != nullptr) {
            auto tf = theme->icon_font();
            if (tf) {
                icon_font = tf->font();
            }
        }
    }

    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    char tbuf[32];
    char dbuf[32];
    if (tm_now.tm_year + 1900 >= 2025) {  // 系统时间已同步（OTA server_time）
        std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        std::snprintf(dbuf, sizeof(dbuf), "%02d.%02d", tm_now.tm_mon + 1, tm_now.tm_mday);
    } else {
        std::snprintf(tbuf, sizeof(tbuf), "--:--");
        std::snprintf(dbuf, sizeof(dbuf), "--.--");
    }
    lv_label_set_text(time_label, tbuf);
    if (date_label != nullptr) {
        lv_label_set_text(date_label, dbuf);
    }

    if (wifi_label != nullptr) {
        const char* icon = Board::GetInstance().GetNetworkStateIcon();
        if (icon_font != nullptr && icon != nullptr && icon[0] != '\0') {
            lv_obj_set_style_text_font(wifi_label, icon_font, 0);
            lv_label_set_text(wifi_label, icon);
            lv_obj_set_style_text_color(wifi_label,
                                        WifiManager::GetInstance().IsConnected() ? ClrAccent() : ClrDim(),
                                        0);
        } else {
            lv_label_set_text(wifi_label, "");
        }
    }

    if (battery_label != nullptr) {
        const char* icon = nullptr;
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            if (charging) {
                icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
            } else {
                static const char* levels[] = {
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_0, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL};
                int idx = level <= 0 ? 0 : (level >= 100 ? 7 : 1 + ((level - 1) * 6 / 99));
                icon = levels[idx];
            }
        }
        if (icon_font != nullptr && icon != nullptr) {
            lv_obj_set_style_text_font(battery_label, icon_font, 0);
            lv_label_set_text(battery_label, icon);
        } else {
            lv_label_set_text(battery_label, "");
        }
    }

    // 四项对齐排布：时间（最左）→ 日期 → Wi-Fi → 电量（最右）
    lv_obj_align(time_label, LV_ALIGN_LEFT_MID, 8, 0);
    if (date_label != nullptr) {
        lv_obj_align_to(date_label, time_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    }
    if (wifi_label != nullptr && battery_label != nullptr) {
        lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_align_to(wifi_label, battery_label, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    }
}
