#pragma once
// Class identification for net objects. A class is recognised by its TypeInfo RVA when
// one is supplied (COffsets.hpp), otherwise by the il2cpp class name ("cim", "qak", ...).
// The offset of the name pointer inside Il2CppClass is not assumed: it is measured once
// from classes whose TypeInfo RVA is already known (player, registry, store, ...).

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include <cstring>
#include <unordered_map>

namespace klass {
    enum class Kind : uint8_t { Other, Player, Grenade, GrenadeCollision, WeaponDrop };

    inline int g_name_offset = -1;
    inline int g_pid = -1;
    inline long long g_calibration_stamp = 0;
    inline std::unordered_map<uint64_t, Kind> g_kinds;

    inline bool read_name(uint64_t name_ptr, char* out, size_t cap) {
        if (!plausible(name_ptr) || cap < 4) return false;
        std::memset(out, 0, cap);
        if (!mem_read(name_ptr, out, cap - 1)) {
            // The string may end right before an unmapped page: fall back to short reads.
            std::memset(out, 0, cap);
            for (size_t i = 0; i + 1 < cap; ++i) {
                if (!mem_read(name_ptr + i, &out[i], 1) || out[i] == 0) break;
            }
        }
        for (size_t i = 0; i + 1 < cap; ++i) {
            if (out[i] == 0) return i > 0;
            if (static_cast<unsigned char>(out[i]) < 0x20 || static_cast<unsigned char>(out[i]) > 0x7E) return false;
        }
        return false;
    }

    inline bool name_at(uint64_t klass_ptr, int offset, char* out, size_t cap) {
        return read_name(rpm<uint64_t>(klass_ptr + uint64_t(offset)), out, cap);
    }

    struct Known { uint64_t typeinfo; const char* name; };

    inline bool calibrate() {
        if (g_pid != proc::pid) { g_pid = proc::pid; g_name_offset = -1; g_kinds.clear(); }
        if (g_name_offset >= 0) return true;
        const long long now = diag::now_ms();
        if (now - g_calibration_stamp < 1000) return false;
        g_calibration_stamp = now;

        const Known known[] = {
            {v100::player_typeinfo, "ake"}, {v100::registry_typeinfo, "gat"},
            {v100::store_typeinfo, "gzx"},  {v100::transforms_typeinfo, "dwc"},
            {v100::model_typeinfo, "jgi"},  {v100::stats_typeinfo, "ylw"},
            {v100::weapon_part_typeinfo, "nrk"}, {v100::ios_typeinfo, "ios"},
        };
        uint64_t classes[8]{};
        int loaded = 0;
        for (int i = 0; i < 8; ++i) {
            classes[i] = rpm<uint64_t>(proc::lib + known[i].typeinfo);
            if (plausible(classes[i])) ++loaded;
        }
        if (loaded < 2) return false;

        int best_offset = -1, best_hits = 0;
        for (int offset = 0; offset <= 0x200; offset += 8) {
            int hits = 0;
            char text[16];
            for (int i = 0; i < 8; ++i) {
                if (!plausible(classes[i]) || !name_at(classes[i], offset, text, sizeof(text))) continue;
                if (std::strcmp(text, known[i].name) == 0) ++hits;
            }
            if (hits > best_hits) { best_hits = hits; best_offset = offset; }
        }
        if (best_offset < 0 || best_hits < 2) {
            diag::log("WARN", "class name offset not found (loaded=%d). Fill the TypeInfo RVAs in COffsets.hpp", loaded);
            return false;
        }
        g_name_offset = best_offset;
        diag::log("INFO", "Il2CppClass name offset = +0x%x (matched %d of %d known classes)", best_offset, best_hits, loaded);
        return true;
    }

    inline Kind kind_of(uint64_t klass_ptr) {
        if (!plausible(klass_ptr)) return Kind::Other;
        const auto found = g_kinds.find(klass_ptr);
        if (found != g_kinds.end()) return found->second;

        Kind kind = Kind::Other;
        if (klass_ptr == rpm<uint64_t>(proc::lib + v100::player_typeinfo)) kind = Kind::Player;
        else if (v100::grenade_typeinfo && klass_ptr == rpm<uint64_t>(proc::lib + v100::grenade_typeinfo)) kind = Kind::Grenade;
        else if (v100::grenade_collision_typeinfo && klass_ptr == rpm<uint64_t>(proc::lib + v100::grenade_collision_typeinfo)) kind = Kind::GrenadeCollision;
        else if (v100::drop_typeinfo && klass_ptr == rpm<uint64_t>(proc::lib + v100::drop_typeinfo)) kind = Kind::WeaponDrop;
        else if (calibrate()) {
            char text[16];
            if (name_at(klass_ptr, g_name_offset, text, sizeof(text))) {
                if (!std::strcmp(text, "cim")) kind = Kind::Grenade;
                else if (!std::strcmp(text, "qak")) kind = Kind::GrenadeCollision;
                else if (!std::strcmp(text, "njg")) kind = Kind::WeaponDrop;
                else if (!std::strcmp(text, "ake")) kind = Kind::Player;
            } else return Kind::Other;  // unreadable name: do not cache, try again later
        } else return Kind::Other;      // not calibrated yet: do not cache

        if (g_kinds.size() > 512) g_kinds.clear();
        g_kinds.emplace(klass_ptr, kind);
        return kind;
    }

    // Is this object an instance of the class called `name`? (name offset must be known)
    inline bool is_named(uint64_t object, const char* name) {
        if (!calibrate()) return false;
        char text[16];
        const uint64_t k = rpm<uint64_t>(object);
        return plausible(k) && name_at(k, g_name_offset, text, sizeof(text)) && !std::strcmp(text, name);
    }
}
