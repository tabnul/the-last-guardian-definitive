// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
// Executable memory near the game's code for the guest hook trampolines (native/native_hooks.cpp). From the old port's shim/tlg_cpu_patches.cpp (merge note 5).
#include <cstdint>
#include <mutex>
#include <string>
#include "common/logging/log.h"
#include "native/shadps4/core/cpu_patches.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace MemoryPatcher {
/// Names the per game config files of the guest hooks (CUSA03627.hooks.json); set when our Gnm registers (Libraries::VkbGnmDriver::RegisterLib).
/// Their common/memory_patcher.cpp declares the same variable but is not built into tlggpu.
std::string g_game_serial = "UNKNOWN";
} // namespace MemoryPatcher

namespace Core {
namespace {
constexpr size_t AreaSize = 4 * 1024 * 1024;
constexpr uintptr_t Reach = 0x70000000; // below the 2 GB limit of rel32 so the whole area stays reachable
std::mutex area_mutex;
uint8_t* area_base = nullptr;
size_t area_used = 0;

uint8_t* AllocateNear(const void* code_ptr) {
#ifdef _WIN32
    const uintptr_t anchor = reinterpret_cast<uintptr_t>(code_ptr) & ~uintptr_t(0xffff);
    for (uintptr_t delta = 0x10000; delta < Reach; delta += 0x10000) {
        for (const uintptr_t candidate : {anchor + Reach - delta, anchor - Reach + delta}) {
            if (candidate < 0x10000) {
                continue;
            }
            void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), AreaSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) {
                return static_cast<uint8_t*>(p);
            }
        }
    }
#else
    // Linux: the same search as above with mmap. MAP_FIXED_NOREPLACE takes the address or fails (never replaces a mapping);
    // a kernel older than 4.17 treats it as a hint only, so an answer outside the reach is given back and the search goes on.
    const uintptr_t anchor = reinterpret_cast<uintptr_t>(code_ptr) & ~uintptr_t(0xffff);
    for (uintptr_t delta = 0x10000; delta < Reach; delta += 0x10000) {
        for (const uintptr_t candidate : {anchor + Reach - delta, anchor - Reach + delta}) {
            if (candidate < 0x10000) {
                continue;
            }
            void* p = mmap(reinterpret_cast<void*>(candidate), AreaSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) {
                continue;
            }
            const uintptr_t got = reinterpret_cast<uintptr_t>(p);
            const uintptr_t distance = got > anchor ? got - anchor : anchor - got;
            if (distance + AreaSize <= Reach) {
                return static_cast<uint8_t*>(p);
            }
            munmap(p, AreaSize);
        }
    }
#endif
    return nullptr;
}
} // namespace

bool EmitInTrampolineArea(const void* code_ptr, const std::function<bool(Xbyak::CodeGenerator&)>& emit) {
    std::scoped_lock lock{area_mutex};
    if (!area_base) {
        area_base = AllocateNear(code_ptr);
        if (!area_base) {
            LOG_ERROR(Core, "guest hooks: no executable memory near the game's code");
            return false;
        }
    }
    Xbyak::CodeGenerator gen(AreaSize - area_used, area_base + area_used);
    try {
        if (emit(gen)) {
            area_used += (gen.getSize() + 15) & ~size_t(15);
            return true;
        }
    } catch (const Xbyak::Error&) {
    }
    return false;
}
} // namespace Core
