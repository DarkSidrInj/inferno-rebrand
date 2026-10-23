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
// A field still missing after that is placed from fog start with the layout traced on device
// and kept only if its value is sane.
//
// Writing a field alone does nothing visible: Unity hands fog to the renderer inside its own
// setters. So a change is made by *replaying* the game's setter (CUtils/CArm64.hpp): the
// setter runs in the interpreter with the new value, against live memory, and the stores it
// would make (the field plus what it updates behind it) are written to the game. A replay is
// used only when every store is fully known; otherwise the field is written directly. The object moves on scene loads, so fog start is re-read every
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
#include <unordered_map>
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
        // Offsets from fog start: the Standoff 2 (Unity 6) layout traced on device,
        // m_Fog +0, m_FogMode +4, m_FogColor +8, m_LinearFogStart +0x18, End +0x1C, Density +0x20.
        {"fog", Bool, v100::rs_get_fog, v100::rs_set_fog, "get_fog", "set_fog", -0x18, true},
        {"mode", Int, v100::rs_get_fog_mode, v100::rs_set_fog_mode, "get_fogMode", "set_fogMode", -0x14, true},
        {"density", Float, v100::rs_get_fog_density, v100::rs_set_fog_density, "get_fogDensity", "set_fogDensity", 0x08, true},
        {"start", Float, v100::rs_get_fog_start, v100::rs_set_fog_start, "get_fogStartDistance", "set_fogStartDistance", 0, true},
        {"end", Float, v100::rs_get_fog_end, v100::rs_set_fog_end, "get_fogEndDistance", "set_fogEndDistance", 0x04, true},
        {"color", Color, v100::rs_get_fog_color_injected, v100::rs_set_fog_color_injected,
         "get_fogColor_Injected", "set_fogColor_Injected", -0x10, true},
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
    inline uint64_t g_setter[kFields]{};      // code that sets the field, for replays
    inline bool g_setter_logged[kFields]{};
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

    // The code that writes this field: a filled setter wrapper or the setter icall, accepted
    // only when tracing it stores to the address the getter reads.
    inline void find_setter(Field f) {
        const Spec& spec = kSpecs[f];
        g_setter[f] = 0;
        g_setter_logged[f] = false;
        const uint64_t want = g_fields[f].address;
        if (!want) return;
        std::string why;
        if (!icalls::slot_empty(proc::lib + spec.setter) && trace_setter(proc::lib + spec.setter, spec.kind, why) == want) {
            g_setter[f] = proc::lib + spec.setter;
            return;
        }
        for (const auto& c : icalls::candidates(full_name(spec.icall_set)))
            if (trace_setter(c.fn, spec.kind, why) == want) { g_setter[f] = c.fn; return; }
    }

    inline void read_original();
    inline void scan_params();

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

        for (Field f : {Fog, Mode, Start, End, FogColor}) find_setter(f);
        diag::log("FOG", "setters: fog=%s mode=%s start=%s end=%s color=%s", g_setter[Fog] ? "yes" : "no",
                  g_setter[Mode] ? "yes" : "no", g_setter[Start] ? "yes" : "no", g_setter[End] ? "yes" : "no",
                  g_setter[FogColor] ? "yes" : "no");

        g_ready = g_fields[Fog].address && g_fields[Start].address && g_fields[End].address;
        if (g_ready) {
            read_original();
            diag::log("FOG", "map fog: on=%d mode=%d density=%.4f start=%.1f end=%.1f color=(%.2f %.2f %.2f)", g_original.fog,
                      g_original.mode, g_original.density, g_original.start, g_original.end, g_original.color[0],
                      g_original.color[1], g_original.color[2]);
            scan_params();
        }
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

    // Where a failed replay spent its steps: the setter's code, the hottest instructions with the
    // code around them, the branches that split paths and the most called functions. Enough to
    // disassemble the engine path offline.
    inline void dump_words(const char* what, uint64_t at, int count) {
        std::vector<uint32_t> words(size_t(count), 0);
        if (!mem_read(at, words.data(), words.size() * 4)) { diag::log("FOG", "  %s 0x%llx unreadable", what, (unsigned long long)at); return; }
        std::string text;
        char hex[10];
        for (uint32_t w : words) { std::snprintf(hex, sizeof(hex), "%08x ", w); text += hex; }
        diag::log("FOG", "  %s 0x%llx: %s", what, (unsigned long long)at, text.c_str());
    }

    inline void report_replay(const char* name, uint64_t fn, const a64::Session& s) {
        auto top = [](const std::unordered_map<uint64_t, uint32_t>& counts, size_t n) {
            std::vector<std::pair<uint64_t, uint32_t>> v(counts.begin(), counts.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            if (v.size() > n) v.resize(n);
            return v;
        };
        diag::log("FOG", "%s setter 0x%llx replay profile: %zu distinct pcs, %zu fork sites, %zu callees", name,
                  (unsigned long long)fn, s.hot.size(), s.fork_sites.size(), s.call_targets.size());
        dump_words("entry", fn, 48);
        for (const auto& h : top(s.hot, 3)) {
            diag::log("FOG", "  hot pc 0x%llx x%u", (unsigned long long)h.first, h.second);
            dump_words("around", h.first - 32, 24);
        }
        for (const auto& f : top(s.fork_sites, 4)) diag::log("FOG", "  fork pc 0x%llx x%u", (unsigned long long)f.first, f.second);
        for (const auto& c : top(s.call_targets, 6)) {
            diag::log("FOG", "  callee 0x%llx x%u", (unsigned long long)c.first, c.second);
            dump_words("callee", c.first, 24);
        }
    }

    // Runs the field's setter with `value` and writes what it stores. False when there is no
    // setter or the replay cannot be fully known.
    inline bool replay_setter(Field f, const uint8_t* value, size_t size) {
        const uint64_t fn = g_setter[f];
        if (!fn) return false;
        const Spec& spec = kSpecs[f];
        a64::Machine m;
        m.pc = fn;
        m.x[0] = {0, true, 0, 0};
        m.x[1] = {0, true, 0, 0};
        if (spec.kind == Float) {
            uint32_t bits = 0;
            std::memcpy(&bits, value, 4);
            m.v[0] = {bits, true, 0, a64::kArg};
        } else if (spec.kind == Bool || spec.kind == Int) {
            uint64_t v = 0;
            std::memcpy(&v, value, size);
            m.x[0] = {v, true, 0, a64::kArg};
        } else {  // Color by reference
            m.x[0] = {a64::kOutBuffer, true, 0, 0};
            uint8_t padded[32]{};
            std::memcpy(padded, value, size);
            for (int o = 0; o < 16; o += 4) {
                a64::Reg r{};
                std::memcpy(&r.value, padded + o, 8);
                std::memcpy(&r.hi, padded + o + 8, 8);
                r.known = true;
                r.tag = a64::kArg;
                m.mem[a64::kOutBuffer + uint64_t(o)] = r;
            }
        }
        std::vector<a64::Store> stores;
        std::string why;
        const uint64_t field = g_fields[f].address;
        // Engine setters are long (thread checks, ApplyFog, dirty tracking): replays run on a
        // worker thread with a budget far above what a trace needs.
        a64::Session session;
        session.max_steps = 400000;
        session.max_total = 3000000;
        session.max_forks = 512;
        session.max_site_forks = 3;
        session.max_depth = 12;
        session.profile = !g_setter_logged[f];
        if (!a64::replay(m, field, stores, why, &session)) {
            if (!g_setter_logged[f]) {
                g_setter_logged[f] = true;
                diag::log("FOG", "%s setter not replayable (%s, %ld steps), writing the field", spec.name, why.c_str(), session.total_steps);
                report_replay(spec.name, fn, session);
            }
            return false;
        }
        // The setter must store exactly the requested value into the field.
        for (const auto& st : stores) {
            if (field < st.address || field + size > st.address + uint64_t(st.size)) continue;
            uint8_t bytes[16];
            std::memcpy(bytes, &st.value, 8);
            std::memcpy(bytes + 8, &st.hi, 8);
            if (std::memcmp(bytes + (field - st.address), value, size) != 0) {
                if (!g_setter_logged[f]) { g_setter_logged[f] = true; diag::log("FOG", "%s setter stored another value, writing the field", spec.name); }
                return false;
            }
        }
        if (!g_setter_logged[f]) {
            g_setter_logged[f] = true;
            std::string list;
            char item[48];
            for (size_t i = 0; i < stores.size() && i < 6; ++i) {
                std::snprintf(item, sizeof(item), " 0x%llx/%d", (unsigned long long)stores[i].address, stores[i].size);
                list += item;
            }
            diag::log("FOG", "%s setter replayed: %zu stores%s", spec.name, stores.size(), list.c_str());
        }
        return a64::commit(stores);
    }

    // Sets one field the way the game would (setter replay), or directly when that cannot be done.
    inline void set_field(Field f, const void* value, size_t size) {
        const uint64_t a = g_fields[f].address;
        uint8_t now[16]{};
        if (!a || (mem_read(a, now, size) && std::memcmp(now, value, size) == 0)) return;
        if (replay_setter(f, static_cast<const uint8_t*>(value), size)) return;
        if (!mem_write(a, value, size)) {
            static long long fail_log = 0;
            if (diag::every(fail_log, 5000)) diag::log("ERROR", "FOG write to 0x%llx failed", (unsigned long long)a);
        }
    }

    inline void restore() {
        set_field(Fog, &g_original.fog, 1);
        if (g_fields[Mode].address) set_field(Mode, &g_original.mode, 4);
        set_field(Start, &g_original.start, 4);
        set_field(End, &g_original.end, 4);
        if (g_fields[FogColor].address) set_field(FogColor, g_original.color, sizeof(g_original.color));
        diag::log("FOG", "restored game fog fog=%d start=%.1f end=%.1f", g_original.fog, g_original.start, g_original.end);
    }

    // ---- renderer fog parameters --------------------------------------------------------------
    // What the shaders read is not RenderSettings but unity_FogParams =
    //   (density / sqrt(ln 2), density / ln 2, -1 / (end - start), end / (end - start))
    // and unity_FogColor, which Unity recomputes only inside its fog setters. They are found by
    // value (computed from the map's own fog) and rewritten directly, so fog changes even when
    // a setter cannot be replayed. The map's fog mode decides what is written: linear uses
    // start/end, exponential modes get a density that fades out at the max distance.
    // `ring` = a per-frame copy (constant ring buffer) Unity refills from the source; written once
    // to tell it apart, then left to follow the source.
    struct ParamsCopy { uint64_t address; float original[4]; bool ring = false; };
    struct ColorCopy { uint64_t address; float original[4]; bool linear; bool ring = false; };
    inline long long g_params_probe = 0;  // when every copy was written once, 0 = not yet
    inline bool g_params_sorted = false;
    inline std::vector<ParamsCopy> g_params;
    inline std::vector<ColorCopy> g_colors;
    inline bool g_params_logged = false;

    inline void params_for(float density, float start, float end, float out[4]) {
        out[0] = density * 1.2011224087f;
        out[1] = density * 1.4426950408f;
        const float diff = end - start;
        const float inv = std::fabs(diff) > 0.0001f ? 1.0f / diff : 0.0f;
        out[2] = -inv;
        out[3] = end * inv;
    }

    inline bool close(float a, float b, float rel = 1e-4f, float abs_eps = 1e-6f) {
        return std::isfinite(a) && std::fabs(a - b) <= abs_eps + rel * std::fabs(b);
    }

    inline float to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

    inline void scan_params() {
        g_params.clear();
        g_colors.clear();
        g_params_probe = 0;
        g_params_sorted = false;
        float want[4];
        params_for(g_original.density, g_original.start, g_original.end, want);
        if (want[2] == 0.f && want[3] == 0.f) { diag::log("FOG", "renderer params: map has no linear fog range to search for"); return; }
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        if (g_scan_maps.empty()) refresh_scan_maps();
        for (const auto& m : g_scan_maps) ranges.push_back({m.begin, m.end});
        for (const auto& seg : icalls::unity_segments()) if (!seg.exec) ranges.push_back({seg.begin, seg.end});
        std::vector<uint8_t> buffer;
        uint64_t scanned = 0;
        for (const auto& range : ranges) {
            for (uint64_t at = range.first; at < range.second && g_params.size() < 1024;) {
                const size_t chunk = size_t(std::min<uint64_t>(range.second - at, 1024 * 1024));
                buffer.assign(chunk, 0);
                if (mem_read(at, buffer.data(), chunk)) {
                    for (size_t i = 0; i + 16 <= chunk; i += 4) {
                        float v[4];
                        std::memcpy(v, buffer.data() + i, 16);
                        if (!close(v[2], want[2]) || !close(v[3], want[3]) || !close(v[0], want[0]) || !close(v[1], want[1])) continue;
                        ParamsCopy copy{at + i, {}, false};
                        std::memcpy(copy.original, v, 16);
                        g_params.push_back(copy);
                        // unity_FogColor sits near unity_FogParams in the built-in parameter block
                        for (int64_t d = -0x200; d <= 0x200; d += 4) {
                            float c[4];
                            if (!mem_read(at + i + uint64_t(d), c, 16)) continue;
                            bool gamma = true, lin = true;
                            for (int k = 0; k < 3; ++k) {
                                gamma &= close(c[k], g_original.color[k], 0.f, 2e-3f);
                                lin &= close(c[k], to_linear(g_original.color[k]), 0.f, 3e-3f);
                            }
                            if (!(gamma || lin) || (g_original.color[0] == 0.f && g_original.color[1] == 0.f && g_original.color[2] == 0.f)) continue;
                            ColorCopy cc{at + i + uint64_t(d), {}, !gamma && lin, false};
                            std::memcpy(cc.original, c, 16);
                            g_colors.push_back(cc);
                            break;
                        }
                    }
                }
                scanned += chunk;
                at += chunk;
            }
        }
        diag::log("FOG", "renderer params: %zu copies of unity_FogParams (%.4f %.4f %.6f %.4f), %zu fog colors, %.0f MiB searched",
                  g_params.size(), want[0], want[1], want[2], want[3], g_colors.size(), double(scanned) / 1048576.0);
        for (size_t i = 0; i < g_params.size() && i < 8; ++i) diag::log("FOG", "  params at 0x%llx", (unsigned long long)g_params[i].address);
        if (g_params.size() > 8) diag::log("FOG", "  ... %zu more", g_params.size() - 8);
    }

    inline void write_params(float start, float end, const float color[3]) {
        if (g_params.empty()) return;
        float density = g_original.density;
        if (g_original.mode == 2) density = 4.605f / std::max(end, 1.f);       // exp: 1% left at the max distance
        else if (g_original.mode == 3) density = 2.146f / std::max(end, 1.f);  // exp2: same
        float want[4];
        params_for(density, start, end, want);
        float rgbs[2][3];
        for (int k = 0; k < 3; ++k) { rgbs[0][k] = color[k]; rgbs[1][k] = to_linear(color[k]); }
        const long long now = diag::now_ms();

        // Unity refills its per-frame copies from the source every frame; writing those too only
        // fights it (flicker). Every copy is written once, and 300 ms later the ones that went
        // back are known to be refilled copies and are left alone.
        if (!g_params_sorted && g_params_probe && now - g_params_probe >= 300) {
            size_t kept = 0, colors_kept = 0;
            for (auto& p : g_params) {
                float v[4];
                p.ring = !(mem_read(p.address, v, 16) && std::memcmp(v, want, 16) == 0);
                kept += !p.ring;
            }
            for (auto& c : g_colors) {
                float v[3];
                c.ring = !(mem_read(c.address, v, 12) && std::memcmp(v, rgbs[c.linear ? 1 : 0], 12) == 0);
                colors_kept += !c.ring;
            }
            g_params_sorted = true;
            diag::log("FOG", "renderer params: %zu of %zu copies hold their value (sources), the rest are refilled every frame; colors %zu of %zu",
                      kept, g_params.size(), colors_kept, g_colors.size());
        }
        for (const auto& p : g_params) {
            if (p.ring && g_params_sorted) continue;
            float v[4];
            if (mem_read(p.address, v, 16) && std::memcmp(v, want, 16) != 0) mem_write(p.address, want, 16);
        }
        for (const auto& c : g_colors) {
            if (c.ring && g_params_sorted) continue;
            float v[3];
            const float* rgb = rgbs[c.linear ? 1 : 0];
            if (mem_read(c.address, v, 12) && std::memcmp(v, rgb, 12) != 0) mem_write(c.address, rgb, 12);
        }
        if (!g_params_probe) g_params_probe = now;
        if (!g_params_logged) {
            g_params_logged = true;
            diag::log("FOG", "renderer params written: mode=%d density=%.4f start=%.1f end=%.1f", g_original.mode, density, start, end);
        }
    }

    inline void restore_params() {
        for (const auto& p : g_params) if (!p.ring) mem_write(p.address, p.original, 16);
        for (const auto& c : g_colors) if (!c.ring) mem_write(c.address, c.original, 16);
        g_params_logged = false;
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

    struct Want { uint8_t fog; int32_t mode; float start, end; float color[4]; };

    inline Want desired() {
        Want w{};
        w.fog = 1;
        w.mode = 1;  // FogMode.Linear: start and end only apply in this mode
        w.start = std::max(cfg::fog::min_distance, 0.f);
        w.end = std::max(cfg::fog::max_distance, w.start + 1.f);
        w.color[3] = 1.f;
        if (cfg::fog::auto_color) estimate_color(w.color);
        else { w.color[0] = cfg::fog::color.x; w.color[1] = cfg::fog::color.y; w.color[2] = cfg::fog::color.z; }
        return w;
    }

    inline bool differs(Field f, const void* value, size_t size) {
        uint8_t now[16]{};
        return g_fields[f].address && !(mem_read(g_fields[f].address, now, size) && std::memcmp(now, value, size) == 0);
    }

    inline bool fields_differ(const Want& w) {
        return differs(Fog, &w.fog, 1) || differs(Mode, &w.mode, 4) || differs(Start, &w.start, 4) ||
               differs(End, &w.end, 4) || differs(FogColor, w.color, 16);
    }

    // Field changes (setter replays) run on the worker thread.
    inline void apply_fields(const Want& w) {
        set_field(Fog, &w.fog, 1);
        set_field(Mode, &w.mode, 4);
        set_field(Start, &w.start, 4);
        set_field(End, &w.end, 4);
        set_field(FogColor, w.color, 16);
    }

    inline void apply() {
        const Want w = desired();
        write_params(w.start, w.end, w.color);
        if (!g_resolving.load() && fields_differ(w)) {
            g_resolving = true;
            std::thread([w] { apply_fields(w); g_resolving = false; }).detach();
        }
        if (!g_applied) {
            g_applied = true;
            diag::log("FOG", "applied start=%.1f end=%.1f color=(%.2f %.2f %.2f) %s", w.start, w.end, w.color[0], w.color[1],
                      w.color[2], cfg::fog::auto_color ? "estimated" : "manual");
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
            g_params.clear();
            g_colors.clear();
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
                restore_params();
                g_ready = g_applied = false;
                g_last_resolve = 0;
                return;
            }
        }

        if (!cfg::fog::enabled) {
            restore_params();
            g_resolving = true;
            std::thread([] { restore(); g_resolving = false; }).detach();
            g_applied = false;
            return;
        }
        if (!g_applied && g_params.empty()) read_original();
        if (now - g_last_write < 100) return;
        g_last_write = now;
        apply();
    }

    // Waits for work started by tick(): a resolution, a field update or a restore.
    inline void wait() { while (g_resolving.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }

    // Menu "Rescan" button.
    inline void rescan() {
        if (g_resolving.load()) return;
        if (g_applied) { restore_params(); restore(); }
        g_ready = g_applied = false;
        g_last_resolve = 0;
    }
}
