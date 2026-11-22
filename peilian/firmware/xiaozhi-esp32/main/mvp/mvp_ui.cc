/* MVP 工牌模式 LVGL 界面实现（M3）。
 *
 * 只做表现：主菜单覆盖层 + 工牌页覆盖层（均挂在 lv_layer_top，不改小智既有对话页）。
 * 所有 LVGL 调用都在 DisplayLockGuard 内；录音计时用独立 esp_timer 周期刷新，
 * 回调内先取显示锁再触碰 LVGL（与 lvgl_display 的 notification_timer_ 同一套路，
 * 避免与 LVGL 定时器的加锁顺序冲突）。
 */
#include "mvp_ui.h"

#include "mvp_config.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "board.h"
#include "display.h"
#include "gif/lvgl_gif.h"
#include "lvgl_font.h"

namespace {

// 深色风格：黑底 / 白字 / 绿色强调 / 红色录音。
lv_color_t ClrBg()     { return lv_color_black(); }
lv_color_t ClrAccent() { return lv_color_make(0, 190, 95); }
lv_color_t ClrText()   { return lv_color_white(); }
lv_color_t ClrDim()    { return lv_color_make(125, 125, 125); }
lv_color_t ClrRec()    { return lv_color_make(225, 60, 50); }
lv_color_t ClrCard()   { return lv_color_make(24, 24, 30); }

const char* kMenuLabels[4] = {
    "工牌",
    "智能陪练",
    "日常对话",
    "设置",
};

bool MenuEnabled(int i) { return i >= 0 && i < 4; }

const char* kSettingsLabels[4] = {
    "设备信息",
    "音量调节",
    "Wi-Fi 重新配网",
    "恢复出厂设置",
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

    // 主菜单
    lv_obj_t* menu_root = nullptr;
    lv_obj_t* menu_items[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* menu_labels[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* menu_hint = nullptr;
    int menu_selected = 0;

    // 工牌页
    lv_obj_t* badge_root = nullptr;
    lv_obj_t* card = nullptr;
    lv_obj_t* avatar = nullptr;
    lv_obj_t* avatar_label = nullptr;
    lv_obj_t* avatar_img = nullptr;
    std::vector<uint8_t> avatar_gif_data;   // GIF 源字节，须比 avatar_gif 活得久
    lv_img_dsc_t avatar_gif_dsc = {};       // 仅构造 LvglGif 时用（data 指向上面 vector）
    std::unique_ptr<LvglGif> avatar_gif;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* title_label = nullptr;
    lv_obj_t* emp_label = nullptr;
    lv_obj_t* qr_label = nullptr;
    lv_obj_t* status_bar = nullptr;
    lv_obj_t* status_label = nullptr;
    bool badge_visible = false;

    // 陪练页（内容随 PracticeView 整体重建）
    lv_obj_t* practice_root = nullptr;
    PracticeView practice_view;

    // 设置页（内容随 SettingsView 整体重建）
    lv_obj_t* settings_root = nullptr;
    SettingsView settings_view;

    esp_timer_handle_t tick = nullptr;
    std::function<void(bool&, uint32_t&)> provider;

    void EnsureObjects();
    void ApplyMenuStyle();
    void RefreshStatus();
    void ApplyAvatarGif(std::vector<uint8_t>&& data);
    void BuildPractice();
    void BuildSettings();
    void StartTick();
    void StopTick();
    void Tick();

    static void TickCallback(void* arg) { static_cast<Impl*>(arg)->Tick(); }

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
    if (menu_root != nullptr) {
        return;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        if (display->width() > 0) w = display->width();
        if (display->height() > 0) h = display->height();
    }
    font = ActiveFont();

    lv_obj_t* top = lv_layer_top();

    // ---- 主菜单 ----
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

    MakeLabel(menu_root, "主菜单", 0, 18, w);
    for (int i = 0; i < 4; i++) {
        menu_items[i] = MakeBox(menu_root, 24, 72 + i * 54, w - 48, 48);
        lv_obj_set_flex_flow(menu_items[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(menu_items[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        menu_labels[i] = MakeCenteredLabel(menu_items[i], kMenuLabels[i]);
    }
    menu_hint = MakeLabel(menu_root, "OK 进入    长按返回", 0, h - 36, w);
    lv_obj_set_style_text_color(menu_hint, ClrDim(), 0);

    // ---- 工牌页 ----
    badge_root = lv_obj_create(top);
    lv_obj_remove_flag(badge_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(badge_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(badge_root, 0, 0);
    lv_obj_set_size(badge_root, w, h);
    lv_obj_set_style_bg_opa(badge_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge_root, ClrBg(), 0);
    lv_obj_set_style_border_width(badge_root, 0, 0);
    lv_obj_set_style_radius(badge_root, 0, 0);
    lv_obj_set_style_pad_all(badge_root, 0, 0);

    card = MakeBox(badge_root, 12, 20, w - 24, 178);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, ClrCard(), 0);
    lv_obj_set_style_radius(card, 16, 0);

    int card_w = w - 24;
    avatar = MakeBox(card, (card_w - 64) / 2, 14, 64, 64);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(avatar, ClrAccent(), 0);
    lv_obj_set_flex_flow(avatar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(avatar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    avatar_label = MakeCenteredLabel(avatar, "?");
    lv_obj_set_style_text_color(avatar_label, ClrBg(), 0);

    name_label = MakeLabel(card, "", 8, 88, card_w - 16);
    title_label = MakeLabel(card, "", 8, 114, card_w - 16);
    lv_obj_set_style_text_color(title_label, ClrDim(), 0);
    emp_label = MakeLabel(card, "", 8, 138, card_w - 16);
    lv_obj_set_style_text_color(emp_label, ClrDim(), 0);
    qr_label = MakeLabel(card, "", 8, 158, card_w - 16);
    lv_obj_set_style_text_color(qr_label, ClrDim(), 0);

    status_bar = MakeBox(badge_root, 20, 210, w - 40, 54);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, ClrCard(), 0);
    status_label = MakeCenteredLabel(status_bar, "OK 开始录音");
    lv_obj_set_style_text_color(status_label, ClrAccent(), 0);

    lv_obj_t* badge_hint = MakeLabel(badge_root, "OK 录音/停止    长按菜单", 0, h - 32, w);
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
        MakeLabel(practice_root, "OK 开始陪练    长按返回菜单", 0, h - 32, w);
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
        MakeLabel(practice_root, "OK 说话/打断    长按结束陪练", 0, h - 32, w);
    } else {  // kScore
        MakeLabel(practice_root, "陪练评分", 0, 24, w);
        lv_obj_t* score_label = MakeLabel(practice_root, v.score.c_str(), 0, 62, w);
        lv_obj_set_style_text_color(score_label, ClrAccent(), 0);
        lv_obj_t* fb = MakeLabel(practice_root, v.feedback.c_str(), 16, 110, w - 32);
        lv_obj_set_style_text_align(fb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(fb, ClrDim(), 0);
        MakeLabel(practice_root, "OK 返回场景选择", 0, h - 32, w);
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
        MakeLabel(settings_root, "OK 进入    长按返回", 0, h - 32, w);
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
    } else {
        lv_label_set_text(status_label, "OK 开始录音");
        lv_obj_set_style_text_color(status_label, ClrAccent(), 0);
        if (status_bar != nullptr) {
            lv_obj_set_style_bg_color(status_bar, ClrCard(), 0);
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
    if (impl_->badge_root != nullptr) {
        lv_obj_add_flag(impl_->badge_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->StopTick();
    if (impl_->menu_root != nullptr) {
        lv_obj_remove_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->ApplyMenuStyle();
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

void MvpUi::HideMenu() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);
    if (!guard) {
        return;
    }
    if (impl_ != nullptr && impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
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
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->badge_root != nullptr) {
        lv_obj_remove_flag(impl_->badge_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = true;
    impl_->StartTick();
    impl_->RefreshStatus();
}

void MvpUi::HideBadge() {
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
    if (impl_->badge_root != nullptr) {
        lv_obj_add_flag(impl_->badge_root, LV_OBJ_FLAG_HIDDEN);
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

    /* 未绑定（无效/无姓名）：显示设备 ID 引导去管理后台绑定 */
    const bool bound = profile.valid && !profile.name.empty();
    if (impl_->avatar_label != nullptr) {
        std::string name = bound ? profile.name : std::string("?");
        char first[8];
        FirstUtf8Char(name, first, sizeof(first));
        lv_label_set_text(impl_->avatar_label, first);
    }
    if (impl_->name_label != nullptr) {
        lv_label_set_text(impl_->name_label, bound ? profile.name.c_str() : "未绑定工牌");
    }
    if (impl_->title_label != nullptr) {
        lv_label_set_text(impl_->title_label, bound ? profile.title.c_str() : "请在管理后台绑定该设备");
    }
    if (impl_->emp_label != nullptr) {
        std::string emp;
        if (bound && !profile.employee_no.empty()) {
            emp = "工号 " + profile.employee_no;
        } else if (!bound) {
            emp = "设备ID " + GetMvpConfig().device_id;
        }
        lv_label_set_text(impl_->emp_label, emp.c_str());
    }
    if (impl_->qr_label != nullptr) {
        std::string qr = (bound && !profile.qr_text.empty()) ? ("码 " + profile.qr_text) : std::string("");
        lv_label_set_text(impl_->qr_label, qr.c_str());
    }
}

/* 头像动图应用（须在 DisplayLockGuard 内调用）：空数据/解析失败回落姓名首字占位。
 * GIF 源字节存 Impl::avatar_gif_data（比 LvglGif 活得久，gifdec 从内存直接解码）。 */
void MvpUi::Impl::ApplyAvatarGif(std::vector<uint8_t>&& data) {
    avatar_gif.reset();
    avatar_gif_data.clear();
    auto show_fallback = [this]() {
        if (avatar_img != nullptr) {
            lv_obj_add_flag(avatar_img, LV_OBJ_FLAG_HIDDEN);
        }
        if (avatar_label != nullptr) {
            lv_obj_clear_flag(avatar_label, LV_OBJ_FLAG_HIDDEN);
        }
    };
    if (data.empty()) {
        show_fallback();
        return;
    }
    avatar_gif_data = std::move(data);
    avatar_gif_dsc = lv_img_dsc_t{};
    avatar_gif_dsc.data = avatar_gif_data.data();
    avatar_gif_dsc.data_size = avatar_gif_data.size();
    avatar_gif = std::make_unique<LvglGif>(&avatar_gif_dsc);
    if (avatar_gif->image_dsc() == nullptr) {
        ESP_LOGW("mvp_ui", "头像 GIF 解析失败 len=%u，回落首字", (unsigned)avatar_gif_data.size());
        avatar_gif.reset();
        avatar_gif_data.clear();
        show_fallback();
        return;
    }
    if (avatar_img == nullptr) {
        avatar_img = lv_image_create(avatar);
        lv_obj_set_size(avatar_img, 64, 64);
        lv_obj_center(avatar_img);
    }
    avatar_gif->SetFrameCallback([this]() {
        if (avatar_img != nullptr && avatar_gif != nullptr) {
            lv_image_set_src(avatar_img, avatar_gif->image_dsc());
        }
    });
    lv_image_set_src(avatar_img, avatar_gif->image_dsc());
    lv_obj_clear_flag(avatar_img, LV_OBJ_FLAG_HIDDEN);
    if (avatar_label != nullptr) {
        lv_obj_add_flag(avatar_label, LV_OBJ_FLAG_HIDDEN);
    }
    avatar_gif->Start();
}

void MvpUi::SetAvatarGif(std::vector<uint8_t> gif_data) {
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
    impl_->ApplyAvatarGif(std::move(gif_data));
}

void MvpUi::ClearAvatarGif() {
    SetAvatarGif({});
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
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->badge_root != nullptr) {
        lv_obj_add_flag(impl_->badge_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    impl_->StopTick();
    if (impl_->settings_root != nullptr) {
        lv_obj_add_flag(impl_->settings_root, LV_OBJ_FLAG_HIDDEN);
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
    if (impl_->menu_root != nullptr) {
        lv_obj_add_flag(impl_->menu_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (impl_->badge_root != nullptr) {
        lv_obj_add_flag(impl_->badge_root, LV_OBJ_FLAG_HIDDEN);
    }
    impl_->badge_visible = false;
    if (impl_->practice_root != nullptr) {
        lv_obj_add_flag(impl_->practice_root, LV_OBJ_FLAG_HIDDEN);
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
