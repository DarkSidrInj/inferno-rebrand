#pragma once
// The layer Unity actually culls with. Writing a GameObject's layer does nothing for a renderer that
// is already in the render scene (measured on device: layer written and kept, enemies unchanged):
// the scene keeps its own copy per renderer, indexed by the renderer's scene index. That index is
// the low dword at renderer+0x198 (valid for the arms and every drawn player, -1 for the local body
// that is not in the scene).
//
// The copy is found without knowing Unity's layout, from renderers whose layer is known:
//   first-person arms, gloves and weapon: layer 8 (0x100 as a mask)
//   enemy / teammate bodies:              layer 0 (0x1)
// Two shapes are searched, both checked against every known renderer at once:
//   SoA  one array of 1 or 4 byte elements indexed by the scene index (layer or 1<<layer)
//   AoS  an array of nodes holding the renderer pointer; the node size comes from the pointer hits
//        of several renderers, the layer field from the bytes of those nodes
// A layout is accepted only when it reproduces the layers of all of them, so a wrong guess cannot
// pass and nothing is written to anything that was not verified.

#include "CGameContext.hpp"
#include <algorithm>
#include <climits>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace scenenode {
    struct Ref { uint64_t renderer{}; int index{}; int layer{}; };

    struct Layout {
        bool ok{};
        bool soa{};
        uint64_t base{};   // SoA: address of element 0; AoS: address of node 0's renderer pointer
        int stride{};      // SoA: element width; AoS: node size
        int rel{};         // AoS: layer field offset from the renderer pointer
        int width{1};      // bytes of the layer field
        bool mask{};       // the field holds 1 << layer
        int refs{};        // renderers it was verified against
        const char* how{""};
    };

    inline constexpr uint64_t kIndexField = 0x198;  // renderer + 0x198: scene index (low dword)
    inline constexpr int kMaxIndex = 1 << 20;

    inline int index_of(uint64_t renderer) {
        const int32_t v = rpm<int32_t>(renderer + kIndexField);
        return v >= 0 && v < kMaxIndex ? int(v) : -1;
    }

    inline uint32_t load(const uint8_t* p, int width) { uint32_t v = 0; std::memcpy(&v, p, size_t(width)); return v; }
    inline uint32_t expect(int layer, bool mask) { return mask ? (1u << layer) : uint32_t(layer); }

    inline uint64_t address(const Layout& l, int index) {
        return l.soa ? l.base + uint64_t(index) * uint64_t(l.stride)
                     : l.base + uint64_t(index) * uint64_t(l.stride) + uint64_t(int64_t(l.rel));
    }

    inline uint32_t read_field(const Layout& l, int index) {
        uint8_t raw[4]{};
        if (!mem_read(address(l, index), raw, size_t(l.width))) return 0xFFFFFFFFu;
        return load(raw, l.width);
    }

    inline void write_field(const Layout& l, int index, uint32_t value) {
        uint8_t raw[4]{};
        std::memcpy(raw, &value, size_t(l.width));
        mem_write(address(l, index), raw, size_t(l.width));
    }

    // ---- SoA ----------------------------------------------------------------------------------------
    inline bool find_soa(const std::vector<Ref>& refs, Layout& out) {
        int anchor = -1, lo = INT_MAX, hi = 0;
        for (size_t k = 0; k < refs.size(); ++k) {
            if (refs[k].layer == 8) anchor = int(k);
            lo = std::min(lo, refs[k].index);
            hi = std::max(hi, refs[k].index);
        }
        if (anchor < 0 || hi - lo > 300000) return false;
        if (g_scan_maps.empty()) refresh_scan_maps();
        const std::vector<remote_map> maps = g_scan_maps;
        const struct { int width; bool mask; } forms[] = {{1, false}, {4, false}, {4, true}};
        for (const auto& form : forms) {
            const int width = form.width;
            const size_t span = (size_t(hi - lo + 1) * size_t(width) + 64 + 7) & ~size_t(7);
            const size_t chunk = 8u << 20;
            const uint32_t want_anchor = expect(8, form.mask);
            std::vector<uint8_t> buf;
            for (const auto& m : maps) {
                for (uint64_t at = m.begin; at < m.end; at += chunk) {
                    const uint64_t from = at >= m.begin + span ? at - span : m.begin;
                    const size_t len = size_t(std::min<uint64_t>(m.end - from, (at - from) + chunk + span));
                    buf.resize(len);
                    if (!mem_read(from, buf.data(), len)) continue;
                    const size_t first = size_t(at - from), last = std::min(len, first + chunk);
                    for (size_t p = first; p + size_t(width) <= last; p += size_t(width)) {
                        if (width == 1 && buf[p] != uint8_t(want_anchor)) continue;
                        if (width == 4 && load(buf.data() + p, 4) != want_anchor) continue;
                        bool all = true;
                        for (const Ref& r : refs) {
                            const int64_t off = int64_t(p) + int64_t(r.index - refs[size_t(anchor)].index) * width;
                            if (off < 0 || size_t(off) + size_t(width) > len || load(buf.data() + off, width) != expect(r.layer, form.mask)) { all = false; break; }
                        }
                        if (!all) continue;
                        out = {};
                        out.ok = out.soa = true;
                        out.stride = width;
                        out.width = width;
                        out.mask = form.mask;
                        out.base = from + p - uint64_t(refs[size_t(anchor)].index) * uint64_t(width);
                        out.refs = int(refs.size());
                        out.how = "SoA";
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // ---- AoS ----------------------------------------------------------------------------------------
    inline bool find_aos(const std::vector<Ref>& refs, Layout& out) {
        if (refs.size() < 6) return false;
        std::vector<uint64_t> sorted;
        for (const Ref& r : refs) sorted.push_back(r.renderer);
        std::sort(sorted.begin(), sorted.end());
        const uint64_t min_ptr = sorted.front(), max_ptr = sorted.back();
        std::unordered_map<uint64_t, std::vector<uint64_t>> hits;  // renderer -> addresses holding its pointer
        if (g_scan_maps.empty()) refresh_scan_maps();
        const std::vector<remote_map> maps = g_scan_maps;
        std::vector<uint64_t> words;
        for (const auto& m : maps) {
            for (uint64_t at = m.begin; at < m.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(m.end - at, 4u << 20)) & ~size_t(7);
                if (chunk < 8) break;
                words.resize(chunk / 8);
                if (mem_read(at, words.data(), chunk)) {
                    for (size_t i = 0; i < words.size(); ++i) {
                        const uint64_t v = words[i];
                        if (v < min_ptr || v > max_ptr || !std::binary_search(sorted.begin(), sorted.end(), v)) continue;
                        auto& list = hits[v];
                        if (list.size() < 64) list.push_back(at + i * 8);
                    }
                }
                at += chunk;
            }
        }
        // Node size from pairs of renderers, verified against all of them.
        Layout best;
        int best_matched = 0;
        uint64_t best_slot0 = 0;
        for (size_t a = 0; a < refs.size(); ++a) {
            for (size_t b = a + 1; b < refs.size(); ++b) {
                const int di = refs[a].index - refs[b].index;
                if (di == 0) continue;
                for (uint64_t ha : hits[refs[a].renderer]) {
                    for (uint64_t hb : hits[refs[b].renderer]) {
                        const int64_t d = int64_t(ha) - int64_t(hb);
                        if (d % di != 0) continue;
                        const int64_t stride = d / di;
                        if (stride < 16 || stride > 2048 || stride % 8) continue;
                        const uint64_t slot0 = ha - uint64_t(refs[a].index) * uint64_t(stride);
                        int matched = 0;
                        for (const Ref& r : refs) {
                            const uint64_t want = slot0 + uint64_t(r.index) * uint64_t(stride);
                            const auto& list = hits[r.renderer];
                            matched += std::find(list.begin(), list.end(), want) != list.end();
                        }
                        if (matched > best_matched) {
                            best_matched = matched;
                            best_slot0 = slot0;
                            best = {};
                            best.stride = int(stride);
                        }
                    }
                }
            }
        }
        if (best_matched < 6 || best_matched * 5 < int(refs.size()) * 4) return false;
        // The layer field: the bytes of every verified node, one offset that reproduces all layers.
        const int stride = best.stride;
        std::vector<std::vector<uint8_t>> nodes;
        std::vector<const Ref*> used;
        for (const Ref& r : refs) {
            const uint64_t slot = best_slot0 + uint64_t(r.index) * uint64_t(stride);
            const auto& list = hits[r.renderer];
            if (std::find(list.begin(), list.end(), slot) == list.end()) continue;
            std::vector<uint8_t> bytes(size_t(stride) * 3);
            if (!mem_read(slot - uint64_t(stride), bytes.data(), bytes.size())) continue;
            nodes.push_back(std::move(bytes));
            used.push_back(&r);
        }
        if (used.size() < 6) return false;
        const struct { int width; bool mask; } forms[] = {{1, false}, {4, false}, {4, true}};
        for (const auto& form : forms) {
            for (int rel = -stride; rel + form.width <= 2 * stride; rel += form.width == 4 ? 4 : 1) {
                bool all = true;
                for (size_t k = 0; k < used.size() && all; ++k)
                    all = load(nodes[k].data() + stride + rel, form.width) == expect(used[k]->layer, form.mask);
                if (!all) continue;
                out = best;
                out.ok = true;
                out.soa = false;
                out.base = best_slot0;
                out.rel = rel;
                out.width = form.width;
                out.mask = form.mask;
                out.refs = int(used.size());
                out.how = "AoS";
                return true;
            }
        }
        return false;
    }

    // Verified: the layout reproduces every renderer's layer.
    inline bool still_valid(const Layout& l, const std::vector<Ref>& anchors) {
        if (!l.ok) return false;
        for (const Ref& r : anchors)
            if (read_field(l, r.index) != expect(r.layer, l.mask)) return false;
        return true;
    }

    inline bool calibrate(const std::vector<Ref>& refs, Layout& out) {
        // At least a few of each kind or the layers prove nothing.
        int eights = 0, zeros = 0;
        for (const Ref& r : refs) { eights += r.layer == 8; zeros += r.layer == 0; }
        if (eights < 1 || zeros < 3) return false;
        if (find_aos(refs, out)) return true;
        // A flat array is matched by values alone, so it needs enough renderers to be unambiguous.
        return refs.size() >= 10 && eights >= 3 && find_soa(refs, out);
    }
}
