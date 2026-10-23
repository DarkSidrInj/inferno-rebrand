#pragma once
// Native icall lookup by name, for icalls the game has never called (their il2cpp wrapper's
// function-pointer slot is still empty, so a wrapper cannot be followed into the engine).
//
// The engine registers every icall with its name, e.g.
//   scripting_add_internal_call("UnityEngine.RenderSettings::get_fogStartDistance()", &fn)
// so the name string sits in libunity next to a reference to the native function. Lookup:
//   1. find the name strings (one memmem per class prefix over libunity's readable segments)
//   2. data: a pointer to the string followed by a function pointer, or a name array with the
//      function array right after it
//   3. code: every ADRP that lands on a string's page starts a small window that is evaluated
//      in order (ADRP, ADR, ADD, LDR from the GOT, MOV); at each call in it, when one argument
//      register holds the name, the other argument registers are candidates. Instruction order
//      and interleaving do not matter.
// Candidates are not trusted here: the caller traces each one (CUtils/CArm64.hpp) and keeps
// the one that behaves like the accessor it wants. The scan runs once per process, on the
// caller's background thread, and is shared by every caller.

#include "CGameContext.hpp"
#include "../CUtils/CArm64.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace icalls {
    // Names this build looks up, without the signature part.
    inline const char* const kNames[] = {
        "UnityEngine.RenderSettings::get_fog", "UnityEngine.RenderSettings::set_fog",
        "UnityEngine.RenderSettings::get_fogMode", "UnityEngine.RenderSettings::set_fogMode",
        "UnityEngine.RenderSettings::get_fogDensity", "UnityEngine.RenderSettings::set_fogDensity",
        "UnityEngine.RenderSettings::get_fogStartDistance", "UnityEngine.RenderSettings::set_fogStartDistance",
        "UnityEngine.RenderSettings::get_fogEndDistance", "UnityEngine.RenderSettings::set_fogEndDistance",
        "UnityEngine.RenderSettings::get_fogColor_Injected", "UnityEngine.RenderSettings::set_fogColor_Injected",
        "UnityEngine.RenderSettings::get_ambientEquatorColor_Injected", "UnityEngine.RenderSettings::set_ambientEquatorColor_Injected",
        "UnityEngine.RenderSettings::get_ambientSkyColor_Injected", "UnityEngine.RenderSettings::set_ambientSkyColor_Injected",
        "UnityEngine.RenderSettings::GetRenderSettings_Injected",
        "UnityEngine.Animator::get_cullingMode_Injected", "UnityEngine.Animator::set_cullingMode_Injected",
        "UnityEngine.Animator::get_cullingMode", "UnityEngine.Animator::set_cullingMode",
        "UnityEngine.Renderer::GetMaterialCount_Injected", "UnityEngine.Renderer::GetMaterialCount",
    };
    inline const char* const kPrefixes[] = {"UnityEngine.RenderSettings::", "UnityEngine.Animator::", "UnityEngine.Renderer::"};

    struct Segment { uint64_t begin, end; bool exec; };
    struct Candidate { std::string name; uint64_t fn; const char* how; };

    inline std::mutex g_mutex;
    inline int g_pid = -1;
    inline bool g_scanned = false;
    inline std::vector<Segment> g_segments;
    inline std::vector<Candidate> g_candidates;
    inline std::vector<std::pair<std::string, uint64_t>> g_strings;  // name -> string address
    inline size_t g_data_refs = 0, g_code_refs = 0;

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
            if (!name.empty() && (name.find("libunity.so") != std::string::npos || (!lib_path.empty() && name == lib_path)))
                out.push_back(entry.first);
        }
        return out;
    }

    inline bool executable(uint64_t a) {
        for (const Segment& s : g_segments) if (s.exec && a >= s.begin && a < s.end) return true;
        return false;
    }

    // Every plausible target is kept: the caller's trace is the real check. Targets inside
    // libunity's code are tried first (see candidates()).
    inline void add(const std::string& name, uint64_t fn, const char* how) {
        if (!plausible(fn) || (fn & 3)) return;
        for (const auto& c : g_candidates) if (c.name == name && c.fn == fn) return;
        size_t same = 0;
        for (const auto& c : g_candidates) same += c.name == name;
        if (same >= 16) return;
        g_candidates.push_back({name, fn, how});
    }

    inline bool known_name(const std::string& full) {
        for (const char* n : kNames) if (full == n) return true;
        return false;
    }

    inline void find_strings() {
        std::vector<uint8_t> buffer;
        for (const Segment& seg : g_segments) {
            for (uint64_t at = seg.begin; at < seg.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(seg.end - at, 1024 * 1024));
                const size_t len = size_t(std::min<uint64_t>(seg.end - at, chunk + 256));
                buffer.assign(len + 1, 0);
                if (mem_read(at, buffer.data(), len)) {
                    for (const char* prefix : kPrefixes) {
                        const size_t plen = std::strlen(prefix);
                        for (size_t i = 0; i < chunk;) {
                            const void* hit = memmem(buffer.data() + i, chunk - i, prefix, plen);
                            if (!hit) break;
                            const size_t off = size_t(static_cast<const uint8_t*>(hit) - buffer.data());
                            size_t end = off + plen;
                            while (end < len && (std::isalnum(buffer[end]) || buffer[end] == '_')) ++end;
                            if (end < len && (buffer[end] == 0 || buffer[end] == '(')) {
                                const std::string full(reinterpret_cast<const char*>(buffer.data() + off), end - off);
                                if (known_name(full)) g_strings.push_back({full, at + off});
                            }
                            i = off + 1;
                        }
                    }
                }
                at += chunk;
            }
        }
    }

    inline const std::string* name_at(uint64_t a) {
        for (const auto& s : g_strings) if (s.second == a) return &s.first;
        return nullptr;
    }

    inline void scan_data() {
        std::vector<uint64_t> wanted;
        for (const auto& s : g_strings) wanted.push_back(s.second);
        std::sort(wanted.begin(), wanted.end());
        auto is_name_ptr = [](uint64_t v) {
            char text[12]{};
            return plausible(v) && mem_read(v, text, 12) && std::strncmp(text, "UnityEngine.", 12) == 0;
        };
        std::vector<uint64_t> words;
        for (const Segment& seg : g_segments) {
            if (seg.exec) continue;
            for (uint64_t at = seg.begin; at < seg.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(seg.end - at, 1024 * 1024)) & ~size_t(7);
                if (chunk < 16) break;
                words.assign(chunk / 8, 0);
                if (mem_read(at, words.data(), chunk)) {
                    for (size_t i = 0; i < words.size(); ++i) {
                        if (!std::binary_search(wanted.begin(), wanted.end(), words[i])) continue;
                        const std::string* name = name_at(words[i]);
                        if (!name) continue;
                        ++g_data_refs;
                        const uint64_t slot = at + i * 8;
                        add(*name, rpm<uint64_t>(slot + 8), "name/function pair");
                        uint64_t first = slot, last = slot;
                        for (int guard = 0; guard < 4096 && is_name_ptr(rpm<uint64_t>(last + 8)); ++guard) last += 8;
                        for (int guard = 0; guard < 4096 && is_name_ptr(rpm<uint64_t>(first - 8)); ++guard) first -= 8;
                        const uint64_t index = (slot - first) / 8;
                        for (uint64_t gap : {uint64_t(0), uint64_t(8)})
                            add(*name, rpm<uint64_t>(last + 8 + gap + index * 8), "name array + function array");
                    }
                }
                at += chunk;
            }
        }
    }

    // Evaluates code[from, to) in order, tracking only what builds addresses. At each call or
    // jump after `site`, an argument register holding `text` makes the other argument
    // registers that point into code candidates for `name`.
    inline bool evaluate(const std::vector<uint32_t>& code, uint64_t base, size_t from, size_t to, size_t site,
                         uint64_t text, const std::string& name) {
        std::array<uint64_t, 32> value{};
        std::array<bool, 32> known{};
        bool found = false;
        for (size_t j = from; j < to && j < code.size(); ++j) {
            const uint32_t ins = code[j];
            const uint64_t pc = base + j * 4;
            const int rd = int(ins & 31), rn = int((ins >> 5) & 31);
            const bool call = (ins & 0xFC000000) == 0x94000000 || (ins & 0xFC000000) == 0x14000000 ||
                              (ins & 0xFE1FFC1F) == 0xD61F0000 || (ins & 0xFE1F0C1F) == 0xD61F0800;
            if (call) {
                if (j > site) {
                    for (int r = 0; r <= 3; ++r) {
                        if (!known[r] || value[r] != text) continue;
                        for (int q = 0; q <= 3; ++q) {
                            if (q == r || !known[q]) continue;
                            const size_t before = g_candidates.size();
                            add(name, value[q], "registration call site");
                            found |= g_candidates.size() != before;
                        }
                    }
                    if (found) return true;
                }
                for (int r = 0; r <= 18; ++r) known[r] = false;  // a call clobbers the argument registers
                continue;
            }
            if ((ins & 0x1F000000) == 0x10000000) {  // ADR / ADRP
                const uint64_t imm = a64::sext((uint64_t((ins >> 5) & 0x7FFFF) << 2) | ((ins >> 29) & 3), 21);
                value[rd] = (ins & 0x80000000) ? (pc & ~uint64_t(0xFFF)) + (imm << 12) : pc + imm;
                known[rd] = true;
            } else if ((ins & 0xFF800000) == 0x91000000) {  // ADD Xd, Xn, #imm
                if (rn != 31 && known[rn]) {
                    value[rd] = value[rn] + (uint64_t((ins >> 10) & 0xFFF) << (((ins >> 22) & 1) ? 12 : 0));
                    known[rd] = true;
                } else known[rd] = false;
            } else if ((ins & 0xFFC00000) == 0xF9400000) {  // LDR Xt, [Xn, #imm] (GOT entry)
                if (rn != 31 && known[rn]) {
                    value[rd] = rpm<uint64_t>(value[rn] + uint64_t((ins >> 10) & 0xFFF) * 8);
                    known[rd] = plausible(value[rd]);
                } else known[rd] = false;
            } else if ((ins & 0xFFE0FFE0) == 0xAA0003E0) {  // MOV Xd, Xm
                const int rm = int((ins >> 16) & 31);
                known[rd] = known[rm];
                value[rd] = value[rm];
            } else if ((ins & 0x0A000000) == 0x08000000) {  // loads/stores: a load clobbers Rt
                if ((ins >> 22) & 1) known[rd] = false;
            } else {
                known[rd] = false;  // anything else may write Rd
            }
        }
        return found;
    }

    inline std::vector<std::vector<uint32_t>> g_unmatched_sites;  // for the log

    inline void scan_code() {
        std::unordered_map<uint64_t, std::vector<uint64_t>> by_page;
        for (const auto& s : g_strings) by_page[s.second & ~uint64_t(0xFFF)].push_back(s.second);
        std::vector<uint32_t> code;
        for (const Segment& seg : g_segments) {
            if (!seg.exec) continue;
            for (uint64_t at = seg.begin; at + 8 < seg.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(seg.end - at, 4 * 1024 * 1024)) & ~size_t(3);
                const uint64_t start = at >= seg.begin + 256 ? at - 256 : seg.begin;  // window context before
                const size_t len = size_t(std::min<uint64_t>(seg.end - start, (at - start) + chunk + 256)) & ~size_t(3);
                code.assign(len / 4, 0);
                if (mem_read(start, code.data(), len)) {
                    const size_t first = size_t(at - start) / 4, last = first + chunk / 4;
                    for (size_t k = first; k < last && k < code.size(); ++k) {
                        const uint32_t a = code[k];
                        if ((a & 0x9F000000) != 0x90000000) continue;
                        const uint64_t pc = start + k * 4;
                        const uint64_t page = (pc & ~uint64_t(0xFFF)) + (a64::sext((uint64_t((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3), 21) << 12);
                        const auto hit = by_page.find(page);
                        if (hit == by_page.end()) continue;
                        // the ADD completing the address may come a few instructions later
                        const int reg = int(a & 31);
                        for (size_t j = k + 1; j < k + 8 && j < code.size(); ++j) {
                            const uint32_t b = code[j];
                            if ((b & 0xFF800000) != 0x91000000 || int((b >> 5) & 31) != reg) continue;
                            const uint64_t text = page + ((b >> 10) & 0xFFF);
                            if (std::find(hit->second.begin(), hit->second.end(), text) == hit->second.end()) break;
                            ++g_code_refs;
                            const std::string* name = name_at(text);
                            const size_t from = k >= 16 ? k - 16 : 0;
                            if (name && !evaluate(code, start, from, k + 28, j, text, *name) && g_unmatched_sites.size() < 4) {
                                std::vector<uint32_t> words(code.begin() + long(from), code.begin() + long(std::min(code.size(), k + 16)));
                                g_unmatched_sites.push_back(words);
                            }
                            break;
                        }
                    }
                }
                at += chunk;
            }
        }
    }

    inline void scan_locked() {
        g_scanned = true;
        g_segments = unity_segments();
        g_candidates.clear();
        g_strings.clear();
        g_unmatched_sites.clear();
        g_data_refs = g_code_refs = 0;
        if (g_segments.empty()) { diag::log("WARN", "ICALL scan: no libunity mappings"); return; }
        find_strings();
        if (g_strings.empty()) { diag::log("WARN", "ICALL scan: no icall names found in libunity"); return; }
        scan_data();
        scan_code();
        diag::log("ICALL", "scan: %zu segments, %zu names, %zu data refs, %zu code refs, %zu candidates",
                  g_segments.size(), g_strings.size(), g_data_refs, g_code_refs, g_candidates.size());
        for (const auto& c : g_candidates)
            diag::log("ICALL", "  candidate %s = 0x%llx (%s)", c.name.c_str(), (unsigned long long)c.fn, c.how);
        for (const auto& words : g_unmatched_sites) {
            std::string text;
            char hex[10];
            for (uint32_t w : words) { std::snprintf(hex, sizeof(hex), "%08x ", w); text += hex; }
            diag::log("ICALL", "  unmatched site: %s", text.c_str());
        }
    }

    // Candidate native functions for `name` (no signature), most specific first.
    inline std::vector<Candidate> candidates(const std::string& name) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pid != proc::pid) { g_pid = proc::pid; g_scanned = false; }
        if (!g_scanned) scan_locked();
        std::vector<Candidate> out;
        for (const auto& c : g_candidates) if (c.name == name && executable(c.fn)) out.push_back(c);
        for (const auto& c : g_candidates) if (c.name == name && !executable(c.fn)) out.push_back(c);
        return out;
    }

    // Is the il2cpp wrapper at `entry` still waiting for its icall (function-pointer slot 0)?
    // Reads the first ADRP + LDR pair, which is how every wrapper loads its slot.
    inline bool slot_empty(uint64_t entry) {
        uint32_t words[12]{};
        if (!mem_read(entry, words, sizeof(words))) return false;
        for (int i = 0; i + 1 < 12; ++i) {
            const uint32_t a = words[i];
            if ((a & 0x9F000000) != 0x90000000) continue;
            const int reg = int(a & 31);
            for (int j = i + 1; j < std::min(i + 4, 12); ++j) {
                const uint32_t b = words[j];
                if ((b & 0xFFC00000) != 0xF9400000 || int((b >> 5) & 31) != reg) continue;
                const uint64_t pc = entry + uint64_t(i) * 4;
                const uint64_t page = (pc & ~uint64_t(0xFFF)) + (a64::sext((uint64_t((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3), 21) << 12);
                uint64_t value = rpm<uint64_t>(page + uint64_t((b >> 10) & 0xFFF) * 8);
                // PIC wrappers load the slot's address from the GOT first: ldr xT, [xT]
                const int rt = int(b & 31);
                if (j + 1 < 12 && (words[j + 1] & 0xFFC00000) == 0xF9400000 && int((words[j + 1] >> 5) & 31) == rt && plausible(value))
                    value = rpm<uint64_t>(value + uint64_t((words[j + 1] >> 10) & 0xFFF) * 8);
                return value == 0;
            }
            return false;
        }
        return false;
    }
}
