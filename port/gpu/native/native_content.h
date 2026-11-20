// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"

/// What is left of the F2 / M announced content store (the copy of the engine's writes, the write ledger, the ring windows, the late-read census, the raster
/// submissions' ledger), deleted on 2026-11-20 (backend note 105): it was fed by the writer announcements and the old raster path, neither of which runs with the
/// engine-to-Vulkan backend. Kept: the guarded hashes of guest memory the debug tools use (byte audit, UI trace, N0 census, resource audit).
namespace Core::NativeContent {

/// The low 32 bits of XXH3 of `count` consecutive chunks of `chunk_bytes` bytes of guest memory from `address`, into `out`; false if the memory is not readable
[[nodiscard]] bool HashChunksGuarded(u64 address, u64 count, u64 chunk_bytes, u32* out);
/// XXH64 of `size` bytes of guest memory; false if the memory is not readable
[[nodiscard]] bool HashGuarded64(u64 address, u64 size, u64& out);

} // namespace Core::NativeContent
