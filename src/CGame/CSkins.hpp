#pragma once
// Skin changer for the weapons in the local player's hands (only this client sees it).
// From dump.cs:
//   - beh (chx part of every player: the weapon in hands) caches what it loaded as
//     acxp = (WeaponId @+0x40, WeaponMetaId @+0x42, view mode @+0x44); <opId> yppb @+0x48 grows
//     with every load (SetWeaponContent). Its onk (+0x80) is the match's weapon meta registry yvh.
//   - yvh.jmqq (+0x10) = List<WeaponMetaData>; WeaponMetaData (0x38 bytes: FixedList32Bytes is
//     8-aligned) = OwnerId u8 @0, SkinId i32 @4, CharmId i32 @8, stickers @0x10, SkinPatternId @0x30.
//     The content loader reads SkinId from there by the weapon's WeaponMetaId.
// The entry of the weapon in hands gets the chosen SkinId, and acxp's meta id is spoiled so beh
// sees a different weapon state and loads again. An entry may be shared by several weapons of
// one owner (same meta data), so it always carries the skin of the weapon in hands; the game's
// value is put back when nothing is chosen, or on disable.
// Whether WeaponMetaId.Raw is the list index or index + 1 is calibrated in game from other
// players' skins: skin ids below 200000 carry their weapon (47502 = set 4, weapon 75, skin 02).

#include "CGameContext.hpp"
#include "CClass.hpp"
#include "../CConfigs/CConfig.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skins {
    inline constexpr uint64_t kBehState = 0x40, kBehMeta = 0x42, kBehOpId = 0x48, kBehRegistry = 0x80;
    inline constexpr uint64_t kRegistryList = 0x10;
    inline constexpr uint64_t kEntryStride = 0x38, kEntryOwner = 0x0, kEntrySkin = 0x4;

    struct Skin { int id; const char* name; };
    // The skins offered in the menu.
    inline constexpr Skin kSkins[] = {
        // Knife_Butterfly / Бабочка
        {47502, "Butterfly Gold"},          {47503, "Butterfly DragonGlass"},  {47504, "Butterfly Red"},
        {47505, "Butterfly Starfall"},      {57501, "Butterfly New1"},         {97500, "Butterfly ColdFlame"},
        {157500, "Butterfly Kumo"},         {240007, "Butterfly Glitch"},      {240059, "Butterfly Saphira"},
        {240130, "Butterfly Ripple"},       {240225, "Butterfly Gambit"},      {240269, "Butterfly ArcaneSurge"},
        {240319, "Butterfly WildWest"},     {240331, "Butterfly JadeStone"},   {240369, "Butterfly Twilight"},
    };

    struct Patch { int original{}; int written{}; };
    struct View { int weapon = -1; int current = 0; std::vector<int> ids; std::vector<std::string> names; std::string status; };

    inline int g_pid = -1;
    inline std::mutex g_mutex;                       // g_status
    inline std::string g_status = "waiting for a match";

    inline std::unordered_map<uint64_t, uint64_t> g_beh;  // player -> beh
    inline int g_index_shift = -1;                   // list index = Raw - shift
    inline int g_votes[2][2]{};                      // [shift][matches weapon]
    inline std::unordered_map<uint64_t, bool> g_voted;  // (beh, meta) pairs counted once
    inline uint64_t g_list = 0;                      // registry list the patches are in
    inline std::unordered_map<int, Patch> g_patched; // entry index -> values
    inline std::atomic<int> g_held_weapon{-1};
    inline constexpr int kSpoiled = 0xFFFE;
    inline uint64_t g_reload_beh = 0;
    inline int g_reload_op = 0, g_reload_meta = 0;
    inline long long g_reload_at = 0, g_log = 0;

    inline void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (s != g_status) { g_status = s; diag::log("SKINS", "%s", s.c_str()); }
    }

    // Weapon of a skin id in the old numbering (set * 10000 + weapon * 100 + n), or -1.
    inline int weapon_of(int skin) { return skin >= 10000 && skin < 200000 ? (skin / 100) % 100 : -1; }

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

    // One vote per (weapon view, meta id) with an old-numbered skin: which index shift puts a
    // skin of that weapon in the entry.
    inline void vote(const std::vector<uint64_t>& players) {
        if (g_index_shift >= 0) return;
        for (uint64_t p : players) {
            const uint64_t b = find_beh(p);
            Registry r;
            if (!b || !registry_of(b, r)) continue;
            const int weapon = rpm<uint8_t>(b + kBehState);
            const int meta = rpm<uint16_t>(b + kBehMeta);
            if (!weapon || meta == kSpoiled || !g_voted.emplace((b << 16) ^ uint64_t(meta) ^ (uint64_t(weapon) << 56), true).second) continue;
            for (int shift = 0; shift < 2; ++shift) {
                const uint64_t e = entry(r, meta - shift);
                const int w = e ? weapon_of(rpm<int>(e + kEntrySkin)) : -1;
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
        if (g_pid != proc::pid) reset_process();
        settle();
        if (!cfg::skins::enabled) { restore(); g_held_weapon = -1; return; }
        if (!local || proc::pid <= 0) return;
        if (g_index_shift < 0) report(local, players);
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
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            v.status = g_status;
        }
        v.ids.push_back(0);
        v.names.push_back("Original");
        for (const Skin& s : kSkins) { v.ids.push_back(s.id); v.names.push_back(s.name); }
        const int chosen = v.weapon >= 0 && v.weapon < 256 ? cfg::skins::chosen[v.weapon] : 0;
        for (size_t i = 0; i < v.ids.size(); ++i) if (v.ids[i] == chosen) v.current = int(i);
        return v;
    }
}
