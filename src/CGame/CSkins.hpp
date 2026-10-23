#pragma once
// Skin changer for the local player's weapons (only this client sees it).
// On device the weapon in hands is rebuilt every frame from network snapshots, and a weapon without
// a skin points at the blank registry entry 0 shared by everyone, so neither can be held. What the
// game builds the model from is the weapon content resolver instead (dump.cs):
//   - beh (chx part: the weapon in hands) caches what it loaded as acxp = (WeaponId @+0x40,
//     WeaponMetaId @+0x42, view @+0x44); its op id @+0x48 grows with every load. Its yoi (+0x68)
//     is the resolver ybt.
//   - ybt: iwf<WeaponAnimationParameters> @+0x28, iwf<WeaponSoundParameters> @+0x30,
//     iwf<WeaponMeshParameters> @+0x38, and the cache Dictionary<(WeaponId, skin), dlz> @+0x40.
//   - iwf<T>: Dictionary<WeaponId, T> (the weapon's own content) and Dictionary<int, T> (by skin
//     id), among others; generic, so their offsets are found in game by their keys.
//   - dlz: mesh @+0x10, animations @+0x18, sounds @+0x20.
// A chosen skin: the weapon's entry in each WeaponId dictionary gets the skin's value, and every
// cached dlz of that weapon gets the skin's mesh, animations and sounds. A default knife with a
// Butterfly skin then loads as that Butterfly (every default knife this client draws does). beh's
// cached meta id is spoiled so the weapon in hands loads again. All put back on Original/disable.

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
    inline constexpr uint64_t kBehState = 0x40, kBehMeta = 0x42, kBehView = 0x44, kBehOpId = 0x48, kBehResolver = 0x68;
    inline constexpr uint64_t kResolverParams[3] = {0x38, 0x28, 0x30};   // mesh, animations, sounds
    inline constexpr uint64_t kResolverCache = 0x40;
    inline constexpr uint64_t kContentParams[3] = {0x10, 0x18, 0x20};    // dlz: mesh, animations, sounds
    inline constexpr uint64_t kDictEntries = 0x18, kDictCount = 0x20, kEntryStride = 0x18, kEntryKey = 0x8, kEntryValue = 0x10;
    inline constexpr const char* kParamNames[3] = {"mesh", "animations", "sounds"};

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

    struct View { int weapon = -1; int current = 0; std::vector<int> ids; std::vector<std::string> names; std::string status; };
    // The two dictionaries of one iwf<T>, found by their keys.
    struct Params { uint64_t by_weapon{}, by_skin{}; };
    struct Swap { uint64_t at{}; uint64_t original{}, written{}; };  // one pointer we changed

    inline int g_pid = -1;
    inline std::mutex g_mutex;                       // g_status
    inline std::string g_status = "waiting for a match";

    inline std::unordered_map<uint64_t, uint64_t> g_beh;     // player -> beh
    inline uint64_t g_resolver = 0;
    inline Params g_params[3];
    inline bool g_params_logged = false;
    inline std::map<int, int> g_applied;              // weapon -> skin in place
    inline std::map<int, std::vector<Swap>> g_swaps;  // weapon -> pointers changed for it
    struct Values { uint64_t original[3]{}, written[3]{}; };
    inline std::map<int, Values> g_values;            // weapon -> its own and the skin's params
    inline std::atomic<int> g_held_weapon{-1};
    inline constexpr int kSpoiled = 0xFFFE;
    inline uint64_t g_reload_beh = 0;
    inline int g_reload_op = 0, g_reload_meta = 0;
    inline long long g_reload_at = 0, g_log = 0;

    inline void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (s != g_status) { g_status = s; diag::log("SKINS", "%s", s.c_str()); }
    }

    inline std::string class_of(uint64_t object) {
        char name[48];
        if (!plausible(object) || !klass::calibrate() || !klass::name_at(rpm<uint64_t>(object), klass::g_name_offset, name, sizeof(name))) return {};
        return name;
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

    // ---- dictionaries ------------------------------------------------------------------------------
    // Every used entry of a Dictionary<K, V> with an 8-byte-or-smaller key and a reference value.
    template <class F> inline void each_entry(uint64_t dict, F f) {
        const uint64_t entries = rpm<uint64_t>(dict + kDictEntries);
        const int count = rpm<int>(dict + kDictCount);
        const uint64_t length = plausible(entries) ? rpm<uint64_t>(entries + 0x18) : 0;
        if (!plausible(entries) || count <= 0 || count > 100000 || length < uint64_t(count)) return;
        for (int i = 0; i < count; ++i) {
            const uint64_t e = entries + 0x20 + uint64_t(i) * kEntryStride;
            if (rpm<int>(e) < 0 || rpm<int>(e + 4) < -1) continue;  // free entry
            if (!f(e)) return;
        }
    }

    // Address of the value slot for `key` (compared on `key_bytes` bytes), or 0.
    inline uint64_t find_value(uint64_t dict, uint64_t key, int key_bytes) {
        uint64_t found = 0;
        each_entry(dict, [&](uint64_t e) {
            uint64_t k = 0;
            mem_read(e + kEntryKey, &k, size_t(key_bytes));
            if (k == key) { found = e + kEntryValue; return false; }
            return true;
        });
        return found;
    }

    // The WeaponId- and int-keyed dictionaries of an iwf<T>: any Dictionary field whose keys are all
    // below 256 (weapons) or all at least 1000 (skins).
    inline Params find_params(uint64_t iwf) {
        Params p;
        for (uint64_t off = 0x10; off <= 0x80; off += 8) {
            const uint64_t d = rpm<uint64_t>(iwf + off);
            if (class_of(d).rfind("Dictionary", 0) != 0) continue;
            int small = 0, large = 0, other = 0;
            each_entry(d, [&](uint64_t e) {
                const uint64_t k = rpm<uint64_t>(e + kEntryKey);
                if (k >> 32) ++other;                         // a pointer: string key
                else if ((k & 0xFFFFFFFF) < 256) ++small;
                else if ((k & 0xFFFFFFFF) >= 1000 && (k & 0xFFFFFFFF) < 100000000) ++large;
                else ++other;
                return small + large + other < 64;
            });
            if (small && !large && !other && !p.by_weapon) p.by_weapon = d;
            else if (large && !small && !other && !p.by_skin) p.by_skin = d;
        }
        return p;
    }

    inline bool resolve(uint64_t beh) {
        const uint64_t r = rpm<uint64_t>(beh + kBehResolver);
        if (r == g_resolver && g_params[0].by_weapon) return true;
        const std::string name = class_of(r);
        if (name != "ybt") {
            if (diag::every(g_log, 5000)) diag::log("SKINS", "content resolver is \"%s\", not ybt", name.c_str());
            return false;
        }
        g_resolver = r;
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            g_params[k] = find_params(rpm<uint64_t>(r + kResolverParams[k]));
            ok &= g_params[k].by_weapon && g_params[k].by_skin;
        }
        if (!g_params_logged || ok) {
            g_params_logged = true;
            for (int k = 0; k < 3; ++k) {
                int nw = 0, ns = 0;
                if (g_params[k].by_weapon) each_entry(g_params[k].by_weapon, [&](uint64_t) { ++nw; return true; });
                if (g_params[k].by_skin) each_entry(g_params[k].by_skin, [&](uint64_t) { ++ns; return true; });
                diag::log("SKINS", "%s: %d weapons, %d skins", kParamNames[k], nw, ns);
            }
        }
        return ok;
    }

    inline void put_back(int weapon) {
        const auto it = g_swaps.find(weapon);
        if (it != g_swaps.end())
            for (const Swap& s : it->second) if (rpm<uint64_t>(s.at) == s.written) wpm<uint64_t>(s.at, s.original);
        // Contents the game built while the skin was in place.
        const auto v = g_values.find(weapon);
        if (v != g_values.end() && plausible(g_resolver))
            each_entry(rpm<uint64_t>(g_resolver + kResolverCache), [&](uint64_t e) {
                const uint64_t content = rpm<uint64_t>(e + kEntryValue);
                if (rpm<uint8_t>(e + kEntryKey) != weapon || !plausible(content)) return true;
                for (int k = 0; k < 3; ++k)
                    if (v->second.original[k] && rpm<uint64_t>(content + kContentParams[k]) == v->second.written[k])
                        wpm<uint64_t>(content + kContentParams[k], v->second.original[k]);
                return true;
            });
        g_values.erase(weapon);
        g_swaps.erase(weapon);
        g_applied.erase(weapon);
    }

    // The weapon's content becomes the skin's; false (nothing changed) when the skin is not known.
    inline bool apply(int weapon, int skin) {
        uint64_t values[3]{};
        for (int k = 0; k < 3; ++k) {
            const uint64_t at = find_value(g_params[k].by_skin, uint64_t(uint32_t(skin)), 4);
            values[k] = at ? rpm<uint64_t>(at) : 0;
        }
        if (!values[0]) { diag::log("SKINS", "skin %d: no mesh entry (mesh %d anim %d sound %d)", skin, !!values[0], !!values[1], !!values[2]); return false; }
        put_back(weapon);
        std::vector<Swap> swaps;
        auto change = [&](uint64_t at, uint64_t value) {
            const uint64_t was = rpm<uint64_t>(at);
            if (!value || was == value) return;
            wpm<uint64_t>(at, value);
            swaps.push_back({at, was, value});
        };
        Values v;
        for (int k = 0; k < 3; ++k)
            if (const uint64_t at = find_value(g_params[k].by_weapon, uint64_t(weapon), 1)) {
                v.original[k] = rpm<uint64_t>(at);
                v.written[k] = values[k];
                change(at, values[k]);
            }
        g_values[weapon] = v;
        int cached = 0;
        each_entry(rpm<uint64_t>(g_resolver + kResolverCache), [&](uint64_t e) {
            if (rpm<uint8_t>(e + kEntryKey) != weapon) return true;
            const uint64_t content = rpm<uint64_t>(e + kEntryValue);
            if (!plausible(content)) return true;
            for (int k = 0; k < 3; ++k) change(content + kContentParams[k], values[k]);
            ++cached;
            return true;
        });
        g_swaps[weapon] = swaps;
        g_applied[weapon] = skin;
        diag::log("SKINS", "weapon %d -> skin %d: %zu pointers (%d cached contents)", weapon, skin, swaps.size(), cached);
        return true;
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
        if (g_swaps.empty()) return;
        std::vector<int> weapons;
        for (const auto& e : g_swaps) weapons.push_back(e.first);
        for (int w : weapons) put_back(w);
        if (g_reload_beh && klass::is_named(g_reload_beh, "beh")) {
            const int meta = rpm<uint16_t>(g_reload_beh + kBehMeta);
            if (meta != kSpoiled) { g_reload_at = 0; reload(g_reload_beh, meta); }
        }
        diag::log("SKINS", "original weapons back");
    }

    inline void reset_process() {
        g_pid = proc::pid;
        g_beh.clear();
        g_resolver = 0;
        for (auto& p : g_params) p = {};
        g_params_logged = false;
        g_applied.clear();
        g_swaps.clear();
        g_values.clear();
        g_reload_beh = 0;
        g_reload_at = 0;
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
        const int cached = rpm<uint16_t>(b + kBehMeta);
        if (cached == kSpoiled) return;  // our reload is pending
        const int held = rpm<uint8_t>(b + kBehState);
        g_held_weapon = held;
        if (!resolve(b)) { set_status("weapon content not found"); return; }

        // Every weapon with a chosen skin (they stay changed, so switching shows them at once).
        bool held_changed = false;
        for (int w = 1; w < 256; ++w) {
            const int chosen = cfg::skins::chosen[w];
            const auto it = g_applied.find(w);
            const int now = it == g_applied.end() ? 0 : it->second;
            if (chosen == now) continue;
            if (chosen > 0) { if (!apply(w, chosen)) { put_back(w); cfg::skins::chosen[w] = 0; } }
            else put_back(w);
            held_changed |= w == held;
        }
        const int chosen = held < 256 ? cfg::skins::chosen[held] : 0;
        set_status("weapon " + std::to_string(held) + ": " + (chosen > 0 ? "skin " + std::to_string(chosen) : "original"));
        if (held_changed) reload(b, cached);
    }

    // ---- menu -------------------------------------------------------------------------------------
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


