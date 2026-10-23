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
//   - iwf<T> also has a name table: knife models by template ("skinTemplate/KnifeButterfly", ...),
//     seen on device; the int table was empty there.
// A chosen skin: the loader reads the skin id from the registry entry of the weapon's meta id, which
// for a weapon without a skin is entry 0, shared by everyone. While the knife is in hands that
// entry carries the chosen id (the game reloads the knife on its own often, and every load must see
// it); the knife is made to load once (beh's cached meta id spoiled). Another weapon in hands puts
// the entry back and that weapon loads again, clean. The game then resolves (weapon, skin) itself:
// on device a default knife with a Butterfly skin id was built from the Butterfly template with the
// skin's packs. cfg::skins::force_model (no menu entry) also writes the skin's model into the
// weapon's own table entries and cached contents, the earlier way: the model without its packs.
// All put back on Original or disable.
//
// Read from the game code (skin_code.bin, beh at RVA 0x98A....): beh.plkc runs every frame, takes the
// active weapon item (qjf), builds (WeaponId @item+0x10, WeaponMetaId from the registry, view) and, when
// it differs from beh's cached tuple, starts SetWeaponContent. That loads fsne(WeaponId, 0) (the base
// model) and then, if the registry entry of the meta id has a SkinId, fsne(WeaponId, SkinId): the skin
// packs on top. The model of a default knife (70) with the skin of a Butterfly (75) is therefore the
// default knife's base with Butterfly paint: "two knives". The item is rebuilt by the game every frame,
// so the WeaponId can not be held in its data; with cfg::skins::code_patch the one instruction of
// plkc that reads it (ldrb w1,[x9,#0x10] at RVA 0x98A9264) becomes a call to a few instructions in the
// zero padding at the end of the code mapping, which answer the Butterfly's id for knives (70..89)
// while a Butterfly skin is chosen. Opt-in, written back on disable and on exit.

#include "CGameContext.hpp"
#include "CClass.hpp"
#include "../CConfigs/CConfig.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
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
    inline constexpr uint64_t kBehRegistry = 0x80, kRegistryList = 0x10, kMetaSkin = 0x4;

    inline constexpr const char* kButterflyModel = "skinTemplate/KnifeButterfly";
    struct Skin { int id; const char* name; const char* model; int weapon; };   // model: the game's skin template; weapon: the WeaponId it belongs to
    // The skins offered in the menu.
    inline constexpr Skin kSkins[] = {
        // Knife_Butterfly / Бабочка
        {47502, "Butterfly Gold", kButterflyModel, 75},          {47503, "Butterfly DragonGlass", kButterflyModel, 75},  {47504, "Butterfly Red", kButterflyModel, 75},
        {47505, "Butterfly Starfall", kButterflyModel, 75},      {57501, "Butterfly New1", kButterflyModel, 75},         {97500, "Butterfly ColdFlame", kButterflyModel, 75},
        {157500, "Butterfly Kumo", kButterflyModel, 75},         {240007, "Butterfly Glitch", kButterflyModel, 75},      {240059, "Butterfly Saphira", kButterflyModel, 75},
        {240130, "Butterfly Ripple", kButterflyModel, 75},       {240225, "Butterfly Gambit", kButterflyModel, 75},      {240269, "Butterfly ArcaneSurge", kButterflyModel, 75},
        {240319, "Butterfly WildWest", kButterflyModel, 75},     {240331, "Butterfly JadeStone", kButterflyModel, 75},   {240369, "Butterfly Twilight", kButterflyModel, 75},
    };

    struct View { int weapon = -1; int current = 0; std::vector<int> ids; std::vector<std::string> names; std::string status; };
    // The two dictionaries of one iwf<T>, found by their keys.
    struct Params { uint64_t by_weapon{}, by_skin{}, by_name{}; };
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
    // The registry entry that carries the knife's skin id while the knife is in hands.
    struct Hold { uint64_t at{}; int original{}, written{}; uint64_t beh{}; int loaded_for{}; };
    inline Hold g_hold;
    inline constexpr long long kHoldMs = 3000;
    // The skins in the menu are knife skins: knife WeaponIds (70 default knife on device, 75 Butterfly
    // by its skin ids; grenades start at 90).
    inline bool knife(int weapon) { return weapon >= 70 && weapon < 90; }

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

    // A managed string as ASCII ("" when it is not one).
    inline std::string managed_text(uint64_t str) {
        if (!plausible(str)) return {};
        const int n = rpm<int>(str + 0x10);
        if (n <= 0 || n > 128) return {};
        std::vector<uint16_t> w(size_t(n), 0);
        if (!mem_read(str + 0x14, w.data(), w.size() * 2)) return {};
        std::string out;
        for (uint16_t c : w) out.push_back(c >= 0x20 && c < 0x7F ? char(c) : '?');
        return out;
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
            else if (other && !small && !large && !p.by_name) {
                bool text = false;
                each_entry(d, [&](uint64_t e) { text = !managed_text(rpm<uint64_t>(e + kEntryKey)).empty(); return false; });
                if (text) p.by_name = d;
            }
        }
        return p;
    }

    // Once per resolver: every dictionary of the three tables and of the content cache, with keys.
    inline void dump(uint64_t resolver) {
        auto keys = [](uint64_t d, int max) {
            std::string out;
            int n = 0;
            each_entry(d, [&](uint64_t e) {
                const uint64_t k = rpm<uint64_t>(e + kEntryKey);
                char one[64];
                const std::string t = (k >> 32) ? managed_text(k) : std::string();
                if (!t.empty()) std::snprintf(one, sizeof(one), " \"%.40s\"", t.c_str());
                else std::snprintf(one, sizeof(one), " %llx", (unsigned long long)k);
                out += one;
                return ++n < max;
            });
            return out;
        };
        auto count = [](uint64_t d) { int n = 0; each_entry(d, [&](uint64_t) { ++n; return true; }); return n; };
        for (int k = 0; k < 3; ++k) {
            const uint64_t iwf = rpm<uint64_t>(resolver + kResolverParams[k]);
            for (uint64_t off = 0x10; off <= 0x80; off += 8) {
                const uint64_t d = rpm<uint64_t>(iwf + off);
                if (class_of(d).rfind("Dictionary", 0) != 0) continue;
                diag::log("SKINS", "%s +0x%llx: %d entries:%s", kParamNames[k], (unsigned long long)off, count(d), keys(d, 6).c_str());
            }
            diag::log("SKINS", "%s: by weapon %s, by skin id %s, by name %s", kParamNames[k], g_params[k].by_weapon ? "yes" : "no",
                      g_params[k].by_skin ? "yes" : "no", g_params[k].by_name ? "yes" : "no");
        }
        const uint64_t cache = rpm<uint64_t>(resolver + kResolverCache);
        diag::log("SKINS", "content cache: %d entries (weapon | skin << 32):%s", count(cache), keys(cache, 16).c_str());
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
            ok &= g_params[k].by_weapon != 0;
        }
        if (!g_params_logged) {
            g_params_logged = true;
            dump(r);
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
        const char* from = "skin id table";
        const std::string id = std::to_string(skin);
        const char* model = nullptr;
        for (const Skin& k : kSkins) if (k.id == skin) model = k.model;
        if (model)  // the skin's model template in the name table
            for (int k = 0; k < 3; ++k) {
                if (!g_params[k].by_name) continue;
                each_entry(g_params[k].by_name, [&](uint64_t e) {
                    if (managed_text(rpm<uint64_t>(e + kEntryKey)) != model) return true;
                    values[k] = rpm<uint64_t>(e + kEntryValue);
                    return false;
                });
                if (values[k]) from = model;
            }
        for (int k = 0; k < 3; ++k) {
            if (values[k]) continue;
            uint64_t at = g_params[k].by_skin ? find_value(g_params[k].by_skin, uint64_t(uint32_t(skin)), 4) : 0;
            if (!at && g_params[k].by_name)  // the name table: the id as the key, or in it
                each_entry(g_params[k].by_name, [&](uint64_t e) {
                    const std::string t = managed_text(rpm<uint64_t>(e + kEntryKey));
                    if (t == id || t.find(id) != std::string::npos) { at = e + kEntryValue; from = "name table"; return false; }
                    return true;
                });
            values[k] = at ? rpm<uint64_t>(at) : 0;
        }
        if (!values[0])  // someone in the match has it: its built content
            each_entry(rpm<uint64_t>(g_resolver + kResolverCache), [&](uint64_t e) {
                const uint64_t content = rpm<uint64_t>(e + kEntryValue);
                if (rpm<int>(e + kEntryKey + 4) != skin || !plausible(content)) return true;
                for (int k = 0; k < 3; ++k) values[k] = rpm<uint64_t>(content + kContentParams[k]);
                from = "content of another player's weapon";
                return false;
            });
        // Animations stay the weapon's own: on device the Butterfly ones stuck to every weapon.
        values[1] = 0;
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
        diag::log("SKINS", "weapon %d -> skin %d (%s): %zu pointers (%d cached contents)", weapon, skin, from, swaps.size(), cached);
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

    // The match's weapon meta registry (beh+0x80 -> List<WeaponMetaData> @+0x10): 0x38 bytes per entry,
    // SkinId at +4. Entry 0 is the blank one that weapons without a skin use.
    inline constexpr uint64_t kMetaDataStride = 0x38, kEntrySkin = 0x4;
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
        return index >= 0 && index < r.count ? r.data + uint64_t(index) * kMetaDataStride : 0;
    }

    // Registry entry (WeaponMetaData) of a meta id: index = meta id (entry 0 = the blank one that
    // weapons without a skin use; seen on device).
    inline uint64_t meta_entry(uint64_t beh, int meta) {
        const uint64_t reg = rpm<uint64_t>(beh + kBehRegistry);
        const uint64_t list = plausible(reg) ? rpm<uint64_t>(reg + kRegistryList) : 0;
        const uint64_t items = plausible(list) ? rpm<uint64_t>(list + v100::list_items) : 0;
        const int count = plausible(list) ? rpm<int>(list + v100::list_count) : 0;
        if (!plausible(items) || meta < 0 || meta >= count || count > 4096) return 0;
        return items + v100::array_data + uint64_t(meta) * kMetaDataStride;
    }

    // What the game built for a weapon: every cached content of it, with its params and packs.
    inline void dump_contents(int weapon) {
        if (!plausible(g_resolver)) return;
        const auto v = g_values.find(weapon);
        if (v != g_values.end())
            diag::log("SKINS", "weapon %d own mesh/anim/sound 0x%llx 0x%llx 0x%llx, written 0x%llx 0x%llx 0x%llx", weapon,
                      (unsigned long long)v->second.original[0], (unsigned long long)v->second.original[1], (unsigned long long)v->second.original[2],
                      (unsigned long long)v->second.written[0], (unsigned long long)v->second.written[1], (unsigned long long)v->second.written[2]);
        each_entry(rpm<uint64_t>(g_resolver + kResolverCache), [&](uint64_t e) {
            if (rpm<uint8_t>(e + kEntryKey) != weapon) return true;
            const uint64_t c = rpm<uint64_t>(e + kEntryValue);
            const uint64_t packs = plausible(c) ? rpm<uint64_t>(c + 0x28) : 0;
            diag::log("SKINS", "  built (%d, skin %d): mesh 0x%llx anim 0x%llx sound 0x%llx, packs %d, +0x30 %d +0x34 %d", weapon,
                      rpm<int>(e + kEntryKey + 4), (unsigned long long)rpm<uint64_t>(c + 0x10), (unsigned long long)rpm<uint64_t>(c + 0x18),
                      (unsigned long long)rpm<uint64_t>(c + 0x20), plausible(packs) ? rpm<int>(packs + v100::list_count) : -1,
                      int(rpm<uint8_t>(c + 0x30)), rpm<int>(c + 0x34));
            return true;
        });
    }

    // ---- what the loader sees for the local knife (opt-in code patch) ----------------------------
    // Two instructions of the weapon view become calls into a few instructions in the zero padding at the end
    // of the code mapping:
    //   site 1 (RVA 0x98A9264, beh.plkc)          ldrb w1,[x9,#0x10] : the item's WeaponId -> the skin's weapon (75)
    //   site 2 (RVA 0x98B32E4, SetWeaponContent)  ldr w9,[x19,#0x54] : the SkinId read from the registry entry of
    //                                                                 the meta id -> the chosen skin (also stored
    //                                                                 back, the loader reads it a second time)
    // only for the local beh (site 1: x19 == data.beh, site 2: x20 == data.beh), for knives (70..89). Every
    // other player passes through unchanged and the match's registry is never touched.
    inline constexpr uint64_t kSite1Rva = 0x98A9264, kSite2Rva = 0x98B32E4;
    inline constexpr uint32_t kOriginal1 = 0x39404121, kOriginal2 = 0xb9405669;
    inline constexpr uint32_t kCave[26] = {
        0x39404121, 0x51011831, 0x7100523f, 0x54000102, 0x100002d0, 0xf9400211, 0xeb13023f, 0x54000081,
        0xb9400a11, 0x34000051, 0x2a1103e1, 0xd65f03c0,                     // A: weapon id, ends with ret at 0x2c
        0xb9405669, 0x100001aa, 0xf940014b, 0xeb0b029f, 0x54000121, 0x3940d26b, 0x5101196b, 0x71004d7f,
        0x540000a8, 0xb9400d4b, 0x3400006b, 0x2a0b03e9, 0xb9005669, 0xd65f03c0};   // B (at 0x30): skin id, ret at 0x64
    inline constexpr uint64_t kCaveEntryB = 0x30, kCaveData = 0x68, kCaveSize = 0x80;   // data: u64 beh, u32 weapon, u32 skin
    struct CaveData { uint64_t beh{}; uint32_t weapon{}, skin{}; };
    struct CodePatch { uint64_t site1{}, site2{}, cave{}; CaveData now{}; };
    inline CodePatch g_patch;
    inline uint64_t g_cave_override = 0;                      // tests

    inline uint64_t exec_mapping_end(uint64_t address) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
        FILE* f = std::fopen(path, "r");
        if (!f) return 0;
        char line[512];
        uint64_t end = 0;
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long lo = 0, hi = 0;
            char perms[5]{};
            if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) == 3 && perms[2] == 'x' && address >= lo && address < hi) { end = hi; break; }
        }
        std::fclose(f);
        return end;
    }

    // Code pages are read-only for process_vm_writev, and the shared /proc/<pid>/mem handle is read-only:
    // write through a handle of its own.
    inline bool code_write(uint64_t address, const void* data, size_t size) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/mem", proc::pid);
        const int fd = ::open(path, O_RDWR);
        if (fd < 0) { diag::log("SKINS", "code patch: cannot open %s for writing (errno %d)", path, errno); return false; }
        const ssize_t n = ::pwrite(fd, data, size, off_t(address));
        const int err = errno;
        ::close(fd);
        if (n != ssize_t(size)) { diag::log("SKINS", "code patch: write of %zu bytes at 0x%llx gave %zd (errno %d)", size, (unsigned long long)address, n, err); return false; }
        return true;
    }
    template <class T> inline bool code_write(uint64_t address, const T& value) { return code_write(address, &value, sizeof(T)); }

    inline uint32_t bl_word(uint64_t from, uint64_t to) { return 0x94000000u | uint32_t(((int64_t(to) - int64_t(from)) >> 2) & 0x3FFFFFF); }

    inline bool patch_install() {
        if (g_patch.site1) return true;
        const uint64_t site1 = proc::lib + kSite1Rva, site2 = proc::lib + kSite2Rva;
        const uint32_t word1 = rpm<uint32_t>(site1), word2 = rpm<uint32_t>(site2);
        uint64_t cave = g_cave_override;
        if (!cave) {
            const uint64_t end = exec_mapping_end(site1);
            if (!end) { diag::log("SKINS", "code patch: code mapping not found"); return false; }
            cave = (end - kCaveSize) & ~uint64_t(0xF);
        }
        for (uint64_t site : {site1, site2}) {
            const int64_t distance = int64_t(cave) - int64_t(site);
            if (distance > 0x7FFFFF0 || distance < -0x7FFFFF0) { diag::log("SKINS", "code patch: cave out of reach"); return false; }
        }
        uint8_t now[kCaveSize];
        mem_read(cave, now, sizeof(now));
        const bool ours = word1 == bl_word(site1, cave) && word2 == bl_word(site2, cave + kCaveEntryB) &&
                          std::memcmp(now, kCave, sizeof(kCave)) == 0;
        if (!ours) {
            if (word1 != kOriginal1 || word2 != kOriginal2) {
                diag::log("SKINS", "code patch: instructions at the sites are 0x%08x 0x%08x, not the known ones (other build)", word1, word2);
                return false;
            }
            for (uint8_t b : now) if (b) { diag::log("SKINS", "code patch: no zero padding at 0x%llx", (unsigned long long)cave); return false; }
            uint8_t blob[kCaveSize]{};
            std::memcpy(blob, kCave, sizeof(kCave));
            auto rollback = [&] {
                if (rpm<uint32_t>(site1) != kOriginal1) code_write(site1, kOriginal1);
                if (rpm<uint32_t>(site2) != kOriginal2) code_write(site2, kOriginal2);
                code_write(cave, std::vector<uint8_t>(kCaveSize, 0).data(), kCaveSize);
            };
            if (!code_write(cave, blob, sizeof(blob))) return false;
            if (!code_write(site1, bl_word(site1, cave)) || !code_write(site2, bl_word(site2, cave + kCaveEntryB)) ||
                rpm<uint32_t>(site1) != bl_word(site1, cave) || rpm<uint32_t>(site2) != bl_word(site2, cave + kCaveEntryB)) {
                rollback();
                diag::log("SKINS", "code patch: the code did not take the write");
                return false;
            }
        }
        g_patch = {site1, site2, cave, {}};
        mem_read(cave + kCaveData, &g_patch.now, sizeof(CaveData));
        diag::log("SKINS", "code patch: 0x%llx and 0x%llx -> cave 0x%llx (%zu bytes)", (unsigned long long)site1, (unsigned long long)site2,
                  (unsigned long long)cave, sizeof(kCave));
        return true;
    }

    // What the local beh's knife loads as: the weapon id (0: its own) and the skin id (0: the game's).
    // True when it changed.
    inline bool patch_set(uint64_t beh, int weapon, int skin) {
        if (!g_patch.site1) return false;
        const CaveData want{weapon || skin ? beh : 0, uint32_t(weapon), uint32_t(skin)};
        if (want.beh == g_patch.now.beh && want.weapon == g_patch.now.weapon && want.skin == g_patch.now.skin) return false;
        if (!code_write(g_patch.cave + kCaveData, want)) return false;
        const bool changed = want.weapon != g_patch.now.weapon || want.skin != g_patch.now.skin;
        g_patch.now = want;
        return changed;
    }

    inline void patch_remove() {
        if (!g_patch.site1) return;
        if (rpm<uint32_t>(g_patch.site1) == bl_word(g_patch.site1, g_patch.cave)) code_write(g_patch.site1, kOriginal1);
        if (rpm<uint32_t>(g_patch.site2) == bl_word(g_patch.site2, g_patch.cave + kCaveEntryB)) code_write(g_patch.site2, kOriginal2);
        code_write(g_patch.cave, std::vector<uint8_t>(kCaveSize, 0).data(), kCaveSize);
        diag::log("SKINS", "code patch removed");
        g_patch = {};
    }

    inline void release_hold() {
        if (g_hold.at && rpm<int>(g_hold.at + kMetaSkin) == g_hold.written) wpm<int>(g_hold.at + kMetaSkin, g_hold.original);
        g_hold = {};
    }

    inline void restore() {
        const bool patched = g_patch.site1 != 0 && (g_patch.now.weapon != 0 || g_patch.now.skin != 0);
        patch_remove();
        const bool held = g_hold.at != 0 || patched;
        release_hold();
        if (g_swaps.empty() && !held) return;
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
        g_hold = {};
        g_patch = {};   // the patched process is gone
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
        // With the code patch the knife in hands loads (and beh caches it) as the skin's weapon: it is still
        // the knife the choices are made for.
        int held = rpm<uint8_t>(b + kBehState);
        if (g_patch.now.weapon && held == int(g_patch.now.weapon)) held = 70;
        g_held_weapon = held;
        if (!resolve(b)) { set_status("weapon content not found"); return; }

        // Every weapon with a chosen skin (they stay changed, so switching shows them at once).
        bool held_changed = false;
        static bool force_model = cfg::skins::force_model;
        if (force_model != cfg::skins::force_model) {  // switched: everything again
            force_model = cfg::skins::force_model;
            std::vector<int> weapons;
            for (const auto& e : g_applied) weapons.push_back(e.first);
            for (int w : weapons) { put_back(w); held_changed |= w == held; }
        }
        static bool mode_logged = false;
        if (!mode_logged) {
            mode_logged = true;
            diag::log("SKINS", "mode: knives only, skin id held %lld ms, force model %s", (long long)kHoldMs, force_model ? "ON" : "off");
        }
        for (int w = 1; w < 256; ++w) {
            if (!knife(w) && cfg::skins::chosen[w]) cfg::skins::chosen[w] = 0;  // a knife skin on a gun
            const int chosen = cfg::skins::chosen[w];
            const auto it = g_applied.find(w);
            const int now = it == g_applied.end() ? 0 : it->second;
            if (chosen == now) continue;
            put_back(w);
            if (chosen > 0) {
                if (!force_model) g_applied[w] = chosen;
                else if (!apply(w, chosen)) { put_back(w); cfg::skins::chosen[w] = 0; }
            }
            held_changed |= w == held;
        }
        const int chosen = held < 256 && g_applied.count(held) ? cfg::skins::chosen[held] : 0;
        set_status("weapon " + std::to_string(held) + ": " + (chosen > 0 ? "skin " + std::to_string(chosen) : "original"));

        // The local knife (opt-in code patch): loads as the skin's weapon with the skin's id.
        static bool patch_failed = false;       // tried and refused: not again until the checkbox is switched off and on
        if (!cfg::skins::code_patch) patch_failed = false;
        else if (!g_patch.site1 && !patch_failed && !patch_install()) {
            patch_failed = true;
            set_status("code patch not possible, see the log");
        }
        if (!cfg::skins::code_patch && g_patch.site1) patch_remove();
        bool patch_changed = false;
        const bool patched = g_patch.site1 != 0;
        if (patched) {
            const int skin = cfg::skins::chosen[70];
            int model = 0;
            if (skin > 0) for (const Skin& k : kSkins) if (k.id == skin) model = k.weapon;
            patch_changed = patch_set(b, model, skin > 0 ? skin : 0);
            if (patch_changed) diag::log("SKINS", "knives load as weapon %d, skin %d", model ? model : 70, skin > 0 ? skin : 0);
        }

        // Without the code patch: the knife's skin id in the blank entry while the knife is in hands. The
        // killfeed then shows that skin for every knife kill.
        if (!patched && chosen > 0 && knife(held)) {
            const uint64_t e = meta_entry(b, cached);
            if (!e) { set_status("knife: no registry entry for meta id " + std::to_string(cached)); return; }
            if (g_hold.at && g_hold.at != e) release_hold();
            if (!g_hold.at) g_hold = {e, rpm<int>(e + kMetaSkin), chosen, 0, 0};
            if (rpm<int>(e + kMetaSkin) != chosen) wpm<int>(e + kMetaSkin, chosen);
            g_hold.written = chosen;
            if (held_changed || patch_changed || g_hold.beh != b || g_hold.loaded_for != chosen) {
                g_hold.beh = b;
                g_hold.loaded_for = chosen;
                diag::log("SKINS", "knife %d: skin %d held in registry entry %d (was %d)", held, chosen, cached, g_hold.original);
                reload(b, cached);
            }
        } else if (g_hold.at) {
            release_hold();
            diag::log("SKINS", "weapon %d in hands: registry entry back, it loads clean", held);
            reload(b, cached);
        }
        // The loader picks a changed weapon id up by itself when the tuple changes; the same tuple with another
        // skin id needs a nudge.
        if (patched && patch_changed && knife(held)) reload(b, cached);
    }

    // ---- game code for offline reading -----------------------------------------------------------
    // The weapon view classes beh and gby of the dump (their methods, RVA 0x98A4874..0x98C3A6C, plus
    // slack: the async MoveNext of SetWeaponContent, the mesh and material setters, LoadArms) as
    // read from the running game: ~144 KB instead of the 50 MB library. File layout: "SKCD", u32
    // version, u64 first RVA, u64 size, u64 library base, then the bytes (32-byte header). Disassemble offline with
    // the RVAs of dump.cs.
    inline constexpr uint64_t kCodeFirst = 0x98A0000, kCodeEnd = 0x98C4000;

    inline bool dump_code() {
        if (proc::pid <= 0 || !plausible(proc::lib)) { diag::log("SKINS", "code dump: game not found"); return false; }
        std::vector<uint8_t> bytes(size_t(kCodeEnd - kCodeFirst), 0);
        size_t bad = 0;
        for (uint64_t at = 0; at < bytes.size(); at += 0x1000)
            if (!mem_read(proc::lib + kCodeFirst + at, bytes.data() + at, 0x1000)) ++bad;
        const char* paths[] = {"/sdcard/skin_code.bin", "/storage/emulated/0/skin_code.bin", "/data/local/tmp/skin_code.bin"};
        for (const char* path : paths) {
            FILE* f = std::fopen(path, "wb");
            if (!f) continue;
            const uint32_t version = 1;
            const uint64_t first = kCodeFirst, size = bytes.size(), base = proc::lib;
            std::fwrite("SKCD", 1, 4, f);
            std::fwrite(&version, 4, 1, f);
            std::fwrite(&first, 8, 1, f);
            std::fwrite(&size, 8, 1, f);
            std::fwrite(&base, 8, 1, f);
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            diag::log("SKINS", "code dump: %zu KB (%zu unreadable pages) written to %s", bytes.size() / 1024, bad, path);
            set_status(std::string("code dump: ") + path);
            return true;
        }
        diag::log("SKINS", "code dump: no writable path");
        set_status("code dump: cannot write the file");
        return false;
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
        if (!knife(v.weapon)) {
            v.status = "take the knife";
            return v;
        }
        for (const Skin& s : kSkins) { v.ids.push_back(s.id); v.names.push_back(s.name); }
        const int chosen = v.weapon >= 0 && v.weapon < 256 ? cfg::skins::chosen[v.weapon] : 0;
        for (size_t i = 0; i < v.ids.size(); ++i) if (v.ids[i] == chosen) v.current = int(i);
        return v;
    }
}


