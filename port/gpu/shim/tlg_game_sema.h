// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the game's kernel semaphores, signalled from the GPU library. The loader hands over its
// semaphore signal at boot (tlggpu_set_game_sema_signal); unset: nothing is signalled.

#pragma once

#include <atomic>
#include <cstdint>

namespace TlgGameSema {
/// Adds `count` to the game's semaphore `id` and wakes its waiters, as the game's own signal call does; 0 = done.
using Signal = std::int32_t (*)(std::uint32_t id, std::int32_t count);
inline std::atomic<Signal> signal{nullptr};
} // namespace TlgGameSema
