// SPDX-License-Identifier: GPL-2.0-or-later
// tlg-native, N7.5: in place buffers. The emulator's guest memory is a pagefile backed section; the runtime keeps one always read/write view of all of it (the "backing", the
// view the host side reads and writes without touching the guest views' page protection). `VK_EXT_external_memory_host` lets the GPU read the pages of that view where they are:
// a buffer bound to such memory sees every CPU write at once, with no copy and no dirty page tracking (tools/hostptr_probe shows this works on the target device for exactly this
// kind of memory, with the 4 KiB import alignment, 1 GB imported in 0.3 s because the driver pins the pages).
//
// The backing is imported in chunks of 256 MiB on first use. `Find` answers where a guest range is: the chunk's buffer and the offset in it, or a null buffer (the range is not
// inside one mapped region, spans two chunks, or the import failed): the caller then takes its ordinary path.
//
// GPU pool (TLG_VKB_GPU_POOL, Linux): where the driver cannot import the runtime's shared memory (RADV imports only private anonymous memory), the guest pool itself
// is made our memory: allocated here, exported as a dma-buf file, and mapped by the runtime where it maps its memfd otherwise (same aliases, backing view and
// protections). A chunk is then a buffer bound into that memory at the chunk's offset instead of an import: the same windows, the same in place reads.

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace VkbVulkan {

class Instance;

namespace GpuPool {
/// At the device's creation, before the game has memory: decides whether the guest pool is made our memory (TLG_VKB_GPU_POOL; unset: only when a trial import
/// of shared memory fails and a trial of the pool works) and hands the runtime its provider. Does nothing on Windows.
void Setup(const Instance& instance);
/// The pool's memory once the runtime has taken it, else null
vk::DeviceMemory Memory() noexcept;
u64 Bytes() noexcept;
u32 MemoryType() noexcept;
/// Why the pool is or is not used (for the log and the report)
const char* Reason() noexcept;
/// Guest requests for execute permission on the pool's memory that were dropped (the driver refuses executable mappings of it)
u64 ExecDropped() noexcept;
} // namespace GpuPool

class HostImport {
public:
    static constexpr u64 ChunkBytes = 256ull << 20;
    static constexpr u64 OverlapBytes = 64ull << 20; ///< each chunk's window reaches this far into the next one
    static constexpr u32 MaxChunks = 64;

    explicit HostImport(const Instance& instance);
    ~HostImport();

    struct Window {
        vk::Buffer buffer{};
        u64 offset{0};
        u64 chunk_remaining{0}; ///< bytes from `offset` to the end of the chunk's buffer
    };
    /// A window for [address, address + size) of guest memory; `buffer` is null when there is none.
    Window Find(VAddr address, u64 size);

    [[nodiscard]] bool Available() const noexcept {
        return available;
    }
    [[nodiscard]] u64 ImportedBytes() const noexcept {
        return imported_bytes.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u32 ImportedChunks() const noexcept {
        return imported_chunks.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u32 FailedChunks() const noexcept {
        return failed_chunks.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool OnGpuPool() const noexcept {
        return gpu_pool;
    }

private:
    struct Chunk {
        vk::Buffer buffer{};
        vk::DeviceMemory memory{};
        u64 bytes{0};
        bool tried{false};
    };
    bool ImportChunk(u32 index, Chunk& chunk, u8* backing_base, u64 section_bytes);
    bool BindPoolChunk(u32 index, Chunk& chunk, u64 begin);

    const Instance& instance;
    bool available{false};
    bool gpu_pool{false}; ///< the guest pool is our memory (GpuPool): chunks are bound into it, not imported
    u32 memory_type_bits{0};
    u64 alignment{4096};
    std::array<Chunk, MaxChunks> chunks{};
    std::mutex mutex;
    std::atomic<u64> imported_bytes{0};
    std::atomic<u32> imported_chunks{0}, failed_chunks{0};
};

} // namespace VkbVulkan
