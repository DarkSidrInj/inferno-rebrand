// The menu, drawn like the Neverlose / HexSync one: a dark rounded shell, a sidebar with the sections, a
// toolbar, cards of rows (toggle, select, slider, colour, button) and popups for selects and colours.
// Everything is laid out in the design's own units (the shell is 748 x 576) and scaled to the screen.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "CMenu.hpp"
#include "Widgets/CStatusBar.hpp"
#include "../CConfigs/CConfig.hpp"
#include "Theme/CTheme.hpp"
#include "../CGame/CSkins.hpp"
#include "../CGame/CFog.hpp"
#include "../CGame/CBhop.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "Android_draw/draw.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

// Normally defined by draw.cpp (ImGui_init loads the fonts there). The weak definitions keep the menu building and
// running, with the default font and without icons, when an older draw.cpp / draw.h is still in the tree.
__attribute__((weak)) ImFont* nlText = nullptr;
__attribute__((weak)) ImFont* nlBold = nullptr;
__attribute__((weak)) ImFont* nlIcons = nullptr;

namespace ui::menu {
namespace {
    constexpr ImVec2 kShell(748.f, 576.f);
    constexpr float kBody = 15.f, kControl = 14.f, kSmall = 12.f, kCaption = 10.f, kTitle = 16.f, kIcon = 13.f;
    constexpr float kRow = 37.f;

    constexpr ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

    enum class Page { Players, World, Skins, Misc, Settings };
    enum class K { Toggle, ToggleColor, Color, Select, Slider, Button, Info };

    struct Row {
        const char* label = "";
        K kind = K::Toggle;
        bool* b = nullptr;
        ImVec4* c = nullptr;
        float* f = nullptr;
        float mn = 0.f, mx = 1.f;
        const char* fmt = "%.1f";
        const char* const* items = nullptr;      // static list of a select
        int count = 0;
        const std::vector<std::string>* dyn = nullptr;   // or a list that changes
        int cur = 0;
        std::function<void(int)> pick;
        std::function<void()> act;
        std::string text;                        // Info: the value
    };

    struct Popup {
        bool open = false;
        int owner = -1;
        ImVec2 anchor{};                 // screen
        float width = 0.f;               // screen
        std::vector<std::string> items;
        int cur = 0;
        std::function<void(int)> pick;
        float scroll = 0.f;
        int opened_frame = 0;
    };

    struct ColorPopup {
        bool open = false;
        int owner = -1;
        ImVec4* target = nullptr;
        float h = 0.f, s = 0.f, v = 0.f;
        ImVec2 anchor{};                 // screen: the swatch's right edge
        int opened_frame = 0;
    };

    bool g_exit = false;

    struct State {
        Page page = Page::Players;
        float page_mix = 1.f;
        float scroll = 0.f, scroll_target = 0.f, content_h = 0.f;
        bool scroll_drag = false;
        float scroll_press_y = 0.f, scroll_press_value = 0.f;
        Popup popup;
        ColorPopup color;
        ImVec2 pos{-1.f, -1.f};
    } st;

    // The frame's frame of reference.
    struct Frame {
        ImVec2 origin{};
        float s = 1.f;
        ImDrawList* d = nullptr;
        bool input = true;       // false while a popup or the opening animation owns the input
    } fr;

    float g_exit_armed = -100.f;     // time of the first tap on the exit button

    ImVec2 P(float x, float y) { return fr.origin + ImVec2(x, y) * fr.s; }
    ImVec2 P(ImVec2 v) { return P(v.x, v.y); }
    float Z(float v) { return v * fr.s; }
    ImFont* text_font() { return nlText ? nlText : ImGui::GetFont(); }
    ImFont* bold_font() { return nlBold ? nlBold : ImGui::GetFont(); }
    // An icon of the icon font; nothing when that font is not there (its codes would be boxes in the others).
    void icon(float x, float y, float h, ImU32 color, const char* glyph, float size);

    ImU32 mix(ImU32 a, ImU32 b, float t) {
        return ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(a), ImGui::ColorConvertU32ToFloat4(b), ImSaturate(t)));
    }

    float motion(ImGuiID id, float target, float speed = 16.f, float initial = -1.f) {
        float* value = ImGui::GetStateStorage()->GetFloatRef(id, initial < 0.f ? target : initial);
        *value = ImLerp(*value, target, 1.f - std::exp(-speed * ImGui::GetIO().DeltaTime));
        if (std::fabs(*value - target) < 0.0005f) *value = target;
        return *value;
    }

    void text(float x, float y, ImU32 color, const char* value, float size = kBody, ImFont* font = nullptr) {
        fr.d->AddText(font ? font : text_font(), Z(size), P(x, y), color, value);
    }

    float text_width(const char* value, float size = kBody, ImFont* font = nullptr) {
        return (font ? font : text_font())->CalcTextSizeA(Z(size), FLT_MAX, 0.f, value).x / fr.s;
    }

    // Vertically centred in a box of height h.
    void text_mid(float x, float y, float h, ImU32 color, const char* value, float size = kBody, ImFont* font = nullptr) {
        ImFont* f = font ? font : text_font();
        const float th = f->CalcTextSizeA(Z(size), FLT_MAX, 0.f, value).y / fr.s;
        text(x, y + std::floor((h - th) * 0.5f), color, value, size, f);
    }

    void icon(float x, float y, float h, ImU32 color, const char* glyph, float size) {
        if (nlIcons) text_mid(x, y, h, color, glyph, size, nlIcons);
    }

    void rect(float x, float y, float w, float h, ImU32 color, float r = 0.f, ImDrawFlags flags = 0) {
        fr.d->AddRectFilled(P(x, y), P(x + w, y + h), color, Z(r), flags);
    }
    void outline(float x, float y, float w, float h, ImU32 color, float r = 0.f, float t = 1.f) {
        fr.d->AddRect(P(x, y), P(x + w, y + h), color, Z(r), 0, Z(t) < 1.f ? 1.f : Z(t));
    }
    void line(float x1, float y1, float x2, float y2, ImU32 color, float t = 1.f) {
        fr.d->AddLine(P(x1, y1), P(x2, y2), color, Z(t) < 1.f ? 1.f : Z(t));
    }

    // An invisible button over a design-space rectangle; false (and not hovered) when the input is not ours.
    bool hit(const char* id, float x, float y, float w, float h, bool* hovered = nullptr, bool* active = nullptr) {
        if (hovered) *hovered = false;
        if (active) *active = false;
        if (!fr.input) return false;
        ImGui::SetCursorScreenPos(P(x, y));
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(Z(w), Z(h)));
        if (hovered) *hovered = ImGui::IsItemHovered();
        if (active) *active = ImGui::IsItemActive();
        return clicked;
    }

    void chevron(float x, float y, ImU32 color) {
        line(x, y, x + 3, y + 3, color, 1.3f);
        line(x + 3, y + 3, x, y + 6, color, 1.3f);
    }

    void chevron_down(float x, float y, ImU32 color) {
        line(x, y, x + 4, y + 4, color, 1.f);
        line(x + 4, y + 4, x + 8, y, color, 1.f);
    }

    // ---- controls ----------------------------------------------------------------------------------

    void toggle(float x, float y, bool* value, bool enabled = true) {
        bool hover = false;
        if (enabled && hit("##toggle", x - 5, y - 6, 39, 30, &hover) && value) *value = !*value;
        const ImGuiID iid = ImGui::GetID("##toggle");
        const bool on_now = value && *value;
        const float on = motion(iid ^ 0x55aa721u, on_now ? 1.f : 0.f, 19.f);
        const float h = motion(iid ^ 0x118a0u, hover ? 1.f : 0.f, 20.f);
        rect(x - 1 - h, y - 1 - h, 31 + 2 * h, 21 + 2 * h, C(75, 126, 255, int(45 * h)), 10);
        rect(x, y, 29, 18, mix(C(29, 33, 43), C(75, 126, 255), on), 9);
        fr.d->AddCircleFilled(P(x + ImLerp(9.f, 20.f, on), y + 9), Z(7), mix(C(133, 144, 156), C(248, 249, 252), on));
    }

    void open_popup(int owner, const Row& row, ImVec2 anchor, float width) {
        const bool same = st.popup.open && st.popup.owner == owner;
        st.popup.open = !same;
        st.color.open = false;
        st.popup.owner = owner;
        st.popup.anchor = anchor;
        st.popup.width = width;
        st.popup.items.clear();
        if (row.dyn) st.popup.items = *row.dyn;
        else for (int i = 0; i < row.count; ++i) st.popup.items.push_back(row.items[i]);
        st.popup.cur = row.cur;
        st.popup.pick = row.pick;
        st.popup.scroll = 0.f;
        st.popup.opened_frame = ImGui::GetFrameCount();
    }

    void open_color(int owner, ImVec4* target, ImVec2 anchor) {
        const bool same = st.color.open && st.color.owner == owner;
        st.color.open = !same;
        st.popup.open = false;
        st.color.owner = owner;
        st.color.target = target;
        st.color.anchor = anchor;
        st.color.opened_frame = ImGui::GetFrameCount();
        if (target) ImGui::ColorConvertRGBtoHSV(target->x, target->y, target->z, st.color.h, st.color.s, st.color.v);
    }

    void swatch(float x, float y, float w, float h, const ImVec4& c, bool hot) {
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 1.f));
        rect(x, y, w, h, col, 5);
        if (hot) outline(x - 1, y - 1, w + 2, h + 2, C(255, 255, 255, 70), 6);
    }

    void draw_row(float cx, float cw, float y, int index, Row& r, int id) {
        if (index) line(cx + 12, y, cx + cw - 12, y, C(28, 31, 40));
        text_mid(cx + 13, y, kRow, C(207, 209, 218), r.label, kBody);
        ImGui::PushID(id);
        switch (r.kind) {
        case K::Toggle:
            toggle(cx + cw - 43, y + 9, r.b);
            break;
        case K::ToggleColor:
        case K::Color: {
            const bool with_toggle = r.kind == K::ToggleColor;
            const float sx = with_toggle ? cx + cw - 63 : cx + cw - 43;
            const float sw = with_toggle ? 15.f : 29.f, sh = with_toggle ? 15.f : 18.f;
            const float sy = with_toggle ? y + 11 : y + 9;
            bool hover = false;
            ImGui::PushID("##swatch");
            const bool clicked = hit("##swatch", sx - 4, sy - 5, sw + 8, sh + 10, &hover);
            ImGui::PopID();
            const bool is_open = st.color.open && st.color.owner == id;
            if (clicked && r.c) open_color(id, r.c, P(sx + sw + 8, sy + sh * 0.5f));
            if (r.c) swatch(sx, sy, sw, sh, *r.c, hover || is_open);
            if (with_toggle) toggle(cx + cw - 43, y + 9, r.b);
            break;
        }
        case K::Select: {
            const float bw = ImMin(134.f, cw * 0.48f);
            const float bx = cx + cw - bw - 13, by = y + 7;
            bool hover = false;
            const bool clicked = hit("##select", bx, by, bw, 23, &hover);
            const float response = motion(ImGui::GetID("##select") ^ 0x6161u, hover || (st.popup.open && st.popup.owner == id) ? 1.f : 0.f);
            if (clicked) open_popup(id, r, P(bx, by), Z(bw));
            rect(bx, by, bw, 23, mix(C(25, 28, 38), C(31, 38, 54), response), 5);
            outline(bx, by, bw, 23, mix(C(32, 35, 46), C(75, 126, 255), response), 5);
            const char* shown = "Select";
            if (r.dyn) { if (r.cur >= 0 && r.cur < int(r.dyn->size())) shown = (*r.dyn)[size_t(r.cur)].c_str(); }
            else if (r.cur >= 0 && r.cur < r.count) shown = r.items[r.cur];
            // clip the value to the box
            const ImVec2 cmin = P(bx + 4, by), cmax = P(bx + bw - 18, by + 23);
            fr.d->PushClipRect(cmin, cmax, true);
            text_mid(bx + 7, by, 23, C(170, 173, 184), shown, kControl);
            fr.d->PopClipRect();
            chevron_down(bx + bw - 15, by + 9, C(139, 143, 154));
            break;
        }
        case K::Slider: {
            const float tw = 76.f;
            const float start_x = cx + cw - 150, start_y = y + 18;
            bool active = false;
            hit("##slider", start_x - 8, start_y - 11, tw + 16, 24, nullptr, &active);
            if (active && r.f) {
                const float t = ImClamp((ImGui::GetIO().MousePos.x - P(start_x, 0).x) / Z(tw), 0.f, 1.f);
                *r.f = r.mn + t * (r.mx - r.mn);
            }
            const float norm = r.f && r.mx > r.mn ? ImClamp((*r.f - r.mn) / (r.mx - r.mn), 0.f, 1.f) : 0.f;
            const float shown = motion(ImGui::GetID("##slider") ^ 0xa8f1u, norm, 14.f, norm);
            rect(start_x, start_y, tw, 3, C(34, 38, 48), 2);
            rect(start_x, start_y, tw * shown, 3, C(75, 126, 255), 2);
            fr.d->AddCircleFilled(P(start_x + tw * shown, start_y + 1.5f), Z(5.5f), C(247, 248, 252));
            char buf[32] = "";
            if (r.f) std::snprintf(buf, sizeof(buf), r.fmt, *r.f);
            const float pw = 46.f;
            const float px = cx + cw - 56;
            rect(px, y + 8, pw, 21, C(25, 28, 38), 5);
            float size = kControl;
            while (size > 8.f && text_width(buf, size) > pw - 8.f) size -= 1.f;
            const float tw_text = text_width(buf, size);
            text_mid(px + (pw - tw_text) * 0.5f, y + 8, 21, C(166, 169, 179), buf, size);
            break;
        }
        case K::Button: {
            bool hover = false;
            const bool clicked = hit("##button", cx + 6, y + 3, cw - 12, kRow - 6, &hover);
            const float response = motion(ImGui::GetID("##button") ^ 0x4242u, hover ? 1.f : 0.f);
            if (response > 0.001f) rect(cx + 6, y + 3, cw - 12, kRow - 6, C(75, 126, 255, int(26 * response)), 8);
            chevron(cx + cw - 22, y + 15, mix(C(187, 190, 199), C(120, 160, 255), response));
            if (clicked && r.act) r.act();
            break;
        }
        case K::Info: {
            float size = kControl;
            while (size > 8.f && text_width(r.text.c_str(), size) > cw * 0.55f) size -= 1.f;
            const float w = text_width(r.text.c_str(), size);
            text_mid(cx + cw - 14 - w, y, kRow, C(150, 154, 166), r.text.c_str(), size);
            break;
        }
        }
        ImGui::PopID();
    }

    struct Columns { float y[2] = {84.f, 84.f}; int id = 0; };
    Columns cols;
    const float kColX[2] = {167.f, 458.f};
    const float kColW[2] = {281.f, 277.f};

    void card(int col, const char* title, std::vector<Row> rows) {
        const float x = kColX[col], w = kColW[col];
        const float y = cols.y[col] - st.scroll;
        const float h = float(rows.size()) * kRow;
        text(x + 12, y - 18, C(89, 94, 106), title, kCaption);
        rect(x, y, w, h, C(17, 19, 27, 224), 14);
        outline(x, y, w, h, C(31, 34, 44), 14);
        for (size_t i = 0; i < rows.size(); ++i) draw_row(x, w, y + float(i) * kRow, int(i), rows[i], cols.id++);
        cols.y[col] += h + 36.f;
    }

    Row toggle_row(const char* label, bool* b) { Row r; r.label = label; r.kind = K::Toggle; r.b = b; return r; }
    Row toggle_color(const char* label, bool* b, ImVec4* c) { Row r; r.label = label; r.kind = K::ToggleColor; r.b = b; r.c = c; return r; }
    Row color_row(const char* label, ImVec4* c) { Row r; r.label = label; r.kind = K::Color; r.c = c; return r; }
    Row slider_row(const char* label, float* f, float mn, float mx, const char* fmt) {
        Row r; r.label = label; r.kind = K::Slider; r.f = f; r.mn = mn; r.mx = mx; r.fmt = fmt; return r;
    }
    Row select_row(const char* label, int* value, const char* const* items, int count) {
        Row r; r.label = label; r.kind = K::Select; r.items = items; r.count = count; r.cur = *value;
        r.pick = [value](int i) { *value = i; };
        return r;
    }
    Row button_row(const char* label, std::function<void()> act) { Row r; r.label = label; r.kind = K::Button; r.act = std::move(act); return r; }
    Row info_row(const char* label, std::string value) { Row r; r.label = label; r.kind = K::Info; r.text = std::move(value); return r; }

    // ---- pages -------------------------------------------------------------------------------------

    void players_page() {
        static const char* const kBoxTypes[] = {"Full", "Corner"};
        static const char* const kMaterials[] = {"Flat", "Untouchable", "Outline"};
        card(0, "ENEMY", {
            toggle_color("Box", &cfg::esp::box, &cfg::esp::box_col),
            toggle_color("Name", &cfg::esp::name, &cfg::esp::name_col),
            toggle_color("Health Bar", &cfg::esp::health, &cfg::esp::health_col),
            toggle_color("Armor Bar", &cfg::esp::armor, &cfg::esp::armor_col),
            toggle_color("Helmet Mark", &cfg::esp::helmet, &cfg::esp::helmet_col),
            toggle_color("Distance", &cfg::esp::distance, &cfg::esp::distance_col),
        });
        card(0, "BOX", {
            select_row("Box Type", &cfg::esp::box_type, kBoxTypes, 2),
            slider_row("Box Rounding", &cfg::esp::box_rounding, 0.f, 10.f, "%.0f"),
            slider_row("Fallback FOV", &cfg::esp::fallback_fov, 20.f, 120.f, "%.0f"),
        });
        card(1, "SKELETON", {
            toggle_color("Skeleton", &cfg::esp::skeleton, &cfg::esp::skeleton_col),
            toggle_row("All Bones", &cfg::esp::skeleton_all),
            toggle_row("Bones Behind Walls", &cfg::esp::skeleton_occlusion),
            slider_row("Skeleton Width", &cfg::esp::skeleton_thickness, 1.f, 4.f, "%.1f"),
        });
        card(1, "CHAMS", {
            toggle_row("Material Chams", &cfg::chams::enabled),
            select_row("Material", &cfg::chams::material, kMaterials, 3),
            toggle_row("Enemies", &cfg::chams::enemies),
            toggle_row("Teammates", &cfg::chams::teammates),
            toggle_row("Local Body", &cfg::chams::local_body),
            toggle_row("Arms", &cfg::chams::local_arms),
            toggle_row("Weapon", &cfg::chams::local_weapon),
            toggle_color("Overlay Through Walls", &cfg::chams::overlay, &cfg::chams::overlay_col),
        });
    }

    void world_page() {
        card(0, "NIGHT MODE", {
            toggle_row("Night Mode", &cfg::night::enabled),
            slider_row("Strength", &cfg::night::strength, 0.f, 1.f, "%.2f"),
        });
        card(0, "FOG", {
            toggle_row("Fog", &cfg::fog::enabled),
            toggle_row("Auto Color", &cfg::fog::auto_color),
            slider_row("Min Distance", &cfg::fog::min_distance, 0.f, 200.f, "%.0f m"),
            slider_row("Max Distance", &cfg::fog::max_distance, 10.f, 400.f, "%.0f m"),
            color_row("Color", &cfg::fog::color),
            button_row("Rescan World", [] { worldfog::rescan(); }),
            toggle_row("Engine Writes (risky)", &cfg::fog::engine),
        });
        card(1, "WORLD ESP", {
            toggle_color("Grenades", &cfg::esp::grenade, &cfg::esp::grenade_col),
            toggle_color("Dropped Weapons", &cfg::esp::dropped, &cfg::esp::dropped_col),
            slider_row("Range", &cfg::esp::world_distance, 10.f, 200.f, "%.0f m"),
        });
    }

    void skins_page() {
        static skins::View view;
        static long long refreshed = 0;
        const long long now = diag::now_ms();
        if (now - refreshed > 250) { refreshed = now; view = skins::view(); }

        Row skin;
        skin.label = "Knife Skin";
        skin.kind = K::Select;
        skin.dyn = &view.names;
        skin.cur = view.current;
        skin.pick = [](int i) {
            if (view.weapon >= 0 && view.weapon < 256 && i >= 0 && i < int(view.ids.size())) {
                cfg::skins::chosen[view.weapon] = view.ids[size_t(i)];
                view.current = i;
            }
        };

        static std::vector<std::string> model_ids;
        if (model_ids.empty()) for (int i = 70; i < 90; ++i) model_ids.push_back(std::to_string(i));
        Row model;
        model.label = "Knife Model Id";
        model.kind = K::Select;
        model.dyn = &model_ids;
        model.cur = ImClamp(cfg::skins::model - 70, 0, 19);
        model.pick = [](int i) { cfg::skins::model = 70 + i; };

        std::vector<Row> main = {toggle_row("Skin Changer", &cfg::skins::enabled), skin, toggle_row("Full Model (patches game code)", &cfg::skins::code_patch)};
        if (cfg::skins::code_patch) {
            main.push_back(model);
            main.push_back(button_row("Find The Butterfly WeaponId", [] { skins::probe_request(); }));
        }
        card(0, "SKINS", std::move(main));
        card(1, "STATUS", {
            info_row("State", view.status),
            info_row("Weapon In Hands", view.weapon >= 0 ? std::to_string(view.weapon) : std::string("-")),
        });
        card(1, "TOOLS", {
            button_row("Dump Game Code To /sdcard", [] { skins::dump_code(); }),
            button_row("Dump Resolver Code", [] { skins::dump_code(true); }),
            button_row("Reset All Skins", [] { for (int& id : cfg::skins::chosen) id = 0; }),
        });
    }

    void misc_page() {
        static const char* const kYaw[] = {"Legacy", "Spin", "Jitter", "Backwards", "None"};
        static const char* const kPitch[] = {"None", "Down", "Up"};
        char button[48];
        if (bhop::jump_button_set()) std::snprintf(button, sizeof(button), "%.0f, %.0f", cfg::bhop::jump_x, cfg::bhop::jump_y);
        else std::snprintf(button, sizeof(button), "not set");
        card(0, "BUNNYHOP", {
            toggle_row("Bunnyhop", &cfg::bhop::enabled),
            toggle_row("Strafe While Holding", &cfg::bhop::strafe),
            button_row("Set Jump Button", [] { bhop::start_calibration(); }),
            info_row("Jump Button", button),
            slider_row("Button Radius", &cfg::bhop::radius, 40.f, 260.f, "%.0f"),
            info_row("Jump Held", bhop::g_held ? "yes" : "no"),
            info_row("Vertical Speed", strafe::g_vert >= 0 ? "found" : "measuring"),
        });
        card(0, "MOVEMENT", {
            toggle_row("Auto Strafe", &cfg::strafe::enabled),
            slider_row("Turn Per Step", &cfg::strafe::turn, 0.5f, 15.f, "%.1f"),
            slider_row("Extra Speed", &cfg::strafe::gain, 0.f, 3.f, "%.1f %%"),
            slider_row("Min Speed", &cfg::strafe::min_speed, 0.f, 10.f, "%.1f"),
        });
        card(1, "ANTI-AIM", {
            toggle_row("Enabled", &cfg::antiaim::enabled),
            select_row("Yaw", &cfg::antiaim::yaw_mode, kYaw, 5),
            select_row("Pitch", &cfg::antiaim::pitch_mode, kPitch, 3),
            slider_row("Spin Speed", &cfg::antiaim::spin_speed, 90.f, 2000.f, "%.0f"),
            slider_row("Jitter Range", &cfg::antiaim::jitter, 10.f, 180.f, "%.0f"),
        });
    }

    void settings_page() {
        char screen[48];
        std::snprintf(screen, sizeof(screen), "%.0f x %.0f", g_sw, g_sh);
        static const char* const kSizes[] = {"60%", "80%", "100%", "120%", "140%"};
        static const float kScales[] = {0.6f, 0.8f, 1.f, 1.2f, 1.4f};
        int nearest = 0;
        for (int i = 1; i < 5; ++i) if (std::fabs(kScales[i] - cfg::gui::scale) < std::fabs(kScales[nearest] - cfg::gui::scale)) nearest = i;
        Row size;
        size.label = "Menu Size";
        size.kind = K::Select;
        size.items = kSizes;
        size.count = 5;
        size.cur = nearest;
        size.pick = [](int i) { cfg::gui::scale = kScales[i]; };
        card(0, "INTERFACE", {size, info_row("Screen", screen)});
    }

    // ---- popups (windows of their own, above the menu) -----------------------------------------------

    constexpr ImGuiWindowFlags kPopupFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;

    void select_popup(float alpha) {
        Popup& p = st.popup;
        if (!p.open) return;
        const float s = fr.s;
        const float row_h = 32.f * s;
        const int visible = ImMin(int(p.items.size()), 8);
        const ImVec2 size(p.width, visible * row_h + 8.f * s);
        ImVec2 pos(p.anchor.x, p.anchor.y + (23.f * s - size.y) * 0.5f);
        pos.x = ImClamp(pos.x, 10.f, g_sw - size.x - 10.f);
        pos.y = ImClamp(pos.y, 10.f, g_sh - size.y - 10.f);
        const float open = motion(ImGui::GetID("##nl_popup_open"), 1.f, 20.f, 0.f);

        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::SetNextWindowFocus();   // above the menu, always
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        if (!ImGui::Begin("##nl_select", nullptr, kPopupFlags)) { ImGui::End(); ImGui::PopStyleVar(2); return; }
        ImDrawList* d = ImGui::GetWindowDrawList();
        const int first = d->VtxBuffer.Size;
        d->AddRectFilled(pos - ImVec2(5, 2) * s, pos + size + ImVec2(5, 8) * s, C(0, 0, 0, 55), 18.f * s);
        d->AddRectFilled(pos, pos + size, C(20, 20, 29, 250), 16.f * s);
        d->AddRect(pos, pos + size, C(57, 61, 76, 205), 16.f * s);
        d->AddLine(pos + ImVec2(16, 1) * s, pos + ImVec2(size.x - 16 * s, 1), C(255, 255, 255, 22));

        // Scroll the list by dragging inside it, or with the wheel.
        const float max_scroll = ImMax(0.f, float(p.items.size()) * row_h - visible * row_h);
        ImGuiIO& io = ImGui::GetIO();
        const bool inside = io.MousePos.x >= pos.x && io.MousePos.x <= pos.x + size.x && io.MousePos.y >= pos.y && io.MousePos.y <= pos.y + size.y;
        if (inside && io.MouseWheel != 0.f) p.scroll -= io.MouseWheel * row_h;
        p.scroll = ImClamp(p.scroll, 0.f, max_scroll);

        ImGui::PushClipRect(pos + ImVec2(0, 4 * s), pos + size - ImVec2(0, 4 * s), true);
        const bool accepts = ImGui::GetFrameCount() > p.opened_frame;
        bool dragging = false;
        for (int i = 0; i < int(p.items.size()); ++i) {
            const ImVec2 rp = pos + ImVec2(4 * s, 4 * s + i * row_h - p.scroll);
            if (rp.y + row_h < pos.y || rp.y > pos.y + size.y) continue;
            ImGui::SetCursorScreenPos(rp);
            ImGui::PushID(7000 + i);
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(size.x - 8 * s, row_h));
            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            const ImGuiID row_id = ImGui::GetID("##row_motion");
            ImGui::PopID();
            if (active && std::fabs(io.MouseDelta.y) > 0.f && max_scroll > 0.f) { p.scroll = ImClamp(p.scroll - io.MouseDelta.y, 0.f, max_scroll); dragging = true; }
            const float hover = motion(row_id ^ 0x9921u, hovered ? 1.f : 0.f, 22.f);
            if (hover > 0.001f) d->AddRectFilled(rp, rp + ImVec2(size.x - 8 * s, row_h), C(75, 126, 255, int(25 * hover)), 10.f * s);
            const bool selected = p.cur == i;
            if (selected) {
                const ImVec2 c = rp + ImVec2(14, 16) * s;
                d->AddLine(c, c + ImVec2(3, 3) * s, C(230, 233, 241), 1.5f * s);
                d->AddLine(c + ImVec2(3, 3) * s, c + ImVec2(8, -3) * s, C(230, 233, 241), 1.5f * s);
            }
            const ImVec2 ts = text_font()->CalcTextSizeA(kControl * s, FLT_MAX, 0.f, p.items[size_t(i)].c_str());
            d->AddText(text_font(), kControl * s, rp + ImVec2(35 * s, (row_h - ts.y) * 0.5f), selected ? C(224, 229, 243) : C(182, 185, 196), p.items[size_t(i)].c_str());
            if (accepts && clicked && !dragging && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 36.f) {
                if (p.pick) p.pick(i);
                p.open = false;
            }
        }
        ImGui::PopClipRect();
        if (p.items.size() > size_t(visible)) {   // a thin scroll mark
            const float track = size.y - 16 * s;
            const float thumb = track * visible / float(p.items.size());
            const float ty = pos.y + 8 * s + (track - thumb) * (max_scroll > 0 ? p.scroll / max_scroll : 0.f);
            d->AddRectFilled(ImVec2(pos.x + size.x - 5 * s, ty), ImVec2(pos.x + size.x - 3 * s, ty + thumb), C(120, 130, 160, 120), s);
        }
        if (accepts && ImGui::IsMouseClicked(0) && !inside) p.open = false;
        const float eased = 1.f - std::pow(1.f - open, 3.f);
        const ImVec2 pivot = pos + size * 0.5f;
        for (int i = first; i < d->VtxBuffer.Size; ++i) {
            ImDrawVert& v = d->VtxBuffer[i];
            v.pos = pivot + (v.pos - pivot) * ImLerp(0.96f, 1.f, eased) + ImVec2(4.f * (1.f - eased) * s, 0);
            const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 255u;
            v.col = (v.col & 0x00ffffffu) | ((ImU32)(a * eased * alpha) << IM_COL32_A_SHIFT);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    void checker(ImDrawList* d, ImVec2 a, ImVec2 b, float cell) {
        d->AddRectFilled(a, b, C(60, 62, 70));
        bool flip = false;
        for (float x = a.x; x < b.x; x += cell, flip = !flip) {
            bool on = flip;
            for (float y = a.y; y < b.y; y += cell, on = !on)
                if (on) d->AddRectFilled(ImVec2(x, y), ImVec2(ImMin(x + cell, b.x), ImMin(y + cell, b.y)), C(95, 98, 108));
        }
    }

    void color_popup(float alpha) {
        ColorPopup& cp = st.color;
        if (!cp.open || !cp.target) return;
        const float s = fr.s;
        const ImVec2 size(312.f * s, 372.f * s);
        ImVec2 pos(cp.anchor.x, cp.anchor.y - 30.f * s);
        if (pos.x + size.x > g_sw - 10.f) pos.x = cp.anchor.x - size.x - 40.f * s;   // no room on the right: on the left
        pos.x = ImClamp(pos.x, 10.f, g_sw - size.x - 10.f);
        pos.y = ImClamp(pos.y, 10.f, g_sh - size.y - 10.f);
        const float open = motion(ImGui::GetID("##nl_color_open"), 1.f, 20.f, 0.f);

        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::SetNextWindowFocus();   // above the menu, always
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        if (!ImGui::Begin("##nl_color", nullptr, kPopupFlags)) { ImGui::End(); ImGui::PopStyleVar(2); return; }
        ImDrawList* d = ImGui::GetWindowDrawList();
        const int first = d->VtxBuffer.Size;
        const ImU32 bg = C(21, 23, 32, 252);
        d->AddRectFilled(pos - ImVec2(4, 2) * s, pos + size + ImVec2(4, 7) * s, C(0, 0, 0, 40), 20.f * s);
        d->AddRectFilled(pos, pos + size, bg, 18.f * s);
        d->AddRect(pos, pos + size, C(48, 52, 66), 18.f * s);

        ImGuiIO& io = ImGui::GetIO();
        ImVec4& target = *cp.target;
        const float alpha_value = target.w;
        bool changed = false;

        // header chip: the colour and its name
        const ImVec2 chip = pos + ImVec2(104, 14) * s;
        d->AddRectFilled(chip, chip + ImVec2(104, 36) * s, C(31, 34, 47), 10.f * s);
        d->AddCircleFilled(chip + ImVec2(20, 18) * s, 9.f * s, ImGui::ColorConvertFloat4ToU32(ImVec4(target.x, target.y, target.z, 1.f)));
        {
            const char* name = "Primary";
            const ImVec2 ts = text_font()->CalcTextSizeA(kBody * s, FLT_MAX, 0.f, name);
            d->AddText(text_font(), kBody * s, chip + ImVec2(38 * s, (36 * s - ts.y) * 0.5f), C(224, 228, 240), name);
        }

        // saturation / value square
        const ImVec2 sv_min = pos + ImVec2(16, 64) * s, sv_max = pos + ImVec2(296, 238) * s;
        float rr, gg, bb;
        ImGui::ColorConvertHSVtoRGB(cp.h, 1.f, 1.f, rr, gg, bb);
        const ImU32 hue_col = ImGui::ColorConvertFloat4ToU32(ImVec4(rr, gg, bb, 1.f));
        d->AddRectFilledMultiColor(sv_min, sv_max, C(255, 255, 255), hue_col, hue_col, C(255, 255, 255));
        d->AddRectFilledMultiColor(sv_min, sv_max, C(0, 0, 0, 0), C(0, 0, 0, 0), C(0, 0, 0, 255), C(0, 0, 0, 255));
        d->AddRect(sv_min, sv_max, bg, 10.f * s, 0, 6.f * s);          // rounds the corners of the gradient
        ImGui::SetCursorScreenPos(sv_min);
        ImGui::InvisibleButton("##sv", sv_max - sv_min);
        if (ImGui::IsItemActive()) {
            cp.s = ImClamp((io.MousePos.x - sv_min.x) / (sv_max.x - sv_min.x), 0.f, 1.f);
            cp.v = 1.f - ImClamp((io.MousePos.y - sv_min.y) / (sv_max.y - sv_min.y), 0.f, 1.f);
            changed = true;
        }
        const ImVec2 handle(sv_min.x + (sv_max.x - sv_min.x) * cp.s, sv_min.y + (sv_max.y - sv_min.y) * (1.f - cp.v));
        d->AddCircleFilled(handle, 8.f * s, C(0, 0, 0, 90));
        d->AddCircle(handle, 7.f * s, C(255, 255, 255), 0, 2.f * s);
        d->AddCircleFilled(handle, 5.f * s, ImGui::ColorConvertFloat4ToU32(ImVec4(target.x, target.y, target.z, 1.f)));

        // hue bar
        const ImVec2 hue_min = pos + ImVec2(16, 258) * s, hue_max = pos + ImVec2(296, 270) * s;
        {
            static const ImU32 stops[7] = {C(255, 0, 0), C(255, 255, 0), C(0, 255, 0), C(0, 255, 255), C(0, 0, 255), C(255, 0, 255), C(255, 0, 0)};
            const float w = (hue_max.x - hue_min.x) / 6.f;
            for (int i = 0; i < 6; ++i)
                d->AddRectFilledMultiColor(ImVec2(hue_min.x + w * i, hue_min.y), ImVec2(hue_min.x + w * (i + 1), hue_max.y), stops[i], stops[i + 1], stops[i + 1], stops[i]);
            d->AddRect(hue_min, hue_max, bg, 6.f * s, 0, 4.f * s);
        }
        ImGui::SetCursorScreenPos(ImVec2(hue_min.x, hue_min.y - 8 * s));
        ImGui::InvisibleButton("##hue", ImVec2(hue_max.x - hue_min.x, hue_max.y - hue_min.y + 16 * s));
        if (ImGui::IsItemActive()) {
            cp.h = ImClamp((io.MousePos.x - hue_min.x) / (hue_max.x - hue_min.x), 0.f, 0.9999f);
            changed = true;
        }
        const float hx = hue_min.x + (hue_max.x - hue_min.x) * cp.h;
        d->AddCircleFilled(ImVec2(hx, (hue_min.y + hue_max.y) * 0.5f), 7.f * s, C(0, 0, 0, 90));
        d->AddCircleFilled(ImVec2(hx, (hue_min.y + hue_max.y) * 0.5f), 6.f * s, C(255, 255, 255));
        d->AddCircleFilled(ImVec2(hx, (hue_min.y + hue_max.y) * 0.5f), 4.f * s, hue_col);

        // alpha bar
        const ImVec2 a_min = pos + ImVec2(16, 286) * s, a_max = pos + ImVec2(296, 298) * s;
        {
            ImGui::PushClipRect(a_min, a_max, true);
            checker(d, a_min, a_max, 6.f * s);
            ImGui::PopClipRect();
            const ImU32 solid = ImGui::ColorConvertFloat4ToU32(ImVec4(target.x, target.y, target.z, 1.f));
            d->AddRectFilledMultiColor(a_min, a_max, solid & 0x00ffffffu, solid, solid, solid & 0x00ffffffu);
            d->AddRect(a_min, a_max, bg, 6.f * s, 0, 4.f * s);
        }
        ImGui::SetCursorScreenPos(ImVec2(a_min.x, a_min.y - 8 * s));
        ImGui::InvisibleButton("##alpha", ImVec2(a_max.x - a_min.x, a_max.y - a_min.y + 16 * s));
        float new_alpha = alpha_value;
        if (ImGui::IsItemActive()) {
            new_alpha = ImClamp((io.MousePos.x - a_min.x) / (a_max.x - a_min.x), 0.f, 1.f);
            changed = true;
        }
        const float ax = a_min.x + (a_max.x - a_min.x) * new_alpha;
        d->AddCircleFilled(ImVec2(ax, (a_min.y + a_max.y) * 0.5f), 7.f * s, C(0, 0, 0, 90));
        d->AddCircleFilled(ImVec2(ax, (a_min.y + a_max.y) * 0.5f), 6.f * s, C(255, 255, 255));
        d->AddCircleFilled(ImVec2(ax, (a_min.y + a_max.y) * 0.5f), 4.f * s, ImGui::ColorConvertFloat4ToU32(ImVec4(target.x, target.y, target.z, new_alpha)));

        if (changed) {
            ImGui::ColorConvertHSVtoRGB(cp.h, cp.s, cp.v, target.x, target.y, target.z);
            target.w = new_alpha;
        }

        // hex
        d->AddLine(pos + ImVec2(16, 318) * s, pos + ImVec2(296, 318) * s, C(34, 37, 49));
        {
            const char* label = "HEX";
            const ImVec2 ls = text_font()->CalcTextSizeA(kControl * s, FLT_MAX, 0.f, label);
            d->AddText(text_font(), kControl * s, pos + ImVec2(22 * s, 340 * s - ls.y * 0.5f), C(160, 164, 178), label);
            const ImVec2 field = pos + ImVec2(104, 326) * s;
            d->AddRectFilled(field, field + ImVec2(190, 28) * s, C(29, 32, 44), 8.f * s);
            d->AddCircleFilled(field + ImVec2(18, 14) * s, 7.f * s, ImGui::ColorConvertFloat4ToU32(ImVec4(target.x, target.y, target.z, 1.f)));
            char hex[16];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", int(ImSaturate(target.x) * 255.f + .5f), int(ImSaturate(target.y) * 255.f + .5f),
                          int(ImSaturate(target.z) * 255.f + .5f), int(ImSaturate(target.w) * 255.f + .5f));
            const ImVec2 hs = text_font()->CalcTextSizeA(kControl * s, FLT_MAX, 0.f, hex);
            d->AddText(text_font(), kControl * s, field + ImVec2(36 * s, (28 * s - hs.y) * 0.5f), C(200, 204, 218), hex);
        }

        const bool inside = io.MousePos.x >= pos.x && io.MousePos.x <= pos.x + size.x && io.MousePos.y >= pos.y && io.MousePos.y <= pos.y + size.y;
        if (ImGui::GetFrameCount() > cp.opened_frame && ImGui::IsMouseClicked(0) && !inside) cp.open = false;

        const float eased = 1.f - std::pow(1.f - open, 3.f);
        const ImVec2 pivot = pos + size * 0.5f;
        for (int i = first; i < d->VtxBuffer.Size; ++i) {
            ImDrawVert& v = d->VtxBuffer[i];
            v.pos = pivot + (v.pos - pivot) * ImLerp(0.96f, 1.f, eased);
            const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 255u;
            v.col = (v.col & 0x00ffffffu) | ((ImU32)(a * eased * alpha) << IM_COL32_A_SHIFT);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // ---- shell ---------------------------------------------------------------------------------------

    void change_page(Page next) {
        if (st.page == next) return;
        st.page = next;
        st.page_mix = 0.f;
        st.scroll = st.scroll_target = 0.f;
        st.popup.open = false;
        st.color.open = false;
    }

    void sidebar() {
        const bool visuals = st.page == Page::Players || st.page == Page::World;
        rect(0, 0, 158, kShell.y, C(18, 21, 30, 231), 14, ImDrawFlags_RoundCornersLeft);
        rect(145, 0, 13, kShell.y, C(18, 21, 30, 231));
        line(158, 0, 158, kShell.y, C(30, 33, 43));
        rect(15, 11, 30, 32, C(8, 27, 48), 7);
        text(20, 17, C(94, 185, 255), "MS", kTitle, bold_font());
        text(53, 13, C(228, 230, 236), "morskie svinki", 13.f, bold_font());
        text(53, 33, C(91, 96, 108), "Standoff 2", 8.f);
        line(10, 56, 147, 56, C(28, 31, 41));
        text(16, 67, C(91, 96, 108), "COMMON", kCaption);

        auto nav = [&](int id, const char* glyph, const char* label, float y, Page target, bool selected, float indent = 0.f) {
            const float x = 7 + indent, w = 140 - indent;
            ImGui::PushID(id);
            bool hover = false;
            const bool clicked = hit("##nav", x, y, w, 30, &hover);
            const float r = motion(ImGui::GetID("##nav") ^ 0x7771u, selected ? 1.f : hover ? 0.48f : 0.f);
            ImGui::PopID();
            if (clicked) change_page(target);
            if (r > 0.001f) rect(x, y, w, 30, mix(C(18, 21, 30, 0), C(39, 43, 54), r), 6);
            icon(x + 10, y, 30, mix(C(137, 142, 153), C(82, 141, 255), r), glyph, kIcon);
            text_mid(x + 31, y, 30, mix(C(145, 149, 159), C(226, 228, 235), r), label, kBody);
        };

        nav(0, u8"", "Visuals", 84, Page::Players, visuals);
        const float expand = motion(ImGui::GetID("##visual_expand"), visuals ? 1.f : 0.f, 18.f);
        if (expand > 0.02f) {
            const int first = fr.d->VtxBuffer.Size;
            nav(1, u8"", "Players", 120, Page::Players, st.page == Page::Players, 14);
            nav(2, u8"", "World", 156, Page::World, st.page == Page::World, 14);
            for (int i = first; i < fr.d->VtxBuffer.Size; ++i) {
                ImDrawVert& v = fr.d->VtxBuffer[i];
                const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 255u;
                v.col = (v.col & 0xffffffu) | ((ImU32)(a * expand) << IM_COL32_A_SHIFT);
            }
        }
        const float shift = 72.f * expand;
        nav(3, u8"", "Skins", 120 + shift, Page::Skins, st.page == Page::Skins);
        nav(4, u8"", "Miscellaneous", 156 + shift, Page::Misc, st.page == Page::Misc);
        nav(5, u8"", "Settings", 192 + shift, Page::Settings, st.page == Page::Settings);

        // the clock where the account sits in the original
        time_t now = time(nullptr);
        tm* local = localtime(&now);
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);
        rect(7, 531, 140, 38, C(39, 43, 54, 150), 6);
        fr.d->AddCircleFilled(P(28, 550), Z(14), C(24, 36, 64));
        text_mid(22, 531, 38, C(94, 185, 255), "M", kBody, bold_font());
        text(43, 535, C(225, 227, 233), "Local time", kControl);
        text(43, 551, C(111, 116, 128), clock, kSmall);
    }

    const char* page_title() {
        switch (st.page) {
            case Page::Players: return "Players";
            case Page::World: return "World";
            case Page::Skins: return "Skins";
            case Page::Misc: return "Miscellaneous";
            case Page::Settings: return "Settings";
        }
        return "";
    }

    const char* page_icon() {
        switch (st.page) {
            case Page::Players: return u8"";
            case Page::World: return u8"";
            case Page::Skins: return u8"";
            case Page::Misc: return u8"";
            case Page::Settings: return u8"";
        }
        return "";
    }

    void toolbar() {
        rect(158, 0, 590, 56, C(11, 13, 20, 225), 14, ImDrawFlags_RoundCornersTopRight);
        line(158, 56, 748, 56, C(27, 30, 39));
        rect(169, 14, 166, 30, C(17, 19, 27), 6);
        outline(169, 14, 166, 30, C(28, 31, 41), 6);
        icon(181, 14, 30, C(194, 197, 206), page_icon(), kIcon);
        text_mid(217, 14, 30, C(184, 187, 197), page_title(), kControl);
        chevron(317, 25, C(130, 135, 146));
        // exit: the first tap arms it, the second one (within 3 s) leaves
        {
            const float now = float(ImGui::GetTime());
            const bool armed = now - g_exit_armed < 3.f;
            bool hover = false;
            ImGui::PushID("##exit");
            const bool clicked = hit("##exit", 664, 14, 72, 30, &hover);
            const float r = motion(ImGui::GetID("##exit_m"), armed ? 1.f : hover ? 0.5f : 0.f, 18.f);
            ImGui::PopID();
            if (clicked) {
                if (armed) g_exit = true;
                else g_exit_armed = now;
            }
            const ImU32 red = C(255, 112, 135);
            rect(664, 14, 72, 30, mix(C(17, 19, 27), C(58, 24, 34), r), 6);
            outline(664, 14, 72, 30, mix(C(28, 31, 41), red, r * 0.8f), 6);
            const char* label = armed ? "Confirm" : "Exit";
            const float lw = text_width(label, kControl);
            if (nlIcons) {
                icon(675, 14, 30, mix(C(184, 187, 197), red, r), u8"\uf011", kIcon);
                text_mid(695 + (36 - lw) * 0.5f - 4, 14, 30, mix(C(184, 187, 197), red, r), label, armed ? 12.f : kControl);
            } else {
                text_mid(664 + (72 - lw) * 0.5f, 14, 30, mix(C(184, 187, 197), red, r), label, kControl);
            }
        }
    }

    void page_content() {
        cols = Columns{};
        ImGui::PushID(int(st.page));
        switch (st.page) {
            case Page::Players: players_page(); break;
            case Page::World: world_page(); break;
            case Page::Skins: skins_page(); break;
            case Page::Misc: misc_page(); break;
            case Page::Settings: settings_page(); break;
        }
        ImGui::PopID();
        st.content_h = ImMax(cols.y[0], cols.y[1]) + 12.f;   // design units, scroll excluded
    }
}

bool exit_requested() { return g_exit; }

void render() {
    bar::render();

    static float ma = 0.f;
    ImGuiIO& io = ImGui::GetIO();
    const float dt = io.DeltaTime;
    ma += ((bar::g_open ? 1.f : 0.f) - ma) * ImClamp(12.f * dt, 0.f, 1.f);
    if (ma < 0.01f) { st.popup.open = false; st.color.open = false; return; }
    const float game = bar::game_alpha();
    const float alpha = ma * game;

    // Setting the jump button: the menu steps aside so the game's own button is in view; the next finger is the one.
    if (bhop::g_calibrating) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const char* line1 = "Tap and hold your jump button";
        const char* line2 = "the menu comes back by itself";
        const float size = ImClamp(g_sw * 0.02f, 18.f, 40.f);
        ImFont* f = bold_font();
        const ImVec2 t1 = f->CalcTextSizeA(size, FLT_MAX, 0.f, line1), t2 = text_font()->CalcTextSizeA(size * 0.7f, FLT_MAX, 0.f, line2);
        const float w = ImMax(t1.x, t2.x) + size * 2.f, h = t1.y + t2.y + size * 1.6f;
        const ImVec2 a((g_sw - w) * 0.5f, g_sh * 0.06f), b(a.x + w, a.y + h);
        fg->AddRectFilled(a, b, C(13, 15, 22, 235), size * 0.6f);
        fg->AddRect(a, b, C(75, 126, 255, 200), size * 0.6f);
        fg->AddText(f, size, ImVec2(a.x + (w - t1.x) * 0.5f, a.y + size * 0.6f), C(228, 230, 236), line1);
        fg->AddText(text_font(), size * 0.7f, ImVec2(a.x + (w - t2.x) * 0.5f, a.y + size * 0.6f + t1.y + size * 0.3f), C(150, 154, 166), line2);
        st.popup.open = false;
        st.color.open = false;
        return;
    }

    // The dim behind the menu.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(g_sw, g_sh), IM_COL32(0, 0, 0, int(130 * alpha)));

    fr.s = ImClamp(ImMin(g_sw * 0.62f / kShell.x, g_sh * 0.86f / kShell.y) * cfg::gui::scale, 0.6f, 3.f);
    const ImVec2 size = kShell * fr.s;
    if (st.pos.x < 0.f) st.pos = ImVec2((g_sw - size.x) * 0.5f, (g_sh - size.y) * 0.5f);
    st.pos.x = ImClamp(st.pos.x, 40.f - size.x, g_sw - 40.f);
    st.pos.y = ImClamp(st.pos.y, 0.f, g_sh - 40.f);

    ImGui::SetNextWindowPos(st.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;   // a tap on the menu must not lift it over its own popups
    if (ImGui::Begin("##nl_menu", nullptr, flags)) {
        fr.origin = ImGui::GetWindowPos();
        fr.d = ImGui::GetWindowDrawList();
        const bool animating = ma < 0.98f;
        fr.input = !animating && !st.popup.open && !st.color.open;
        const int shell_first = fr.d->VtxBuffer.Size;

        // soft shadow, then the shell
        for (int i = 4; i >= 1; --i)
            fr.d->AddRectFilled(P(0, 0) - ImVec2(float(i * 4), float(i * 3)) * fr.s * 0.5f, P(kShell) + ImVec2(float(i * 4), float(i * 5)) * fr.s * 0.5f,
                                C(0, 0, 0, 14), Z(14 + i * 3));
        rect(0, 0, kShell.x, kShell.y, C(13, 15, 22, 232), 14);
        outline(0, 0, kShell.x, kShell.y, C(31, 34, 44), 14);

        // dragging by the toolbar
        {
            bool active = false;
            hit("##drag", 160, 2, 500, 54, nullptr, &active);
            if (active) st.pos += io.MouseDelta;
        }

        sidebar();
        toolbar();

        // the page, scrolled and clipped to the content area
        const int content_first = fr.d->VtxBuffer.Size;
        const ImVec2 cmin = P(159, 57), cmax = P(747, 575);
        ImGui::PushClipRect(cmin, cmax, true);
        page_content();
        ImGui::PopClipRect();

        // scrolling: the wheel, or a drag that starts on empty ground (not on a control)
        const float view_h = 519.f;
        const float max_scroll = ImMax(0.f, st.content_h - view_h - 10.f);
        const bool in_content = io.MousePos.x >= cmin.x && io.MousePos.x <= cmax.x && io.MousePos.y >= cmin.y && io.MousePos.y <= cmax.y;
        if (fr.input && in_content && io.MouseWheel != 0.f) st.scroll_target -= io.MouseWheel * 60.f;
        if (fr.input && in_content && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
            st.scroll_drag = true;
            st.scroll_press_y = io.MousePos.y;
            st.scroll_press_value = st.scroll_target;
        }
        if (!ImGui::IsMouseDown(0)) st.scroll_drag = false;
        if (st.scroll_drag) {
            st.scroll_target = st.scroll_press_value - (io.MousePos.y - st.scroll_press_y) / fr.s;
            st.scroll = st.scroll_target;   // follows the finger
        }
        st.scroll_target = ImClamp(st.scroll_target, 0.f, max_scroll);
        st.scroll = ImLerp(st.scroll, st.scroll_target, st.scroll_drag ? 1.f : ImClamp(16.f * dt, 0.f, 1.f));
        if (std::fabs(st.scroll - st.scroll_target) < 0.2f) st.scroll = st.scroll_target;
        if (max_scroll > 0.f) {   // a thin mark on the right edge
            const float track = view_h - 16.f;
            const float thumb = ImMax(30.f, track * view_h / (view_h + max_scroll));
            const float ty = 65.f + (track - thumb) * (st.scroll / max_scroll);
            rect(742.f, ty, 2.f, thumb, C(110, 120, 150, 110), 1);
        }

        // the page changing: slides in and fades in
        st.page_mix = ImLerp(st.page_mix, 1.f, 1.f - std::exp(-15.f * dt));
        const float pe = 1.f - std::pow(1.f - st.page_mix, 3.f);
        for (int i = content_first; i < fr.d->VtxBuffer.Size; ++i) {
            ImDrawVert& v = fr.d->VtxBuffer[i];
            v.pos += ImVec2(9.f * (1.f - pe) * fr.s, 0.f);
            const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 255u;
            v.col = (v.col & 0xffffffu) | ((ImU32)(a * pe) << IM_COL32_A_SHIFT);
        }

        // opening: the shell grows from 92 % and fades in
        const float eased = 1.f - std::pow(1.f - ma, 4.f);
        const ImVec2 pivot = fr.origin + size * 0.5f;
        const float grow = ImLerp(0.92f, 1.f, eased);
        for (int i = shell_first; i < fr.d->VtxBuffer.Size; ++i) {
            ImDrawVert& v = fr.d->VtxBuffer[i];
            if (animating) v.pos = pivot + (v.pos - pivot) * grow + ImVec2(0.f, (1.f - eased) * 16.f);
            const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 255u;
            v.col = (v.col & 0xffffffu) | ((ImU32)(a * ma * game) << IM_COL32_A_SHIFT);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    select_popup(alpha);
    color_popup(alpha);
}

}  // namespace ui::menu
