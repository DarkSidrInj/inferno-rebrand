#pragma once
// Skin changer for the local player's weapons (only this client sees it).
// From dump.cs, confirmed on device:
//   - qqt (the player's networked weaponry component) keeps List<WeaponSnapshot> @+0x58:
//     {WeaponId u8 @0, WeaponMetaId u16 @2, ammo, ..., slot u8 @0xC}, 0x10 bytes each. The weapon
//     items (qjf) are rebuilt from it all the time, so writes into an item do not last.
//   - beh (chx part: the weapon in hands) caches what it loaded as acxp = (WeaponId @+0x40,
//     WeaponMetaId @+0x42, view @+0x44); its op id @+0x48 grows with every load. Its onk (+0x80) is
//     the match's weapon meta registry yvh: List<WeaponMetaData> @+0x10, 0x38 bytes each
//     (OwnerId u8 @0, SkinId i32 @4, CharmId @8, stickers @0x10, SkinPatternId @0x30).
//   - a weapon without a skin has meta id 0: entry 0, blank, shared by every such weapon.
// A chosen skin gets its own registry entry (copy of the weapon's entry with that SkinId,
// appended in the list's spare room) and the snapshot's meta id points at it; a Butterfly skin on
// another knife also makes the snapshot a Butterfly (WeaponId 75), so the model matches. The
// snapshot is written again whenever the game writes it back (counted in the log). beh's cached
// meta id is spoiled so the weapon in hands loads again. All put back on Original or disable.
// Meta id numbering (index, or index + 1) is read from other players' skinned weapons; without
// any, index (entry 0 blank = meta id 0 "no skin").

#include "CGameContext.hpp"
#include "CClass.hpp"
#include "../CConfigs/CConfig.hpp"
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skins {
    inline constexpr uint64_t kBehState = 0x40, kBehMeta = 0x42, kBehView = 0x44, kBehOpId = 0x48, kBehRegistry = 0x80;
    inline constexpr uint64_t kWeaponrySnapshots = 0x58, kWeaponryActiveSlot = 0x60;
    inline constexpr uint64_t kSnapStride = 0x10, kSnapWeapon = 0x0, kSnapMeta = 0x2, kSnapSlot = 0xC;
    inline constexpr uint64_t kRegistryList = 0x10;
    inline constexpr uint64_t kEntryStride = 0x38, kEntryOwner = 0x0, kEntrySkin = 0x4, kEntryCharm = 0x8;
    inline constexpr int kButterfly = 75;
    inline constexpr int kFirstKnife = 70, kLastKnife = 99;   // knife WeaponIds (70: default knife on device)

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

    inline bool butterfly_skin(int id) {
        for (const Skin& s : kSkins) if (s.id == id) return true;
        return false;
    }
    inline bool knife(int weapon) { return weapon >= kFirstKnife && weapon <= kLastKnife; }
    // One snapshot we changed: as the game had it, and as we wrote it.
    struct Snap { int weapon{}, meta{}; int w_weapon{}, w_meta{}; int skin{}; };
    struct View { int weapon = -1; int current = 0; std::vector<int> ids; std::vector<std::string> names; std::string status; };

    inline int g_pid = -1;
    inline std::mutex g_mutex;                       // g_status
    inline std::string g_status = "waiting for a match";

    inline std::unordered_map<uint64_t, uint64_t> g_beh;      // player -> beh
    inline std::unordered_map<uint64_t, uint64_t> g_weaponry; // player -> qqt
    inline uint64_t g_snaps_list = 0;                // the snapshot list the changes are in
    inline std::map<int, Snap> g_snaps;              // snapshot index -> change
    inline uint64_t g_registry = 0;                  // registry list our entries are in
    inline std::map<std::pair<int, int>, int> g_entries;  // (original meta id, weapon) -> our entry index
    inline int g_registry_base = -1;                 // list count before our first entry
    inline int g_shift = -1;                         // entry index = meta id - shift
    inline int g_rewrites = 0, g_full_logged = 0;
    inline std::atomic<int> g_held_weapon{-1};
    inline constexpr int kSpoiled = 0xFFFE;
    inline uint64_t g_reload_beh = 0;
    inline int g_reload_op = 0, g_reload_meta = 0;
    inline long long g_reload_at = 0, g_log = 0;

    inline void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (s != g_status) { g_status = s; diag::log("SKINS", "%s", s.c_str()); }
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
    // The player's qqt: among its parts arrays (wmz) or its parts list.
    inline uint64_t find_weaponry(uint64_t player) {
        const auto it = g_weaponry.find(player);
        if (it != g_weaponry.end() && it->second && klass::is_named(it->second, "qqt")) return it->second;
        uint64_t found = 0;
        const uint64_t container = rpm<uint64_t>(player + v100::net_systems);
        for (uint64_t field = 0x10; plausible(container) && field <= 0x38 && !found; field += 8) {
            const uint64_t array = rpm<uint64_t>(container + field);
            const uint64_t count = plausible(array) ? rpm<uint64_t>(array + v100::array_length) : 0;
            for (uint64_t i = 0; i < count && i < 256; ++i) {
                const uint64_t c = rpm<uint64_t>(array + v100::array_data + i * 8);
                if (plausible(c) && klass::is_named(c, "qqt")) { found = c; break; }
            }
        }
        const uint64_t parts = rpm<uint64_t>(player + v100::player_parts);
        const uint64_t list = plausible(parts) ? rpm<uint64_t>(parts + v100::parts_list) : 0;
        const uint64_t items = plausible(list) ? rpm<uint64_t>(list + v100::list_items) : 0;
        const int n = plausible(list) ? rpm<int>(list + v100::list_count) : 0;
        for (int i = 0; !found && plausible(items) && i < n && i < 128; ++i) {
            const uint64_t c = rpm<uint64_t>(items + v100::array_data + uint64_t(i) * 8);
            if (plausible(c) && klass::is_named(c, "qqt")) found = c;
        }
        if (g_weaponry.size() > 128) g_weaponry.clear();
        g_weaponry[player] = found;
        return found;
    }

    struct Snapshots { uint64_t list{}, data{}; int count{}; };

    inline bool snapshots_of(uint64_t weaponry, Snapshots& s) {
        s.list = rpm<uint64_t>(weaponry + kWeaponrySnapshots);
        const uint64_t items = plausible(s.list) ? rpm<uint64_t>(s.list + v100::list_items) : 0;
        s.count = plausible(s.list) ? rpm<int>(s.list + v100::list_count) : 0;
        if (!plausible(items) || s.count <= 0 || s.count > 32 || rpm<uint64_t>(items + v100::array_length) < uint64_t(s.count)) return false;
        s.data = items + v100::array_data;
        return true;
    }

    inline uint64_t snap(const Snapshots& s, int i) { return s.data + uint64_t(i) * kSnapStride; }

    // Meta id numbering from other players' weapons in hands: a skinned weapon's entry has a skin.
    inline void calibrate(const std::vector<uint64_t>& players, const Registry& r) {
        if (g_shift >= 0) return;
        int votes[2]{};
        for (uint64_t p : players) {
            const uint64_t b = find_beh(p);
            const int m = b ? rpm<uint16_t>(b + kBehMeta) : 0;
            if (m <= 0 || m == kSpoiled) continue;
            for (int shift = 0; shift < 2; ++shift) {
                const uint64_t e = entry(r, m - shift);
                if (e && rpm<int>(e + kEntrySkin) > 0) ++votes[shift];
            }
        }
        if (votes[0] != votes[1] && std::max(votes[0], votes[1]) >= 2) {
            g_shift = votes[1] > votes[0] ? 1 : 0;
            diag::log("SKINS", "meta id = entry index%s (votes %d / %d)", g_shift ? " + 1" : "", votes[0], votes[1]);
        }
    }

    inline int shift() { return g_shift >= 0 ? g_shift : 0; }

    // Our entry for one weapon (original meta id, weapon): a copy of its original entry with the
    // skin, at the end of the list; a new skin is written into the same entry. Its meta id, or -1
    // when the list has no room.
    inline int entry_for(const Registry& r, int meta, int weapon, int skin) {
        if (r.list != g_registry) { g_registry = r.list; g_entries.clear(); g_registry_base = -1; }
        const auto key = std::make_pair(meta, weapon);
        const auto it = g_entries.find(key);
        if (it != g_entries.end()) {
            const uint64_t e = entry(r, it->second);
            if (e) {
                if (rpm<int>(e + kEntrySkin) != skin) wpm<int>(e + kEntrySkin, skin);
                return it->second + shift();
            }
            g_entries.erase(it);  // the list was rebuilt
        }
        const uint64_t items = rpm<uint64_t>(r.list + v100::list_items);
        if (uint64_t(r.count) + 1 > rpm<uint64_t>(items + v100::array_length) || r.count + shift() >= kSpoiled) return -1;
        uint8_t data[kEntryStride]{};
        if (const uint64_t from = entry(r, meta - shift())) mem_read(from, data, sizeof(data));
        std::memcpy(data + kEntrySkin, &skin, 4);
        for (size_t i = 0; i < kEntryStride; i += 8) {
            uint64_t w;
            std::memcpy(&w, data + i, 8);
            wpm<uint64_t>(r.data + uint64_t(r.count) * kEntryStride + i, w);
        }
        if (g_registry_base < 0) g_registry_base = r.count;
        g_entries[key] = r.count;
        wpm<int>(r.list + v100::list_count, r.count + 1);
        diag::log("SKINS", "registry entry %d: weapon %d skin %d (copy of meta id %d)", r.count, weapon, skin, meta);
        return r.count + shift();
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

    inline void restore_snaps() {
        Snapshots s{g_snaps_list, 0, 0};
        const uint64_t items = plausible(g_snaps_list) ? rpm<uint64_t>(g_snaps_list + v100::list_items) : 0;
        const int count = plausible(g_snaps_list) ? rpm<int>(g_snaps_list + v100::list_count) : 0;
        for (const auto& e : g_snaps) {
            if (!plausible(items) || e.first >= count) continue;
            const uint64_t at = items + v100::array_data + uint64_t(e.first) * kSnapStride;
            if (rpm<uint8_t>(at + kSnapWeapon) == e.second.w_weapon && rpm<uint16_t>(at + kSnapMeta) == e.second.w_meta) {
                wpm<uint8_t>(at + kSnapWeapon, uint8_t(e.second.weapon));
                wpm<uint16_t>(at + kSnapMeta, uint16_t(e.second.meta));
            }
        }
        g_snaps.clear();
        (void)s;
    }

    inline void restore() {
        if (g_snaps.empty() && g_entries.empty()) return;
        restore_snaps();
        if (plausible(g_registry) && g_registry_base >= 0) {
            const int count = rpm<int>(g_registry + v100::list_count);
            int last = -1;
            for (const auto& e : g_entries) last = std::max(last, e.second);
            if (count == last + 1) wpm<int>(g_registry + v100::list_count, g_registry_base);
        }
        g_entries.clear();
        g_registry_base = -1;
        if (g_reload_beh && klass::is_named(g_reload_beh, "beh")) {
            const int meta = rpm<uint16_t>(g_reload_beh + kBehMeta);
            if (meta != kSpoiled) { g_reload_at = 0; reload(g_reload_beh, meta); }
        }
        diag::log("SKINS", "original weapons back");
    }

    inline void reset_process() {
        g_pid = proc::pid;
        g_beh.clear();
        g_weaponry.clear();
        g_snaps.clear();
        g_snaps_list = 0;
        g_entries.clear();
        g_registry = 0;
        g_registry_base = -1;
        g_shift = -1;
        g_reload_beh = 0;
        g_reload_at = 0;
    }

    // Every 5 s: the weapon in hands, the snapshots, the registry.
    inline void report(uint64_t beh, uint64_t weaponry, const Snapshots* s, const Registry* r) {
        if (!diag::every(g_log, 5000)) return;
        char snaps[256] = "";
        for (int i = 0; s && i < s->count && i < 8; ++i) {
            const size_t n = std::strlen(snaps);
            std::snprintf(snaps + n, sizeof(snaps) - n, " [%d] w%d m%d s%d;", i, int(rpm<uint8_t>(snap(*s, i) + kSnapWeapon)),
                          int(rpm<uint16_t>(snap(*s, i) + kSnapMeta)), int(rpm<uint8_t>(snap(*s, i) + kSnapSlot)));
        }
        const uint64_t items = r ? rpm<uint64_t>(r->list + v100::list_items) : 0;
        diag::log("SKINS", "state: beh w%d m%d view %d op %d; weaponry %s slot %d:%s registry %d/%llu; rewrites %d",
                  int(rpm<uint8_t>(beh + kBehState)), int(rpm<uint16_t>(beh + kBehMeta)), int(rpm<uint8_t>(beh + kBehView)),
                  rpm<int>(beh + kBehOpId), weaponry ? "ok" : "NOT found", weaponry ? int(rpm<uint8_t>(weaponry + kWeaponryActiveSlot)) : -1,
                  snaps, r ? r->count : 0, (unsigned long long)(items ? rpm<uint64_t>(items + v100::array_length) : 0), g_rewrites);
        g_rewrites = 0;
    }

    // Once per frame.
    inline void tick(uint64_t local, const std::vector<uint64_t>& players) {
        if (g_pid != proc::pid) reset_process();
        settle();
        if (!cfg::skins::enabled) { restore(); g_held_weapon = -1; return; }
        if (!local || proc::pid <= 0) return;

        const uint64_t b = find_beh(local);
        if (!b) { set_status("weapon view not found"); return; }
        const uint64_t weaponry = find_weaponry(local);
        Snapshots s;
        Registry r;
        const bool have_snaps = weaponry && snapshots_of(weaponry, s);
        const bool have_registry = registry_of(b, r);
        report(b, weaponry, have_snaps ? &s : nullptr, have_registry ? &r : nullptr);
        if (!weaponry) { set_status("weaponry not found"); return; }
        if (!have_snaps) { set_status("no weapons"); return; }
        if (!have_registry) { set_status("weapon registry not found"); return; }
        calibrate(players, r);
        if (s.list != g_snaps_list) { g_snaps_list = s.list; g_snaps.clear(); }  // new life / new match

        const int active_slot = rpm<uint8_t>(weaponry + kWeaponryActiveSlot);
        int held = -1, held_weapon = -1, held_meta = -1;  // as the game had it / as it should load
        bool held_reskinned = false;                      // same entry, another skin: load again
        for (int i = 0; i < s.count; ++i) {
            const uint64_t at = snap(s, i);
            const int w = rpm<uint8_t>(at + kSnapWeapon), m = rpm<uint16_t>(at + kSnapMeta);
            auto found = g_snaps.find(i);
            Snap orig{w, m, w, m, 0};
            if (found != g_snaps.end()) {
                if (w == found->second.w_weapon && m == found->second.w_meta) orig = found->second;
                else if (w == found->second.weapon && m == found->second.meta) { orig = found->second; ++g_rewrites; }  // written back
                else { g_snaps.erase(found); found = g_snaps.end(); }  // another weapon in this slot
            }
            const bool in_hands = rpm<uint8_t>(at + kSnapSlot) == active_slot;
            if (in_hands) { held = orig.weapon; held_weapon = orig.weapon; held_meta = orig.meta; }
            const int chosen = orig.weapon < 256 ? cfg::skins::chosen[orig.weapon] : 0;
            if (chosen <= 0) {
                if (found == g_snaps.end()) continue;
                if (w != orig.weapon || m != orig.meta) {
                    wpm<uint8_t>(at + kSnapWeapon, uint8_t(orig.weapon));
                    wpm<uint16_t>(at + kSnapMeta, uint16_t(orig.meta));
                }
                g_snaps.erase(found);
                continue;
            }
            const int meta = entry_for(r, orig.meta, orig.weapon, chosen);
            if (meta < 0) {
                if (g_full_logged++ % 600 == 0) diag::log("SKINS", "registry has no room for skin %d (%d entries)", chosen, r.count);
                continue;
            }
            registry_of(b, r);  // count may have grown
            Snap want = orig;
            want.w_weapon = knife(orig.weapon) && butterfly_skin(chosen) ? kButterfly : orig.weapon;
            want.w_meta = meta;
            want.skin = chosen;
            if (in_hands && found != g_snaps.end() && found->second.skin != chosen) held_reskinned = true;
            if (w != want.w_weapon || m != want.w_meta) {
                wpm<uint8_t>(at + kSnapWeapon, uint8_t(want.w_weapon));
                wpm<uint16_t>(at + kSnapMeta, uint16_t(want.w_meta));
                if (found == g_snaps.end() || found->second.skin != chosen)
                    diag::log("SKINS", "weapon %d -> %d, meta id %d -> %d (skin %d)", orig.weapon, want.w_weapon, orig.meta, meta, chosen);
            }
            g_snaps[i] = want;
            if (in_hands) { held_weapon = want.w_weapon; held_meta = want.w_meta; }
        }
        if (held < 0) held = rpm<uint8_t>(b + kBehState);
        g_held_weapon = held;
        const int chosen = held >= 0 && held < 256 ? cfg::skins::chosen[held] : 0;
        set_status("weapon " + std::to_string(held) + ": " + (chosen > 0 ? "skin " + std::to_string(chosen) : "original"));
        // The weapon in hands loads again when what it loaded is not what it should show.
        const int cached = rpm<uint16_t>(b + kBehMeta);
        if (held_weapon >= 0 && cached != kSpoiled &&
            (held_reskinned || int(rpm<uint8_t>(b + kBehState)) != held_weapon || cached != held_meta))
            reload(b, cached);
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

