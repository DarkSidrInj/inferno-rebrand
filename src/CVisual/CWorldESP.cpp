#include "CVisualManager.hpp"
#include "../CGame/CGameContext.hpp"
#include "../CMath/CGameMath.hpp"
#include "../CGame/CPlayer.hpp"
#include "../CGame/CSkeleton.hpp"
#include "../CGame/CAnimCull.hpp"
#include "../CGame/CWorld.hpp"
#include "../CMenu/Theme/CTheme.hpp"
#include "../CConfigs/CConfig.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

extern ImFont* espFont;

namespace {
    using namespace skeleton;

    struct Link { int a, b; };

    ImU32 tint(const ImVec4& c, float alpha) {
        return IM_COL32(int(c.x * 255.f), int(c.y * 255.f), int(c.z * 255.f), int(std::clamp(c.w * alpha, 0.f, 1.f) * 255.f));
    }

    // A pose that is no longer refreshed fades out instead of cutting off.
    float pose_alpha(const Pose& pose) {
        if (pose.live) return 1.f;
        const float age = float(diag::now_ms() - pose.stamp) / 1000.f;
        return std::clamp(1.f - (age - .25f) / .75f, 0.f, 1.f);
    }

    // Overlay chams: the body drawn on screen as filled limbs along the bones, so it shows through
    // walls whatever the renderer does. Each limb is a capsule whose radius is a body-part width
    // in meters, projected at the limb's depth.
    void draw_overlay(ImDrawList* dl, const Pose& pose, const matrix& view, const matrix& projection) {
        const float alpha = pose_alpha(pose);
        if (alpha <= 0.f) return;
        const float* m = reinterpret_cast<const float*>(&view);
        const Vector3 up(m[1], m[5], m[9]);  // camera up, from the view matrix
        auto project = [&](int bone, float radius, ImVec2& at, float& r) {
            if (!pose.valid[bone] || !world_to_screen(pose.position[bone], view, projection, at)) return false;
            ImVec2 edge;
            if (!world_to_screen(pose.position[bone] + up * radius, view, projection, edge)) return false;
            r = std::max(1.f, std::hypot(edge.x - at.x, edge.y - at.y));
            return r < 400.f;
        };
        const ImU32 fill = tint(cfg::chams::overlay_col, alpha);
        const int upper = pose.valid[Chest] ? Chest : (pose.valid[Spine] ? Spine : Neck);
        struct Limb { int a, b; float ra, rb; };
        const Limb limbs[] = {
            {Hips, upper, .15f, .17f}, {upper, Neck, .15f, .06f},
            {LUpperLeg, LLowerLeg, .085f, .065f}, {RUpperLeg, RLowerLeg, .085f, .065f},
            {LLowerLeg, LFoot, .065f, .05f}, {RLowerLeg, RFoot, .065f, .05f},
            {LFoot, LToes, .05f, .04f}, {RFoot, RToes, .05f, .04f},
            {Hips, LUpperLeg, .1f, .09f}, {Hips, RUpperLeg, .1f, .09f},
            {LShoulder, LUpperArm, .06f, .055f}, {RShoulder, RUpperArm, .06f, .055f},
            {LUpperArm, LLowerArm, .055f, .045f}, {RUpperArm, RLowerArm, .055f, .045f},
            {LLowerArm, LHand, .045f, .04f}, {RLowerArm, RHand, .045f, .04f},
            {upper, LShoulder, .08f, .06f}, {upper, RShoulder, .08f, .06f},
        };
        for (const Limb& l : limbs) {
            ImVec2 a, b;
            float ra = 0.f, rb = 0.f;
            if (!project(l.a, l.ra, a, ra) || !project(l.b, l.rb, b, rb)) continue;
            // Tapered capsule: quad between the two end circles, plus the circles.
            const float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
            if (len > .5f) {
                const float nx = -dy / len, ny = dx / len;
                dl->AddQuadFilled(ImVec2(a.x + nx * ra, a.y + ny * ra), ImVec2(b.x + nx * rb, b.y + ny * rb),
                                  ImVec2(b.x - nx * rb, b.y - ny * rb), ImVec2(a.x - nx * ra, a.y - ny * ra), fill);
            }
            dl->AddCircleFilled(a, ra, fill, 16);
            dl->AddCircleFilled(b, rb, fill, 16);
        }
        ImVec2 head;
        float rh = 0.f;
        if (project(Head, .12f, head, rh)) {
            // The head bone sits at the skull base: the skull is centered a bit above it.
            ImVec2 top;
            if (pose.valid[Head] && world_to_screen(pose.position[Head] + up * .09f, view, projection, top)) head = top;
            dl->AddCircleFilled(head, rh, fill, 24);
        }
    }

    void draw_pose(ImDrawList* dl, const Pose& pose, const matrix& view, const matrix& projection) {
        ImVec2 screen[kCount];
        bool visible[kCount];
        for (int i = 0; i < kCount; ++i)
            visible[i] = pose.valid[i] && world_to_screen(pose.position[i], view, projection, screen[i]);

        const int upper = pose.valid[Chest] ? Chest : (pose.valid[Spine] ? Spine : Neck);
        std::vector<Link> links = {
            {Head, Neck}, {Neck, upper}, {upper, Spine}, {Spine, Hips},
            {upper, LShoulder}, {LShoulder, LUpperArm}, {LUpperArm, LLowerArm}, {LLowerArm, LHand},
            {upper, RShoulder}, {RShoulder, RUpperArm}, {RUpperArm, RLowerArm}, {RLowerArm, RHand},
            {Hips, LUpperLeg}, {LUpperLeg, LLowerLeg}, {LLowerLeg, LFoot}, {LFoot, LToes},
            {Hips, RUpperLeg}, {RUpperLeg, RLowerLeg}, {RLowerLeg, RFoot}, {RFoot, RToes},
        };
        if (!pose.valid[Spine]) links.push_back({upper, Hips});

        const float alpha = pose_alpha(pose);
        if (alpha <= 0.f) return;
        const float width = std::max(1.f, cfg::esp::skeleton_thickness);
        const ImU32 outline = IM_COL32(0, 0, 0, int(190.f * alpha));
        const ImU32 bone = tint(cfg::esp::skeleton_col, alpha);

        if (cfg::esp::skeleton_all && pose.nodes.size() > 1) {
            // Whole rig: every transform below the hips, each linked to its parent.
            std::vector<ImVec2> rig(pose.nodes.size());
            std::vector<char> rig_visible(pose.nodes.size());
            for (size_t i = 0; i < pose.nodes.size(); ++i)
                rig_visible[i] = world_to_screen(pose.nodes[i].position, view, projection, rig[i]);
            for (int pass = 0; pass < 2; ++pass) {
                const ImU32 color = pass == 0 ? outline : bone;
                const float line = pass == 0 ? width + 2.2f : width;
                for (size_t i = 0; i < pose.nodes.size(); ++i) {
                    const int parent = pose.nodes[i].parent;
                    if (parent < 0 || !rig_visible[i] || !rig_visible[size_t(parent)]) continue;
                    dl->AddLine(rig[i], rig[size_t(parent)], color, line);
                }
            }
        } else {
            // Outline first, so no dark seam cuts the joints of the coloured pass.
            for (const Link& link : links) {
                if (link.a == link.b || !visible[link.a] || !visible[link.b]) continue;
                dl->AddLine(screen[link.a], screen[link.b], outline, width + 2.2f);
            }
            for (const Link& link : links) {
                if (link.a == link.b || !visible[link.a] || !visible[link.b]) continue;
                dl->AddLine(screen[link.a], screen[link.b], bone, width);
            }
            for (int i = 0; i < kCount; ++i)
                if (visible[i] && i != Head) dl->AddCircleFilled(screen[i], width * .75f, bone, 10);
        }

        // Head ring, sized by the neck-to-head distance on screen.
        if (visible[Head] && visible[Neck]) {
            const float dx = screen[Head].x - screen[Neck].x, dy = screen[Head].y - screen[Neck].y;
            const float radius = std::clamp(std::sqrt(dx * dx + dy * dy) * .9f, 3.f, 16.f);
            dl->AddCircle(screen[Head], radius, outline, 0, width + 2.2f);
            dl->AddCircle(screen[Head], radius, bone, 0, width);
        }
    }

    void draw_item(ImDrawList* dl, const matrix& view, const matrix& projection, const Vector3& local,
                   const world::Item& item, const ImVec4& color) {
        const float distance = calculate_distance(item.position, local);
        if (distance > cfg::esp::world_distance) return;
        ImVec2 point;
        if (!world_to_screen(item.position, view, projection, point)) return;

        const ImU32 fill = tint(color, 1.f);
        dl->AddCircle(point, 5.5f, IM_COL32(0, 0, 0, 200), 16, 3.f);
        dl->AddCircle(point, 5.5f, fill, 16, 1.5f);
        if (!espFont) return;

        char scratch[24];
        const char* label = world::weapon_label(item.weapon, scratch, sizeof(scratch));
        char text[48];
        std::snprintf(text, sizeof(text), "%s  %dm", label, int(distance));
        const float size = std::clamp(420.f / (distance + 8.f), 11.f, 16.f);
        const ImVec2 extent = espFont->CalcTextSizeA(size, FLT_MAX, 0.f, text);
        visuals::draw_text_outlined(dl, espFont, size, ImVec2(std::round(point.x - extent.x * .5f), std::round(point.y - extent.y - 8.f)), fill, text);
    }
}

void visuals::draw_skeletons(const matrix& view, const matrix& projection, const std::vector<uint64_t>& enemies) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    skeleton::g_all_bones = cfg::esp::skeleton_all;
    for (uint64_t enemy : enemies) {
        const Vector3 feet = player::position(enemy);
        Pose pose;
        if (!skeleton::get(enemy, feet, pose)) continue;
        if (cfg::chams::enabled && cfg::chams::overlay) draw_overlay(dl, pose, view, projection);
        if (cfg::esp::skeleton) draw_pose(dl, pose, view, projection);
    }
    skeleton::prune(diag::now_ms());

    static long long log_stamp = 0;
    if (diag::every(log_stamp, 5000)) {
        size_t live = 0;
        for (const auto& entry : skeleton::g_cache) if (entry.second.pose.live) ++live;
        diag::log("SKELETON", "enemies=%zu cached=%zu live=%zu dictOffset=+0x%llx", enemies.size(),
                  skeleton::g_cache.size(), live, (unsigned long long)skeleton::g_dict_offset);
        // Where each tracked player stopped: the stage names the first thing that failed.
        std::map<std::string, int> stages;
        for (const auto& entry : skeleton::g_cache) ++stages[entry.second.stage];
        for (const auto& stage : stages) diag::log("SKELETON", "  %d player(s): %s", stage.second, stage.first.c_str());
    }
}

void visuals::draw_world_items(const matrix& view, const matrix& projection, const Vector3& local_position) {
    if (!cfg::esp::grenade && !cfg::esp::dropped) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    size_t grenades = 0, drops = 0;
    if (cfg::esp::grenade) {
        for (const world::Item& item : world::grenades()) {
            draw_item(dl, view, projection, local_position, item, cfg::esp::grenade_col);
            ++grenades;
        }
    }
    if (cfg::esp::dropped) {
        for (const world::Item& item : world::drops()) {
            draw_item(dl, view, projection, local_position, item, cfg::esp::dropped_col);
            ++drops;
        }
    }
    static long long log_stamp = 0;
    if (diag::every(log_stamp, 5000))
        diag::log("WORLD", "entities=%zu grenades=%zu drops=%zu classNameOffset=%d", world::entities().size(),
                  grenades, drops, klass::g_name_offset);
}
