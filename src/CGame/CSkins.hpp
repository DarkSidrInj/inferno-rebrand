#pragma once
// Skin changer for the weapon in the local player's hands (only this client sees it).
// From dump.cs:
//   - beh (chx part of every player: the weapon in hands) caches what it loaded as
//     acxp = (WeaponId @+0x40, WeaponMetaId @+0x42, view mode @+0x44); <opId> yppb @+0x48 grows
//     with every load (SetWeaponContent). Its onk (+0x80) is the match's weapon meta registry yvh,
//     its nrk (+0x58) the weapon part, whose WeaponContext (+0x30) holds ActiveWeapon (+0x78).
//   - ActiveWeapon is a qjf: WeaponId u8 @+0x10, its own WeaponMetaData copy @+0x18
//     (OwnerId u8 @0, SkinId i32 @4, CharmId @8, stickers @0x10, SkinPatternId @0x30; 0x38 bytes),
//     WeaponMetaId u16 @+0x50. beh computes the meta id of the state it loads from it.
//   - yvh.jmqq (+0x10) = List<WeaponMetaData>, read by the loader by meta id.
// On device a weapon without a skin has meta id 0, shared by every such weapon, so the registry
// entry is not touched. The held qjf gets the chosen SkinId in its meta copy, and two copies of
// that meta data are appended to the registry (Raw may be the index or index + 1; the second copy
// is right either way), its meta id pointing there. A Butterfly skin on another knife also makes
// it a Butterfly (WeaponId 75), so the model matches the skin. beh's cached meta id is spoiled so
// it loads again. Everything goes back when nothing is chosen, or on disable.

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
    inline constexpr uint64_t kBehState = 0x40, kBehMeta = 0x42, kBehOpId = 0x48, kBehPart = 0x58, kBehRegistry = 0x80;
    inline constexpr uint64_t kPartActiveWeapon = 0x78;
    inline constexpr uint64_t kItemWeapon = 0x10, kItemMeta = 0x18, kItemMetaId = 0x50;
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

    // What a weapon item holds; `orig` as the game set it, `written` as we left it.
    struct Fields { int weapon{}; int skin{}; int meta_id{}; };
    struct Item { Fields orig{}, written{}; uint64_t list{}; int appended_at = -1; };
    struct View { int weapon = -1; int current = 0; std::vector<int> ids; std::vector<std::string> names; std::string status; };

    inline int g_pid = -1;
    inline std::mutex g_mutex;                       // g_status
    inline std::string g_status = "waiting for a match";

    inline std::unordered_map<uint64_t, uint64_t> g_beh;  // player -> beh
    inline std::unordered_map<uint64_t, Item> g_items;    // qjf -> what we changed
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

    inline uint64_t active_weapon(uint64_t beh) {
        const uint64_t part = rpm<uint64_t>(beh + kBehPart);
        const uint64_t item = plausible(part) ? rpm<uint64_t>(part + kPartActiveWeapon) : 0;
        return plausible(item) && klass::is_named(item, "qjf") ? item : 0;
    }

    inline Fields read_fields(uint64_t item) {
        return {int(rpm<uint8_t>(item + kItemWeapon)), rpm<int>(item + kItemMeta + kEntrySkin), int(rpm<uint16_t>(item + kItemMetaId))};
    }

    inline bool same(const Fields& a, const Fields& b) { return a.weapon == b.weapon && a.skin == b.skin && a.meta_id == b.meta_id; }

    inline void write_fields(uint64_t item, const Fields& f) {
        wpm<uint8_t>(item + kItemWeapon, uint8_t(f.weapon));
        wpm<int>(item + kItemMeta + kEntrySkin, f.skin);
        wpm<uint16_t>(item + kItemMetaId, uint16_t(f.meta_id));
    }

    // Two copies of the item's meta data (with `skin`) at the end of the registry list; the meta id
    // that names the second one under either numbering, or -1 when the list has no room.
    inline int append_meta(const Registry& r, uint64_t item, int skin, int& appended_at) {
        const uint64_t items = rpm<uint64_t>(r.list + v100::list_items);
        const uint64_t capacity = rpm<uint64_t>(items + v100::array_length);
        if (uint64_t(r.count) + 2 > capacity || r.count + 1 >= kSpoiled) return -1;
        uint8_t meta[kEntryStride];
        if (!mem_read(item + kItemMeta, meta, sizeof(meta))) return -1;
        std::memcpy(meta + kEntrySkin, &skin, 4);
        for (int k = 0; k < 2; ++k)
            for (size_t i = 0; i < kEntryStride; i += 8) {
                uint64_t w;
                std::memcpy(&w, meta + i, 8);
                wpm<uint64_t>(r.data + uint64_t(r.count + k) * kEntryStride + i, w);
            }
        appended_at = r.count;
        wpm<int>(r.list + v100::list_count, r.count + 2);
        return r.count + 1;
    }

    // Our two copies are still the last entries of the list with that skin.
    inline bool appended_valid(const Item& it, int skin) {
        Registry r;
        if (it.appended_at < 0 || !plausible(it.list)) return false;
        const uint64_t items = rpm<uint64_t>(it.list + v100::list_items);
        const int count = rpm<int>(it.list + v100::list_count);
        if (!plausible(items) || count < it.appended_at + 2) return false;
        for (int k = 0; k < 2; ++k)
            if (rpm<int>(items + v100::array_data + uint64_t(it.appended_at + k) * kEntryStride + kEntrySkin) != skin) return false;
        return true;
    }

    inline void put_back(uint64_t item, Item& it) {
        if (klass::is_named(item, "qjf") && same(read_fields(item), it.written)) write_fields(item, it.orig);
        if (it.appended_at >= 0 && plausible(it.list) && rpm<int>(it.list + v100::list_count) == it.appended_at + 2)
            wpm<int>(it.list + v100::list_count, it.appended_at);
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
        if (g_items.empty()) return;
        for (auto& e : g_items) put_back(e.first, e.second);
        g_items.clear();
        if (g_reload_beh && klass::is_named(g_reload_beh, "beh")) {
            const int meta = rpm<uint16_t>(g_reload_beh + kBehMeta);
            if (meta != kSpoiled) { g_reload_at = 0; reload(g_reload_beh, meta); }
        }
        diag::log("SKINS", "original weapons back");
    }

    inline void reset_process() {
        g_pid = proc::pid;
        g_beh.clear();
        g_items.clear();
        g_reload_beh = 0;
        g_reload_at = 0;
    }

    // Every 5 s: the chain from the local player to the weapon item and the registry.
    inline void report(uint64_t beh, uint64_t item) {
        if (!diag::every(g_log, 5000)) return;
        Registry r;
        const bool reg = registry_of(beh, r);
        const uint64_t items = reg ? rpm<uint64_t>(r.list + v100::list_items) : 0;
        char entries[200] = "";
        for (int i = 0; reg && i < r.count && i < 4; ++i) {
            const uint64_t e = entry(r, i);
            const size_t n = std::strlen(entries);
            std::snprintf(entries + n, sizeof(entries) - n, " [%d] owner %d skin %d charm %d;", i, int(rpm<uint8_t>(e + kEntryOwner)),
                          rpm<int>(e + kEntrySkin), rpm<int>(e + kEntryCharm));
        }
        const Fields f = item ? read_fields(item) : Fields{};
        diag::log("SKINS", "state: beh weapon %d meta %d view %d op %d; item 0x%llx weapon %d skin %d meta id %d owner %d; registry %s %d/%llu:%s",
                  int(rpm<uint8_t>(beh + kBehState)), int(rpm<uint16_t>(beh + kBehMeta)), int(rpm<uint8_t>(beh + 0x44)),
                  rpm<int>(beh + kBehOpId), (unsigned long long)item, f.weapon, f.skin, f.meta_id,
                  item ? int(rpm<uint8_t>(item + kItemMeta + kEntryOwner)) : -1, reg ? "ok" : "NOT found", reg ? r.count : 0,
                  (unsigned long long)(items ? rpm<uint64_t>(items + v100::array_length) : 0), entries);
    }

    // Once per frame.
    inline void tick(uint64_t local, const std::vector<uint64_t>& players) {
        (void)players;
        if (g_pid != proc::pid) reset_process();
        settle();
        if (!cfg::skins::enabled) { restore(); g_held_weapon = -1; return; }
        if (!local || proc::pid <= 0) return;

        const uint64_t b = find_beh(local);
        if (!b) { set_status("weapon view not found"); return; }
        const uint64_t item = active_weapon(b);
        report(b, item);
        if (!item) { set_status("weapon in hands not found"); return; }
        if (rpm<uint16_t>(b + kBehMeta) == kSpoiled) return;  // our reload is pending
        Registry r;
        if (!registry_of(b, r)) { set_status("weapon registry not found"); return; }

        const Fields now = read_fields(item);
        auto found = g_items.find(item);
        if (found != g_items.end() && !same(now, found->second.written)) {  // the game set it again
            g_items.erase(found);
            found = g_items.end();
        }
        Item it = found != g_items.end() ? found->second : Item{now, now, r.list, -1};
        const int weapon = it.orig.weapon;
        g_held_weapon = weapon;
        const int chosen = weapon < 256 ? cfg::skins::chosen[weapon] : 0;
        set_status("weapon " + std::to_string(weapon) + ": " + (chosen > 0 ? "skin " + std::to_string(chosen) : "original"));

        if (chosen <= 0) {
            if (found == g_items.end()) return;
            put_back(item, it);
            g_items.erase(item);
            diag::log("SKINS", "weapon %d: original back (skin %d, meta id %d)", weapon, it.orig.skin, it.orig.meta_id);
            reload(b, rpm<uint16_t>(b + kBehMeta));
            return;
        }
        Fields want{knife(weapon) && butterfly_skin(chosen) ? kButterfly : weapon, chosen, it.written.meta_id};
        if (found != g_items.end() && same(it.written, want) && (it.appended_at < 0 || appended_valid(it, chosen))) return;  // in place
        // Fresh meta data for this skin.
        if (found != g_items.end()) put_back(item, it);
        it = Item{it.orig, {}, r.list, -1};
        registry_of(b, r);
        wpm<int>(item + kItemMeta + kEntrySkin, chosen);  // the copy the appended entries are made from
        const int id = append_meta(r, item, chosen, it.appended_at);
        want.meta_id = id >= 0 ? id : it.orig.meta_id;
        write_fields(item, want);
        it.written = want;
        g_items[item] = it;
        diag::log("SKINS", "weapon %d -> %d skin %d meta id %d -> %d (%s)", weapon, want.weapon, chosen, it.orig.meta_id, want.meta_id,
                  id >= 0 ? "meta data appended" : "registry full: meta copy only");
        reload(b, rpm<uint16_t>(b + kBehMeta));
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
