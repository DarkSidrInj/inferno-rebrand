#include "CVisualManager.hpp"
#include "../CGame/CGameContext.hpp"
#include "../CMath/CGameMath.hpp"
#include "../CGame/CPlayer.hpp"
#include "../CGame/CCamera.hpp"
#include "../CMenu/Theme/CTheme.hpp"
#include "../CConfigs/CConfig.hpp"
#include "CFog.hpp"
#include "../CProtection/COxorany.hpp"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <unordered_map>

extern ImFont* espFont;

static void esp_wait(const char* stage) {
    static long long last = 0;
    if (diag::every(last, 5000)) diag::log("WAIT", "ESP blocked at: %s", stage);
}

void visuals::draw() {
    bool any = cfg::esp::box || cfg::esp::name || cfg::esp::health || cfg::esp::distance ||
               cfg::esp::skeleton || cfg::esp::grenade || cfg::esp::dropped;
    if (!any) { esp_wait("all ESP options disabled"); return; }

    std::vector<uint64_t> Players = players();
    if (Players.empty()) { esp_wait("player discovery / registry scan"); return; }
    struct name_cache_entry { uint32_t id{}; std::string name; long long refreshed{}; };
    static std::unordered_map<uint64_t, name_cache_entry> name_cache;
    static size_t name_cursor = 0;
    // Resolve at most one name per frame. Name discovery touches several
    // component objects, so batching all ten players caused visible UI stalls.
    {
        const uint64_t object = Players[name_cursor++ % Players.size()];
        const uint32_t id = rpm<uint32_t>(object + v100::net_object_id);
        name_cache_entry& entry = name_cache[object];
        const long long now = diag::now_ms();
        if (entry.id != id || now - entry.refreshed > 5000) {
            entry.id = id;
            entry.name = player_name(object);
            entry.refreshed = now;
        }
    }
    static long long object_log = 0;
    if (diag::every(object_log, 10000)) {
        for (size_t i = 0; i < Players.size(); ++i) {
            const uint64_t object = Players[i];
            const Vector3 position = player::position(object);
            std::string name_text = name_cache[object].name;
            if (name_text.empty()) name_text = "<name unavailable>";
            diag::log("PLAYER", "[%zu] object=0x%llx id=%u name=%s local=%d team=%d hp=%d pos=(%.3f %.3f %.3f)",
                      i, (unsigned long long)object, rpm<uint32_t>(object + v100::net_object_id),
                      name_text.c_str(), player::local(object) ? 1 : 0, player::team(object),
                      player::health(object), position.x, position.y, position.z);
        }
    }
    uint64_t LocalPlayer = 0;
    for (uint64_t p : Players) if (player::local(p)) { LocalPlayer = p; break; }
    if (!LocalPlayer) { esp_wait("local player flag"); return; }
    Vector3 LocalPosition = player::position(LocalPlayer);
    int LocalTeam = player::team(LocalPlayer);
    if (LocalTeam != 1 && LocalTeam != 2) { esp_wait("local team validation"); return; }
    if (!std::isfinite(LocalPosition.x) ||
        (LocalPosition.x == 0.f && LocalPosition.y == 0.f && LocalPosition.z == 0.f)) {
        esp_wait("local transform hierarchy"); return;
    }
    int enemies = 0, alive = 0, positioned = 0, projected = 0;
    struct enemy_frame {
        uint64_t object{};
        Vector3 position{};
        int health{};
        float distance{};
    };
    std::vector<enemy_frame> frame_enemies;
    for (uint64_t Player : Players) {
        if (!Player || Player == LocalPlayer) continue;
        int PlayerTeam = player::team(Player);
        if (PlayerTeam == LocalTeam || (PlayerTeam != 1 && PlayerTeam != 2)) continue;
        ++enemies;

        Vector3 PlayerPosition = player::position(Player);
        if (PlayerPosition.x == 0.f && PlayerPosition.y == 0.f && PlayerPosition.z == 0.f) continue;
        ++positioned;

        int Health = player::health(Player);
        if (Health <= 0) continue;
        ++alive;

        float Distance = calculate_distance(PlayerPosition, LocalPosition);
        if (Distance > 500.f) continue;

        frame_enemies.push_back({Player, PlayerPosition, Health, Distance});
    }

    // The game's own camera (GameCamera holder -> UnityEngine.Camera -> native Camera) carries
    // the live matrices: shot animation, shake and the scoped FOV. Use it directly. The
    // candidate scoring below is only the fallback when the holder is not found.
    matrix ViewMatrix{}, ProjectionMatrix{};
    game_camera::Reading GameCamera;
    if (game_camera::acquire(Players, LocalPlayer, GameCamera)) {
        ViewMatrix = GameCamera.view;
        ProjectionMatrix = GameCamera.projection;
        if (g_camera != GameCamera.native)
            diag::log("CAMERA", "selected game camera native=0x%llx holder=0x%llx", (unsigned long long)GameCamera.native,
                      (unsigned long long)GameCamera.holder);
        g_camera = GameCamera.native;
        static long long source_log = 0;
        if (diag::every(source_log, 5000))
            diag::log("CAMERA", "source=game camera holder fov=%.1f (live, follows scope)", game_camera::fov_degrees(ProjectionMatrix));
    } else {
        // Do not trust the first camera-shaped native object. Unity keeps scene,
        // UI, preview and stale Camera instances alive together. Score every
        // validated candidate against the live player coordinates and select the
        // one whose view/projection pair actually places bodies on this display.
        player::g_fallback_fov = std::clamp(cfg::esp::fallback_fov, 20.f, 120.f);
        std::vector<camera_candidate> Cameras = camera_candidates(LocalPlayer);
        // In 1.0.0 the gameplay Camera is no longer reachable through the
        // Transform/GameObject component list. The local ios controller still
        // owns the live camera transforms, so build a view matrix from their
        // hierarchy instead of falling back to a multi-gigabyte heap scan.
        std::vector<camera_candidate> TransformCameras =
            player::camera_fallback_candidates(LocalPlayer);
        Cameras.insert(Cameras.end(), TransformCameras.begin(), TransformCameras.end());
        if (Cameras.empty()) { esp_wait("native / transform camera resolution"); return; }

        uint64_t selected_camera = 0;
        bool selected_synthetic = false;
        int selected_score = -1, selected_front = 0, selected_onscreen = 0;
        float selected_camera_distance = 1000000.f;
        static long long candidate_log = 0;
        const bool log_candidates = diag::every(candidate_log, 5000);
        for (const auto& Camera : Cameras) {
            int score = 0, front = 0, onscreen = 0;
            const float* view = reinterpret_cast<const float*>(&Camera.view);
            const Vector3 camera_position(
                -(view[0] * view[12] + view[1] * view[13] + view[2] * view[14]),
                -(view[4] * view[12] + view[5] * view[13] + view[6] * view[14]),
                -(view[8] * view[12] + view[9] * view[13] + view[10] * view[14]));
            const float camera_distance = calculate_distance(camera_position, LocalPosition);
            if (camera_distance < 2.f) score += 100;
            else if (camera_distance < 10.f) score += 80;
            else if (camera_distance < 30.f) score += 30;
            const float* projection = reinterpret_cast<const float*>(&Camera.projection);
            if (std::fabs(projection[11]) > .5f && std::fabs(projection[15]) < .1f) score += 30;
            for (const auto& Enemy : frame_enemies) {
                Vector3 Head(Enemy.position.x, Enemy.position.y + 1.67f, Enemy.position.z);
                ImVec2 screen_head, screen_foot;
                if (!world_to_screen(Head, Camera.view, Camera.projection, screen_head) ||
                    !world_to_screen(Enemy.position, Camera.view, Camera.projection, screen_foot)) continue;
                if (!std::isfinite(screen_head.x) || !std::isfinite(screen_head.y) ||
                    !std::isfinite(screen_foot.x) || !std::isfinite(screen_foot.y)) continue;
                ++front;
                ++score;
                const bool head_inside = screen_head.x >= -g_sw * .25f && screen_head.x <= g_sw * 1.25f &&
                                         screen_head.y >= -g_sh * .25f && screen_head.y <= g_sh * 1.25f;
                const bool foot_inside = screen_foot.x >= -g_sw * .25f && screen_foot.x <= g_sw * 1.25f &&
                                         screen_foot.y >= -g_sh * .25f && screen_foot.y <= g_sh * 1.25f;
                const float body_height = std::fabs(screen_foot.y - screen_head.y);
                if (head_inside && foot_inside) { ++onscreen; score += 6; }
                if (body_height >= 2.f && body_height <= g_sh * 1.5f) score += 3;
            }
            // The game's own Camera has the live view (shot animation, shake) and projection
            // (scoped FOV). Prefer it over the synthetic camera unless it projects nothing.
            if (!Camera.synthetic && front > 0) score += 40;
            // Keep the current winner on equal scores to avoid camera flicker.
            if (score > selected_score || (score == selected_score && Camera.address == g_camera)) {
                selected_synthetic = Camera.synthetic;
                selected_score = score;
                selected_front = front;
                selected_onscreen = onscreen;
                selected_camera_distance = camera_distance;
                selected_camera = Camera.address;
                ViewMatrix = Camera.view;
                ProjectionMatrix = Camera.projection;
            }
            if (log_candidates)
                diag::log("CAMERA", "candidate=0x%llx score=%d front=%d onscreen=%d localDistance=%.2f proj=(%.3f %.3f)",
                          (unsigned long long)Camera.address, score, front, onscreen, camera_distance,
                          Camera.projection.m11, Camera.projection.m22);
        }
        if (!selected_camera || (selected_camera_distance >= 30.f && selected_onscreen == 0)) {
            g_camera = 0;
            esp_wait("camera candidates do not project players");
            return;
        }
        if (g_camera != selected_camera) {
            diag::log("CAMERA", "selected=0x%llx score=%d front=%d onscreen=%d localDistance=%.2f candidates=%zu",
                      (unsigned long long)selected_camera, selected_score, selected_front,
                      selected_onscreen, selected_camera_distance, Cameras.size());
        }
        g_camera = selected_camera;
        {
            static long long source_log = 0;
            if (diag::every(source_log, 5000)) {
                const float p5 = ProjectionMatrix.m22;
                const float fov = p5 > 0.001f ? 2.f * std::atan(1.f / p5) * 57.29578f : 0.f;
                diag::log("CAMERA", "source=%s fov=%.1f%s", selected_synthetic ? "synthetic(ios transforms)" : "native Camera",
                          fov, selected_synthetic ? " (fixed, does not follow scope)" : " (live)");
            }
        }

    }

    for (const auto& Enemy : frame_enemies) {
        const uint64_t Player = Enemy.object;
        const Vector3 PlayerPosition = Enemy.position;
        const int Health = Enemy.health;
        const float Distance = Enemy.distance;

        Vector3 HeadPosition(PlayerPosition.x, PlayerPosition.y + 1.67f, PlayerPosition.z);

        ImVec2 ScreenHead, ScreenFoot;
        bool HeadVisible = world_to_screen(HeadPosition, ViewMatrix, ProjectionMatrix, ScreenHead);
        bool FootVisible = world_to_screen(PlayerPosition, ViewMatrix, ProjectionMatrix, ScreenFoot);
        if (!HeadVisible || !FootVisible) continue;
        ++projected;

        float x1 = roundf(ScreenHead.x);
        float y1 = roundf(fminf(ScreenHead.y, ScreenFoot.y));
        float x2 = roundf(ScreenFoot.x);
        float y2 = roundf(fmaxf(ScreenHead.y, ScreenFoot.y));

        float bh = fabsf(y2 - y1);
        float bw = roundf(bh * 0.25f);
        float cx = roundf((x1 + x2) * 0.5f);

        ImVec2 BoxMin(cx - bw, y1);
        ImVec2 BoxMax(cx + bw, y2);

        float inv_dist = 1.f / (Distance + 0.1f);
        float font_sz = std::clamp(400.f * inv_dist, 10.f, 18.f);
        distfog::g_factor = distfog::factor_for(Distance);

        if (cfg::esp::box) {
            dbox(BoxMin, BoxMax, 1.f);
        }

        if (cfg::esp::health) {
            dhp(Health, BoxMin, BoxMax, bh, 1.f);
        }

        if (cfg::esp::name) {
            const auto found = name_cache.find(Player);
            const char* label = found != name_cache.end() && !found->second.name.empty()
                ? found->second.name.c_str() : "Enemy";
            dnick(label, BoxMax, cx, font_sz, 1.f);
        }

        if (cfg::esp::distance) {
            ddist(Distance, BoxMax.x, BoxMin.y, font_sz, 1.f);
        }
    }
    distfog::g_factor = 0.f;
    std::vector<uint64_t> enemy_objects;
    enemy_objects.reserve(frame_enemies.size());
    for (const auto& Enemy : frame_enemies) enemy_objects.push_back(Enemy.object);
    if (cfg::esp::skeleton) draw_skeletons(ViewMatrix, ProjectionMatrix, enemy_objects);
    draw_world_items(ViewMatrix, ProjectionMatrix, LocalPosition);

    static long long frame_log = 0;
    if (diag::every(frame_log, 5000)) {
        diag::log(projected ? "INFO" : "WARN",
                  "ESP frame players=%zu local=0x%llx team=%d pos=(%.2f %.2f %.2f) enemies=%d positioned=%d alive=%d projected=%d camera=0x%llx",
                  Players.size(), (unsigned long long)LocalPlayer, LocalTeam,
                  LocalPosition.x, LocalPosition.y, LocalPosition.z, enemies, positioned, alive,
                  projected, (unsigned long long)g_camera);
    }
}

void visuals::dbox(const ImVec2& t, const ImVec2& b, float a) {
    if (a < 0.01f) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    float x1 = t.x, y1 = t.y, x2 = b.x, y2 = b.y;
    float r = cfg::esp::box_rounding;

    ImU32 col = distfog::col(cfg::esp::box_col, a);

    if (cfg::esp::box_type == 0) {
        dl->AddRect(ImVec2(x1 + 2.f, y1 + 2.f), ImVec2(x2 + 2.f, y2 + 2.f), IM_COL32(0, 0, 0, (int)(100 * a)), r, 0, 2.f);
        dl->AddRect(ImVec2(x1 - 1.f, y1 - 1.f), ImVec2(x2 + 1.f, y2 + 1.f), IM_COL32(0, 0, 0, (int)(180 * a)), r, 0, 1.f);
        dl->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), col, r, 0, 1.f);
    } else {
        float w = x2 - x1, h = y2 - y1;
        float sz = std::min(w, h) * 0.25f;
        float cr = std::min(r, sz * 0.5f);

        ImU32 s1 = IM_COL32(0, 0, 0, (int)(100 * a));
        ImU32 s2 = IM_COL32(0, 0, 0, (int)(180 * a));

        auto corner = [&](float cx, float cy, float dx, float dy) {
            if (cr > 0.5f) {
                float arc_cx = cx + dx * cr;
                float arc_cy = cy + dy * cr;

                float angle_start, angle_end;
                if (dx > 0 && dy > 0) { angle_start = 3.14159265f; angle_end = 4.71238898f; }
                else if (dx < 0 && dy > 0) { angle_start = 4.71238898f; angle_end = 6.28318530f; }
                else if (dx > 0 && dy < 0) { angle_start = 1.57079632f; angle_end = 3.14159265f; }
                else { angle_start = 0.f; angle_end = 1.57079632f; }

                dl->PathArcTo(ImVec2(arc_cx + 2.f * (dx > 0 ? 1 : -1), arc_cy + 2.f * (dy > 0 ? 1 : -1)), cr, angle_start, angle_end, 8);
                dl->PathStroke(s1, 0, 2.f);
                dl->AddLine(ImVec2(cx + dx * cr, cy + 2.f * (dy > 0 ? 1 : -1)), ImVec2(cx + dx * sz, cy + 2.f * (dy > 0 ? 1 : -1)), s1, 2.f);
                dl->AddLine(ImVec2(cx + 2.f * (dx > 0 ? 1 : -1), cy + dy * cr), ImVec2(cx + 2.f * (dx > 0 ? 1 : -1), cy + dy * sz), s1, 2.f);

                dl->PathArcTo(ImVec2(arc_cx, arc_cy), cr, angle_start, angle_end, 8);
                dl->PathStroke(s2, 0, 1.f);
                dl->AddLine(ImVec2(cx + dx * cr, cy), ImVec2(cx + dx * sz, cy), s2, 1.f);
                dl->AddLine(ImVec2(cx, cy + dy * cr), ImVec2(cx, cy + dy * sz), s2, 1.f);

                dl->PathArcTo(ImVec2(arc_cx, arc_cy), cr, angle_start, angle_end, 8);
                dl->PathStroke(col, 0, 1.f);
                dl->AddLine(ImVec2(cx + dx * cr, cy), ImVec2(cx + dx * sz, cy), col, 1.f);
                dl->AddLine(ImVec2(cx, cy + dy * cr), ImVec2(cx, cy + dy * sz), col, 1.f);
            } else {
                dl->AddLine(ImVec2(cx + 2.f * (dx > 0 ? 1 : -1), cy + 2.f * (dy > 0 ? 1 : -1)), ImVec2(cx + dx * sz + 2.f * (dx > 0 ? 1 : -1), cy + 2.f * (dy > 0 ? 1 : -1)), s1, 2.f);
                dl->AddLine(ImVec2(cx + 2.f * (dx > 0 ? 1 : -1), cy + 2.f * (dy > 0 ? 1 : -1)), ImVec2(cx + 2.f * (dx > 0 ? 1 : -1), cy + dy * sz + 2.f * (dy > 0 ? 1 : -1)), s1, 2.f);

                dl->AddLine(ImVec2(cx, cy), ImVec2(cx + dx * sz, cy), s2, 1.f);
                dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + dy * sz), s2, 1.f);

                dl->AddLine(ImVec2(cx, cy), ImVec2(cx + dx * sz, cy), col, 1.f);
                dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + dy * sz), col, 1.f);
            }
        };

        corner(x1, y1, 1, 1);
        corner(x2, y1, -1, 1);
        corner(x1, y2, 1, -1);
        corner(x2, y2, -1, -1);
    }
}

void visuals::dhp(int hp, const ImVec2& t, const ImVec2& b, float box_h, float a) {
    if (a < 0.01f) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    hp = std::clamp(hp, 0, 100);
    float pct = hp / 100.f;

    float bx = roundf(t.x - 6.f);
    float bw = 3.f;
    float bh = roundf(b.y - t.y);
    float fh = bh * pct;

    float top_y = t.y;
    float bot_y = b.y;
    float fill_top = roundf(bot_y - fh);

    dl->AddRectFilled(ImVec2(bx - 2.f, top_y - 2.f), ImVec2(bx + bw + 2.f, bot_y + 2.f), IM_COL32(0, 0, 0, (int)(120 * a)), 1.f);
    dl->AddRectFilled(ImVec2(bx - 1.f, top_y - 1.f), ImVec2(bx + bw + 1.f, bot_y + 1.f), IM_COL32(0, 0, 0, (int)(200 * a)), 0.f);
    dl->AddRect(ImVec2(bx - 1.f, top_y - 1.f), ImVec2(bx + bw + 1.f, bot_y + 1.f), IM_COL32(30, 30, 30, (int)(255 * a)), 0, 0, 1.f);

    ImU32 col_top = IM_COL32(
        static_cast<int>(cfg::esp::health_col.x * 255),
        static_cast<int>(cfg::esp::health_col.y * 255),
        static_cast<int>(cfg::esp::health_col.z * 255),
        static_cast<int>(cfg::esp::health_col.w * 255 * a)
    );
    ImU32 col_bot = IM_COL32(
        static_cast<int>(cfg::esp::health_col.x * 180),
        static_cast<int>(cfg::esp::health_col.y * 180),
        static_cast<int>(cfg::esp::health_col.z * 180),
        static_cast<int>(cfg::esp::health_col.w * 255 * a)
    );

    if (fh > 1.f) {
        dl->AddRectFilledMultiColor(ImVec2(bx, fill_top), ImVec2(bx + bw, bot_y), col_top, col_top, col_bot, col_bot);
    }

    if (hp < 100 && espFont && bh > 20.f) {
        char txt[8];
        snprintf(txt, sizeof(txt), "%d", hp);
        float fs = 10.f;
        ImVec2 ts = espFont->CalcTextSizeA(fs, FLT_MAX, 0.f, txt);
        float tx = roundf(bx + (bw - ts.x) * 0.5f);
        float ty = roundf(fill_top - ts.y * 0.5f);
        if (ty < top_y) ty = top_y;
        if (ty + ts.y > bot_y) ty = bot_y - ts.y;
        draw_text_outlined(dl, espFont, fs, ImVec2(tx, ty), IM_COL32(255, 255, 255, (int)(255 * a)), txt);
    }
}

void visuals::dnick(const char* name, const ImVec2& box_max, float cx, float sz, float a) {
    if (!espFont || a < 0.01f || !name) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    ImVec2 ts = espFont->CalcTextSizeA(sz, FLT_MAX, 0.f, name);
    ImVec2 tp(roundf(cx - ts.x * 0.5f), roundf(box_max.y + 4.f));

    ImU32 col = distfog::col(cfg::esp::name_col, a);
    draw_text_outlined(dl, espFont, sz, tp, col, name);
}

void visuals::ddist(float dist, float x, float y, float sz, float a) {
    if (!espFont || a < 0.01f) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    char txt[16];
    snprintf(txt, sizeof(txt), "%dm", static_cast<int>(dist));

    ImVec2 tp(roundf(x + 5.f), roundf(y));

    ImU32 col = distfog::col(cfg::esp::distance_col, a);
    draw_text_outlined(dl, espFont, sz, tp, col, txt);
}

void visuals::draw_text_outlined(ImDrawList* dl, ImFont* font, float size, const ImVec2& pos, ImU32 color, const char* text) {
    if (!font || !dl) return;

    int a = (color >> IM_COL32_A_SHIFT) & 0xFF;
    int s1 = static_cast<int>(a * 0.4f);
    int s2 = static_cast<int>(a * 0.7f);

    dl->AddText(font, size, ImVec2(pos.x + 2.f, pos.y + 2.f), IM_COL32(0, 0, 0, s1), text);
    dl->AddText(font, size, ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, s2), text);
    dl->AddText(font, size, pos, color, text);
}
