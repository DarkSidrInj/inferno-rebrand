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
//   3. icall by name    (libunity registers "UnityEngine.RenderSettings::get_fog..." with its
//                        function; found by the name string, then a {name, fn} table, parallel
//                        name/function arrays, or an ADRP/ADD x0, ADRP/ADD x1 call site)
// A field still missing after that is placed from fog start with Unity's member order
// (m_Fog, m_FogColor, m_FogMode, m_FogDensity, m_LinearFogStart, m_LinearFogEnd) and kept
// only if its value is sane. The object moves on scene loads, so fog start is re-read every
// two seconds and everything is resolved again when it moved. Turning the fog off writes the
// game's values back.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "../CUtils/CArm64.hpp"
#include "../CConfigs/CConfig.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
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
    inline bool g_icalls_scanned = false;
    inline std::vector<std::pair<std::string, uint64_t>> g_icalls;  // icall name -> native function

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

    inline uint64_t trace_object(std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = proc::lib + v100::rs_get_render_settings_injected;
        start.x[0] = {0, true, 0, 0};
        auto goal = [](const a64::Machine& m) { return m.returned && m.x[0].known && outside(m.x[0].value); };
        a64::Machine found;
        if (a64::explore(session, start, goal, found)) return found.x[0].value;
        why = session.why;
        return 0;
    }

    // ---- icall registration lookup in libunity ------------------------------------------------

    struct Segment { uint64_t begin, end; bool exec; };

    inline std::vector<Segment> unity_segments() {
        std::vector<Segment> out;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
        FILE* file = std::fopen(path, "r");
        if (!file) return out;
        char line[1024];
        std::string lib_path;
        std::vector<std::pair<Segment, std::string>> all;
        while (std::fgets(line, sizeof(line), file)) {
            unsigned long long begin = 0, end = 0;
            char perms[5]{};
            if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, perms) != 3 || perms[0] != 'r') continue;
            const char* slash = std::strchr(line, '/');
            std::string name = slash ? std::string(slash) : std::string();
            while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
            if (begin == proc::lib && !name.empty()) lib_path = name;
            all.push_back({{begin, end, perms[2] == 'x'}, name});
        }
        std::fclose(file);
        for (const auto& entry : all) {
            const std::string& name = entry.second;
            if (name.empty()) continue;
            if (name.find("libunity.so") != std::string::npos || (!lib_path.empty() && name == lib_path))
                out.push_back(entry.first);
        }
        return out;
    }

    // Validates a candidate native function by tracing it like the wrapper would be.
    inline bool icall_traces(uint64_t fn, const std::string& icall) {
        for (int f = 0; f < kFields; ++f) {
            const Spec& spec = kSpecs[f];
            std::string why;
            if (icall == spec.icall_get) return trace_getter(fn, spec.kind, why) != 0;
            if (icall == spec.icall_set) return trace_setter(fn, spec.kind, why) != 0;
        }
        return false;
    }

    inline void scan_icalls() {
        g_icalls_scanned = true;
        g_icalls.clear();
        const std::vector<Segment> segments = unity_segments();
        if (segments.empty()) { diag::log("WARN", "FOG icall scan: no libunity mappings found"); return; }
        static const char kPrefix[] = "UnityEngine.RenderSettings::";
        const size_t prefix_len = sizeof(kPrefix) - 1;

        // 1. the name strings
        std::vector<std::pair<std::string, uint64_t>> names;  // icall short name -> string address
        std::vector<uint8_t> buffer;
        uint64_t scanned = 0;
        for (const Segment& seg : segments) {
            for (uint64_t at = seg.begin; at < seg.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(seg.end - at, 1024 * 1024));
                buffer.assign(chunk + 128, 0);
                const size_t len = size_t(std::min<uint64_t>(seg.end - at, chunk + 128));
                if (mem_read(at, buffer.data(), len)) {
                    for (size_t i = 0; i + prefix_len < chunk;) {
                        const void* hit = memmem(buffer.data() + i, chunk - i, kPrefix, prefix_len);
                        if (!hit) break;
                        const size_t off = size_t(static_cast<const uint8_t*>(hit) - buffer.data());
                        const char* tail = reinterpret_cast<const char*>(buffer.data() + off + prefix_len);
                        for (int f = 0; f < kFields; ++f) {
                            for (const char* icall : {kSpecs[f].icall_get, kSpecs[f].icall_set}) {
                                const size_t n = std::strlen(icall);
                                if (off + prefix_len + n < len && std::strncmp(tail, icall, n) == 0 && (tail[n] == 0 || tail[n] == '('))
                                    names.push_back({icall, at + off});
                            }
                        }
                        i = off + 1;
                    }
                }
                scanned += chunk;
                at += chunk;
            }
        }
        if (names.empty()) {
            diag::log("WARN", "FOG icall scan: no RenderSettings icall names in %zu libunity segments (%.1f MiB)",
                      segments.size(), double(scanned) / 1048576.0);
            return;
        }
        std::vector<uint64_t> wanted;
        for (const auto& n : names) wanted.push_back(n.second);
        std::sort(wanted.begin(), wanted.end());
        auto name_at = [&](uint64_t a) -> const std::string* {
            for (const auto& n : names) if (n.second == a) return &n.first;
            return nullptr;
        };
        auto accept = [&](const std::string& icall, uint64_t fn, const char* how) {
            for (const auto& known : g_icalls) if (known.first == icall) return;
            if (!plausible(fn) || !icall_traces(fn, icall)) return;
            g_icalls.push_back({icall, fn});
            diag::log("FOG", "icall %s = 0x%llx (%s)", icall.c_str(), (unsigned long long)fn, how);
        };
        auto is_name_ptr = [&](uint64_t v) {
            char text[12]{};
            return plausible(v) && mem_read(v, text, 12) && std::strncmp(text, "UnityEngine.", 12) == 0;
        };

        // 2. data: {name, fn} pairs, or name and function arrays side by side
        for (const Segment& seg : segments) {
            if (seg.exec) continue;
            for (uint64_t at = seg.begin; at < seg.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(seg.end - at, 1024 * 1024)) & ~size_t(7);
                if (chunk < 16) break;
                std::vector<uint64_t> words(chunk / 8);
                if (mem_read(at, words.data(), chunk)) {
                    for (size_t i = 0; i < words.size(); ++i) {
                        if (!std::binary_search(wanted.begin(), wanted.end(), words[i])) continue;
                        const std::string* icall = name_at(words[i]);
                        if (!icall) continue;
                        const uint64_t slot = at + i * 8;
                        accept(*icall, rpm<uint64_t>(slot + 8), "name/function pair");
                        // parallel arrays: find this name array's end, functions follow it
                        uint64_t last = slot;
                        for (int guard = 0; guard < 2048 && is_name_ptr(rpm<uint64_t>(last + 8)); ++guard) last += 8;
                        uint64_t first = slot;
                        for (int guard = 0; guard < 2048 && is_name_ptr(rpm<uint64_t>(first - 8)); ++guard) first -= 8;
                        const uint64_t count = (last - first) / 8 + 1, index = (slot - first) / 8;
                        for (uint64_t gap : {uint64_t(0), uint64_t(8)})
                            accept(*icall, rpm<uint64_t>(last + 8 + gap + index * 8), "name array + function array");
                        (void)count;
                    }
                }
                at += chunk;
            }
        }

        // 3. code: adrp x0 / add x0 (name) followed by adrp x1 / add x1 (function)
        for (const Segment& seg : segments) {
            if (!seg.exec) continue;
            for (uint64_t at = seg.begin; at + 8 < seg.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(seg.end - at, 4 * 1024 * 1024)) & ~size_t(3);
                std::vector<uint32_t> code(chunk / 4 + 16, 0);
                const size_t len = size_t(std::min<uint64_t>(seg.end - at, chunk + 64)) & ~size_t(3);
                if (mem_read(at, code.data(), len)) {
                    const size_t count = len / 4;
                    auto adrp_add = [&](size_t k, int reg, uint64_t& out) {
                        if (k + 1 >= count) return false;
                        const uint32_t a = code[k], b = code[k + 1];
                        if ((a & 0x9F00001F) != uint32_t(0x90000000 | reg)) return false;
                        if ((b & 0xFFC003FF) != uint32_t(0x91000000 | (reg << 5) | reg)) return false;
                        const uint64_t pc = at + k * 4;
                        const uint64_t imm = a64::sext((uint64_t((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3), 21);
                        out = (pc & ~uint64_t(0xFFF)) + (imm << 12) + ((b >> 10) & 0xFFF);
                        return true;
                    };
                    for (size_t k = 0; k + 1 < std::min(count, chunk / 4); ++k) {
                        uint64_t text = 0;
                        if (!adrp_add(k, 0, text) || !std::binary_search(wanted.begin(), wanted.end(), text)) continue;
                        const std::string* icall = name_at(text);
                        for (size_t j = k + 2; icall && j < k + 10; ++j) {
                            uint64_t fn = 0;
                            if (adrp_add(j, 1, fn)) { accept(*icall, fn, "registration call site"); break; }
                        }
                    }
                }
                at += chunk;
            }
        }
        diag::log("FOG", "icall scan: %zu names, %zu functions resolved", names.size(), g_icalls.size());
    }

    inline uint64_t icall(const char* name) {
        for (const auto& known : g_icalls) if (known.first == name) return known.second;
        return 0;
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

    inline bool resolve_field(Field f, bool allow_icall_scan) {
        const Spec& spec = kSpecs[f];
        Resolved& r = g_fields[f];
        r = Resolved{};
        std::string why_get, why_set;
        if (uint64_t a = trace_getter(proc::lib + spec.getter, spec.kind, why_get)) {
            r = {a, "getter wrapper", proc::lib + spec.getter, false};
            return true;
        }
        if (uint64_t a = trace_setter(proc::lib + spec.setter, spec.kind, why_set)) {
            r = {a, "setter wrapper", proc::lib + spec.setter, true};
            return true;
        }
        if (allow_icall_scan && !g_icalls_scanned) scan_icalls();
        std::string why;
        if (uint64_t fn = icall(spec.icall_get)) {
            if (uint64_t a = trace_getter(fn, spec.kind, why)) { r = {a, "icall getter", fn, false}; return true; }
        }
        if (uint64_t fn = icall(spec.icall_set)) {
            if (uint64_t a = trace_setter(fn, spec.kind, why)) { r = {a, "icall setter", fn, true}; return true; }
        }
        r.how = "missing (getter: " + why_get + ", setter: " + why_set + ")";
        return false;
    }

    inline void resolve_all() {
        g_ready = false;
        g_applied = false;
        std::string why;
        g_object = trace_object(why);
        if (g_object) diag::log("FOG", "GetRenderSettings -> 0x%llx", (unsigned long long)g_object);
        else diag::log("FOG", "GetRenderSettings not readable (%s)", why.c_str());

        // Fog start first: it anchors the inference of the rest.
        resolve_field(Start, true);
        for (int f = 0; f < kFields; ++f) if (f != Start) resolve_field(Field(f), true);

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
        if (!g_ready) diag::log("WARN", "FOG not usable: fog, start and end are all needed");
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
            g_pid = proc::pid;
            g_ready = g_applied = false;
            g_icalls_scanned = false;
            g_icalls.clear();
            g_last_resolve = 0;
            for (auto& f : g_fields) f = Resolved{};
        }
        if (proc::pid <= 0 || !plausible(proc::lib)) return;
        if (!cfg::fog::enabled && !g_applied) return;
        const long long now = diag::now_ms();

        if (!g_ready) {
            if (!cfg::fog::enabled || now - g_last_resolve < 3000) return;
            g_last_resolve = now;
            resolve_all();
            if (!g_ready) return;
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

    // Menu "Rescan" button.
    inline void rescan() {
        if (g_applied) restore();
        g_ready = g_applied = false;
        g_icalls_scanned = false;
        g_last_resolve = 0;
    }
}
