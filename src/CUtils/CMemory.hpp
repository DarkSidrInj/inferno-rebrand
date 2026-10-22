#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include "../CProtection/COxorany.hpp"
#include "CDiagnostics.hpp"

namespace proc {
    inline int pid = -1;
    inline uint64_t lib = 0;
    inline int fd = -1;
    inline int pm = -1;
}

namespace sys {
    #if defined(__aarch64__)
    static constexpr long SYS_openat = 56;
    static constexpr long SYS_close = 57;
    static constexpr long SYS_read = 63;
    static constexpr long SYS_getdents64 = 61;
    static constexpr long SYS_vm_read = 270;
    static constexpr long SYS_vm_write = 271;
    #elif defined(__x86_64__)
    static constexpr long SYS_openat = 257;
    static constexpr long SYS_close = 3;
    static constexpr long SYS_read = 0;
    static constexpr long SYS_getdents64 = 217;
    static constexpr long SYS_vm_read = 310;
    static constexpr long SYS_vm_write = 311;
    #endif

    #ifndef AT_FDCWD
    #define AT_FDCWD -100
    #endif

    static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
        long r;
        #if defined(__aarch64__)
        register long x8 __asm__("x8") = n;
        register long x0 __asm__("x0") = a1;
        register long x1 __asm__("x1") = a2;
        register long x2 __asm__("x2") = a3;
        register long x3 __asm__("x3") = a4;
        register long x4 __asm__("x4") = a5;
        register long x5 __asm__("x5") = a6;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
        r = x0;
        #elif defined(__x86_64__)
        register long rax asm("rax") = n;
        register long rdi asm("rdi") = a1;
        register long rsi asm("rsi") = a2;
        register long rdx asm("rdx") = a3;
        register long r10 asm("r10") = a4;
        register long r8 asm("r8") = a5;
        register long r9 asm("r9") = a6;
        asm volatile("syscall" : "=a"(r) : "a"(rax), "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
        #else
        r = syscall(n, a1, a2, a3, a4, a5, a6);
        #endif
        return r;
    }

    static inline int openat(int fd, const char* p, int f) {
        return static_cast<int>(sc6(SYS_openat, fd, reinterpret_cast<long>(p), f, 0, 0, 0));
    }

    static inline int close(int fd) {
        return static_cast<int>(sc6(SYS_close, fd, 0, 0, 0, 0, 0));
    }

    static inline long read(int fd, void* b, size_t c) {
        return sc6(SYS_read, fd, reinterpret_cast<long>(b), c, 0, 0, 0);
    }

    static inline long getdents64(int fd, void* d, size_t c) {
        return sc6(SYS_getdents64, fd, reinterpret_cast<long>(d), c, 0, 0, 0);
    }

    static inline long vm_read(int pid, struct iovec* l, unsigned long lc, struct iovec* r, unsigned long rc) {
        return sc6(SYS_vm_read, pid, reinterpret_cast<long>(l), lc, reinterpret_cast<long>(r), rc, 0);
    }

    static inline long vm_write(int pid, struct iovec* l, unsigned long lc, struct iovec* r, unsigned long rc) {
        return sc6(SYS_vm_write, pid, reinterpret_cast<long>(l), lc, reinterpret_cast<long>(r), rc, 0);
    }
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

inline bool addr_valid(uint64_t a) {
    return a >= 0x1000;
}

inline bool mem_read(uint64_t a, void* b, size_t l) {
    if (proc::pid < 0 || a == 0 || l == 0 || b == nullptr) {
        return false;
    }

    if (a < 0x1000) {
        return false;
    }

    struct iovec loc[1];
    struct iovec rem[1];

    loc[0].iov_base = b;
    loc[0].iov_len = l;
    rem[0].iov_base = reinterpret_cast<void*>(a);
    rem[0].iov_len = l;

    ssize_t result = sys::vm_read(proc::pid, loc, 1, rem, 1);

    if (result != static_cast<long>(l) && proc::pm >= 0) {
        result = ::pread(proc::pm, b, l, static_cast<off_t>(a));
    }

    if (result != static_cast<long>(l)) {
        static long long last_log = 0;
        if (diag::every(last_log, 10000))
            diag::log("ERROR", "remote read failed pid=%d addr=0x%llx size=%zu result=%lld errno=%d",
                      proc::pid, (unsigned long long)a, l, (long long)result, errno);
        memset(b, 0, l);
        return false;
    }

    return true;
}

inline bool mem_write(uint64_t a, const void* b, size_t l) {
    if (proc::pid < 0 || a == 0 || l == 0 || b == nullptr) return false;

    struct iovec loc[1];
    struct iovec rem[1];

    loc[0].iov_base = const_cast<void*>(b);
    loc[0].iov_len = l;
    rem[0].iov_base = reinterpret_cast<void*>(a);
    rem[0].iov_len = l;

    long result = sys::vm_write(proc::pid, loc, 1, rem, 1);
    if (result != static_cast<long>(l) && proc::pm >= 0)
        result = ::pwrite(proc::pm, b, l, static_cast<off_t>(a));
    return result == static_cast<long>(l);
}

template<typename T>
inline T rpm(uint64_t a) {
    T v{};
    mem_read(a, &v, sizeof(T));
    return v;
}

template<typename T>
inline bool wpm(uint64_t a, const T& d) {
    return mem_write(a, &d, sizeof(T));
}

inline int get_pid() {
    int pfd = sys::openat(AT_FDCWD, oxorany("/proc"), O_RDONLY | O_DIRECTORY);
    if (pfd < 0) return -1;

    char buf[4096];

    while (true) {
        long nr = sys::getdents64(pfd, buf, sizeof(buf));
        if (nr <= 0) break;

        char* p = buf;
        while (p < buf + nr) {
            auto* e = reinterpret_cast<linux_dirent64*>(p);

            if (e->d_type == 4) {
                char* n = e->d_name;
                bool in = true;
                for (char* c = n; *c; c++) {
                    if (*c < '0' || *c > '9') { in = false; break; }
                }

                if (in && *n != '\0') {
                    int pid = 0;
                    for (char* c = n; *c; c++) pid = pid * 10 + (*c - '0');

                    char cp[256];
                    int ps = 0;

                    const char* ps_ = oxorany("/proc/");
                    while (*ps_) cp[ps++] = *ps_++;
                    for (char* c = n; *c; c++) cp[ps++] = *c;
                    const char* cs = oxorany("/cmdline");
                    while (*cs) cp[ps++] = *cs++;
                    cp[ps] = '\0';

                    int cfd = sys::openat(AT_FDCWD, cp, O_RDONLY);
                    if (cfd >= 0) {
                        char cmd[256] = {0};
                        long b = sys::read(cfd, cmd, sizeof(cmd) - 1);
                        sys::close(cfd);

                        if (b > 0) {
                            cmd[b] = '\0';

                            const char* tg = oxorany("com.axlebolt.standoff2");
                            int tl = 0;
                            while (tg[tl]) tl++;

                            bool exact = b >= tl;
                            for (int j = 0; exact && j < tl; ++j) exact = cmd[j] == tg[j];
                            exact = exact && cmd[tl] == '\0';
                            if (exact) {
                                sys::close(pfd);
                                return pid;
                            }
                        }
                    }
                }
            }

            p += e->d_reclen;
        }
    }

    sys::close(pfd);
    return -1;
}

inline uint64_t get_lib() {
    if (proc::pid <= 0) return 0;
    static long long last_report = 0;
    const bool report = diag::every(last_report, 2000);
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
    FILE* file = std::fopen(path, "r");
    if (!file) {
        diag::log("ERROR", "cannot open %s for libunity discovery errno=%d", path, errno);
        return 0;
    }

    struct Segment { uint64_t begin, end, file_offset; char perms[5]; };
    std::vector<Segment> segments;
    std::vector<uint64_t> readable_starts;
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0, file_offset = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s %llx", &begin, &end, perms, &file_offset) != 4 || end <= begin)
            continue;
        if (perms[0] == 'r') readable_starts.push_back(uint64_t(begin));
        if (!std::strstr(line, "libunity.so")) continue;
        Segment segment{uint64_t(begin), uint64_t(end), uint64_t(file_offset), {}};
        std::memcpy(segment.perms, perms, sizeof(segment.perms));
        segments.push_back(segment);
        if (report)
            diag::log("MAP", "libunity segment=%llx-%llx perms=%s fileOffset=0x%llx",
                      begin, end, perms, file_offset);
    }
    std::fclose(file);

    constexpr uint32_t kElfMagic = 0x464c457f;
    constexpr uint64_t kSignatureA = 0x6327AD0;
    constexpr uint64_t kSignatureB = 0x6327C50;
    constexpr uint32_t kExpectedA = 0x14001bce;
    constexpr uint32_t kExpectedB = 0x1400eca0;
    for (const Segment& segment : segments) {
        uint32_t magic = 0;
        if (!mem_read(segment.begin, &magic, sizeof(magic)) || magic != kElfMagic) continue;
        uint32_t sig_a = 0, sig_b = 0;
        const bool read_a = mem_read(segment.begin + kSignatureA, &sig_a, sizeof(sig_a));
        const bool read_b = mem_read(segment.begin + kSignatureB, &sig_b, sizeof(sig_b));
        if (report)
            diag::log("INFO", "ELF base candidate=0x%llx signatures=%08x/%08x expected=%08x/%08x",
                      (unsigned long long)segment.begin, sig_a, sig_b, kExpectedA, kExpectedB);
        if (read_a && read_b && sig_a == kExpectedA && sig_b == kExpectedB) {
            diag::log("INFO", "validated Standoff2 1.0.0 libunity load bias=0x%llx",
                      (unsigned long long)segment.begin);
            return segment.begin;
        }
    }

    // Pathnames differ across extracted-library and base.apk deployments. If the
    // pathname route missed, inspect every readable mapping start for an ELF header;
    // exact code signatures keep this exhaustive fallback unambiguous.
    for (uint64_t candidate : readable_starts) {
        uint32_t magic = 0;
        if (!mem_read(candidate, &magic, sizeof(magic)) || magic != kElfMagic) continue;
        uint32_t sig_a = 0, sig_b = 0;
        if (mem_read(candidate + kSignatureA, &sig_a, sizeof(sig_a)) &&
            mem_read(candidate + kSignatureB, &sig_b, sizeof(sig_b)) &&
            sig_a == kExpectedA && sig_b == kExpectedB) {
            diag::log("INFO", "validated anonymous/APK libunity ELF base=0x%llx",
                      (unsigned long long)candidate);
            return candidate;
        }
    }

    // Some Android linkers omit the ELF header page from the visible pathname mapping.
    // Try both conventional and APK-relative calculations, but still require both exact
    // 1.0.0 ARM64 instruction signatures before accepting a candidate.
    std::vector<uint64_t> candidates;
    for (const Segment& segment : segments) {
        candidates.push_back(segment.begin);
        if (segment.begin >= segment.file_offset) candidates.push_back(segment.begin - segment.file_offset);
    }
    for (uint64_t candidate : candidates) {
        uint32_t sig_a = 0, sig_b = 0;
        if (mem_read(candidate + kSignatureA, &sig_a, sizeof(sig_a)) &&
            mem_read(candidate + kSignatureB, &sig_b, sizeof(sig_b)) &&
            sig_a == kExpectedA && sig_b == kExpectedB) {
            diag::log("INFO", "validated fallback libunity load bias=0x%llx", (unsigned long long)candidate);
            return candidate;
        }
    }
    if (report)
        diag::log("ERROR", "no mapping matches the LemmingRMT 1.0.0 ARM64 signatures; namedSegments=%zu readableStarts=%zu",
                  segments.size(), readable_starts.size());
    return 0;
}

inline void init_mem() {
    if (proc::pid <= 0) return;
    if (proc::pm >= 0) sys::close(proc::pm);
    proc::pm = -1;

    char mp[64];
    int ps = 0;

    const char* ps_ = oxorany("/proc/");
    while (*ps_) mp[ps++] = *ps_++;

    int pid = proc::pid;
    char pds[16];
    int pl = 0;
    int tm = pid;
    while (tm > 0) { pds[pl++] = '0' + (tm % 10); tm /= 10; }
    for (int i = pl - 1; i >= 0; i--) mp[ps++] = pds[i];

    const char* ms = oxorany("/mem");
    while (*ms) mp[ps++] = *ms++;
    mp[ps] = '\0';

    proc::pm = sys::openat(AT_FDCWD, mp, O_RDONLY);
    diag::log(proc::pm >= 0 ? "INFO" : "WARN", "remote memory fallback %s path=%s fd=%d",
              proc::pm >= 0 ? "ready" : "unavailable", mp, proc::pm);
}

namespace game {
    inline int tick = 0;
    inline int fail_count = 0;
    inline int check_tick = 0;

    inline bool valid() {
        if (proc::pid <= 0) {
            proc::pid = get_pid();
            if (proc::pid <= 0) {
                static long long last_pid_log = 0;
                if (diag::every(last_pid_log, 2000)) diag::log("WAIT", "process com.axlebolt.standoff2 not found");
                return false;
            }
            diag::log("INFO", "game process found pid=%d", proc::pid);
            init_mem();
        }

        if (proc::lib == 0) {
            if (proc::pm < 0) init_mem();
            proc::lib = get_lib();
            if (proc::lib != 0) {
                diag::log("INFO", "libunity.so base=0x%llx", (unsigned long long)proc::lib);
            } else {
                static long long last_lib_log = 0;
                if (diag::every(last_lib_log, 2000)) diag::log("WAIT", "libunity.so mapping not ready for pid=%d", proc::pid);
            }
            return false;
        }

        tick++;
        if (tick >= 60) {
            int p = get_pid();
            if (p != proc::pid) {
                if (proc::pm >= 0) { sys::close(proc::pm); proc::pm = -1; }
                proc::pid = p;
                proc::lib = 0;
                tick = 0;
                diag::log("WARN", "game process changed old/new pid=%d; resetting library discovery", p);
                return false;
            }
            tick = 0;
        }

        return true;
    }

    inline void check_lib(uint64_t pm) {
        check_tick++;
        if (check_tick >= 60) {
            if (pm == 0) {
                fail_count++;
                if (fail_count >= 5) {
                    uint64_t lib = get_lib();
                    if (lib != 0 && lib != proc::lib) {
                        proc::lib = lib;
                        if (proc::pm >= 0) { sys::close(proc::pm); proc::pm = -1; }
                        init_mem();
                    }
                    fail_count = 3;
                }
            } else {
                fail_count = 0;
            }
            check_tick = 0;
        }
    }

    inline void init() {
        proc::pid = get_pid();
        diag::log(proc::pid > 0 ? "INFO" : "WAIT", "initial process lookup pid=%d", proc::pid);
        if (proc::pid > 0) {
            init_mem();
            proc::lib = get_lib();
            if (proc::lib != 0) {
                diag::log("INFO", "initial libunity.so base=0x%llx", (unsigned long long)proc::lib);
            }
        }
    }
}
