// Linux side of the few Windows calls the new engine uses. Empty on Windows: there the real Windows
// calls are used and nothing here is seen.
#pragma once
#if !defined(_WIN32)
#include <chrono>
#include <csetjmp>
#include <cstdint>
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
#endif
