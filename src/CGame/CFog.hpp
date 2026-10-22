#pragma once
// In-game fog: UnityEngine.RenderSettings fog on/off, linear start/end and color, written
// into the game's native RenderSettings object.
//
// Where the fields are is learned from the game's own code, not guessed. Each field has an
// il2cpp wrapper RVA (script.json, COffsets.hpp). The interpreter in CUtils/CArm64.hpp walks
// a getter wrapper -> its resolved icall -> GetRenderSettings() -> the field load, and the
// address that load reads from is the field. Nothing is executed in the game.
//
// Per field, in order:
//   1. getter wrapper   (works once the game has called it, so its icall slot is filled)
//   2. setter wrapper   (the store of the argument gives the address)
//   3. icall by name    (CIcalls.hpp: libunity registers "UnityEngine.RenderSettings::get_fog..."
//                        with its function; every candidate is traced and must behave)
// A wrapper whose icall slot is still empty (the game never called it) is skipped outright.
// A field still missing after that is placed from fog start with Unity's member order
// (m_Fog, m_FogColor, m_FogMode, m_FogDensity, m_LinearFogStart, m_LinearFogEnd) and kept
// only if its value is sane. The object moves on scene loads, so fog start is re-read every
// two seconds and everything is resolved again when it moved. Turning the fog off writes the
// game's values back.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "../CUtils/CArm64.hpp"
#include "CIcalls.hpp"
#include "../CConfigs/CConfig.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace worldfog {
    enum Kind { Bool, Int, Float, Color };
    enum Field { Fog, Mode, Density, Start, End, FogColor, Equator, Sky, kFields };

    struct Spec {
        const char* name;
        Kind kind;
        uint64_t getter, setter;      // wrapper RVAs
        const char* icall_get;
        const char* icall_set;
        int from_start;               // offset from m_LinearFogStart in Unity's member order, for inference
        bool inferable;
    };

    inline const Spec kSpecs[kFields] = {
        {"fog", Bool, v100::rs_get_fog, v100::rs_set_fog, "get_fog", "set_fog", -0x1C, true},
        {"mode", Int, v100::rs_get_fog_mode, v100::rs_set_fog_mode, "get_fogMode", "set_fogMode", -0x08, true},
        {"density", Float, v100::rs_get_fog_density, v100::rs_set_fog_density, "get_fogDensity", "set_fogDensity", -0x04, true},
        {"start", Float, v100::rs_get_fog_start, v100::rs_set_fog_start, "get_fogStartDistance", "set_fogStartDistance", 0, true},
        {"end", Float, v100::rs_get_fog_end, v100::rs_set_fog_end, "get_fogEndDistance", "set_fogEndDistance", 0x04, true},
        {"color", Color, v100::rs_get_fog_color_injected, v100::rs_set_fog_color_injected,
         "get_fogColor_Injected", "set_fogColor_Injected", -0x18, true},
        {"ambientEquator", Color, v100::rs_get_ambient_equator_injected, v100::rs_set_ambient_equator_injected,
         "get_ambientEquatorColor_Injected", "set_ambientEquatorColor_Injected", 0, false},
        {"ambientSky", Color, v100::rs_get_ambient_sky_injected, v100::rs_set_ambient_sky_injected,
         "get_ambientSkyColor_Injected", "set_ambientSkyColor_Injected", 0, false},
    };

    struct Resolved {
        uint64_t address{};
        std::string how = "missing";
        uint64_t entry{};    // code that produced it, for the cheap liveness re-check
        bool setter{};
    };

    struct Original {
        uint8_t fog{}; int32_t mode{}; float density{}, start{}, end{}; float color[4]{};
    };

    inline int g_pid = -1;
    inline Resolved g_fields[kFields];
    inline uint64_t g_object = 0;            // from GetRenderSettings_Injected, when readable
    inline bool g_ready = false;             // fog, start and end are known
    inline long long g_last_resolve = 0, g_last_check = 0, g_last_write = 0;
    inline bool g_applied = false;
    inline Original g_original{};
    inline bool g_dumped = false;            // wrapper code logged once per process
    // Resolution can walk deep into il2cpp (an empty icall slot leads into the resolver), so it
    // runs on its own thread; the frame only picks up the result.
    inline std::atomic<bool> g_resolving{false};

    // ---- reading the game's code ------------------------------------------------------------

    inline bool outside(uint64_t a) { return plausible(a) && !a64::local(a); }

    // Address of the field a getter at `entry` returns.
    inline uint64_t trace_getter(uint64_t entry, Kind kind, std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = entry;
        start.x[0] = {kind == Color ? a64::kOutBuffer : 0, true, 0, 0};  // out Color* for _Injected
        start.x[1] = {0, true, 0, 0};                                     // MethodInfo*
        uint64_t result = 0;
        auto goal = [&](const a64::Machine& m) {
            if (kind == Color) {
                for (const auto& st : m.stores) {
                    if (st.address < a64::kOutBuffer || st.address >= a64::kOutBuffer + 16 || !outside(st.src)) continue;
                    result = st.src - (st.address - a64::kOutBuffer);
                    return true;
                }
                return false;
            }
            if (!m.returned) return false;
            const a64::Reg& r = kind == Float ? m.v[0] : m.x[0];
            if (outside(r.src)) { result = r.src; return true; }
            if (kind == Bool && m.last_load_size == 1 && outside(m.last_load)) { result = m.last_load; return true; }
            return false;
        };
        a64::Machine found;
        if (a64::explore(session, start, goal, found)) return result;
        why = session.why;
        return 0;
    }

    // Address of the field a setter at `entry` stores its argument to.
    inline uint64_t trace_setter(uint64_t entry, Kind kind, std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = entry;
        const a64::Reg arg{1, true, 0, a64::kArg};
        if (kind == Float) { start.v[0] = {0, false, 0, a64::kArg}; start.x[0] = {0, true, 0, 0}; }
        else if (kind == Color) {
            start.x[0] = {a64::kOutBuffer, true, 0, 0};
            for (int o = 0; o < 16; o += 4) start.mem[a64::kOutBuffer + o] = {0, false, 0, a64::kArg};
        } else start.x[0] = arg;
        start.x[1] = {0, true, 0, 0};
        uint64_t result = 0;
        auto goal = [&](const a64::Machine& m) {
            for (const auto& st : m.stores) {
                if (st.tag != a64::kArg || !outside(st.address)) continue;
                result = st.address;
                return true;
            }
            return false;
        };
        a64::Machine found;
        if (a64::explore(session, start, goal, found)) return result;
        why = session.why;
        return 0;
    }

    inline uint64_t trace_object(uint64_t entry, std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = entry;
        start.x[0] = {0, true, 0, 0};
        auto goal = [](const a64::Machine& m) { return m.returned && m.x[0].known && outside(m.x[0].value); };
        a64::Machine found;
        if (a64::explore(session, start, goal, found)) return found.x[0].value;
        why = session.why;
        return 0;
    }

    // ---- diagnostics ----------------------------------------------------------------------------

    // Logs the first instructions of an accessor and follows its first adrp + ldr (the icall
    // slot or its GOT entry) two levels, so a failed trace can be read from the log.
    inline void dump_code(const char* what, uint64_t entry) {
        uint32_t words[24]{};
        if (!mem_read(entry, words, sizeof(words))) { diag::log("FOG", "%s 0x%llx: code unreadable", what, (unsigned long long)entry); return; }
        char text[24 * 9 + 1];
        for (int i = 0; i < 24; ++i) std::snprintf(text + i * 9, 10, "%08x ", words[i]);
        diag::log("FOG", "%s 0x%llx (lib+0x%llx): %s", what, (unsigned long long)entry,
                  (unsigned long long)(entry - proc::lib), text);
        for (int i = 0; i + 1 < 24; ++i) {
            const uint32_t a = words[i];
            if ((a & 0x9F000000) != 0x90000000) continue;
            const int reg = int(a & 31);
            for (int j = i + 1; j < std::min(i + 4, 24); ++j) {
                const uint32_t b = words[j];
                if ((b & 0xFFC00000) != 0xF9400000 || int((b >> 5) & 31) != reg) continue;
                const uint64_t pc = entry + uint64_t(i) * 4;
                const uint64_t imm = a64::sext((uint64_t((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3), 21);
                const uint64_t slot = (pc & ~uint64_t(0xFFF)) + (imm << 12) + uint64_t((b >> 10) & 0xFFF) * 8;
                const uint64_t v1 = rpm<uint64_t>(slot);
                const uint64_t v2 = plausible(v1) ? rpm<uint64_t>(v1) : 0;
                diag::log("FOG", "  adrp+ldr at +%d -> [0x%llx] = 0x%llx -> [..] = 0x%llx", i * 4,
                          (unsigned long long)slot, (unsigned long long)v1, (unsigned long long)v2);
                return;
            }
        }
    }

    // ---- resolution ---------------------------------------------------------------------------

    inline bool value_sane(Field f, uint64_t a) {
        if (!plausible(a)) return false;
        switch (f) {
            case Fog: { const uint8_t v = rpm<uint8_t>(a); return v <= 1; }
            case Mode: { const int32_t v = rpm<int32_t>(a); return v >= 1 && v <= 3; }
            case Density: { const float v = rpm<float>(a); return std::isfinite(v) && v >= 0.f && v <= 10.f; }
            case Start: case End: { const float v = rpm<float>(a); return std::isfinite(v) && v >= 0.f && v <= 1000000.f; }
            default: {
                float c[4]{};
                if (!mem_read(a, c, sizeof(c))) return false;
                for (float v : c) if (!std::isfinite(v) || v < 0.f || v > 8.f) return false;
                return true;
            }
        }
    }

    inline std::string full_name(const char* icall) { return std::string("UnityEngine.RenderSettings::") + icall; }

    // Wrapper first (only when its icall slot is filled; an empty one leads into il2cpp's
    // resolver), then the native icall found by name in libunity.
    inline bool resolve_field(Field f) {
        const Spec& spec = kSpecs[f];
        Resolved& r = g_fields[f];
        r = Resolved{};
        std::string why_get = "icall slot empty", why_set = "icall slot empty";
        if (!icalls::slot_empty(proc::lib + spec.getter)) {
            if (uint64_t a = trace_getter(proc::lib + spec.getter, spec.kind, why_get)) {
                r = {a, "getter wrapper", proc::lib + spec.getter, false};
                return true;
            }
        }
        if (!icalls::slot_empty(proc::lib + spec.setter)) {
            if (uint64_t a = trace_setter(proc::lib + spec.setter, spec.kind, why_set)) {
                r = {a, "setter wrapper", proc::lib + spec.setter, true};
                return true;
            }
        }
        std::string why;
        for (const auto& c : icalls::candidates(full_name(spec.icall_get))) {
            if (uint64_t a = trace_getter(c.fn, spec.kind, why)) { r = {a, std::string("icall getter, ") + c.how, c.fn, false}; return true; }
        }
        for (const auto& c : icalls::candidates(full_name(spec.icall_set))) {
            if (uint64_t a = trace_setter(c.fn, spec.kind, why)) { r = {a, std::string("icall setter, ") + c.how, c.fn, true}; return true; }
        }
        r.how = "missing (getter: " + why_get + ", setter: " + why_set + (why.empty() ? std::string() : ", icall: " + why) + ")";
        return false;
    }

    inline void resolve_all() {
        g_ready = false;
        g_applied = false;
        std::string why = "icall slot empty";
        g_object = 0;
        const uint64_t object_wrapper = proc::lib + v100::rs_get_render_settings_injected;
        if (!icalls::slot_empty(object_wrapper)) g_object = trace_object(object_wrapper, why);
        if (!g_object)
            for (const auto& c : icalls::candidates(full_name("GetRenderSettings_Injected")))
                if ((g_object = trace_object(c.fn, why))) break;
        if (g_object) diag::log("FOG", "GetRenderSettings -> 0x%llx", (unsigned long long)g_object);
        else diag::log("FOG", "GetRenderSettings not readable (%s)", why.c_str());

        // Fog start first: it anchors the inference of the rest.
        resolve_field(Start);
        for (int f = 0; f < kFields; ++f) if (f != Start) resolve_field(Field(f));

        const uint64_t start = g_fields[Start].address;
        for (int f = 0; f < kFields; ++f) {
            Resolved& r = g_fields[f];
            if (r.address && g_object && (r.address < g_object || r.address >= g_object + 0x1000)) {
                diag::log("WARN", "FOG %s at 0x%llx is outside RenderSettings 0x%llx, dropped", kSpecs[f].name,
                          (unsigned long long)r.address, (unsigned long long)g_object);
                r = Resolved{};
            }
            if (!r.address && start && kSpecs[f].inferable && value_sane(Field(f), start + uint64_t(int64_t(kSpecs[f].from_start)))) {
                r.address = start + uint64_t(int64_t(kSpecs[f].from_start));
                r.how = "inferred from fog start";
            }
        }
        for (int f = 0; f < kFields; ++f)
            diag::log("FOG", "  %-15s 0x%llx  %s", kSpecs[f].name, (unsigned long long)g_fields[f].address, g_fields[f].how.c_str());

        g_ready = g_fields[Fog].address && g_fields[Start].address && g_fields[End].address;
        if (!g_ready) {
            diag::log("WARN", "FOG not usable: fog, start and end are all needed");
            if (!g_dumped) {
                g_dumped = true;
                dump_code("get_fogStartDistance wrapper", proc::lib + v100::rs_get_fog_start);
                dump_code("set_fogStartDistance wrapper", proc::lib + v100::rs_set_fog_start);
                dump_code("GetRenderSettings_Injected wrapper", proc::lib + v100::rs_get_render_settings_injected);
            }
        }
    }

    // ---- applying -------------------------------------------------------------------------------

    inline void read_original() {
        g_original.fog = rpm<uint8_t>(g_fields[Fog].address);
        if (g_fields[Mode].address) g_original.mode = rpm<int32_t>(g_fields[Mode].address);
        if (g_fields[Density].address) g_original.density = rpm<float>(g_fields[Density].address);
        g_original.start = rpm<float>(g_fields[Start].address);
        g_original.end = rpm<float>(g_fields[End].address);
        if (g_fields[FogColor].address) mem_read(g_fields[FogColor].address, g_original.color, sizeof(g_original.color));
    }

    inline void restore() {
        wpm<uint8_t>(g_fields[Fog].address, g_original.fog);
        if (g_fields[Mode].address) wpm<int32_t>(g_fields[Mode].address, g_original.mode);
        wpm<float>(g_fields[Start].address, g_original.start);
        wpm<float>(g_fields[End].address, g_original.end);
        if (g_fields[FogColor].address) mem_write(g_fields[FogColor].address, g_original.color, sizeof(g_original.color));
        diag::log("FOG", "restored game fog fog=%d start=%.1f end=%.1f", g_original.fog, g_original.start, g_original.end);
    }

    // Color estimate: the horizon the fog has to blend into, from the scene's ambient equator
    // color (ambient sky when the equator is not readable), else the game's own fog color.
    inline void estimate_color(float out[3]) {
        float c[4]{};
        for (Field f : {Equator, Sky}) {
            if (g_fields[f].address && mem_read(g_fields[f].address, c, sizeof(c)) &&
                std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2])) {
                for (int i = 0; i < 3; ++i) out[i] = std::clamp(c[i], 0.f, 1.f);
                return;
            }
        }
        for (int i = 0; i < 3; ++i) out[i] = std::clamp(g_original.color[i], 0.f, 1.f);
    }

    inline void write_if_different(uint64_t a, const void* value, size_t size) {
        uint8_t now[16]{};
        if (!a || (mem_read(a, now, size) && std::memcmp(now, value, size) == 0)) return;
        if (!mem_write(a, value, size)) {
            static long long fail_log = 0;
            if (diag::every(fail_log, 5000)) diag::log("ERROR", "FOG write to 0x%llx failed", (unsigned long long)a);
        }
    }

    inline void apply() {
        const uint8_t fog = 1;
        const int32_t linear = 1;  // FogMode.Linear: start and end only apply in this mode
        const float start = std::max(cfg::fog::min_distance, 0.f);
        const float end = std::max(cfg::fog::max_distance, start + 1.f);
        float color[4] = {0, 0, 0, 1.f};
        if (cfg::fog::auto_color) estimate_color(color);
        else { color[0] = cfg::fog::color.x; color[1] = cfg::fog::color.y; color[2] = cfg::fog::color.z; }
        write_if_different(g_fields[Fog].address, &fog, 1);
        write_if_different(g_fields[Mode].address, &linear, 4);
        write_if_different(g_fields[Start].address, &start, 4);
        write_if_different(g_fields[End].address, &end, 4);
        write_if_different(g_fields[FogColor].address, color, 16);
        if (!g_applied) {
            g_applied = true;
            diag::log("FOG", "applied start=%.1f end=%.1f color=(%.2f %.2f %.2f) %s", start, end, color[0], color[1],
                      color[2], cfg::fog::auto_color ? "estimated" : "manual");
        }
    }

    // Has RenderSettings moved (scene load)? Re-traces fog start with the code that found it.
    inline bool moved() {
        const Resolved& r = g_fields[Start];
        if (!r.entry) return false;
        std::string why;
        const uint64_t now = r.setter ? trace_setter(r.entry, Float, why) : trace_getter(r.entry, Float, why);
        return now && now != r.address;
    }

    // One call per frame. Does nothing until the fog is switched on, and puts the game's values
    // back when it is switched off.
    inline void tick() {
        if (g_pid != proc::pid) {
            if (g_resolving.load()) return;  // let the old resolution finish first
            g_pid = proc::pid;
            g_ready = g_applied = false;
            g_dumped = false;
            g_last_resolve = 0;
            for (auto& f : g_fields) f = Resolved{};
        }
        if (proc::pid <= 0 || !plausible(proc::lib)) return;
        if (!cfg::fog::enabled && !g_applied) return;
        const long long now = diag::now_ms();

        if (g_resolving.load()) return;
        if (!g_ready) {
            if (!cfg::fog::enabled || now - g_last_resolve < (g_dumped ? 15000 : 3000)) return;
            g_last_resolve = now;
            g_resolving = true;
            std::thread([] { resolve_all(); g_resolving = false; }).detach();
            return;
        }

        if (now - g_last_check > 2000) {
            g_last_check = now;
            if (moved()) {
                diag::log("FOG", "RenderSettings moved (scene load), resolving again");
                g_ready = g_applied = false;
                g_last_resolve = 0;
                return;
            }
        }

        if (!cfg::fog::enabled) {
            restore();
            g_applied = false;
            return;
        }
        if (!g_applied) read_original();
        if (now - g_last_write < 100) return;
        g_last_write = now;
        apply();
    }

    // Waits for a resolution started by tick() (tests and shutdown).
    inline void wait() { while (g_resolving.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }

    // Menu "Rescan" button.
    inline void rescan() {
        if (g_resolving.load()) return;
        if (g_applied) restore();
        g_ready = g_applied = false;
        g_last_resolve = 0;
    }
}
