// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "tlg_toggles.h" // sigjmp_buf, tlg_setjmp, runtime_fault_recover
#include "native/native_content.h"

namespace Core::NativeContent {
namespace {

// ---- guarded reads of guest memory ---------------------------------------------------------------------------------------------------------------------------------
using Thunk = void (*)(void*);
#if defined(TLG_PORT)
__declspec(noinline) bool RunGuarded(Thunk thunk, void* argument) {
    sigjmp_buf recovery;
    if (tlg_setjmp(recovery) != 0) {
        return false;
    }
    runtime_fault_recover = &recovery;
    thunk(argument);
    runtime_fault_recover = nullptr;
    return true;
}
#elif defined(_WIN32)
__declspec(noinline) bool RunGuarded(Thunk thunk, void* argument) {
    __try {
        thunk(argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
#else
bool RunGuarded(Thunk thunk, void* argument) {
    thunk(argument);
    return true;
}
#endif

struct HashJob {
    u64 address;
    u64 count;
    u64 chunk;
    u32* out;
};
} // namespace

bool HashChunksGuarded(u64 address, u64 count, u64 chunk_bytes, u32* out) {
    HashJob job{address, count, chunk_bytes, out};
    return RunGuarded(
        [](void* p) {
            const auto& j = *static_cast<HashJob*>(p);
            const u8* base = reinterpret_cast<const u8*>(j.address);
            for (u64 i = 0; i < j.count; ++i) {
                j.out[i] = static_cast<u32>(XXH3_64bits(base + i * j.chunk, j.chunk));
            }
        },
        &job);
}

bool HashGuarded64(u64 address, u64 size, u64& out) {
    struct Job {
        u64 address, size, out;
    } job{address, size, 0};
    const bool ok = RunGuarded([](void* p) { auto& j = *static_cast<Job*>(p); j.out = XXH64(reinterpret_cast<const void*>(j.address), j.size, 0); }, &job);
    out = job.out;
    return ok;
}

} // namespace Core::NativeContent
