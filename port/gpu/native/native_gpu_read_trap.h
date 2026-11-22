// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Debug switch TLG_VKB_GPU_READ_TRAP (off by default; issue 83, measuring only). The memory under every engine
// resource the graphics card writes is made inaccessible to the processor, so every processor access to it traps.
// The trap notes the game code that did it, lets that one instruction run (single step) and protects the page
// again. Nothing the game sees changes; the game only runs slower. The caught places are in the objects report
// under "gpu_read_trap" and in a file (TLG_VKB_GPU_READ_TRAP_FILE, default the log folder), written every 5 s.
//   1: buffers the graphics card writes (bound as unordered access, or the target of one of our copies or fills)
//   2: those, plus textures the graphics card writes (render targets, depth targets, unordered access)
#pragma once

#include <nlohmann/json.hpp>
#include "common/types.h"

namespace Core::GpuReadTrap {

enum class Kind : u32 {
    UavBuffer = 0,  ///< an engine buffer bound as unordered access (the graphics card writes it)
    CopyTarget = 1, ///< an engine buffer one of our copies or fills wrote
    Texture = 2,    ///< an engine texture the graphics card draws or writes into (mode 2)
};

/// The switch (read once): 0 off, 1 buffers, 2 buffers and textures
u32 Mode();
inline bool On() {
    return Mode() != 0;
}
/// The memory of engine object `id` is written by the graphics card from now on: protect it (a second call for the same id is ignored)
void Watch(u64 id, u64 address, u64 bytes, Kind kind);
/// Engine object `id` is gone: its memory is no longer protected
void Unwatch(u64 id);
/// A new engine object that the processor fills was made at [address, address + bytes): a watched object that started at the same
/// address is treated as gone (the engine reused the memory and its delete was not seen)
void CpuObjectMade(u64 address, u64 bytes);
/// Once per frame (render thread): pages left open after too many traps in one frame are protected again; the file is written every 5 s
void Tick();
nlohmann::json Report();

} // namespace Core::GpuReadTrap
