#pragma once
// Runs a function of the game on one of its own threads, the way a debugger's "call" does: the
// thread is stopped with ptrace while it waits in a system call, every register is saved, it is
// pointed at the function with the arguments in place and a return address in the first page
// (never mapped), and let run until it faults there; then every register goes back and the
// system call restarts as if nothing happened.
// Only stops inside a system call are used: the thread is then between two pieces of its own
// work, not halfway through one (no lock of its own held mid-update). Root, or the game being
// our child, is needed; the game sees a tracer for the length of the call.

#include "CDiagnostics.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <elf.h>
#include <initializer_list>
#include <signal.h>
#include <string>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

namespace remote {
    inline constexpr uint64_t kReturnTrap = 0x40;

#if defined(__aarch64__)
    struct Regs { uint64_t x[31]; uint64_t sp, pc, pstate; };
    inline uint64_t& pc_of(Regs& r) { return r.pc; }
    inline uint64_t result_of(const Regs& r) { return r.x[0]; }
#elif defined(__x86_64__)
    using Regs = user_regs_struct;
    inline uint64_t& pc_of(Regs& r) { return reinterpret_cast<uint64_t&>(r.rip); }
    inline uint64_t result_of(const Regs& r) { return r.rax; }
#endif

    struct State { Regs gp{}; uint8_t fp[1024]{}; size_t fp_len{}; };

    inline bool get_state(int tid, State& s) {
        iovec gp{&s.gp, sizeof(s.gp)}, fp{s.fp, sizeof(s.fp)};
        if (ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS), &gp) != 0) return false;
        if (ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_PRFPREG), &fp) != 0) return false;
        s.fp_len = fp.iov_len;
        return true;
    }

    inline bool set_regs(int tid, const Regs& r) {
        iovec gp{const_cast<Regs*>(&r), sizeof(r)};
        return ptrace(PTRACE_SETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS), &gp) == 0;
    }

    inline bool set_state(int tid, const State& s) {
        iovec fp{const_cast<uint8_t*>(s.fp), s.fp_len};
        return set_regs(tid, s.gp) && ptrace(PTRACE_SETREGSET, tid, reinterpret_cast<void*>(NT_PRFPREG), &fp) == 0;
    }

    // Stopped where the kernel restarts an interrupted system call on resume.
    inline bool in_syscall(int tid, Regs& r) {
#if defined(__aarch64__)
        // arm64 rewinds pc onto the svc (and x0 to its first argument) before the stop.
        errno = 0;
        const long word = ptrace(PTRACE_PEEKTEXT, tid, reinterpret_cast<void*>(r.pc & ~uint64_t(7)), nullptr);
        if (errno) return false;
        const uint32_t insn = uint32_t(uint64_t(word) >> ((r.pc & 4) ? 32 : 0));
        return insn == 0xD4000001u;
#elif defined(__x86_64__)
        // x86 rewinds after the stop, from orig_rax and the -ERESTART* code in rax.
        const long long ax = static_cast<long long>(r.rax);
        return static_cast<long long>(r.orig_rax) >= 0 && (ax == -512 || ax == -513 || ax == -514 || ax == -516);
#endif
    }

    inline int find_thread(int pid, const char* name) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/task", pid);
        DIR* d = opendir(path);
        if (!d) return -1;
        int found = -1;
        while (dirent* e = readdir(d)) {
            const int tid = std::atoi(e->d_name);
            if (tid <= 0) continue;
            char cp[96], comm[32]{};
            std::snprintf(cp, sizeof(cp), "/proc/%d/task/%d/comm", pid, tid);
            if (FILE* f = std::fopen(cp, "r")) {
                if (std::fgets(comm, sizeof(comm), f)) {
                    comm[std::strcspn(comm, "\n")] = 0;
                    if (!std::strcmp(comm, name)) found = tid;
                }
                std::fclose(f);
            }
            if (found > 0) break;
        }
        closedir(d);
        return found;
    }

    // Next stop of tid within timeout_ms; false on timeout or when the thread is gone.
    inline bool next_stop(int tid, int& status, long long timeout_ms) {
        const long long until = diag::now_ms() + timeout_ms;
        for (;;) {
            const int r = waitpid(tid, &status, __WALL | WNOHANG);
            if (r == tid) return WIFSTOPPED(status);
            if (r < 0 || diag::now_ms() > until) return false;
            usleep(200);
        }
    }

    inline bool event_stop(int status) { return (status >> 16) == PTRACE_EVENT_STOP; }

    // Interrupts tid and waits for the interrupt; signals that come first go on to the thread.
    inline bool interrupt(int tid) {
        if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0) return false;
        for (int status = 0;;) {
            if (!next_stop(tid, status, 1000)) return false;
            if (event_stop(status)) return true;
            ptrace(PTRACE_CONT, tid, nullptr, reinterpret_cast<void*>(long(WSTOPSIG(status))));
        }
    }

    // One call of a batch. Integer arguments go to x0.. (rdi..), floats to s0.. (xmm0..), and `blob` is copied onto the
    // thread's stack with its address passed as integer argument `blob_arg` (a struct by reference, e.g. a Color).
    struct Call {
        uint64_t fn = 0;
        uint64_t x[6]{};
        int nx = 0;
        float f[4]{};
        int nf = 0;
        uint8_t blob[64]{};
        int blob_len = 0, blob_arg = 0;
        uint64_t ret = 0;
        bool done = false;
    };

    // Where floating-point argument k lives in the saved FP/SIMD state.
    inline uint8_t* float_slot(State& s, int k) {
#if defined(__aarch64__)
        return s.fp + 16 * k;           // user_fpsimd_state: vregs[32], 16 bytes each
#else
        return s.fp + 160 + 16 * k;     // user_fpregs_struct: xmm_space at 160
#endif
    }

    inline bool poke(int tid, uint64_t at, const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t o = 0; o < len; o += 8) {
            uint64_t word = 0;
            std::memcpy(&word, p + o, len - o < 8 ? len - o : 8);
            if (ptrace(PTRACE_POKEDATA, tid, reinterpret_cast<void*>(at + o), reinterpret_cast<void*>(word)) != 0) return false;
        }
        return true;
    }

    // One call on a thread that is stopped in `saved` (a system call). True when the function returned; the caller puts
    // `saved` back and lets the thread go.
    inline bool run_call(int tid, const State& saved, Call& c, std::string& why) {
        Regs r = saved.gp;
        State st = saved;
        for (int k = 0; k < c.nf; ++k) std::memcpy(float_slot(st, k), &c.f[k], 4);
        uint64_t blob_at = 0;
#if defined(__aarch64__)
        for (int k = 0; k < c.nx; ++k) r.x[k] = c.x[k];
        r.x[30] = kReturnTrap;
        r.sp = (r.sp - 0x2000) & ~uint64_t(0xF);
        r.pc = c.fn;
        r.pstate &= ~(uint64_t(3) << 10);  // BTYPE: the jump is not a branch a BTI landing pad expects
        blob_at = r.sp + 0x100;
        if (c.blob_len > 0) r.x[c.blob_arg] = blob_at;
#elif defined(__x86_64__)
        unsigned long long* slots[6] = {&r.rdi, &r.rsi, &r.rdx, &r.rcx, &r.r8, &r.r9};
        for (int k = 0; k < c.nx; ++k) *slots[k] = c.x[k];
        r.rsp = ((r.rsp - 0x2000) & ~0xFULL) - 8;
        if (ptrace(PTRACE_POKEDATA, tid, reinterpret_cast<void*>(r.rsp), reinterpret_cast<void*>(kReturnTrap)) != 0) {
            why = "cannot write the return address";
            return false;
        }
        blob_at = r.rsp + 0x108;
        if (c.blob_len > 0) *slots[c.blob_arg] = blob_at;
        r.rip = c.fn;
        r.rax = 0;
        r.orig_rax = static_cast<unsigned long long>(-1);  // no system call restart into our call
#endif
        if (c.blob_len > 0 && !poke(tid, blob_at, c.blob, size_t(c.blob_len))) { why = "cannot write the argument"; return false; }
        st.gp = r;
        if (!set_state(tid, st) || ptrace(PTRACE_CONT, tid, nullptr, nullptr) != 0) { why = "cannot start the call"; return false; }

        const long long until = diag::now_ms() + 1500;
        for (int status = 0;;) {
            if (!next_stop(tid, status, until - diag::now_ms())) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) { why = "thread exited during the call"; return false; }
                if (!interrupt(tid)) { why = "call timed out and the thread did not stop"; return false; }
                why = "call timed out";
                return false;
            }
            if (event_stop(status)) { ptrace(PTRACE_CONT, tid, nullptr, nullptr); continue; }
            const int sig = WSTOPSIG(status);
            Regs now{};
            iovec gp{&now, sizeof(now)};
            ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS), &gp);
            if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP) && pc_of(now) == kReturnTrap) {
                c.ret = result_of(now);
                c.done = true;
                return true;
            }
            if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP || sig == SIGABRT) {
                char text[96];
                std::snprintf(text, sizeof(text), "call faulted (signal %d at 0x%llx)", sig, (unsigned long long)pc_of(now));
                why = text;
                return false;
            }
            ptrace(PTRACE_CONT, tid, nullptr, reinterpret_cast<void*>(long(sig)));
        }
    }

    // `count` calls one after the other on thread `tid`, inside a single stop: the thread is caught waiting in a system call
    // (up to `attempts` interrupts), every call runs there, then every register goes back and the system call restarts as if
    // nothing happened. Stops at the first call that fails (its `done` stays false).
    inline bool call_batch(int tid, Call* calls, int count, std::string& why, int attempts = 60) {
        if (count <= 0) return true;
        for (int i = 0; i < count; ++i)
            if (calls[i].nx > 6 || calls[i].nf > 4 || calls[i].blob_len > int(sizeof(calls[i].blob)) ||
                (calls[i].blob_len > 0 && (calls[i].blob_arg < 0 || calls[i].blob_arg >= 6))) { why = "too many arguments"; return false; }
        if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0) { why = "PTRACE_SEIZE errno " + std::to_string(errno); return false; }
        auto leave = [tid](const char* reason, std::string& w) {
            w = reason;
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            return false;
        };

        State saved{};
        bool caught = false;
        for (int i = 0; i < attempts && !caught; ++i) {
            if (!interrupt(tid)) return leave("thread did not stop", why);
            if (!get_state(tid, saved)) return leave("cannot read registers", why);
            if (in_syscall(tid, saved.gp)) { caught = true; break; }
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
            usleep(5000);
        }
        if (!caught) return leave("never caught waiting in a system call", why);

        bool all = true;
        for (int i = 0; i < count && all; ++i) {
            all = run_call(tid, saved, calls[i], why);
            if (!all && (why == "thread exited during the call")) return false;
        }
        // Back to the system call it was waiting in; a pending fault (if any) is dropped.
        const bool restored = set_state(tid, saved);
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        if (!restored) { why = all ? "registers not restored" : why + "; registers not restored"; return false; }
        return all;
    }

    // fn(args...) on thread `tid` of `pid` (up to 6 integer arguments).
    inline bool call(int tid, uint64_t fn, std::initializer_list<uint64_t> args, uint64_t& ret, std::string& why,
                     int attempts = 60) {
        if (args.size() > 6) { why = "too many arguments"; return false; }
        Call c;
        c.fn = fn;
        for (uint64_t a : args) c.x[c.nx++] = a;
        if (!call_batch(tid, &c, 1, why, attempts)) return false;
        ret = c.ret;
        return true;
    }

    inline bool call_on(int pid, const char* thread, uint64_t fn, std::initializer_list<uint64_t> args, uint64_t& ret,
                        std::string& why) {
        const int tid = find_thread(pid, thread);
        if (tid <= 0) { why = std::string("no thread ") + thread; return false; }
        return call(tid, fn, args, ret, why);
    }
}
