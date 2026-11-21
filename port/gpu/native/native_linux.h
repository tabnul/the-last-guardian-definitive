// Linux side of the few Windows calls the new engine uses. Empty on Windows: there the real Windows
// calls are used and nothing here is seen.
#pragma once
#if !defined(_WIN32)
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

// GCC on Linux has no __declspec; the engine only uses __declspec(noinline).
#ifndef __declspec
#define __declspec(attribute) __attribute__((attribute))
#endif

// The guarded reads resume at runtime_fault_recover: on Linux the loader's signal handler (src/probe.c) unblocks
// the signal and calls siglongjmp on it, so the buffer is filled with sigsetjmp (as in the shadPS4 tree on Linux).
// A macro, because sigsetjmp must run in the frame of the guarded function.
#ifndef tlg_setjmp
#define tlg_setjmp(buffer) sigsetjmp(buffer, 0)
#endif

/// Kernel thread id, the Linux counterpart of the Windows thread id (only compared for equality).
inline unsigned long GetCurrentThreadId() {
    return static_cast<unsigned long>(gettid());
}

inline int SwitchToThread() {
    std::this_thread::yield();
    return 1;
}

/// The census clock: nanoseconds of the steady clock, frequency 10^9 per second.
union LARGE_INTEGER {
    long long QuadPart;
};
inline int QueryPerformanceCounter(LARGE_INTEGER* value) {
    value->QuadPart = static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    return 1;
}
inline int QueryPerformanceFrequency(LARGE_INTEGER* value) {
    value->QuadPart = 1000000000ll;
    return 1;
}

/// Writes one 8 byte slot that may sit in a read only page (a table the loader protects after relocation), the Linux
/// counterpart of VirtualProtect(writable) + write + VirtualProtect(old): the page's own protection is read from
/// /proc/self/maps, write access is added for the write and the old protection is put back after.
/// False (nothing written) when the page is not in the map or the protection cannot be changed.
inline bool WriteProtectedSlot(void* slot, std::uint64_t value) {
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    const long page_size = sysconf(_SC_PAGESIZE);
    if ((address & 7) != 0 || page_size <= 0) {
        return false;
    }
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) {
        return false;
    }
    int prot = -1;
    char line[512];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0;
        char perms[5] = {};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) == 3 && address >= start && address < end) {
            prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    std::fclose(maps);
    if (prot < 0) {
        return false;
    }
    void* page = reinterpret_cast<void*>(address & ~static_cast<std::uintptr_t>(page_size - 1));
    if ((prot & PROT_WRITE) == 0 && mprotect(page, static_cast<std::size_t>(page_size), prot | PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    *static_cast<volatile std::uint64_t*>(slot) = value;
    if ((prot & PROT_WRITE) == 0) {
        mprotect(page, static_cast<std::size_t>(page_size), prot);
    }
    return true;
}
#endif
