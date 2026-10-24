// Host preview of the Neverlose-style menu: the real CMenu.cpp rendered by Dear ImGui into a software framebuffer
// (a PPM). Build and run: see README.md. Usage: menu_preview <page 0..4> [color|select|scroll] [out.ppm]
#include "fonts/menu/neverlose_fonts.h"
#include "CMenu/CMenu.cpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

ImFont *fontBold, *fontMedium, *fontDesc, *espFont;   // nlText / nlBold / nlIcons: weak definitions in CMenu.cpp
namespace ui::bar { void render() {} void set_game_alpha(float) {} float game_alpha() { return 1.f; } }

struct Image { int w, h; std::vector<float> px; };

static void raster(const ImDrawData* dd, Image& img, const unsigned char* tex, int tw, int th) {
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const ImVec4 clip = cmd.ClipRect;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k) v[k] = &dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset + i + k] + cmd.VtxOffset];
                float minx = std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}), maxx = std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x});
                float miny = std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}), maxy = std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y});
                int x0 = std::max(int(std::floor(minx)), int(clip.x)), x1 = std::min(int(std::ceil(maxx)), int(clip.z));
                int y0 = std::max(int(std::floor(miny)), int(clip.y)), y1 = std::min(int(std::ceil(maxy)), int(clip.w));
                x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, img.w); y1 = std::min(y1, img.h);
                const float d = (v[1]->pos.y - v[2]->pos.y) * (v[0]->pos.x - v[2]->pos.x) + (v[2]->pos.x - v[1]->pos.x) * (v[0]->pos.y - v[2]->pos.y);
                if (std::fabs(d) < 1e-9f) continue;
                for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float l0 = ((v[1]->pos.y - v[2]->pos.y) * (px - v[2]->pos.x) + (v[2]->pos.x - v[1]->pos.x) * (py - v[2]->pos.y)) / d;
                    const float l1 = ((v[2]->pos.y - v[0]->pos.y) * (px - v[2]->pos.x) + (v[0]->pos.x - v[2]->pos.x) * (py - v[2]->pos.y)) / d;
                    const float l2 = 1.f - l0 - l1;
                    if (l0 < -0.001f || l1 < -0.001f || l2 < -0.001f) continue;
                    float c[4];
                    for (int k = 0; k < 4; ++k) {
                        const int sh = k * 8;
                        c[k] = (((v[0]->col >> sh) & 255) * l0 + ((v[1]->col >> sh) & 255) * l1 + ((v[2]->col >> sh) & 255) * l2) / 255.f;
                    }
                    const float u = v[0]->uv.x * l0 + v[1]->uv.x * l1 + v[2]->uv.x * l2;
                    const float vv = v[0]->uv.y * l0 + v[1]->uv.y * l1 + v[2]->uv.y * l2;
                    int tx = std::clamp(int(u * tw), 0, tw - 1), ty = std::clamp(int(vv * th), 0, th - 1);
                    const unsigned char* t = tex + (ty * tw + tx) * 4;
                    const float a = c[3] * (t[3] / 255.f);
                    float* o = &img.px[(y * img.w + x) * 3];
                    o[0] = o[0] * (1 - a) + c[0] * (t[0] / 255.f) * a;
                    o[1] = o[1] * (1 - a) + c[1] * (t[1] / 255.f) * a;
                    o[2] = o[2] * (1 - a) + c[2] * (t[2] / 255.f) * a;
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    const int page = argc > 1 ? std::atoi(argv[1]) : 0;
    const char* mode = argc > 2 ? argv[2] : "";
    const char* out = argc > 3 ? argv[3] : "out.ppm";
    const int W = 2000, H = 1250;
    g_sw = W; g_sh = H;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(W, H);
    static const ImWchar text_ranges[] = {0x0020, 0x00FF, 0x0400, 0x04FF, 0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0};
    static const ImWchar icon_ranges[] = {0xF000, 0xF8FF, 0};
    ImFontConfig config; config.FontDataOwnedByAtlas = false;
    nlText = io.Fonts->AddFontFromMemoryTTF(nl_font_medium, sizeof(nl_font_medium), 30, &config, text_ranges);
    nlBold = io.Fonts->AddFontFromMemoryTTF(nl_font_bold, sizeof(nl_font_bold), 34, &config, text_ranges);
    nlIcons = io.Fonts->AddFontFromMemoryTTF(nl_font_icons, sizeof(nl_font_icons), 28, &config, icon_ranges);
    io.FontDefault = nlText;
    unsigned char* tex; int tw, th;
    io.Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);
    io.Fonts->SetTexID((ImTextureID)1);
    ImGui::GetStyle().ScaleAllSizes(3.0f);
    ui::bar::g_open = true;
    ui::menu::st.page = (ui::menu::Page)page;
    cfg::esp::box = true; cfg::esp::health = true; cfg::esp::armor = true; cfg::night::enabled = std::getenv("NIGHT") != nullptr; cfg::fog::enabled = std::getenv("FOG") != nullptr; cfg::strafe::enabled = true;
    for (int f = 0; f < 40; ++f) {
        io.DeltaTime = 0.05f;
        io.MousePos = ImVec2(-1000, -1000);
        io.MouseDown[0] = false;
        if (std::strcmp(mode, "select") == 0 && f >= 24 && f <= 26) { io.MousePos = ImVec2(1000, 1180); io.MouseDown[0] = f < 26; }   // taps near the bottom of the shell
        ImGui::NewFrame();
        ui::menu::world_overlay();
        ui::menu::render();
        if (f == 20 && std::strcmp(mode, "color") == 0) ui::menu::open_color(3, &cfg::esp::box_col, ImVec2(1180, 330));
        if (f == 20 && std::strcmp(mode, "select") == 0) { ui::menu::Row r; static const char* items[] = {"Flat", "Untouchable", "Outline"}; r.items = items; r.count = 3; r.cur = 1; r.pick = [](int){}; ui::menu::open_popup(4, r, ImVec2(1100, 560), 220); }
        if (f == 20 && std::strcmp(mode, "scroll") == 0) { ui::menu::st.scroll = ui::menu::st.scroll_target = 120; }
        ImGui::Render();
        if (f == 39) {
            Image img{W, H, std::vector<float>(size_t(W) * H * 3)};
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const float t = float(y) / H;
                float* o = &img.px[(y * W + x) * 3];
                o[0] = 0.25f - 0.15f * t; o[1] = 0.5f - 0.2f * t; o[2] = 0.85f - 0.3f * t;
            }
            raster(ImGui::GetDrawData(), img, tex, tw, th);
            FILE* fp = std::fopen(out, "wb");
            std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
            for (size_t i = 0; i < img.px.size(); ++i) std::fputc(int(std::clamp(img.px[i], 0.f, 1.f) * 255.f + .5f), fp);
            std::fclose(fp);
        }
        if (f == 39) {   // the order the windows are drawn in: the popups must come after the menu
            ImGuiContext& g = *ImGui::GetCurrentContext();
            std::printf("window order:");
            for (ImGuiWindow* w : g.Windows) std::printf(" %s", w->Name);
            std::printf("\n");
        }
        ImGui::EndFrame();
    }
    return 0;
}
