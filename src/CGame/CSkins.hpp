#pragma once
// Skin changer for the weapons in the local player's hands (only this client sees it).
// From dump.cs:
//   - beh (chx part of every player: the weapon in hands) caches what it loaded as
//     acxp = (WeaponId @+0x40, WeaponMetaId @+0x42, view mode @+0x44); <opId> yppb @+0x48 grows
//     with every load (SetWeaponContent). Its onk (+0x80) is the match's weapon meta registry yvh.
//   - yvh.jmqq (+0x10) = List<WeaponMetaData>; WeaponMetaData (0x38 bytes: FixedList32Bytes is
//     8-aligned) = OwnerId u8 @0, SkinId i32 @4, CharmId i32 @8, stickers @0x10, SkinPatternId @0x30.
//     The content loader reads SkinId from there by the weapon's WeaponMetaId.
//   - SkinDefinition (ScriptableObject): _id i32 @+0x18, _weaponId (yrr, byte) @+0x1C.
// The entry of the weapon in hands gets the chosen SkinId, and acxp's meta id is spoiled so beh
// sees a different weapon state and loads again. An entry may be shared by several weapons of
// one owner (same meta data), so it always carries the skin of the weapon in hands; the game's
// value is put back when nothing is chosen, or on disable.
// Calibrated in game (logged): whether WeaponMetaId.Raw is the list index or index + 1 (votes from
// every player's skinned weapon: the entry's SkinId must be a skin of the weapon in hands), and
// where the native ScriptableObject keeps its name.

#include "CGameContext.hpp"
#include "CClass.hpp"
#include "CIcalls.hpp"
#include "../CConfigs/CConfig.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace skins {
    inline constexpr uint64_t kBehState = 0x40, kBehMeta = 0x42, kBehOpId = 0x48, kBehRegistry = 0x80;
    inline constexpr uint64_t kRegistryList = 0x10;
    inline constexpr uint64_t kEntryStride = 0x38, kEntryOwner = 0x0, kEntrySkin = 0x4;
    inline constexpr uint64_t kDefId = 0x18, kDefWeapon = 0x1C;

    struct Def { int id{}; int weapon{}; bool stattrack{}; std::string name; };
    struct Patch { int original{}; int written{}; };
    struct View { int weapon = -1; int current = 0; std::vector<int> ids; std::vector<std::string> names; std::string status; };

    inline int g_pid = -1;
    inline std::mutex g_mutex;                       // g_defs, g_status
    inline std::vector<Def> g_defs;                  // sorted by weapon, then name
    inline std::unordered_map<int, size_t> g_def_by_id;
    inline std::string g_status = "waiting for a match";
    inline std::atomic<bool> g_scanning{false};
    inline std::atomic<bool> g_scanned{false};

    inline std::unordered_map<uint64_t, uint64_t> g_beh;  // player -> beh
    inline int g_index_shift = -1;                   // list index = Raw - shift
    inline int g_votes[2][2]{};                      // [shift][matches weapon]
    inline std::unordered_map<uint64_t, bool> g_voted;  // (beh, meta) pairs counted once
    inline uint64_t g_list = 0;                      // registry list the patches are in
    inline std::unordered_map<int, Patch> g_patched; // entry index -> values
    inline std::atomic<int> g_held_weapon{-1};
    inline std::atomic<int> g_held_skin{0};
    inline constexpr int kSpoiled = 0xFFFE;
    inline uint64_t g_reload_beh = 0;
    inline int g_reload_op = 0, g_reload_meta = 0;
    inline long long g_reload_at = 0, g_log = 0;

    inline void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (s != g_status) { g_status = s; diag::log("SKINS", "%s", s.c_str()); }
    }

    // ---- definitions --------------------------------------------------------------------------
    inline bool class_name(uint64_t k, char* out, size_t cap) { return klass::name_at(k, klass::g_name_offset, out, cap); }

    inline bool def_class_name(uint64_t k, bool& stattrack) {
        char name[32], space[48];
        if (!class_name(k, name, sizeof(name))) return false;
        const bool plain = !std::strcmp(name, "SkinDefinition");
        stattrack = !std::strcmp(name, "StatTrackSkinDefinition");
        return (plain || stattrack) &&
               klass::read_name(rpm<uint64_t>(k + uint64_t(klass::g_name_offset) + 8), space, sizeof(space)) &&
               std::strstr(space, "Inventory");
    }

    // SkinDefinition and StatTrackSkinDefinition among libunity's TypeInfo globals.
    inline std::vector<std::pair<uint64_t, bool>> typeinfo_def_classes() {
        std::vector<std::pair<uint64_t, bool>> out;
        if (!klass::calibrate()) return out;
        std::unordered_map<uint64_t, bool> checked;
        std::vector<uint64_t> words;
        for (const auto& seg : icalls::unity_segments()) {
            if (seg.exec) continue;
            for (uint64_t at = seg.begin; at < seg.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(seg.end - at, 1024 * 1024)) & ~size_t(7);
                if (chunk < 8) break;
                words.assign(chunk / 8, 0);
                if (mem_read(at, words.data(), chunk)) {
                    for (uint64_t v : words) {
                        bool st = false;
                        if (!plausible(v) || !checked.emplace(v, true).second || !def_class_name(v, st)) continue;
                        if (std::none_of(out.begin(), out.end(), [v](const auto& c) { return c.first == v; })) out.push_back({v, st});
                    }
                }
                at += chunk;
            }
        }
        return out;
    }

    inline std::vector<std::pair<uint64_t, uint64_t>> heap_maps() {
        std::vector<std::pair<uint64_t, uint64_t>> maps;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
        FILE* f = std::fopen(path, "r");
        if (!f) return maps;
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long b = 0, e = 0;
            char perms[5]{};
            if (std::sscanf(line, "%llx-%llx %4s", &b, &e, perms) != 3 || perms[0] != 'r' || perms[1] != 'w') continue;
            if (e - b < 0x1000 || e - b > 512ULL * 1024 * 1024 || std::strchr(line, '/') || std::strstr(line, "[stack")) continue;
            maps.push_back({b, e});
        }
        std::fclose(f);
        return maps;
    }

    // Readable non-code mappings that can hold il2cpp metadata (anonymous, or a file named like it).
    inline std::vector<std::pair<uint64_t, uint64_t>> metadata_maps() {
        std::vector<std::pair<uint64_t, uint64_t>> maps;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
        FILE* f = std::fopen(path, "r");
        if (!f) return maps;
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long b = 0, e = 0;
            char perms[5]{};
            if (std::sscanf(line, "%llx-%llx %4s", &b, &e, perms) != 3 || perms[0] != 'r' || perms[2] == 'x') continue;
            if (e - b < 0x1000 || e - b > 1024ULL * 1024 * 1024 || std::strstr(line, "[stack")) continue;
            if (std::strchr(line, '/') && !std::strstr(line, "metadata")) continue;
            maps.push_back({b, e});
        }
        std::fclose(f);
        return maps;
    }

    // The classes by their name strings: every place the metadata holds "\0SkinDefinition\0", then
    // every word in the heap pointing at one of them sitting where Il2CppClass keeps its name.
    // Slower than the TypeInfo globals (two passes over memory), for when those are not filled.
    inline std::vector<std::pair<uint64_t, bool>> string_def_classes() {
        std::vector<std::pair<uint64_t, bool>> out;
        if (!klass::calibrate()) return out;
        const std::string texts[2] = {std::string("\0SkinDefinition\0", 16), std::string("\0StatTrackSkinDefinition\0", 25)};
        std::unordered_map<uint64_t, bool> strings;  // address of the text -> stattrack
        std::vector<char> buf;
        const size_t overlap = 32;
        for (const auto& m : metadata_maps()) {
            for (uint64_t at = m.first; at < m.second;) {
                const size_t chunk = size_t(std::min<uint64_t>(m.second - at, 4 * 1024 * 1024));
                buf.assign(chunk, 0);
                if (mem_read(at, buf.data(), chunk)) {
                    for (int t = 0; t < 2; ++t) {
                        const char* from = buf.data();
                        const char* end = buf.data() + chunk;
                        while (const void* hit = memmem(from, size_t(end - from), texts[t].data(), texts[t].size())) {
                            const char* h = static_cast<const char*>(hit);
                            strings[at + uint64_t(h - buf.data()) + 1] = t == 1;
                            from = h + 1;
                        }
                    }
                }
                if (at + chunk >= m.second) break;
                at += chunk - overlap;
            }
        }
        diag::log("SKINS", "class name strings in memory: %zu", strings.size());
        if (strings.empty()) return out;
        uint64_t lo = ~0ull, hi = 0;
        for (const auto& e : strings) { lo = std::min(lo, e.first); hi = std::max(hi, e.first); }
        std::vector<uint64_t> words;
        for (const auto& m : heap_maps()) {
            for (uint64_t at = m.first; at < m.second;) {
                const size_t chunk = size_t(std::min<uint64_t>(m.second - at, 1024 * 1024)) & ~size_t(7);
                if (chunk < 8) break;
                words.assign(chunk / 8, 0);
                if (mem_read(at, words.data(), chunk)) {
                    for (size_t i = 0; i < words.size(); ++i) {
                        if (words[i] < lo || words[i] > hi || !strings.count(words[i])) continue;
                        const uint64_t k = at + i * 8 - uint64_t(klass::g_name_offset);
                        bool st = false;
                        if (!def_class_name(k, st)) continue;
                        if (std::none_of(out.begin(), out.end(), [k](const auto& c) { return c.first == k; })) out.push_back({k, st});
                    }
                }
                at += chunk;
            }
        }
        return out;
    }

    inline std::vector<std::pair<uint64_t, bool>> find_def_classes() {
        auto classes = typeinfo_def_classes();
        const char* how = "TypeInfo globals";
        if (classes.empty()) { classes = string_def_classes(); how = "name strings"; }
        for (const auto& c : classes)
            diag::log("SKINS", "class %s at 0x%llx (%s)", c.second ? "StatTrackSkinDefinition" : "SkinDefinition",
                      (unsigned long long)c.first, how);
        return classes;
    }

    // Printable name at `p`, or "".
    inline std::string text_at(uint64_t p) {
        char buf[72];
        if (!klass::read_name(p, buf, sizeof(buf))) return {};
        const size_t n = std::strlen(buf);
        return n >= 3 ? std::string(buf, n) : std::string();
    }

    // Where the native object keeps its name: the offset (pointer to the text, or the text inline)
    // that gives a printable, mostly distinct string on the most definitions.
    inline void name_definitions(std::vector<Def>& defs, const std::vector<uint64_t>& natives) {
        int best_off = -1, best_inline = 0, best_score = 0;
        const size_t sample = std::min<size_t>(natives.size(), 300);
        for (int inl = 0; inl < 2; ++inl) {
            for (int off = 0x10; off <= 0x100; off += 8) {
                std::unordered_map<std::string, bool> seen;
                int score = 0;
                for (size_t i = 0; i < sample; ++i) {
                    if (!plausible(natives[i])) continue;
                    const uint64_t at = natives[i] + uint64_t(off);
                    const std::string t = text_at(inl ? at : rpm<uint64_t>(at));
                    if (!t.empty() && seen.emplace(t, true).second) ++score;
                }
                if (score > best_score) { best_score = score; best_off = off; best_inline = inl; }
            }
        }
        const bool ok = best_off >= 0 && best_score * 2 >= int(sample);
        diag::log("SKINS", "names: %s (native +0x%x%s, %d of %zu distinct)", ok ? "found" : "NOT found", best_off,
                  best_inline ? " inline" : "", best_score, sample);
        for (size_t i = 0; i < defs.size(); ++i) {
            std::string t;
            if (ok && plausible(natives[i])) {
                const uint64_t at = natives[i] + uint64_t(best_off);
                t = text_at(best_inline ? at : rpm<uint64_t>(at));
            }
            defs[i].name = (t.empty() ? "#" + std::to_string(defs[i].id) : t) + (defs[i].stattrack ? " (ST)" : "");
        }
    }

    inline void scan_definitions() {
        const long long t0 = diag::now_ms();
        const auto classes = find_def_classes();
        if (classes.empty()) { set_status("skin definitions: class not found"); g_scanned = false; return; }
        std::vector<Def> defs;
        std::vector<uint64_t> natives;
        std::unordered_map<uint64_t, bool> seen_objects;
        std::vector<uint64_t> words;
        for (const auto& m : heap_maps()) {
            for (uint64_t at = m.first; at < m.second;) {
                const size_t chunk = size_t(std::min<uint64_t>(m.second - at, 1024 * 1024)) & ~size_t(7);
                if (chunk < 16) break;
                words.assign(chunk / 8, 0);
                if (mem_read(at, words.data(), chunk)) {
                    for (size_t i = 0; i < words.size(); ++i) {
                        const auto c = std::find_if(classes.begin(), classes.end(), [&](const auto& k) { return k.first == words[i]; });
                        if (c == classes.end()) continue;
                        const uint64_t object = at + i * 8;
                        const uint64_t own = reinterpret_cast<uint64_t>(words.data());
                        if (proc::pid == getpid() && object >= own && object < own + chunk) continue;  // our copy (selftest)
                        const int id = rpm<int>(object + kDefId);
                        const int weapon = rpm<uint8_t>(object + kDefWeapon);
                        const uint64_t native = rpm<uint64_t>(object + 0x10);
                        if (id <= 0 || id > 100000000 || !weapon || (native && !plausible(native))) continue;
                        if (!seen_objects.emplace(object, true).second) continue;
                        defs.push_back({id, weapon, c->second, {}});
                        natives.push_back(native);
                    }
                }
                at += chunk;
            }
        }
        name_definitions(defs, natives);
        std::vector<size_t> order(defs.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return defs[a].weapon != defs[b].weapon ? defs[a].weapon < defs[b].weapon : defs[a].name < defs[b].name;
        });
        std::vector<Def> sorted;
        std::unordered_map<int, size_t> by_id;
        for (size_t i : order) {
            if (by_id.count(defs[i].id)) continue;
            by_id[defs[i].id] = sorted.size();
            sorted.push_back(defs[i]);
        }
        int weapons = 0;
        for (size_t i = 0; i < sorted.size(); ++i) if (!i || sorted[i].weapon != sorted[i - 1].weapon) ++weapons;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_defs = std::move(sorted);
            g_def_by_id = std::move(by_id);
        }
        diag::log("SKINS", "%zu skin definitions for %d weapons (%zu classes, %lld ms)", g_def_by_id.size(), weapons,
                  classes.size(), diag::now_ms() - t0);
        for (size_t i = 0; i < g_defs.size() && i < 6; ++i)
            diag::log("SKINS", "  e.g. id %d weapon %d \"%s\"", g_defs[i].id, g_defs[i].weapon, g_defs[i].name.c_str());
        g_scanned = !g_def_by_id.empty();
        if (!g_scanned) set_status("skin definitions: none in memory");
    }

    inline int def_weapon(int id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_def_by_id.find(id);
        return it == g_def_by_id.end() ? -1 : g_defs[it->second].weapon;
    }

    // ---- match state ---------------------------------------------------------------------------
    inline uint64_t find_beh(uint64_t player) {
        const auto it = g_beh.find(player);
        if (it != g_beh.end() && it->second && klass::is_named(it->second, "beh")) return it->second;
        uint64_t found = 0;
        const uint64_t container = rpm<uint64_t>(player + v100::net_systems);
        if (plausible(container)) {
            for (uint64_t field = 0x10; field <= 0x38 && !found; field += 8) {
                const uint64_t array = rpm<uint64_t>(container + field);
                const uint64_t count = plausible(array) ? rpm<uint64_t>(array + v100::array_length) : 0;
                if (count > 256) continue;
                for (uint64_t i = 0; i < count; ++i) {
                    const uint64_t c = rpm<uint64_t>(array + v100::array_data + i * 8);
                    if (plausible(c) && klass::is_named(c, "beh")) { found = c; break; }
                }
            }
        }
        if (g_beh.size() > 128) g_beh.clear();
        g_beh[player] = found;
        return found;
    }

    struct Registry { uint64_t list{}, data{}; int count{}; };

    inline bool registry_of(uint64_t beh, Registry& r) {
        const uint64_t reg = rpm<uint64_t>(beh + kBehRegistry);
        r.list = plausible(reg) ? rpm<uint64_t>(reg + kRegistryList) : 0;
        const uint64_t items = plausible(r.list) ? rpm<uint64_t>(r.list + v100::list_items) : 0;
        r.count = plausible(r.list) ? rpm<int>(r.list + v100::list_count) : 0;
        const uint64_t length = plausible(items) ? rpm<uint64_t>(items + v100::array_length) : 0;
        if (!plausible(items) || r.count <= 0 || r.count > 4096 || length < uint64_t(r.count)) return false;
        r.data = items + v100::array_data;
        return true;
    }

    inline uint64_t entry(const Registry& r, int index) {
        return index >= 0 && index < r.count ? r.data + uint64_t(index) * kEntryStride : 0;
    }

    // One vote per (weapon view, meta id) with a skin: which index shift puts a skin of that
    // weapon in the entry.
    inline void vote(const std::vector<uint64_t>& players) {
        if (g_index_shift >= 0 || !g_scanned) return;
        for (uint64_t p : players) {
            const uint64_t b = find_beh(p);
            Registry r;
            if (!b || !registry_of(b, r)) continue;
            const int weapon = rpm<uint8_t>(b + kBehState);
            const int meta = rpm<uint16_t>(b + kBehMeta);
            if (!weapon || !g_voted.emplace((b << 16) ^ uint64_t(meta) ^ (uint64_t(weapon) << 56), true).second) continue;
            for (int shift = 0; shift < 2; ++shift) {
                const uint64_t e = entry(r, meta - shift);
                const int skin = e ? rpm<int>(e + kEntrySkin) : 0;
                if (skin <= 0) continue;
                const int w = def_weapon(skin);
                if (w >= 0) ++g_votes[shift][w == weapon ? 1 : 0];
            }
        }
        for (int shift = 0; shift < 2; ++shift) {
            const int good = g_votes[shift][1], other = g_votes[1 - shift][1];
            if (good >= 3 && good > 2 * other) {
                g_index_shift = shift;
                diag::log("SKINS", "meta index = Raw - %d (votes: shift0 %d/%d, shift1 %d/%d)", shift, g_votes[0][1],
                          g_votes[0][0], g_votes[1][1], g_votes[1][0]);
            }
        }
        if (g_index_shift < 0 && diag::every(g_log, 5000))
            diag::log("SKINS", "calibrating meta index: shift0 %d match/%d not, shift1 %d/%d", g_votes[0][1], g_votes[0][0],
                      g_votes[1][1], g_votes[1][0]);
    }

    // Makes beh load its weapon again: the meta id it remembers no longer matches.
    inline void reload(uint64_t beh, int meta) {
        const long long now = diag::now_ms();
        if (beh == g_reload_beh && now - g_reload_at < 400) return;
        g_reload_beh = beh;
        g_reload_meta = meta;
        g_reload_op = rpm<int>(beh + kBehOpId);
        g_reload_at = now;
        wpm<uint16_t>(beh + kBehMeta, uint16_t(kSpoiled));
    }

    // 600 ms after a reload: did beh load again (its op id moved)? If not, its cache is put back
    // and the skin shows from the next weapon switch.
    inline void settle() {
        if (!g_reload_at || diag::now_ms() - g_reload_at < 600) return;
        g_reload_at = 0;
        if (!klass::is_named(g_reload_beh, "beh")) return;
        const int op = rpm<int>(g_reload_beh + kBehOpId);
        const int cached = rpm<uint16_t>(g_reload_beh + kBehMeta);
        if (op != g_reload_op) { diag::log("SKINS", "weapon reloaded (op %d -> %d)", g_reload_op, op); return; }
        if (cached == kSpoiled) wpm<uint16_t>(g_reload_beh + kBehMeta, uint16_t(g_reload_meta));
        diag::log("SKINS", "no reload after the state change: the skin shows from the next weapon switch");
    }

    inline void restore() {
        if (g_patched.empty()) return;
        const uint64_t items = plausible(g_list) ? rpm<uint64_t>(g_list + v100::list_items) : 0;
        const int count = plausible(g_list) ? rpm<int>(g_list + v100::list_count) : 0;
        if (plausible(items))
            for (const auto& e : g_patched) {
                if (e.first < 0 || e.first >= count) continue;
                const uint64_t at = items + v100::array_data + uint64_t(e.first) * kEntryStride + kEntrySkin;
                if (rpm<int>(at) == e.second.written) wpm<int>(at, e.second.original);
            }
        g_patched.clear();
        if (g_reload_beh && klass::is_named(g_reload_beh, "beh")) {
            const int meta = rpm<uint16_t>(g_reload_beh + kBehMeta);
            if (meta != kSpoiled) { g_reload_at = 0; reload(g_reload_beh, meta); }
        }
        diag::log("SKINS", "original skins back");
    }

    inline void reset_process() {
        g_pid = proc::pid;
        g_beh.clear();
        g_index_shift = -1;
        std::memset(g_votes, 0, sizeof(g_votes));
        g_voted.clear();
        g_list = 0;
        g_patched.clear();
        g_reload_beh = 0;
        g_reload_at = 0;
        g_scanned = false;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_defs.clear();
        g_def_by_id.clear();
    }

    inline void rescan() {
        if (g_scanning.exchange(true)) return;
        std::thread([] { scan_definitions(); g_scanning = false; }).detach();
    }

    // Every 5 s until skins apply: the chain from the local player to the registry entry.
    inline void report(uint64_t local, const std::vector<uint64_t>& players) {
        static long long last = 0;
        if (!diag::every(last, 5000)) return;
        int views = 0;
        for (uint64_t p : players) if (find_beh(p)) ++views;
        const uint64_t b = find_beh(local);
        Registry r;
        if (!b) { diag::log("SKINS", "state: weapon view (beh) not found on the local player; %d of %zu players have one", views, players.size()); return; }
        const bool reg = registry_of(b, r);
        const int meta = rpm<uint16_t>(b + kBehMeta);
        char entries[160] = "";
        for (int shift = 0; shift < 2 && reg; ++shift)
            if (const uint64_t e = entry(r, meta - shift)) {
                const size_t n = std::strlen(entries);
                std::snprintf(entries + n, sizeof(entries) - n, " [Raw-%d] owner %d skin %d pattern %d;", shift, int(rpm<uint8_t>(e + kEntryOwner)),
                              rpm<int>(e + kEntrySkin), rpm<int>(e + 0x30));
            }
        diag::log("SKINS", "state: beh 0x%llx weapon %d meta %d view %d op %d; registry %s (%d entries)%s; views %d/%zu",
                  (unsigned long long)b, int(rpm<uint8_t>(b + kBehState)), meta, int(rpm<uint8_t>(b + 0x44)), rpm<int>(b + kBehOpId),
                  reg ? "ok" : "NOT found", reg ? r.count : 0, entries, views, players.size());
    }

    // Once per frame.
    inline void tick(uint64_t local, const std::vector<uint64_t>& players) {
        if (g_pid != proc::pid) { if (g_scanning.load()) return; reset_process(); }
        settle();
        if (!cfg::skins::enabled) { restore(); g_held_weapon = -1; return; }
        if (!local || proc::pid <= 0) return;
        if (g_index_shift < 0) report(local, players);
        if (!g_scanned.load() && !g_scanning.load()) {
            static long long last_scan = 0;
            static int attempts = 0;
            if (!last_scan || diag::now_ms() - last_scan > (attempts ? 60000 : 15000)) {
                last_scan = diag::now_ms();
                ++attempts;
                set_status("reading skin list...");
                rescan();
            }
            return;
        }
        if (!g_scanned.load()) return;
        vote(players);

        const uint64_t b = find_beh(local);
        Registry r;
        if (!b) { set_status("weapon view not found"); return; }
        if (!registry_of(b, r)) { set_status("weapon registry not found"); return; }
        if (r.list != g_list) { g_list = r.list; g_patched.clear(); }  // new match
        const int weapon = rpm<uint8_t>(b + kBehState);
        const int meta = rpm<uint16_t>(b + kBehMeta);
        if (meta == kSpoiled) return;  // our reload is pending
        g_held_weapon = weapon;
        if (g_index_shift < 0) { set_status("calibrating: look at players with skins"); return; }

        const int index = meta - g_index_shift;
        const uint64_t e = entry(r, index);
        if (!e) { set_status("weapon in hands has no meta entry (" + std::to_string(meta) + ")"); return; }
        const int current = rpm<int>(e + kEntrySkin);
        auto it = g_patched.find(index);
        if (it != g_patched.end() && current != it->second.written) { g_patched.erase(it); it = g_patched.end(); }  // the game set it again
        const int original = it != g_patched.end() ? it->second.original : current;
        const int chosen = weapon < 256 ? cfg::skins::chosen[weapon] : 0;
        const int want = chosen > 0 ? chosen : original;
        g_held_skin = want;
        set_status("weapon " + std::to_string(weapon) + ": " + (chosen > 0 ? "skin " + std::to_string(chosen) : "original"));
        if (current == want) return;
        wpm<int>(e + kEntrySkin, want);
        if (want == original) g_patched.erase(index);
        else g_patched[index] = {original, want};
        diag::log("SKINS", "weapon %d meta %d entry %d: skin %d -> %d (owner %d)", weapon, meta, index, current, want,
                  int(rpm<uint8_t>(e + kEntryOwner)));
        reload(b, meta);
    }

    // ---- menu -------------------------------------------------------------------------------------
    inline View view() {
        View v;
        v.weapon = g_held_weapon.load();
        std::lock_guard<std::mutex> lock(g_mutex);
        v.status = g_status;
        v.ids.push_back(0);
        v.names.push_back("Original");
        if (v.weapon < 0) return v;
        for (const auto& d : g_defs) {
            if (d.weapon != v.weapon) continue;
            v.ids.push_back(d.id);
            v.names.push_back(d.name);
        }
        const int chosen = v.weapon < 256 ? cfg::skins::chosen[v.weapon] : 0;
        for (size_t i = 0; i < v.ids.size(); ++i) if (v.ids[i] == chosen) v.current = int(i);
        return v;
    }
}
