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

    // fn(args...) on thread `tid` of `pid` (up to 6 integer arguments). Waits up to `attempts`
    // interrupts for one that lands in a system call.
    inline bool call(int tid, uint64_t fn, std::initializer_list<uint64_t> args, uint64_t& ret, std::string& why,
                     int attempts = 60) {
        if (args.size() > 6) { why = "too many arguments"; return false; }
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

        Regs r = saved.gp;
        const uint64_t* a = args.begin();
#if defined(__aarch64__)
        for (size_t k = 0; k < args.size(); ++k) r.x[k] = a[k];
        r.x[30] = kReturnTrap;
        r.sp = (r.sp - 0x2000) & ~uint64_t(0xF);
        r.pc = fn;
        r.pstate &= ~(uint64_t(3) << 10);  // BTYPE: the jump is not a branch a BTI landing pad expects
#elif defined(__x86_64__)
        unsigned long long* slots[6] = {&r.rdi, &r.rsi, &r.rdx, &r.rcx, &r.r8, &r.r9};
        for (size_t k = 0; k < args.size(); ++k) *slots[k] = a[k];
        r.rsp = ((r.rsp - 0x2000) & ~0xFULL) - 8;
        if (ptrace(PTRACE_POKEDATA, tid, reinterpret_cast<void*>(r.rsp), reinterpret_cast<void*>(kReturnTrap)) != 0) {
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
            return leave("cannot write the return address", why);
        }
        r.rip = fn;
        r.rax = 0;
        r.orig_rax = static_cast<unsigned long long>(-1);  // no system call restart into our call
#endif
        if (!set_regs(tid, r) || ptrace(PTRACE_CONT, tid, nullptr, nullptr) != 0) {
            set_state(tid, saved);
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
            return leave("cannot start the call", why);
        }

        bool done = false;
        const long long until = diag::now_ms() + 1500;
        for (int status = 0;;) {
            if (!next_stop(tid, status, until - diag::now_ms())) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) { why = "thread exited during the call"; return false; }
                if (!interrupt(tid)) return leave("call timed out and the thread did not stop", why);
                why = "call timed out";
                break;
            }
            if (event_stop(status)) { ptrace(PTRACE_CONT, tid, nullptr, nullptr); continue; }
            const int sig = WSTOPSIG(status);
            Regs now{};
            iovec gp{&now, sizeof(now)};
            ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS), &gp);
            if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP) && pc_of(now) == kReturnTrap) {
                ret = result_of(now);
                done = true;
                break;
            }
            if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP || sig == SIGABRT) {
                char text[96];
                std::snprintf(text, sizeof(text), "call faulted (signal %d at 0x%llx)", sig, (unsigned long long)pc_of(now));
                why = text;
                break;
            }
            ptrace(PTRACE_CONT, tid, nullptr, reinterpret_cast<void*>(long(sig)));
        }
        // Back to the system call it was waiting in; the pending fault (if any) is dropped.
        const bool restored = set_state(tid, saved);
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        if (!restored) { why = "registers not restored"; return false; }
        return done;
    }

    inline bool call_on(int pid, const char* thread, uint64_t fn, std::initializer_list<uint64_t> args, uint64_t& ret,
                        std::string& why) {
        const int tid = find_thread(pid, thread);
        if (tid <= 0) { why = std::string("no thread ") + thread; return false; }
        return call(tid, fn, args, ret, why);
    }
}
