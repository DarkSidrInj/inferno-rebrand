#pragma once
// In-game fog: UnityEngine.RenderSettings (fog on/off, linear start/end, color) written
// straight into the game's native RenderSettings object. The managed wrappers in the dump
// (RenderSettings.set_fog / set_fogStartDistance / set_fogEndDistance / set_fogColor) only
// forward to that object, and nothing here calls il2cpp inside the game, so the object is
// found by its layout and the values go in with mem_write.
//
// Native fog block (Unity RenderSettings members, declaration order):
//   +0x00 bool m_Fog | +0x04 ColorRGBAf m_FogColor | +0x14 int m_FogMode (1 = Linear)
//   +0x18 float m_FogDensity | +0x1C float m_LinearFogStart | +0x20 float m_LinearFogEnd
//   +0x24 ambient sky, equator and ground colors (ColorRGBAf, 0x10 apart)
// The ambient colors only help tell the real object from look-alikes and feed the color
// estimate. If they do not match this build, the fog still works and the estimate falls
// back to the fog color the game had.
//
// Scene loads can replace the object: every tick re-checks the block and rescans when it
// stops looking like fog settings. Turning the fog off writes the game's own values back.

#include "CGameContext.hpp"
#include "../CConfigs/CConfig.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace worldfog {
    struct Block {
        uint8_t fog; uint8_t pad[3];
        float color[4];
        int32_t mode;
        float density, start, end;
        float ambient[12];
    };
    static_assert(sizeof(Block) == 0x54, "RenderSettings fog block");
    inline constexpr size_t kWriteSize = offsetof(Block, ambient);  // m_Fog .. m_LinearFogEnd
    inline constexpr long long kWriteMs = 50;

    inline int g_pid = -1;
    inline uint64_t g_address = 0;
    inline Block g_original{};
    inline bool g_applied = false;
    inline long long g_last_write = 0;

    inline std::vector<remote_map> g_maps;
    inline size_t g_map = 0;
    inline uint64_t g_cursor = 0, g_scanned = 0, g_weak = 0;
    inline int g_weak_score = -1;
    inline unsigned g_cycles = 0;

    inline bool unit(float v) { return std::isfinite(v) && v >= 0.f && v <= 1.f; }

    // -1 = not a fog block, otherwise how many of the three ambient alphas are exactly 1.0
    // (0..3). Two or more is how the real object is told from look-alikes.
    inline int score(const Block& b) {
        if (b.fog > 1 || b.mode < 1 || b.mode > 3) return -1;
        for (float c : b.color) if (!unit(c)) return -1;
        if (!unit(b.density)) return -1;
        if (!std::isfinite(b.start) || !std::isfinite(b.end) || b.start < 0.f || b.end <= b.start || b.end > 1000000.f) return -1;
        int hits = 0;
        for (int i = 3; i < 12; i += 4) if (b.ambient[i] == 1.f) ++hits;
        return hits;
    }

    inline void forget() { g_address = 0; g_applied = false; }
    inline void restart_scan() { g_maps.clear(); g_map = 0; g_cursor = 0; g_weak = 0; g_weak_score = -1; }

    inline void log_found(const char* how, uint64_t address, const Block& b, int tail) {
        diag::log("FOG", "RenderSettings %s at 0x%llx after %.1f MiB: fog=%d mode=%d start=%.1f end=%.1f density=%.3f color=(%.2f %.2f %.2f) tail=%d/3",
                  how, (unsigned long long)address, double(g_scanned) / 1048576.0, b.fog, b.mode, b.start, b.end,
                  b.density, b.color[0], b.color[1], b.color[2], tail);
    }

    // Walks the writable private mappings a slice at a time. Chunks overlap by one block so a
    // block straddling a chunk edge is not missed.
    inline bool scan_step(size_t budget = 3 * 1024 * 1024) {
        if (g_maps.empty()) {
            if (g_scan_maps.empty()) refresh_scan_maps();
            g_maps = g_scan_maps;
            g_map = 0; g_cursor = 0;
        }
        std::vector<uint8_t> buffer;
        size_t consumed = 0;
        while (consumed < budget && g_map < g_maps.size()) {
            const remote_map& map = g_maps[g_map];
            const uint64_t cursor = g_cursor ? g_cursor : map.begin;
            const uint64_t left = map.end - cursor;
            size_t bytes = size_t(std::min<uint64_t>(std::min<uint64_t>(budget - consumed, left), 1024 * 1024));
            bytes &= ~size_t(3);
            if (bytes < sizeof(Block)) { ++g_map; g_cursor = 0; continue; }
            buffer.resize(bytes);
            if (mem_read(cursor, buffer.data(), bytes)) {
                for (size_t i = 0; i + sizeof(Block) <= bytes; i += 4) {
                    int32_t mode;
                    std::memcpy(&mode, buffer.data() + i + offsetof(Block, mode), 4);
                    if (mode < 1 || mode > 3) continue;  // rejects nearly all memory before the full check
                    Block b;
                    std::memcpy(&b, buffer.data() + i, sizeof(b));
                    const int tail = score(b);
                    if (tail < 0) continue;
                    if (tail >= 2) {
                        g_address = cursor + i;
                        g_original = b;
                        log_found("found", g_address, b, tail);
                        restart_scan();
                        return true;
                    }
                    if (tail > g_weak_score) { g_weak_score = tail; g_weak = cursor + i; }
                }
            }
            consumed += bytes; g_scanned += bytes;
            if (bytes >= left) { ++g_map; g_cursor = 0; }
            else g_cursor = cursor + bytes - sizeof(Block);
        }
        if (g_map >= g_maps.size()) {
            ++g_cycles;
            Block weak{};
            if (g_weak && mem_read(g_weak, &weak, sizeof(weak)) && score(weak) >= 0) {
                g_address = g_weak;
                g_original = weak;
                log_found("found (ambient tail unconfirmed)", g_address, weak, score(weak));
                restart_scan();
                return true;
            }
            diag::log("WARN", "FOG scan cycle %u complete; no RenderSettings fog block (%.1f MiB read)",
                      g_cycles, double(g_scanned) / 1048576.0);
            restart_scan();
            refresh_scan_maps();
        }
        return false;
    }

    inline bool ambient_ok(const float* c) {
        for (int i = 0; i < 3; ++i) if (!std::isfinite(c[i]) || c[i] < 0.f || c[i] > 4.f) return false;
        return c[3] == 1.f;
    }

    // Color estimate: the horizon the fog has to blend into, taken from the scene's ambient
    // equator color (ambient sky when the equator slot is not a color). Falls back to the fog
    // color the game had before it was touched.
    inline void estimate_color(const Block& b, float out[3]) {
        const float* source = ambient_ok(b.ambient + 4) ? b.ambient + 4
                            : ambient_ok(b.ambient) ? b.ambient : g_original.color;
        for (int i = 0; i < 3; ++i) out[i] = std::clamp(source[i], 0.f, 1.f);
    }

    inline bool same(const Block& a, const Block& b) { return std::memcmp(&a, &b, kWriteSize) == 0; }

    inline void write_block(const Block& b) {
        if (!mem_write(g_address, &b, kWriteSize)) {
            static long long fail_log = 0;
            if (diag::every(fail_log, 5000))
                diag::log("ERROR", "FOG write to 0x%llx failed", (unsigned long long)g_address);
        }
    }

    // One call per frame. Does nothing until the fog is switched on, and puts the game's
    // values back when it is switched off.
    inline void tick() {
        if (g_pid != proc::pid) { g_pid = proc::pid; forget(); restart_scan(); g_cycles = 0; g_scanned = 0; }
        if (proc::pid <= 0 || !plausible(proc::lib)) return;
        if (!cfg::fog::enabled && !g_applied) return;

        if (!g_address) {
            if (!cfg::fog::enabled || !scan_step()) return;
        }

        Block now{};
        if (!mem_read(g_address, &now, sizeof(now)) || score(now) < 0) {
            diag::log("INFO", "FOG block at 0x%llx no longer valid, scanning again", (unsigned long long)g_address);
            forget();
            return;
        }

        if (!cfg::fog::enabled) {
            Block restore = now;
            std::memcpy(&restore, &g_original, kWriteSize);
            write_block(restore);
            g_applied = false;
            diag::log("FOG", "restored game fog fog=%d start=%.1f end=%.1f", g_original.fog, g_original.start, g_original.end);
            return;
        }

        if (!g_applied) g_original = now;  // what the game had, before the first write

        Block want = now;
        want.fog = 1;
        want.mode = 1;  // Linear: start and end only apply in this mode
        want.start = std::max(cfg::fog::min_distance, 0.f);
        want.end = std::max(cfg::fog::max_distance, want.start + 1.f);
        if (cfg::fog::auto_color) {
            estimate_color(now, want.color);
        } else {
            want.color[0] = cfg::fog::color.x; want.color[1] = cfg::fog::color.y; want.color[2] = cfg::fog::color.z;
        }
        want.color[3] = 1.f;

        const long long stamp = diag::now_ms();
        if (same(want, now) || stamp - g_last_write < kWriteMs) { g_applied = true; return; }
        g_last_write = stamp;
        write_block(want);
        if (!g_applied) {
            g_applied = true;
            diag::log("FOG", "applied start=%.1f end=%.1f color=(%.2f %.2f %.2f) %s", want.start, want.end,
                      want.color[0], want.color[1], want.color[2], cfg::fog::auto_color ? "estimated" : "manual");
        }
    }

    // Menu "Rescan" button: forget the object so the next tick looks for it again.
    inline void rescan() { forget(); restart_scan(); }
}
