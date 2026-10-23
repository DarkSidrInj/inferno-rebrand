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
    // Renderers without a scene index (index < 0) are found in the node array by their pointer, once
    // the array's geometry is known from the ones that have one.
    inline constexpr int kMaxNodes = 4096;  // nodes searched for a renderer pointer

    inline int node_slot_index(const Layout& l, uint64_t renderer) {
        if (l.soa || l.stride <= 0) return -1;
        std::vector<uint64_t> words;
        for (int first = 0; first < kMaxNodes; first += 128) {
            const int count = 128;
            std::vector<uint8_t> raw(size_t(count) * size_t(l.stride));
            if (!mem_read(l.base + uint64_t(first) * uint64_t(l.stride), raw.data(), raw.size())) continue;
            for (int k = 0; k < count; ++k) {
                uint64_t ptr = 0;
                std::memcpy(&ptr, raw.data() + size_t(k) * size_t(l.stride), 8);
                if (ptr == renderer) return first + k;
            }
        }
        return -1;
    }

    // Is `index` still this renderer's node?
    inline bool node_is(const Layout& l, int index, uint64_t renderer) {
        if (index < 0) return false;
        if (l.soa) return index_of(renderer) == index;
        return rpm<uint64_t>(l.base + uint64_t(index) * uint64_t(l.stride)) == renderer;
    }

    // The scene node of a renderer: its index field when the node holds its pointer, otherwise a search
    // of the node array (cached; a miss is not searched again for two seconds).
    inline int node_index(const Layout& l, uint64_t renderer) {
        const int index = index_of(renderer);
        if (l.soa) return index;
        auto holds = [&](int i) { return i >= 0 && rpm<uint64_t>(l.base + uint64_t(i) * uint64_t(l.stride)) == renderer; };
        if (holds(index)) return index;
        struct Entry { int index; long long when; };
        static std::unordered_map<uint64_t, Entry> cache;
        static uint64_t cache_base = 0;
        if (cache_base != l.base) { cache.clear(); cache_base = l.base; }
        const long long now = diag::now_ms();
        const auto it = cache.find(renderer);
        if (it != cache.end()) {
            if (holds(it->second.index)) return it->second.index;
            if (now - it->second.when < 2000) return -1;
        }
        const int found = node_slot_index(l, renderer);
        cache[renderer] = {found, now};
        if (cache.size() > 256) cache.clear();
        return found;
    }

    inline bool find_aos(std::vector<Ref> refs, Layout& out) {
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
        {
            size_t with_hits = 0, total_hits = 0;
            for (const Ref& r : refs) { const auto it = hits.find(r.renderer); if (it != hits.end() && !it->second.empty()) { ++with_hits; total_hits += it->second.size(); } }
            diag::log("SCENE", "pointer search: %zu renderers, %zu with a pointer somewhere in memory (%zu pointers)", refs.size(), with_hits, total_hits);
            int shown = 0;
            for (const Ref& r : refs) {
                const auto it = hits.find(r.renderer);
                if (it == hits.end() || it->second.empty() || shown >= 3) continue;
                ++shown;
                const uint64_t at = it->second.front();
                uint64_t around[12]{};
                mem_read(at - 32, around, sizeof(around));
                diag::log("SCENE", "  renderer 0x%llx index %d layer %d: pointer at 0x%llx: %016llx %016llx %016llx %016llx [%016llx] %016llx %016llx %016llx",
                          (unsigned long long)r.renderer, r.index, r.layer, (unsigned long long)at,
                          (unsigned long long)around[0], (unsigned long long)around[1], (unsigned long long)around[2], (unsigned long long)around[3],
                          (unsigned long long)around[4], (unsigned long long)around[5], (unsigned long long)around[6], (unsigned long long)around[7]);
            }
        }
        // Node size from pairs of renderers that have an index, verified against all of them.
        Layout best;
        int best_matched = 0, indexed = 0;
        uint64_t best_slot0 = 0;
        for (const Ref& r : refs) indexed += r.index >= 0;
        for (size_t a = 0; a < refs.size(); ++a) {
            if (refs[a].index < 0) continue;
            for (size_t b = a + 1; b < refs.size(); ++b) {
                if (refs[b].index < 0) continue;
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
                            if (r.index < 0) continue;
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
        if (best_matched < 6 || best_matched * 5 < indexed * 4) {
            diag::log("SCENE", "no node array: best stride %d matched %d of %d indexed renderers", best.stride, best_matched, indexed);
            return false;
        }
        const int stride = best.stride;
        // The renderers without an index: their pointer sitting on a node boundary of this array.
        int derived = 0;
        for (Ref& r : refs) {
            if (r.index >= 0) continue;
            for (uint64_t h : hits[r.renderer]) {
                if (h < best_slot0 || (h - best_slot0) % uint64_t(stride)) continue;
                const uint64_t k = (h - best_slot0) / uint64_t(stride);
                if (k >= uint64_t(kMaxNodes)) continue;
                r.index = int(k);
                ++derived;
                break;
            }
        }
        diag::log("SCENE", "node array: stride %d, %d renderers matched, %d more placed by their pointer", stride, best_matched, derived);
        // The layer field: the bytes of every node, one offset that reproduces all layers.
        std::vector<std::vector<uint8_t>> nodes;
        std::vector<const Ref*> used;
        for (const Ref& r : refs) {
            if (r.index < 0) continue;
            const uint64_t slot = best_slot0 + uint64_t(r.index) * uint64_t(stride);
            if (rpm<uint64_t>(slot) != r.renderer) continue;
            std::vector<uint8_t> bytes(size_t(stride) * 3);
            if (!mem_read(slot - uint64_t(stride), bytes.data(), bytes.size())) continue;
            nodes.push_back(std::move(bytes));
            used.push_back(&r);
        }
        int eights = 0, zeros = 0;
        for (const Ref* r : used) { eights += r->layer == 8; zeros += r->layer == 0; }
        if (used.size() < 6) { diag::log("SCENE", "too few nodes to read a layer field (%zu)", used.size()); return false; }
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
                diag::log("SCENE", "layer field at %+d bytes from the renderer pointer, %d byte(s), %s, verified on %zu nodes (%d on layer 8, %d on layer 0)",
                          rel, form.width, form.mask ? "mask" : "layer", used.size(), eights, zeros);
                return true;
            }
        }
        diag::log("SCENE", "no layer field reproduces the layers of %zu nodes (%d on layer 8, %d on layer 0)", used.size(), eights, zeros);
        if (!nodes.empty()) {
            // The first node of each kind, as bytes, to read the structure offline.
            for (int want : {8, 0}) {
                for (size_t k = 0; k < used.size(); ++k) {
                    if (used[k]->layer != want) continue;
                    std::string text;
                    char b[12];
                    for (int i = 0; i < stride; i += 4) { uint32_t v; std::memcpy(&v, nodes[k].data() + stride + i, 4); std::snprintf(b, sizeof(b), "%08x ", v); text += b; }
                    diag::log("SCENE", "  node of a layer-%d renderer (index %d): %s", want, used[k]->index, text.c_str());
                    break;
                }
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
        // Every candidate layout must reproduce the layer of all renderers, so a few are enough for nodes
        // (matched by pointer); a flat array is matched by values alone and needs more.
        int eights = 0;
        for (const Ref& r : refs) eights += r.layer == 8;
        if (find_aos(refs, out)) return true;
        // A flat array is matched by values alone, so it needs enough renderers to be unambiguous.
        std::vector<Ref> indexed;
        for (const Ref& r : refs) if (r.index >= 0) indexed.push_back(r);
        return indexed.size() >= 10 && eights >= 3 && find_soa(indexed, out);
    }
}
