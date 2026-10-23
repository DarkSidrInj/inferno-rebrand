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
// A chosen skin: the weapon's entry in each WeaponId dictionary gets the skin's model (its template,
// else its id, else another player's built content with that skin), and every cached dlz of that
// weapon gets the same, so a default knife is built as a Butterfly (every default knife this client
// draws is). The skin id itself (the paint) is read from the registry entry of the weapon's meta id,
// which for a weapon without a skin is entry 0, shared: it carries the chosen id only for the load
// of the weapon in hands (beh's cached meta id spoiled) and gets its 0 back 300 ms after that load.
// All put back on Original or disable.

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
    inline constexpr uint64_t kBehRegistry = 0x80, kRegistryList = 0x10, kMetaStride = 0x38, kMetaSkin = 0x4;

    inline constexpr const char* kButterflyModel = "skinTemplate/KnifeButterfly";
    struct Skin { int id; const char* name; const char* model; };   // model: the game's skin template
    // The skins offered in the menu.
    inline constexpr Skin kSkins[] = {
        // Knife_Butterfly / Бабочка
        {47502, "Butterfly Gold", kButterflyModel},          {47503, "Butterfly DragonGlass", kButterflyModel},  {47504, "Butterfly Red", kButterflyModel},
        {47505, "Butterfly Starfall", kButterflyModel},      {57501, "Butterfly New1", kButterflyModel},         {97500, "Butterfly ColdFlame", kButterflyModel},
        {157500, "Butterfly Kumo", kButterflyModel},         {240007, "Butterfly Glitch", kButterflyModel},      {240059, "Butterfly Saphira", kButterflyModel},
        {240130, "Butterfly Ripple", kButterflyModel},       {240225, "Butterfly Gambit", kButterflyModel},      {240269, "Butterfly ArcaneSurge", kButterflyModel},
        {240319, "Butterfly WildWest", kButterflyModel},     {240331, "Butterfly JadeStone", kButterflyModel},   {240369, "Butterfly Twilight", kButterflyModel},
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
    // The skin id written into the registry entry of the weapon in hands for one load.
    struct Transient { uint64_t at{}; int original{}, written{}; int op{}; long long started{}, seen{}; };
    inline Transient g_tr;
    inline uint64_t g_loaded_beh = 0;                // what the weapon in hands was last loaded with by us
    inline int g_loaded_op = -1, g_loaded_skin = 0, g_pending_skin = 0;
    inline long long g_retry_at = 0;

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

    // Registry entry (WeaponMetaData) of a meta id: index = meta id (entry 0 = the blank one that
    // weapons without a skin use; seen on device).
    inline uint64_t meta_entry(uint64_t beh, int meta) {
        const uint64_t reg = rpm<uint64_t>(beh + kBehRegistry);
        const uint64_t list = plausible(reg) ? rpm<uint64_t>(reg + kRegistryList) : 0;
        const uint64_t items = plausible(list) ? rpm<uint64_t>(list + v100::list_items) : 0;
        const int count = plausible(list) ? rpm<int>(list + v100::list_count) : 0;
        if (!plausible(items) || meta < 0 || meta >= count || count > 4096) return 0;
        return items + v100::array_data + uint64_t(meta) * kMetaStride;
    }

    // The entry gets its skin back once the weapon has loaded (300 ms after its op id moved), or
    // after 1.5 s without a load.
    inline void transient_tick(uint64_t beh) {
        if (!g_tr.at) return;
        const long long now = diag::now_ms();
        const int op = rpm<int>(beh + kBehOpId);
        if (!g_tr.seen && op != g_tr.op) g_tr.seen = now;
        if (!(g_tr.seen && now - g_tr.seen > 300) && now - g_tr.started < 1500) return;
        if (rpm<int>(g_tr.at + kMetaSkin) == g_tr.written) wpm<int>(g_tr.at + kMetaSkin, g_tr.original);
        if (g_tr.seen) {
            g_loaded_beh = beh;
            g_loaded_op = op;
            g_loaded_skin = g_pending_skin;
            diag::log("SKINS", "weapon in hands loaded with skin %d (op %d)", g_pending_skin, op);
        } else {
            g_retry_at = now + 2000;
            diag::log("SKINS", "no load within 1.5 s; trying again in 2 s");
        }
        g_tr = {};
    }

    inline void restore() {
        if (g_tr.at && rpm<int>(g_tr.at + kMetaSkin) == g_tr.written) wpm<int>(g_tr.at + kMetaSkin, g_tr.original);
        g_tr = {};
        if (g_swaps.empty() && !g_loaded_skin) return;
        g_loaded_skin = 0;
        g_loaded_beh = 0;
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
        g_tr = {};
        g_loaded_beh = 0;
        g_loaded_op = -1;
        g_loaded_skin = g_pending_skin = 0;
        g_retry_at = 0;
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
        transient_tick(b);
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
        const int chosen = held < 256 && g_applied.count(held) ? cfg::skins::chosen[held] : 0;
        set_status("weapon " + std::to_string(held) + ": " + (chosen > 0 ? "skin " + std::to_string(chosen) : "original"));

        // The weapon in hands loads again when it does not show the chosen skin: its registry entry
        // carries the skin id for that one load.
        const int op = rpm<int>(b + kBehOpId);
        const int shown = g_loaded_beh == b && g_loaded_op == op ? g_loaded_skin : 0;
        if (g_tr.at || (!held_changed && shown == chosen) || diag::now_ms() < g_retry_at) return;
        if (chosen > 0) {
            const uint64_t e = meta_entry(b, cached);
            if (!e) { set_status("weapon in hands: no registry entry for meta id " + std::to_string(cached)); return; }
            g_tr = {e, rpm<int>(e + kMetaSkin), chosen, op, diag::now_ms(), 0};
            wpm<int>(e + kMetaSkin, chosen);
        } else {
            g_tr = {0, 0, 0, op, diag::now_ms(), 0};
        }
        g_pending_skin = chosen;
        reload(b, cached);
        if (!g_tr.at) {  // nothing to put back: count the load when it comes
            g_loaded_beh = b;
            g_loaded_op = -1;
            g_loaded_skin = 0;
        }
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


