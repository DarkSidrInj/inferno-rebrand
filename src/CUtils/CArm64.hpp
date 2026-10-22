#pragma once
// Minimal AArch64 interpreter used to *read* the game's own accessor code, never to run it.
// Given an entry point (an il2cpp wrapper or a native icall), it steps through the
// instructions with a private register file, reads code and data from the game through
// mem_read, and records which addresses values were loaded from and stored to. Nothing is
// ever written to the game. Used to learn where a native field lives, e.g. which address
// RenderSettings::get_fogStartDistance loads s0 from.
//
// Covers what accessors are made of: ADR/ADRP, ADD/SUB, logical ops, MOVZ/MOVN/MOVK,
// bitfield moves, CSEL family, loads and stores (all addressing forms, pairs, literals),
// FMOV, B/BL/BR/BLR/RET (plus the pointer-auth forms), CBZ/CBNZ, TBZ/TBNZ and B.cond.
// A branch whose condition is not known (thread checks, values read from TLS) forks the
// machine. Paths are explored shortest first, so a 20-instruction accessor finishes long
// before an error-reporting path; each branch site may fork only a few times, so loops over
// unknown data cannot blow up. A call that cannot be followed (unknown or unresolved target,
// PLT slot the loader filled with something outside the game, too deep) is stepped over:
// the caller-saved registers become unknown, as after any call.

#include "CMemory.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace a64 {
    inline constexpr int kArg = 1;  // tag carried by an argument value through moves, loads and stores

    struct Reg {
        uint64_t value{};
        bool known{};
        uint64_t src{};  // game address the value was loaded from, 0 = not a load
        int tag{};
    };

    struct Store { uint64_t address{}; int size{}; uint64_t src{}; int tag{}; };

    // Stack and caller buffers live here, never in the game.
    inline constexpr uint64_t kLocalBase = 0x7ff000000000ULL;
    inline constexpr uint64_t kLocalSize = 0x100000;
    inline constexpr uint64_t kStackTop = kLocalBase + 0x80000;
    inline constexpr uint64_t kOutBuffer = kLocalBase + 0xC0000;  // for "out Color" style arguments
    inline bool local(uint64_t a) { return a >= kLocalBase && a < kLocalBase + kLocalSize; }

    struct Machine {
        Reg x[32];  // x[31] is SP; reads of register 31 as XZR are handled per instruction
        Reg v[32];
        bool n{}, z{}, c{}, vf{}, flags_known{};
        std::vector<uint64_t> calls;
        std::unordered_map<uint64_t, Reg> mem;  // local memory, keyed by exact address
        std::vector<Store> stores;
        uint64_t last_load{};  // last game address loaded from, and its size
        int last_load_size{};
        uint64_t pc{};
        int steps{};
        bool returned{};

        Machine() { x[31] = {kStackTop, true, 0, 0}; }
    };

    // Instruction fetch, cached per 4 KiB page for one exploration.
    struct CodeCache {
        std::unordered_map<uint64_t, std::vector<uint8_t>> pages;
        bool fetch(uint64_t pc, uint32_t& ins) {
            const uint64_t page = pc & ~uint64_t(0xFFF);
            auto it = pages.find(page);
            if (it == pages.end()) {
                std::vector<uint8_t> bytes(0x1000);
                if (!mem_read(page, bytes.data(), bytes.size())) bytes.clear();
                it = pages.emplace(page, std::move(bytes)).first;
            }
            if (it->second.empty()) return false;
            std::memcpy(&ins, it->second.data() + (pc - page), 4);
            return true;
        }
    };

    inline uint64_t sext(uint64_t v, int bits) {
        const uint64_t m = uint64_t(1) << (bits - 1);
        v &= (bits == 64) ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1);
        return (v ^ m) - m;
    }

    inline bool bitmask(int n, int imms, int immr, bool is64, uint64_t& out) {
        const int combined = (n << 6) | (~imms & 0x3F);
        int len = -1;
        for (int i = 6; i >= 0; --i) if (combined & (1 << i)) { len = i; break; }
        if (len < 1) return false;
        const int levels = (1 << len) - 1;
        const int s = imms & levels, r = immr & levels, esize = 1 << len;
        const uint64_t emask = esize == 64 ? ~uint64_t(0) : ((uint64_t(1) << esize) - 1);
        uint64_t welem = (s + 1 >= 64) ? ~uint64_t(0) : ((uint64_t(1) << (s + 1)) - 1);
        welem &= emask;
        const uint64_t rot = r ? (((welem >> r) | (welem << (esize - r))) & emask) : welem;
        uint64_t result = 0;
        for (int i = 0; i < 64; i += esize) result |= rot << i;
        out = is64 ? result : (result & 0xFFFFFFFFULL);
        return true;
    }

    struct Session {
        CodeCache code;
        int max_steps = 4000;      // per path
        long max_total = 60000;    // all paths together
        int max_forks = 2000;
        int max_site_forks = 6;    // per branch instruction
        int max_depth = 8;
        const char* why = "";
    };

    // After a call we could not follow: AAPCS64 caller-saved registers hold anything.
    inline void clobber(Machine& m) {
        for (int r = 0; r <= 18; ++r) m.x[r] = Reg{};
        for (int r = 0; r <= 7; ++r) m.v[r] = Reg{};
        for (int r = 16; r <= 31; ++r) m.v[r] = Reg{};
        m.flags_known = false;
    }

    // Result of reading `size` bytes at `addr` for the machine.
    inline Reg load(Machine& m, uint64_t addr, int size, bool sign, bool to64) {
        Reg r{};
        if (local(addr)) {
            const auto it = m.mem.find(addr);
            if (it != m.mem.end()) r = it->second;
            return r;
        }
        r.src = addr;
        m.last_load = addr;
        m.last_load_size = size;
        uint8_t raw[16]{};
        if (!plausible(addr) || !mem_read(addr, raw, size_t(size))) return r;
        uint64_t value = 0;
        std::memcpy(&value, raw, size_t(std::min(size, 8)));
        if (size < 8 && sign) value = sext(value, size * 8);
        if (!to64) value &= 0xFFFFFFFFULL;
        r.value = value;
        r.known = true;
        return r;
    }

    inline void store(Machine& m, uint64_t addr, int size, const Reg& value) {
        m.stores.push_back({addr, size, value.src, value.tag});
        if (local(addr)) m.mem[addr] = value;
    }

    inline bool cond(const Machine& m, int cc, bool& out) {
        if (cc >= 14) { out = true; return true; }
        if (!m.flags_known) return false;
        bool r = false;
        switch (cc >> 1) {
            case 0: r = m.z; break;
            case 1: r = m.c; break;
            case 2: r = m.n; break;
            case 3: r = m.vf; break;
            case 4: r = m.c && !m.z; break;
            case 5: r = m.n == m.vf; break;
            case 6: r = !m.z && m.n == m.vf; break;
        }
        out = (cc & 1) ? !r : r;
        return true;
    }

    inline void set_flags_sub(Machine& m, uint64_t a, uint64_t b, bool is64) {
        const uint64_t mask = is64 ? ~uint64_t(0) : 0xFFFFFFFFULL;
        const int top = is64 ? 63 : 31;
        a &= mask; b &= mask;
        const uint64_t r = (a - b) & mask;
        m.n = (r >> top) & 1; m.z = r == 0; m.c = a >= b;
        m.vf = (((a ^ b) & (a ^ r)) >> top) & 1;
        m.flags_known = true;
    }

    inline void set_flags_add(Machine& m, uint64_t a, uint64_t b, bool is64) {
        const uint64_t mask = is64 ? ~uint64_t(0) : 0xFFFFFFFFULL;
        const int top = is64 ? 63 : 31;
        a &= mask; b &= mask;
        const uint64_t r = (a + b) & mask;
        m.n = (r >> top) & 1; m.z = r == 0; m.c = r < a;
        m.vf = ((~(a ^ b) & (a ^ r)) >> top) & 1;
        m.flags_known = true;
    }

    // Outcome of one step.
    enum class Step { Next, Fork, Done, Fail };

    // Executes one instruction. On Fork, `other` receives the taken path and `m` continues
    // with the fall-through path.
    inline Step step(Session& s, Machine& m, Machine& other) {
        uint32_t ins = 0;
        if (++m.steps > s.max_steps) { s.why = "step budget"; return Step::Fail; }
        if (!s.code.fetch(m.pc, ins)) { s.why = "code unreadable"; return Step::Fail; }
        const uint64_t pc = m.pc;
        m.pc += 4;
        const int rd = int(ins & 31), rn = int((ins >> 5) & 31);
        const bool sf = (ins >> 31) & 1;
        const uint64_t mask = sf ? ~uint64_t(0) : 0xFFFFFFFFULL;
        auto xr = [&](int r) -> Reg { return r == 31 ? Reg{0, true, 0, 0} : m.x[r]; };  // XZR view
        auto setx = [&](int r, Reg value) { if (r != 31) { value.value &= mask; m.x[r] = value; } };
        auto known_value = [](uint64_t v) { return Reg{v, true, 0, 0}; };
        auto unknown_from = [](const Reg& r) { Reg o{}; o.tag = r.tag; o.src = r.src; return o; };

        // ---- branches -----------------------------------------------------------------
        if ((ins & 0x7C000000) == 0x14000000) {  // B / BL
            const uint64_t target = pc + sext(uint64_t(ins & 0x3FFFFFF) << 2, 28);
            if (ins & 0x80000000) {
                if (int(m.calls.size()) >= s.max_depth) { clobber(m); return Step::Next; }  // too deep: step over
                m.calls.push_back(m.pc);
                m.x[30] = known_value(m.pc);
            }
            m.pc = target;
            return Step::Next;
        }
        if ((ins & 0xFE1F0000) == 0xD61F0000) {  // BR/BLR/RET and the pointer-auth forms (BRAA, BLRAA, RETAA)
            const int opc = (ins >> 21) & 3;  // 0 BR, 1 BLR, 2 RET
            if (opc == 3) { s.why = "eret/drps"; return Step::Fail; }
            const Reg target = xr(rn);
            if (opc == 2) {
                if (m.calls.empty()) { m.returned = true; return Step::Done; }
                m.pc = m.calls.back();
                m.calls.pop_back();
                return Step::Next;
            }
            const bool followable = target.known && plausible(target.value);
            if (opc == 1) {  // BLR
                if (!followable || int(m.calls.size()) >= s.max_depth) { clobber(m); return Step::Next; }
                m.calls.push_back(m.pc);
                m.x[30] = known_value(m.pc);
                m.pc = target.value;
                return Step::Next;
            }
            if (!followable) {  // BR: a tail call or PLT jump we cannot follow
                if (m.calls.empty()) { s.why = "tail call to an unresolved target (icall slot empty?)"; return Step::Fail; }
                clobber(m);
                m.pc = m.calls.back();
                m.calls.pop_back();
                return Step::Next;
            }
            m.pc = target.value;
            return Step::Next;
        }
        if ((ins & 0x7E000000) == 0x34000000) {  // CBZ/CBNZ
            const Reg r = xr(rd);
            const uint64_t target = pc + sext(uint64_t((ins >> 5) & 0x7FFFF) << 2, 21);
            const bool nz = (ins >> 24) & 1;
            if (r.known) {
                const bool zero = (r.value & mask) == 0;
                if (zero != nz) m.pc = target;
                return Step::Next;
            }
            other = m; other.pc = target;
            return Step::Fork;
        }
        if ((ins & 0x7E000000) == 0x36000000) {  // TBZ/TBNZ
            const int bit = int(((ins >> 31) << 5) | ((ins >> 19) & 0x1F));
            const Reg r = xr(rd);
            const uint64_t target = pc + sext(uint64_t((ins >> 5) & 0x3FFF) << 2, 16);
            const bool nz = (ins >> 24) & 1;
            if (r.known) {
                const bool set = (r.value >> bit) & 1;
                if (set == nz) m.pc = target;
                return Step::Next;
            }
            other = m; other.pc = target;
            return Step::Fork;
        }
        if ((ins & 0xFF000010) == 0x54000000) {  // B.cond
            const uint64_t target = pc + sext(uint64_t((ins >> 5) & 0x7FFFF) << 2, 21);
            bool take = false;
            if (cond(m, int(ins & 0xF), take)) { if (take) m.pc = target; return Step::Next; }
            other = m; other.pc = target;
            return Step::Fork;
        }
        if ((ins & 0xFFFFF01F) == 0xD503201F) return Step::Next;  // HINT: NOP, PACIASP, AUTIASP, BTI...
        if ((ins & 0xFFF00000) == 0xD5300000) { setx(rd, Reg{}); return Step::Next; }  // MRS: unknown
        if ((ins & 0xFFFFF0FF) == 0xD503309F || (ins & 0xFFFFF0FF) == 0xD50330BF || (ins & 0xFFFFF0FF) == 0xD50330DF)
            return Step::Next;  // DSB/DMB/ISB
        if ((ins & 0xFFE0001F) == 0xD4200000 || (ins & 0xFFE0001F) == 0xD4000001) { s.why = "trap"; return Step::Fail; }

        // ---- PC-relative addressing --------------------------------------------------------
        if ((ins & 0x1F000000) == 0x10000000) {
            const uint64_t imm = sext((uint64_t((ins >> 5) & 0x7FFFF) << 2) | ((ins >> 29) & 3), 21);
            if (ins & 0x80000000) setx(rd, known_value((pc & ~uint64_t(0xFFF)) + (imm << 12)));  // ADRP
            else setx(rd, known_value(pc + imm));                                                // ADR
            return Step::Next;
        }

        // ---- add/sub immediate ----------------------------------------------------------
        if ((ins & 0x1F800000) == 0x11000000) {
            const bool sub = (ins >> 30) & 1, flags = (ins >> 29) & 1;
            const uint64_t imm = uint64_t((ins >> 10) & 0xFFF) << (((ins >> 22) & 1) ? 12 : 0);
            const Reg a = m.x[rn];  // Rn 31 = SP here
            Reg r = unknown_from(a);
            r.src = 0;
            if (a.known) { r.known = true; r.value = (sub ? a.value - imm : a.value + imm) & mask; }
            if (flags) {
                if (a.known) { if (sub) set_flags_sub(m, a.value, imm, sf); else set_flags_add(m, a.value, imm, sf); }
                else m.flags_known = false;
                setx(rd, r);  // Rd 31 = XZR
            } else if (rd == 31) {
                m.x[31] = r;  // SP
            } else setx(rd, r);
            return Step::Next;
        }

        // ---- logical immediate ----------------------------------------------------------
        if ((ins & 0x1F800000) == 0x12000000) {
            const int opc = (ins >> 29) & 3;
            uint64_t imm = 0;
            if (!bitmask((ins >> 22) & 1, (ins >> 10) & 0x3F, (ins >> 16) & 0x3F, sf, imm)) { setx(rd, Reg{}); return Step::Next; }
            const Reg a = xr(rn);
            Reg r = unknown_from(a);
            if (a.known) {
                r.known = true;
                switch (opc) {
                    case 0: case 3: r.value = a.value & imm; break;
                    case 1: r.value = a.value | imm; break;
                    case 2: r.value = a.value ^ imm; break;
                }
                r.value &= mask;
            }
            if (opc == 3) {
                if (a.known) { m.n = (r.value >> (sf ? 63 : 31)) & 1; m.z = r.value == 0; m.c = false; m.vf = false; m.flags_known = true; }
                else m.flags_known = false;
            }
            if (rd == 31 && opc != 3) m.x[31] = r; else setx(rd, r);
            return Step::Next;
        }

        // ---- move wide --------------------------------------------------------------------
        if ((ins & 0x1F800000) == 0x12800000) {
            const int opc = (ins >> 29) & 3, hw = (ins >> 21) & 3;
            const uint64_t imm = uint64_t((ins >> 5) & 0xFFFF) << (hw * 16);
            if (opc == 0) setx(rd, known_value(~imm));                        // MOVN
            else if (opc == 2) setx(rd, known_value(imm));                    // MOVZ
            else if (opc == 3) {                                              // MOVK
                Reg r = xr(rd);
                if (r.known) { r.value = (r.value & ~(uint64_t(0xFFFF) << (hw * 16))) | imm; setx(rd, r); }
            }
            return Step::Next;
        }

        // ---- bitfield (LSL/LSR/ASR/UXT*/SXT*/BFI...) -------------------------------------
        if ((ins & 0x1F800000) == 0x13000000) {
            const int opc = (ins >> 29) & 3, immr = (ins >> 16) & 0x3F, imms = (ins >> 10) & 0x3F;
            const int width = sf ? 64 : 32;
            const Reg a = xr(rn);
            Reg r = unknown_from(a);
            if (a.known && opc != 1) {
                uint64_t src = a.value & mask;
                uint64_t value;
                if (imms >= immr) {  // extract field [immr, imms]
                    const int len = imms - immr + 1;
                    value = (src >> immr) & (len >= 64 ? ~uint64_t(0) : ((uint64_t(1) << len) - 1));
                    if (opc == 0) value = sext(value, len);
                } else {  // insert low imms+1 bits at width - immr
                    const int len = imms + 1;
                    value = (src & ((uint64_t(1) << len) - 1)) << (width - immr);
                    if (opc == 0 && ((src >> imms) & 1)) value |= ~uint64_t(0) << (width - immr + len);
                }
                r.known = true;
                r.value = value & mask;
            }
            setx(rd, r);
            return Step::Next;
        }

        // ---- logical / add-sub shifted register ------------------------------------------
        if ((ins & 0x1F000000) == 0x0A000000 || (ins & 0x1F200000) == 0x0B000000) {
            const bool logical = (ins & 0x1F000000) == 0x0A000000;
            const int shift = (ins >> 22) & 3, amount = (ins >> 10) & 0x3F, rm = (ins >> 16) & 31;
            const Reg a = xr(rn), b = xr(rm);
            Reg r{};
            r.tag = a.tag ? a.tag : b.tag;
            // A plain register move keeps the provenance of its source.
            if (logical && rn == 31 && amount == 0 && ((ins >> 29) & 3) == 1 && !((ins >> 21) & 1)) {
                Reg moved = b;
                if (moved.known) moved.value &= mask;
                setx(rd, moved);
                return Step::Next;
            }
            if (a.known && b.known) {
                uint64_t bv = b.value & mask;
                switch (shift) {
                    case 0: bv <<= amount; break;
                    case 1: bv >>= amount; break;
                    case 2: bv = uint64_t(int64_t(sf ? bv : sext(bv, 32)) >> amount); break;
                    case 3: bv = sf ? ((bv >> amount) | (bv << ((64 - amount) & 63))) : (((bv >> amount) | (bv << ((32 - amount) & 31))) & 0xFFFFFFFFULL); break;
                }
                bv &= mask;
                if (logical && ((ins >> 21) & 1)) bv = ~bv & mask;  // BIC/ORN/EON/BICS
                r.known = true;
                if (logical) {
                    const int opc = (ins >> 29) & 3;
                    r.value = (opc == 0 || opc == 3) ? (a.value & bv) : opc == 1 ? (a.value | bv) : (a.value ^ bv);
                    r.value &= mask;
                    if (opc == 3) { m.n = (r.value >> (sf ? 63 : 31)) & 1; m.z = r.value == 0; m.c = m.vf = false; m.flags_known = true; }
                } else {
                    const bool sub = (ins >> 30) & 1;
                    r.value = (sub ? a.value - bv : a.value + bv) & mask;
                    if ((ins >> 29) & 1) { if (sub) set_flags_sub(m, a.value, bv, sf); else set_flags_add(m, a.value, bv, sf); }
                }
            } else if ((logical && ((ins >> 29) & 3) == 3) || (!logical && ((ins >> 29) & 1))) {
                m.flags_known = false;
            }
            setx(rd, r);
            return Step::Next;
        }

        // ---- conditional select (CSEL, CSINC/CSET, CSINV, CSNEG) ------------------------
        if ((ins & 0x1FE00000) == 0x1A800000) {
            const int rm = (ins >> 16) & 31;
            const bool op = (ins >> 30) & 1, o2 = (ins >> 10) & 1;
            bool pass = false;
            if (!cond(m, int((ins >> 12) & 0xF), pass)) { setx(rd, Reg{}); return Step::Next; }
            Reg r = pass ? xr(rn) : xr(rm);
            if (!pass && r.known) {
                if (!op && o2) r.value = r.value + 1;
                else if (op && !o2) r.value = ~r.value;
                else if (op && o2) r.value = 0 - r.value;
            }
            setx(rd, r);
            return Step::Next;
        }

        // ---- loads and stores ------------------------------------------------------------
        if ((ins & 0x0A000000) == 0x08000000) {
            const bool V = (ins >> 26) & 1;
            const int rt = rd;
            auto reg_for = [&](int r) -> Reg& { return V ? m.v[r] : m.x[r]; };
            auto value_of = [&](int r) -> Reg { return V ? m.v[r] : xr(r); };
            auto put = [&](int r, Reg value, bool to64) {
                if (V) { m.v[r] = value; return; }
                if (r == 31) return;
                if (value.known && !to64) value.value &= 0xFFFFFFFFULL;
                m.x[r] = value;
            };

            // Load/store pair
            if ((ins & 0x3A000000) == 0x28000000) {
                const int opc = (ins >> 30) & 3, mode = (ins >> 23) & 3;
                const bool L = (ins >> 22) & 1;
                const int rt2 = (ins >> 10) & 31;
                const int size = V ? (4 << opc) : (opc == 0 ? 4 : 8);
                const int64_t offset = int64_t(sext((ins >> 15) & 0x7F, 7)) * size;
                const Reg base = m.x[rn];
                if (!base.known) {
                    if (L) { put(rt, Reg{}, size == 8); put(rt2, Reg{}, size == 8); }
                    return Step::Next;
                }
                const uint64_t addr = mode == 1 ? base.value : base.value + uint64_t(offset);
                if (L) {
                    put(rt, load(m, addr, std::min(size, 8), opc == 1 && !V, size == 8 || V), size == 8 || V);
                    put(rt2, load(m, addr + size, std::min(size, 8), opc == 1 && !V, size == 8 || V), size == 8 || V);
                } else {
                    store(m, addr, size, value_of(rt));
                    store(m, addr + size, size, value_of(rt2));
                }
                if (mode == 1 || mode == 3) { Reg nb = base; nb.value = base.value + uint64_t(offset); m.x[rn] = nb; }
                return Step::Next;
            }

            // Load literal
            if ((ins & 0x3B000000) == 0x18000000) {
                const int opc = (ins >> 30) & 3;
                const uint64_t addr = pc + sext(uint64_t((ins >> 5) & 0x7FFFF) << 2, 21);
                if (!V && opc == 3) return Step::Next;  // PRFM
                const int size = V ? (opc == 0 ? 4 : opc == 1 ? 8 : 16) : (opc == 1 ? 8 : 4);
                put(rt, load(m, addr, std::min(size, 8), !V && opc == 2, V || opc != 0), V || opc != 0);
                return Step::Next;
            }

            // Single register: unsigned offset, unscaled / pre / post index, register offset
            if ((ins & 0x3B000000) == 0x39000000 || (ins & 0x3B200000) == 0x38000000 || (ins & 0x3B200C00) == 0x38200800) {
                const int size_bits = (ins >> 30) & 3, opc = (ins >> 22) & 3;
                int size;
                bool is_load, sign = false, to64 = true;
                if (V) {
                    size = (opc & 2) ? 16 : (1 << size_bits);
                    is_load = opc & 1;
                } else {
                    size = 1 << size_bits;
                    if (size_bits == 3 && opc == 2) return Step::Next;  // PRFM
                    is_load = opc != 0;
                    sign = opc >= 2;
                    to64 = size_bits == 3 || opc == 2;
                }
                const Reg base = m.x[rn];  // Rn 31 = SP
                uint64_t addr = 0;
                bool writeback = false;
                uint64_t new_base = 0;
                if ((ins & 0x3B000000) == 0x39000000) {
                    addr = base.value + uint64_t((ins >> 10) & 0xFFF) * uint64_t(size);
                } else if ((ins & 0x3B200C00) == 0x38200800) {
                    const int rm = (ins >> 16) & 31, option = (ins >> 13) & 7;
                    const Reg idx = xr(rm);
                    if (!idx.known) { if (is_load) put(rt, Reg{}, to64); return Step::Next; }
                    uint64_t off = idx.value;
                    if (option == 2) off &= 0xFFFFFFFFULL;
                    else if (option == 6) off = sext(off, 32);
                    if ((ins >> 12) & 1) { int sh = 0; while ((1 << sh) < size) ++sh; off <<= sh; }
                    addr = base.value + off;
                } else {
                    const uint64_t imm = sext((ins >> 12) & 0x1FF, 9);
                    const int kind = (ins >> 10) & 3;
                    addr = kind == 1 ? base.value : base.value + imm;
                    if (kind == 1 || kind == 3) { writeback = true; new_base = base.value + imm; }
                }
                if (!base.known) { if (is_load) put(rt, Reg{}, to64); return Step::Next; }
                if (is_load) put(rt, load(m, addr, std::min(size, 8), sign, to64 || V), to64);
                else store(m, addr, size, value_of(rt));
                if (writeback) { Reg nb = base; nb.value = new_base; m.x[rn] = nb; }
                (void)reg_for;
                return Step::Next;
            }
            // Exclusive / atomic / other: loads become unknown, stores are ignored.
            if ((ins >> 22) & 1) put(rt, Reg{}, true);
            return Step::Next;
        }

        // ---- floating point moves and data processing -------------------------------------
        if ((ins & 0x5F000000) == 0x1E000000) {
            if ((ins & 0x7F20FC00) == 0x1E200000) {  // FP <-> integer conversions, FMOV general
                const int opcode = (ins >> 16) & 7, rmode = (ins >> 19) & 3;
                if (rmode == 0 && opcode == 6) { Reg r = m.v[rn]; if (r.known) r.value &= mask; setx(rd, r); return Step::Next; }  // FMOV Xd/Wd, Vn
                if (rmode == 0 && opcode == 7) { Reg r = xr(rn); if (r.known) r.value &= mask; m.v[rd] = r; return Step::Next; } // FMOV Vd, Xn/Wn
                if (opcode <= 1 || opcode == 4 || opcode == 5) { Reg r{}; r.tag = m.v[rn].tag; setx(rd, r); return Step::Next; }  // to int
                Reg r{}; r.tag = xr(rn).tag; m.v[rd] = r;  // SCVTF/UCVTF
                return Step::Next;
            }
            if ((ins & 0xFF3FFC00) == 0x1E204000) { m.v[rd] = m.v[rn]; return Step::Next; }  // FMOV Vd, Vn
            if ((ins & 0xFF201FE0) == 0x1E201000) { m.v[rd] = Reg{}; return Step::Next; }    // FMOV Vd, #imm
            if ((ins & 0xFF20FC07) == 0x1E202000) { m.flags_known = false; return Step::Next; }  // FCMP/FCMPE, register or #0
            Reg r{}; r.tag = m.v[rn].tag; m.v[rd] = r;  // other FP data processing keeps the argument tag
            return Step::Next;
        }
        if ((ins & 0x0E000000) == 0x0E000000) {  // remaining SIMD/FP: MOV/DUP/MOVI and friends
            Reg r{}; r.tag = m.v[rn].tag; m.v[rd] = r;
            return Step::Next;
        }

        // Anything else: the destination register is no longer known.
        setx(rd, Reg{});
        return Step::Next;
    }

    // Explores from `start` until `goal` accepts a machine (checked after every store and at
    // the final return). The path with the fewest steps always runs next.
    inline bool explore(Session& s, const Machine& start, const std::function<bool(const Machine&)>& goal, Machine& found) {
        std::vector<Machine> pending{start};
        std::unordered_map<uint64_t, int> site_forks;
        int forks = 0;
        long total = 0;
        const char* last_failure = "";
        while (!pending.empty()) {
            size_t best = 0;
            for (size_t i = 1; i < pending.size(); ++i) if (pending[i].steps < pending[best].steps) best = i;
            Machine m = std::move(pending[best]);
            if (best + 1 != pending.size()) pending[best] = std::move(pending.back());
            pending.pop_back();
            for (;;) {
                if (++total > s.max_total) { s.why = "total step budget"; return false; }
                const uint64_t pc = m.pc;
                const size_t before = m.stores.size();
                Machine other;
                const Step result = step(s, m, other);
                if (result == Step::Fail) { last_failure = s.why; break; }
                if (result == Step::Fork) {
                    // Keep both sides only while this branch and the whole search are under
                    // budget; past that, follow the fall-through alone.
                    if (++site_forks[pc] <= s.max_site_forks && ++forks <= s.max_forks) {
                        pending.push_back(std::move(other));
                        pending.push_back(std::move(m));
                        break;
                    }
                    continue;
                }
                if ((m.stores.size() != before || result == Step::Done) && goal(m)) { found = m; return true; }
                if (result == Step::Done) { last_failure = "returned without reaching the goal"; break; }
            }
        }
        s.why = *last_failure ? last_failure : "no path reached the goal";
        return false;
    }
}
