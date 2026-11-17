// SPDX-License-Identifier: GPL-2.0-or-later
// tlg-native, N7.5: see vk_host_import.h.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "common/logging/log.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_host_import.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"

extern "C" {
// runtime_memory.c: where [address, address + size) lives in the backing view (one mapped region), the view's base address and the section's size; 0 when it is not inside one.
int runtime_memory_backing_window(uintptr_t address, uint64_t size, void** base, uint64_t* offset, uint64_t* section_bytes);
#ifndef _WIN32
// runtime_memory.c: the GPU pool provider (accepted only before the pool exists) and whether the pool is GPU memory
typedef int (*RuntimePoolProvider)(uint64_t bytes);
int runtime_memory_set_pool_provider(RuntimePoolProvider provider);
int runtime_memory_pool_is_gpu(uint64_t* exec_dropped);
#endif
}

namespace VkbVulkan {

namespace GpuPool {
namespace {
u32 g_type = ~0u;
std::atomic<VkDeviceMemory> g_memory{VK_NULL_HANDLE};
u64 g_bytes = 0;
const char* g_reason = "not decided";

#ifndef _WIN32
const Instance* g_instance = nullptr;
constexpr vk::BufferUsageFlags WindowUsage =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer;

/// The trial the in place import gets on this driver with the kind of memory the runtime makes (a touched page range of a shared memfd)
bool SharedImportWorks(const Instance& instance) {
    if (!instance.IsExternalMemoryHostSupported()) {
        return false;
    }
    constexpr size_t Bytes = 2u << 20;
    const int fd = memfd_create("tlg-import-trial", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, Bytes) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    void* view = mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (view == MAP_FAILED) {
        return false;
    }
    std::memset(view, 1, Bytes);
    bool ok = false;
    const auto device = instance.GetDevice();
    const auto host = device.getMemoryHostPointerPropertiesEXT(vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, view);
    if (host.result == vk::Result::eSuccess && host.value.memoryTypeBits != 0) {
        const vk::ImportMemoryHostPointerInfoEXT import_info{.handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, .pHostPointer = view};
        auto memory = device.allocateMemory(vk::MemoryAllocateInfo{
            .pNext = &import_info, .allocationSize = Bytes, .memoryTypeIndex = static_cast<u32>(__builtin_ctz(host.value.memoryTypeBits))});
        if (memory.result == vk::Result::eSuccess) {
            device.freeMemory(memory.value);
            ok = true;
        }
    }
    munmap(view, Bytes);
    return ok;
}

/// A host visible, coherent memory type for exportable window buffers: cached first (the CPU reads the game's memory), never the AMD device-coherent kinds
u32 PickType(const Instance& instance) {
    const auto device = instance.GetDevice();
    const vk::ExternalMemoryBufferCreateInfo external{.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    auto buffer = device.createBuffer(vk::BufferCreateInfo{.pNext = &external, .size = 1u << 20, .usage = WindowUsage, .sharingMode = vk::SharingMode::eExclusive});
    if (buffer.result != vk::Result::eSuccess) {
        return ~0u;
    }
    const u32 bits = device.getBufferMemoryRequirements(buffer.value).memoryTypeBits;
    device.destroyBuffer(buffer.value);
    const auto& properties = instance.GetMemoryProperties();
    using F = vk::MemoryPropertyFlagBits;
    const vk::MemoryPropertyFlags avoid = F::eDeviceCoherentAMD | F::eDeviceUncachedAMD;
    for (const vk::MemoryPropertyFlags want : {F::eHostVisible | F::eHostCoherent | F::eHostCached, vk::MemoryPropertyFlags{F::eHostVisible | F::eHostCoherent}}) {
        for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
            const auto flags = properties.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (flags & want) == want && !(flags & avoid)) {
                return i;
            }
        }
    }
    return ~0u;
}

/// Exportable memory of `bytes` bytes and its dma-buf file; the memory is null on failure
vk::DeviceMemory Allocate(u64 bytes, int& fd) {
    fd = -1;
    const auto device = g_instance->GetDevice();
    const vk::ExportMemoryAllocateInfo export_info{.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    auto memory = device.allocateMemory(vk::MemoryAllocateInfo{.pNext = &export_info, .allocationSize = bytes, .memoryTypeIndex = g_type});
    if (memory.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GPU pool: allocation of {} MiB failed ({})", bytes >> 20, vk::to_string(memory.result));
        return {};
    }
    auto file = device.getMemoryFdKHR(vk::MemoryGetFdInfoKHR{.memory = memory.value, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT});
    if (file.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GPU pool: export of {} MiB as a dma-buf failed ({})", bytes >> 20, vk::to_string(file.result));
        device.freeMemory(memory.value);
        return {};
    }
    fd = file.value;
    return memory.value;
}

/// The trial of the pool itself: a small exported memory mapped twice at different offsets must show one CPU write at both addresses
bool PoolTrial() {
    constexpr u64 Bytes = 4u << 20, Half = Bytes / 2;
    int fd = -1;
    const vk::DeviceMemory memory = Allocate(Bytes, fd);
    if (!memory) {
        return false;
    }
    bool ok = false;
    void* whole = mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void* second = mmap(nullptr, Half, PROT_READ | PROT_WRITE, MAP_SHARED, fd, Half);
    if (whole != MAP_FAILED && second != MAP_FAILED) {
        static_cast<volatile u8*>(whole)[Half + 5] = 0x5a;
        ok = static_cast<volatile u8*>(second)[5] == 0x5a;
        if (!ok) {
            LOG_ERROR(Render_Vulkan, "GPU pool: trial: two mappings of the exported memory do not show the same bytes");
        }
    } else {
        LOG_ERROR(Render_Vulkan, "GPU pool: trial: the exported memory cannot be mapped ({})", std::strerror(errno));
    }
    if (whole != MAP_FAILED) {
        munmap(whole, Bytes);
    }
    if (second != MAP_FAILED) {
        munmap(second, Half);
    }
    close(fd);
    g_instance->GetDevice().freeMemory(memory);
    return ok;
}

/// The runtime's provider: called once, on the game thread that makes the pool
int Provide(uint64_t bytes) {
    int fd = -1;
    const vk::DeviceMemory memory = Allocate(bytes, fd);
    if (!memory) {
        g_reason = "the driver refused the pool's allocation or export (see the log)";
        return -1;
    }
    g_bytes = bytes;
    g_memory.store(static_cast<VkDeviceMemory>(memory), std::memory_order_release);
    LOG_INFO(Render_Vulkan, "GPU pool: the guest pool is our memory ({} MiB, memory type {}), handed to the runtime as a dma-buf", bytes >> 20, g_type);
    return fd;
}
#endif
} // namespace

void Setup(const Instance& instance) {
#ifdef _WIN32
    (void)instance;
    g_reason = "not on Windows";
#else
    g_instance = &instance;
    const char* sw = std::getenv("TLG_VKB_GPU_POOL");
    const bool force = sw != nullptr && sw[0] == '1';
    if (sw != nullptr && sw[0] == '0') {
        g_reason = "off (TLG_VKB_GPU_POOL=0)";
    } else if (!instance.IsExternalMemoryDmaBufSupported()) {
        g_reason = "the driver has no dma-buf export (VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf)";
    } else if (!force && SharedImportWorks(instance)) {
        g_reason = "not needed: this driver imports shared guest memory in place";
    } else if ((g_type = PickType(instance)) == ~0u) {
        g_reason = "no host visible memory type for exportable buffers";
    } else if (!PoolTrial()) {
        g_reason = "the trial of the pool failed (see the log)";
    } else if (runtime_memory_set_pool_provider(Provide) != 0) {
        g_reason = "too late: the guest pool existed before the device";
    } else {
        g_reason = force ? "on (TLG_VKB_GPU_POOL=1)" : "on: this driver cannot import shared guest memory in place";
        LOG_INFO(Render_Vulkan, "GPU pool: {}", g_reason);
        return;
    }
    if (force || (sw == nullptr && instance.IsExternalMemoryDmaBufSupported() && g_type != ~0u)) {
        LOG_ERROR(Render_Vulkan, "GPU pool: NOT used: {}", g_reason);
    } else {
        LOG_INFO(Render_Vulkan, "GPU pool: not used: {}", g_reason);
    }
#endif
}

vk::DeviceMemory Memory() noexcept {
    return vk::DeviceMemory{g_memory.load(std::memory_order_acquire)};
}
u64 Bytes() noexcept {
    return g_bytes;
}
u32 MemoryType() noexcept {
    return g_type;
}
const char* Reason() noexcept {
    return g_reason;
}
u64 ExecDropped() noexcept {
#ifdef _WIN32
    return 0;
#else
    uint64_t dropped = 0;
    runtime_memory_pool_is_gpu(&dropped);
    return dropped;
#endif
}
} // namespace GpuPool

HostImport::HostImport(const Instance& instance_) : instance{instance_} {
    if (GpuPool::Memory()) {
        gpu_pool = true;
        available = true;
        LOG_INFO(Render_Vulkan, "in place buffers: the guest pool is GPU memory ({} MiB): chunks are bound into it", GpuPool::Bytes() >> 20);
        return;
    }
    if (!instance.IsExternalMemoryHostSupported()) {
        LOG_INFO(Render_Vulkan, "in place buffers: VK_EXT_external_memory_host is not available");
        return;
    }
    auto properties = instance.GetPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>();
    alignment = properties.get<vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>().minImportedHostPointerAlignment;
    available = alignment != 0 && ChunkBytes % alignment == 0;
    LOG_INFO(Render_Vulkan, "in place buffers: host pointer import available, alignment {}", alignment);
}

HostImport::~HostImport() {
    const auto device = instance.GetDevice();
    for (auto& chunk : chunks) {
        if (chunk.buffer) {
            device.destroyBuffer(chunk.buffer);
        }
        if (chunk.memory) {
            device.freeMemory(chunk.memory);
        }
    }
}

bool HostImport::BindPoolChunk(u32 index, Chunk& chunk, u64 begin) {
    const auto device = instance.GetDevice();
    const vk::ExternalMemoryBufferCreateInfo external_info{.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    auto buffer = device.createBuffer(vk::BufferCreateInfo{
        .pNext = &external_info,
        .size = chunk.bytes,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "in place buffers: pool chunk {}: buffer creation failed ({})", index, vk::to_string(buffer.result));
        return false;
    }
    const auto requirements = device.getBufferMemoryRequirements(buffer.value);
    if (!(requirements.memoryTypeBits & (1u << GpuPool::MemoryType())) || begin % requirements.alignment != 0) {
        LOG_ERROR(Render_Vulkan, "in place buffers: pool chunk {}: the window buffer cannot be bound into the pool (types {:#x}, alignment {})", index, requirements.memoryTypeBits,
                  requirements.alignment);
        device.destroyBuffer(buffer.value);
        return false;
    }
    if (device.bindBufferMemory(buffer.value, GpuPool::Memory(), begin) != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "in place buffers: pool chunk {}: bind failed", index);
        device.destroyBuffer(buffer.value);
        return false;
    }
    chunk.buffer = buffer.value; // chunk.memory stays null: the pool's memory is the runtime's for the whole run
    imported_bytes.fetch_add(chunk.bytes, std::memory_order_relaxed);
    imported_chunks.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan, "in place buffers: pool chunk {} (backing {:#x}, {} MiB) bound", index, begin, chunk.bytes >> 20);
    return true;
}

bool HostImport::ImportChunk(u32 index, Chunk& chunk, u8* backing_base, u64 section_bytes) {
    const u64 begin = u64{index} * ChunkBytes;
    chunk.bytes = std::min(ChunkBytes + OverlapBytes, section_bytes - begin); // the overlap: a range that starts in this chunk and crosses its end is still in one window
    chunk.bytes &= ~(alignment - 1);
    if (chunk.bytes == 0) {
        return false;
    }
    if (gpu_pool) {
        return BindPoolChunk(index, chunk, begin);
    }
    u8* pointer = backing_base + begin;
    const auto device = instance.GetDevice();
    const auto host_properties = device.getMemoryHostPointerPropertiesEXT(vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, pointer);
    if (host_properties.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "in place buffers: chunk {} (backing {:#x}): host pointer properties failed ({})", index, begin, vk::to_string(host_properties.result));
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo external_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const vk::BufferCreateInfo buffer_info{
        .pNext = &external_info,
        .size = chunk.bytes,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    auto buffer = device.createBuffer(buffer_info);
    if (buffer.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "in place buffers: chunk {}: buffer creation failed ({})", index, vk::to_string(buffer.result));
        return false;
    }
    const auto requirements = device.getBufferMemoryRequirements(buffer.value);
    const u32 bits = host_properties.value.memoryTypeBits & requirements.memoryTypeBits;
    const auto& memory_properties = instance.GetMemoryProperties();
    u32 type = ~0u;
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)) {
            type = i;
            break;
        }
    }
    if (type == ~0u) {
        LOG_ERROR(Render_Vulkan, "in place buffers: chunk {}: no host visible memory type for the imported pointer", index);
        device.destroyBuffer(buffer.value);
        return false;
    }
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = pointer,
    };
    const vk::MemoryAllocateInfo allocate_info{
        .pNext = &import_info,
        .allocationSize = (requirements.size + alignment - 1) & ~(alignment - 1),
        .memoryTypeIndex = type,
    };
    auto memory = device.allocateMemory(allocate_info);
    if (memory.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "in place buffers: chunk {} (backing {:#x}, {} MiB): import failed ({})", index, begin, chunk.bytes >> 20, vk::to_string(memory.result));
        device.destroyBuffer(buffer.value);
        return false;
    }
    if (device.bindBufferMemory(buffer.value, memory.value, 0) != vk::Result::eSuccess) {
        device.destroyBuffer(buffer.value);
        device.freeMemory(memory.value);
        return false;
    }
    chunk.buffer = buffer.value;
    chunk.memory = memory.value;
    imported_bytes.fetch_add(chunk.bytes, std::memory_order_relaxed);
    imported_chunks.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan, "in place buffers: imported chunk {} (backing {:#x}, {} MiB)", index, begin, chunk.bytes >> 20);
    return true;
}

HostImport::Window HostImport::Find(VAddr address, u64 size) {
    if (!available || size == 0) {
        return {};
    }
    void* base = nullptr;
    u64 offset = 0, section_bytes = 0;
    if (runtime_memory_backing_window(address, size, &base, &offset, &section_bytes) == 0) {
        return {};
    }
    const u32 index = static_cast<u32>(offset / ChunkBytes);
    if (index >= MaxChunks) {
        return {};
    }
    std::scoped_lock lock{mutex};
    Chunk& chunk = chunks[index];
    if (!chunk.tried) {
        chunk.tried = true;
        if (!ImportChunk(index, chunk, static_cast<u8*>(base), section_bytes)) {
            failed_chunks.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const u64 in_chunk = offset - u64{index} * ChunkBytes;
    if (!chunk.buffer || in_chunk + size > chunk.bytes) {
        return {}; // a range that leaves the chunk's window (longer than the overlap) stays on the ordinary path
    }
    return {chunk.buffer, in_chunk, chunk.bytes - in_chunk};
}

} // namespace VkbVulkan
