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

    // Overlay chams: the body drawn on screen from the bones, so it shows through walls whatever
    // the renderer does. A torso polygon (shoulders to hips), tapered capsules for neck and limbs,
    // and an ellipse-ish head, sized in meters and projected at each bone's depth. Drawn twice:
    // every part a little larger in the outline color, then every part in the fill color, so the
    // outline only shows around the whole silhouette. With an opaque fill (the default) overlaps
    // inside it do not show; a see-through fill darkens where parts overlap.
    struct Capsule { ImVec2 a, b; float ra, rb; };

    void fill_capsule(ImDrawList* dl, const Capsule& c, float grow, ImU32 color) {
        const float ra = c.ra + grow, rb = c.rb + grow;
        const float dx = c.b.x - c.a.x, dy = c.b.y - c.a.y, len = std::hypot(dx, dy);
        if (len > .5f) {
            // Tangent quad between the two end circles (outer tangents for different radii).
            const float ux = dx / len, uy = dy / len;
            const float sin_t = std::clamp((ra - rb) / len, -1.f, 1.f), cos_t = std::sqrt(1.f - sin_t * sin_t);
            const float nx = -uy * cos_t + ux * sin_t, ny = ux * cos_t + uy * sin_t;
            const float mx = uy * cos_t + ux * sin_t, my = -ux * cos_t + uy * sin_t;
            dl->AddQuadFilled(ImVec2(c.a.x + nx * ra, c.a.y + ny * ra), ImVec2(c.b.x + nx * rb, c.b.y + ny * rb),
                              ImVec2(c.b.x + mx * rb, c.b.y + my * rb), ImVec2(c.a.x + mx * ra, c.a.y + my * ra), color);
        }
        dl->AddCircleFilled(c.a, ra, color, 20);
        dl->AddCircleFilled(c.b, rb, color, 20);
    }

    void draw_overlay(ImDrawList* dl, const Pose& pose, const matrix& view, const matrix& projection,
                      const ImVec4& fill_col, const ImVec4& outline_col, float outline_px) {
        const float alpha = pose_alpha(pose);
        if (alpha <= 0.f) return;
        const float* m = reinterpret_cast<const float*>(&view);
        const Vector3 up(m[1], m[5], m[9]);  // camera up, from the view matrix
        ImVec2 at[kCount];
        float px[kCount]{};  // pixels per meter at each bone
        bool ok[kCount]{};
        for (int i = 0; i < kCount; ++i) {
            ImVec2 edge;
            if (!pose.valid[i] || !world_to_screen(pose.position[i], view, projection, at[i]) ||
                !world_to_screen(pose.position[i] + up * .1f, view, projection, edge)) continue;
            px[i] = std::hypot(edge.x - at[i].x, edge.y - at[i].y) * 10.f;
            ok[i] = px[i] > .5f && px[i] < 4000.f;
        }
        const int upper = ok[Chest] ? Chest : (ok[Spine] ? Spine : Neck);
        std::vector<Capsule> parts;
        auto limb = [&](int a, int b, float ra, float rb) {
            if (ok[a] && ok[b]) parts.push_back({at[a], at[b], ra * px[a], rb * px[b]});
        };
        limb(Neck, upper, .055f, .07f);
        limb(LUpperArm, LLowerArm, .05f, .042f);  limb(RUpperArm, RLowerArm, .05f, .042f);
        limb(LLowerArm, LHand, .042f, .035f);     limb(RLowerArm, RHand, .042f, .035f);
        limb(LUpperLeg, LLowerLeg, .08f, .058f);  limb(RUpperLeg, RLowerLeg, .08f, .058f);
        limb(LLowerLeg, LFoot, .058f, .045f);     limb(RLowerLeg, RFoot, .058f, .045f);
        limb(LFoot, LToes, .045f, .04f);          limb(RFoot, RToes, .045f, .04f);
        limb(LShoulder, LUpperArm, .055f, .05f);  limb(RShoulder, RUpperArm, .055f, .05f);

        // Torso: shoulders and hips as a convex quad, pushed out by the body's half depth.
        std::vector<ImVec2> torso;
        const int corners[4] = {LUpperArm, RUpperArm, RUpperLeg, LUpperLeg};
        bool torso_ok = true;
        for (int c : corners) torso_ok &= ok[c];
        if (torso_ok) for (int c : corners) torso.push_back(at[c]);
        if (!torso_ok) limb(Hips, upper, .14f, .16f);
        // Chest / spine / hips capsules round the torso's sides.
        limb(Hips, ok[Spine] ? Spine : upper, .12f, .12f);
        if (ok[Spine]) limb(Spine, upper, .12f, .13f);

        // Head: centered above the head bone (the bone sits at the skull base).
        Capsule head{};
        bool head_ok = false;
        if (ok[Head]) {
            ImVec2 top;
            // Along the neck-to-head direction, so the skull sits right when the body leans or lies.
            Vector3 along = pose.valid[Neck] ? pose.position[Head] - pose.position[Neck] : Vector3(0.f, .2f, 0.f);
            const float along_len = std::max(std::sqrt(along.x * along.x + along.y * along.y + along.z * along.z), .001f);
            along = along * (.2f / along_len);
            if (world_to_screen(pose.position[Head] + along, view, projection, top)) {
                const ImVec2 mid((at[Head].x + top.x) * .5f, (at[Head].y + top.y) * .5f);
                head = {at[Head], mid, .085f * px[Head], .1f * px[Head]};
                head_ok = true;
            }
        }

        auto grown_torso = [&](float grow) {
            ImVec2 center(0.f, 0.f);
            for (const ImVec2& p : torso) { center.x += p.x * .25f; center.y += p.y * .25f; }
            std::vector<ImVec2> out;
            for (const ImVec2& p : torso) {
                const float dx = p.x - center.x, dy = p.y - center.y, d = std::max(std::hypot(dx, dy), 1.f);
                out.push_back(ImVec2(p.x + dx / d * grow, p.y + dy / d * grow));
            }
            return out;
        };
        const float edge = std::max(1.5f, outline_px);
        const ImU32 outline = tint(outline_col, alpha);
        const ImU32 fill = tint(fill_col, alpha);
        for (int pass = 0; pass < 2; ++pass) {
            const float grow = pass == 0 ? edge : 0.f;
            const ImU32 color = pass == 0 ? outline : fill;
            if (pass == 0 && outline_px <= 0.f) continue;
            if (torso_ok) {
                const float side = .09f * (px[LUpperArm] + px[RUpperLeg]) * .5f;  // torso half depth on screen
                const std::vector<ImVec2> poly = grown_torso(grow + side);
                dl->AddConvexPolyFilled(poly.data(), int(poly.size()), color);
            }
            for (const Capsule& c : parts) fill_capsule(dl, c, grow, color);
            if (head_ok) fill_capsule(dl, head, grow, color);
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
        if (cfg::chams::enabled && cfg::chams::overlay)
            draw_overlay(dl, pose, view, projection, cfg::chams::overlay_col, cfg::chams::overlay_outline_col, cfg::chams::overlay_outline);
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

// Third person: the own character, drawn from its bones. Its real body renderer is not part of the
// render scene in first person (scene handle -1) and registering it needs the game's own allocator,
// so the model is drawn here, from the same pose the skeleton uses.
void visuals::draw_local_model(const matrix& view, const matrix& projection, uint64_t local) {
    if (!local) return;
    // The own model must not blink: a frame without a fresh pose keeps the last one for two seconds.
    static Pose last;
    static uint64_t last_for = 0;
    static long long last_ok = 0;
    const long long now = diag::now_ms();
    Pose pose;
    if (skeleton::get(local, player::position(local), pose)) {
        pose.live = true;
        last = pose;
        last_for = local;
        last_ok = now;
    } else if (last_for == local && now - last_ok < 2000) {
        pose = last;
        pose.live = true;
    } else {
        return;
    }
    draw_overlay(ImGui::GetBackgroundDrawList(), pose, view, projection, cfg::thirdperson::model_col,
                 cfg::thirdperson::model_outline_col, cfg::thirdperson::model_outline);
}
