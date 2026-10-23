#pragma once
// Third person. The game moves its camera every frame (nrl.LateTick), so writing the camera's
// transform from outside is a race the game wins. Unity has a switch for exactly this case:
// Camera.worldToCameraMatrix = m makes the camera keep `m` (m_ImplicitWorldToCameraMatrix =
// false) until ResetWorldToCameraMatrix. With the flag off, the matrix written here is the one
// that renders, culls and that the ESP reads; nothing the game does per frame touches it.
//
// Offsets are not guessed:
//   - set_worldToCameraMatrix_Injected (native icall by name, the managed setter is stripped) is
//     traced on the live camera: its byte store of 0 is the flag, its 64-byte store the matrix.
//   - fallback: get_worldToCameraMatrix_Injected (dump.cs wrapper, then by name); its first byte
//     read inside the camera is the flag check. The matrix is the camera_view field the ESP reads.
// Either way the result is checked live: once taken over, the matrix must stay what was written.
// If the game overwrites it, the flag is put back and third person reports itself unavailable.
//
// The eye pose comes from the game's own camera pose (GameCamera holder: nrl.irub transform, or
// the ckk pose it follows), chosen by matching the live matrix before taking over.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CIcalls.hpp"
#include "CCamera.hpp"
#include "CPlayer.hpp"
#include "CAnimCull.hpp"
#include "../CUtils/CArm64.hpp"
#include "../CConfigs/CConfig.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

namespace thirdperson {
    enum Source { None = 0, Transform = 1, State = 2, ObjectTransform = 3 };

    inline int g_pid = -1;
    inline std::atomic<bool> g_resolving{false};
    inline std::atomic<int> g_flag_offset{-1};    // m_ImplicitWorldToCameraMatrix inside the native Camera
    inline std::atomic<int> g_matrix_offset{-1};  // m_WorldToCameraMatrix
    inline std::atomic<int> g_mask_offset{-1};    // m_CullingMask
    inline std::atomic<bool> g_resolved{false};
    inline long long g_last_resolve = 0;
    inline bool g_failed = false;                  // the flag did not hold the matrix: do not try again this process

    inline uint64_t g_camera = 0;   // camera currently taken over (0 = none)
    inline uint64_t g_holder = 0;
    inline Source g_source = None;
    inline int g_index_offset = -1; // transform index offset that matched (Transform / ObjectTransform)
    inline uint64_t g_object_transform = 0;
    inline matrix g_written{};
    inline bool g_have_written = false;
    inline long long g_taken_at = 0;
    inline int g_overwrites = 0;
    inline bool g_verified = false;
    inline uint8_t g_flag_before = 1;
    inline uint32_t g_mask_before = 0;
    inline bool g_mask_set = false;
    inline long long g_calib_log = 0;

    inline float diff(const matrix& a, const matrix& b) {
        const float* x = reinterpret_cast<const float*>(&a);
        const float* y = reinterpret_cast<const float*>(&b);
        float worst = 0.f;
        for (int i = 0; i < 16; ++i) worst = std::max(worst, std::fabs(x[i] - y[i]));
        return worst;
    }

    // Unity's Quaternion.Euler: Z, then X, then Y.
    inline Vector4 euler(const Vector3& e) {
        const float k = 0.00872664626f;  // deg -> rad, halved
        const Vector4 qx(std::sin(e.x * k), 0.f, 0.f, std::cos(e.x * k));
        const Vector4 qy(0.f, std::sin(e.y * k), 0.f, std::cos(e.y * k));
        const Vector4 qz(0.f, 0.f, std::sin(e.z * k), std::cos(e.z * k));
        return player::quaternion_multiply(player::quaternion_multiply(qy, qx), qz);
    }

    inline uint64_t native_of(uint64_t managed) {
        const uint64_t native = plausible(managed) ? rpm<uint64_t>(managed + v100::unity_cached_ptr) : 0;
        return plausible(native) ? native : 0;
    }

    // The camera GameObject's own Transform: the GameObject (camera+0x20) keeps its components as
    // {type, pointer} pairs with the Transform first. Searched, since the array's offset is not in the dump.
    inline uint64_t object_transform(uint64_t camera) {
        const uint64_t go = rpm<uint64_t>(camera + 0x20);
        if (!plausible(go)) return 0;
        for (uint64_t field = 0x18; field <= 0x60; field += 8) {
            const uint64_t data = rpm<uint64_t>(go + field);
            if (!plausible(data)) continue;
            uint64_t pairs[16][2]{};
            if (!mem_read(data, pairs, sizeof(pairs))) continue;
            for (int i = 1; i < 16; ++i)
                if (pairs[i][1] == camera && plausible(pairs[0][1])) return pairs[0][1];
        }
        return 0;
    }

    // Camera pose from one source. Transform sources try the index offsets the skeleton measured.
    inline bool pose(Source source, int index_offset, Vector3& position, Vector4& rotation) {
        if (source == State) {
            const uint64_t state = rpm<uint64_t>(g_holder + v100::gamecam_state);
            if (!plausible(state)) return false;
            position = rpm<Vector3>(state + v100::camstate_position);
            const Vector3 e = rpm<Vector3>(state + v100::camstate_euler);
            if (!std::isfinite(position.x) || !std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z)) return false;
            rotation = euler(e);
            return true;
        }
        const uint64_t native = source == Transform ? native_of(rpm<uint64_t>(g_holder + v100::gamecam_transform)) : g_object_transform;
        return native && player::decode_transform_pose(native, position, rotation, index_offset);
    }

    // Picks the pose source whose view matches the live matrix (camera not taken over yet).
    inline bool calibrate(const game_camera::Reading& cam) {
        g_object_transform = object_transform(cam.native);
        std::vector<int> offsets;
        if (skeleton::g_index_pid == proc::pid && skeleton::g_index_offset >= 0) offsets.push_back(skeleton::g_index_offset);
        for (int off : {0x30, player::g_transform_layout.index_offset, 0x40, 0x10})
            if (off >= 0 && std::find(offsets.begin(), offsets.end(), off) == offsets.end()) offsets.push_back(off);
        float best[4] = {1e9f, 1e9f, 1e9f, 1e9f};
        for (Source source : {Transform, ObjectTransform, State}) {
            for (int off : (source == State ? std::vector<int>{-1} : offsets)) {
                Vector3 p{}; Vector4 q{};
                if (!pose(source, off, p, q) || !player::normalize_quaternion(q)) continue;
                const float d = diff(player::view_from_pose(p, q), cam.view);
                best[source] = std::min(best[source], d);
                if (d > 0.05f) continue;
                g_source = source;
                g_index_offset = off;
                diag::log("TP", "camera pose source: %s (index +0x%x, matrix error %.4f)",
                          source == Transform ? "nrl.irub transform" : source == State ? "ckk pose" : "camera object transform",
                          off < 0 ? 0 : off, d);
                return true;
            }
        }
        if (diag::every(g_calib_log, 3000))
            diag::log("TP", "no camera pose matches the live matrix yet (errors: irub %.3f, object %.3f, ckk %.3f; object transform 0x%llx)",
                      best[Transform], best[ObjectTransform], best[State], (unsigned long long)g_object_transform);
        return false;
    }

    inline void resolve(uint64_t camera) {
        const uint64_t lo = camera + 8, hi = camera + 0x800;
        auto inside = [&](uint64_t a) { return a >= lo && a < hi; };
        std::string why;

        // 1. The setter: copy the live matrix onto itself and watch what it writes.
        for (const auto& c : icalls::candidates("UnityEngine.Camera::set_worldToCameraMatrix_Injected")) {
            a64::Session session;
            a64::Machine start;
            start.pc = c.fn;
            start.x[0] = {camera, true, 0, 0};
            start.x[1] = {camera + v100::camera_view, true, 0, 0};
            int flag = -1, matrix_at = -1;
            auto goal = [&](const a64::Machine& m) {
                if (!m.returned) return false;
                int wide = 0, f = -1, low = -1;
                for (const auto& st : m.stores) {
                    if (!inside(st.address)) continue;
                    if (st.size == 1 && st.known && (st.value & 0xFF) == 0 && f < 0) { f = int(st.address - camera); continue; }
                    if (st.size >= 4) {
                        wide += st.size;
                        if (low < 0 || int(st.address - camera) < low) low = int(st.address - camera);
                    }
                }
                if (f < 0 || wide < 64) return false;
                flag = f; matrix_at = low;
                return true;
            };
            a64::Machine found;
            if (!a64::explore(session, start, goal, found)) { why = session.why; continue; }
            g_flag_offset = flag;
            g_matrix_offset = matrix_at;
            diag::log("TP", "worldToCameraMatrix setter (%s): flag +0x%x (now %u), matrix +0x%x (ESP reads +0x%llx)",
                      c.how, flag, rpm<uint8_t>(camera + uint64_t(flag)), matrix_at, (unsigned long long)v100::camera_view);
            break;
        }

        // 2. The getter's flag check.
        if (g_flag_offset.load() < 0) {
            std::vector<std::pair<const char*, uint64_t>> getters;
            const uint64_t wrapper = proc::lib + v100::camera_get_world_to_camera_injected;
            if (!icalls::slot_empty(wrapper)) getters.push_back({"getter wrapper", wrapper});
            for (const auto& c : icalls::candidates("UnityEngine.Camera::get_worldToCameraMatrix_Injected")) getters.push_back({c.how, c.fn});
            for (const auto& g : getters) {
                a64::Session session;
                a64::Machine start;
                start.pc = g.second;
                start.x[0] = {camera, true, 0, 0};
                start.x[1] = {a64::kOutBuffer, true, 0, 0};
                start.watch_lo = lo;
                start.watch_hi = hi;
                auto goal = [&](const a64::Machine& m) { return m.watch_byte != 0; };
                a64::Machine found;
                if (!a64::explore(session, start, goal, found)) { why = session.why; continue; }
                const uint8_t value = rpm<uint8_t>(found.watch_byte);
                diag::log("TP", "worldToCameraMatrix getter (%s) checks byte +0x%llx (now %u)", g.first,
                          (unsigned long long)(found.watch_byte - camera), value);
                if (value != 1) continue;  // the camera is implicit right now, so the flag must read 1
                g_flag_offset = int(found.watch_byte - camera);
                g_matrix_offset = int(v100::camera_view);
                break;
            }
        }
        if (g_flag_offset.load() < 0)
            diag::log("WARN", "TP implicit-matrix flag not found (%s%s)", why.empty() ? "no accessor traced" : "last: ", why.c_str());

        // Culling mask, so the own body (hidden from the first-person camera) can be shown.
        struct Try { const char* how; uint64_t fn; bool setter; };
        std::vector<Try> tries;
        const uint64_t get_w = proc::lib + v100::camera_get_culling_mask_injected, set_w = proc::lib + v100::camera_set_culling_mask_injected;
        if (!icalls::slot_empty(get_w)) tries.push_back({"getter wrapper", get_w, false});
        if (!icalls::slot_empty(set_w)) tries.push_back({"setter wrapper", set_w, true});
        for (const auto& c : icalls::candidates("UnityEngine.Camera::get_cullingMask_Injected")) tries.push_back({c.how, c.fn, false});
        for (const auto& c : icalls::candidates("UnityEngine.Camera::set_cullingMask_Injected")) tries.push_back({c.how, c.fn, true});
        for (const Try& t : tries) {
            std::string w;
            const int off = animcull::trace(t.fn, camera, t.setter, w);
            if (off < 8 || off == g_flag_offset.load()) continue;
            g_mask_offset = off;
            diag::log("TP", "culling mask at +0x%x (%s, now 0x%08x)", off, t.how, rpm<uint32_t>(camera + uint64_t(off)));
            break;
        }
        if (g_mask_offset.load() < 0) diag::log("TP", "culling mask not found: own body stays as the game hides it");
        g_resolved = true;
    }

    inline void release() {
        if (g_camera) {
            matrix v{}, p{};
            if (g_flag_offset.load() >= 0 && validate_camera(g_camera, v, p, false))
                wpm<uint8_t>(g_camera + uint64_t(g_flag_offset.load()), g_flag_before ? g_flag_before : 1);
            if (g_mask_set && g_mask_offset.load() >= 0 && validate_camera(g_camera, v, p, false))
                wpm<uint32_t>(g_camera + uint64_t(g_mask_offset.load()), g_mask_before);
            diag::log("TP", "camera 0x%llx given back", (unsigned long long)g_camera);
        }
        g_camera = 0;
        g_mask_set = false;
        g_have_written = false;
    }

    inline void reset_process() {
        g_pid = proc::pid;
        g_flag_offset = g_matrix_offset = g_mask_offset = -1;
        g_resolved = false;
        g_failed = false;
        g_last_resolve = 0;
        g_camera = 0;
        g_source = None;
        g_mask_set = false;
        g_have_written = false;
    }

    // Once per frame with the game camera. `out` receives the third-person view while active.
    inline bool tick(const game_camera::Reading* cam, matrix& out) {
        if (g_pid != proc::pid) { if (g_resolving.load()) return false; reset_process(); }
        if (!cfg::thirdperson::enabled || !cam || g_failed) { release(); return false; }
        if (g_camera && g_camera != cam->native) { release(); g_source = None; }

        if (!g_resolved.load()) {
            const long long now = diag::now_ms();
            if (g_resolving.load() || now - g_last_resolve < 10000) return false;
            g_last_resolve = now;
            g_resolving = true;
            const uint64_t camera = cam->native;
            std::thread([camera] { resolve(camera); g_resolving = false; }).detach();
            return false;
        }
        const int flag = g_flag_offset.load(), at = g_matrix_offset.load();
        if (flag < 0 || at < 0) return false;

        g_holder = cam->holder;
        if (g_source == None && (g_camera || !calibrate(*cam))) return false;

        Vector3 position{}; Vector4 rotation{};
        if (!pose(g_source, g_index_offset, position, rotation) || !player::normalize_quaternion(rotation)) return false;
        const Vector3 forward = player::rotate(rotation, Vector3(0.f, 0.f, 1.f));
        const Vector3 right = player::rotate(rotation, Vector3(1.f, 0.f, 0.f));
        const float back = std::clamp(cfg::thirdperson::distance, 0.5f, 10.f);
        const Vector3 eye = position - forward * back + Vector3(0.f, cfg::thirdperson::height, 0.f) + right * cfg::thirdperson::side;
        const matrix view = player::view_from_pose(eye, rotation);

        const uint64_t camera = cam->native;
        const long long now = diag::now_ms();
        if (!g_camera) {
            g_camera = camera;
            g_flag_before = rpm<uint8_t>(camera + uint64_t(flag));
            g_taken_at = now;
            g_overwrites = 0;
            g_verified = false;
            diag::log("TP", "taking camera 0x%llx (flag +0x%x was %u, matrix +0x%x)", (unsigned long long)camera, flag, g_flag_before, at);
        } else if (g_have_written) {
            matrix current{};
            if (mem_read(camera + uint64_t(at), &current, sizeof(current)) && diff(current, g_written) > 1e-4f) ++g_overwrites;
        }
        if (!g_verified && now - g_taken_at > 500) {
            if (g_overwrites > 3) {
                diag::log("WARN", "TP the game rewrote the matrix %d times with flag +0x%x off: not the implicit flag, third person off",
                          g_overwrites, flag);
                release();
                g_failed = true;
                return false;
            }
            g_verified = true;
            diag::log("TP", "third person holds (%d overwrites in the first 500 ms)", g_overwrites);
        }

        wpm<uint8_t>(camera + uint64_t(flag), 0);
        wpm<matrix>(camera + uint64_t(at), view);
        g_written = view;
        g_have_written = true;

        const int mask = g_mask_offset.load();
        if (mask >= 0 && cfg::thirdperson::show_body && !g_mask_set) {
            g_mask_before = rpm<uint32_t>(camera + uint64_t(mask));
            g_mask_set = true;
            diag::log("TP", "culling mask 0x%08x -> 0xffffffff", g_mask_before);
        }
        if (mask >= 0 && g_mask_set) {
            if (cfg::thirdperson::show_body) wpm<uint32_t>(camera + uint64_t(mask), 0xFFFFFFFFu);
            else { wpm<uint32_t>(camera + uint64_t(mask), g_mask_before); g_mask_set = false; }
        }
        out = view;
        return true;
    }

    inline void wait() { while (g_resolving.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
