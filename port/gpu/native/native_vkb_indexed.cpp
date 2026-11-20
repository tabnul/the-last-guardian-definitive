// SPDX-License-Identifier: GPL-2.0-or-later
// Scene on the GPU, R1: the global descriptor set (see native_vkb_indexed.h). Its shape follows the old path's C9 host side (tlg-pc-port-hayato vk_indexed_data.cpp), on the
// backend's own device; slots are keyed by our object handles and freed when the objects go.

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "native/native_vkb_indexed.h"
#include "native/native_vkb_stream.h"
#include "native/shadps4/shader_recompiler/resource.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"
#include "native/vkb_core/vkb_device.h"

namespace Core::Vkb::Indexed {
namespace {
namespace I = VkbShader::Indexed;

constexpr u32 BuffersTable = 0;
constexpr u32 SamplersTable = 1;
constexpr u32 FirstImageTable = 2;
constexpr u32 NumTables = FirstImageTable + I::NumImageArrays;

struct Table {
    u32 binding{};
    vk::DescriptorType type{};
    u32 capacity{};
    absl::flat_hash_map<u64, u32> map; ///< our handle -> slot
    std::vector<u64> owner;            ///< slot -> handle (0: free)
    std::vector<u32> free;
};

struct State {
    bool ready{false}, fits{false};
    vk::Device device{};
    vk::DescriptorSetLayout layout{};
    vk::DescriptorPool pool{};
    vk::DescriptorSet set{};
    vk::Sampler default_sampler{};
    std::array<Table, NumTables> tables{};
    std::atomic<u64> buffer_new{0}, image_new{0}, sampler_new{0}, freed{0}, exhausted{0}, hits{0};
};

std::mutex mutex;
State* state = nullptr;
/// R3c: a per-thread memo of handle -> slot per table (no lock in the common case); any freed slot changes the epoch and empties every memo
std::atomic<u64> g_epoch{1};
struct MemoEntry {
    u64 handle{}, epoch{};
    u32 slot{};
};
constexpr u32 MemoSize = 512;
thread_local std::array<std::array<MemoEntry, MemoSize>, 3> t_memo{}; // buffers, samplers, images (the image key folded into the handle's hash)
inline MemoEntry& MemoFor(u32 kind, u64 handle, u32 key) {
    const u64 h = (handle ^ (u64{key} * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull;
    return t_memo[kind][static_cast<u32>(h >> 55) & (MemoSize - 1)];
}
std::atomic<u64> c_memo_hits{0};

bool Requested() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_RECORD"); return e != nullptr && e[0] == '1'; }();
    return on;
}

bool DeviceFits(const VkbVulkan::Instance& instance) {
    constexpr u32 buffers = 1 + I::MaxBuffers;
    constexpr u32 images = I::NumImageKeys * I::MaxImages;
    constexpr u32 storage_images = I::NumImageKeys * I::MaxStorageImages + 4 * I::NumAtomicFormats * I::MaxAtomicImages;
    constexpr u32 samplers = I::MaxSamplers;
    const auto props = instance.GetPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan12Properties>();
    const auto& p = props.get<vk::PhysicalDeviceVulkan12Properties>();
    const bool fits = p.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= buffers + 8 && p.maxPerStageDescriptorUpdateAfterBindSampledImages >= images + 64 &&
                      p.maxPerStageDescriptorUpdateAfterBindStorageImages >= storage_images + 64 && p.maxPerStageDescriptorUpdateAfterBindSamplers >= samplers + 64 &&
                      p.maxDescriptorSetUpdateAfterBindStorageBuffers >= buffers && p.maxDescriptorSetUpdateAfterBindSampledImages >= images &&
                      p.maxDescriptorSetUpdateAfterBindSamplers >= samplers && p.maxDescriptorSetUpdateAfterBindStorageImages >= storage_images &&
                      p.maxPerStageUpdateAfterBindResources >= buffers + images + storage_images + samplers + 256;
    LOG_INFO(Render_Vulkan, "vkb record mode: device limits (update after bind, per stage) storage buffers {} sampled images {} storage images {} samplers {} resources {}: {}",
             p.maxPerStageDescriptorUpdateAfterBindStorageBuffers, p.maxPerStageDescriptorUpdateAfterBindSampledImages, p.maxPerStageDescriptorUpdateAfterBindStorageImages,
             p.maxPerStageDescriptorUpdateAfterBindSamplers, p.maxPerStageUpdateAfterBindResources, fits ? "enough" : "NOT enough");
    return fits;
}

void WriteBuffer(State& s, u32 slot, vk::Buffer buffer) {
    const vk::DescriptorBufferInfo info{buffer, 0, VK_WHOLE_SIZE};
    const vk::WriteDescriptorSet write{.dstSet = s.set, .dstBinding = I::BufferBinding, .dstArrayElement = slot, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &info};
    s.device.updateDescriptorSets(write, {});
}

void WriteImage(State& s, u32 table, u32 slot, vk::ImageView view, vk::Sampler sampler) {
    const Table& t = s.tables[table];
    const vk::DescriptorImageInfo info{sampler, view, vk::ImageLayout::eGeneral};
    const vk::WriteDescriptorSet write{.dstSet = s.set, .dstBinding = t.binding, .dstArrayElement = slot, .descriptorCount = 1, .descriptorType = t.type, .pImageInfo = &info};
    s.device.updateDescriptorSets(write, {});
}

/// The live state (made at the first call), or null (switch off, or the device lacks the room). Caller holds the mutex.
State* Live() {
    if (state != nullptr) {
        return state->fits ? state : nullptr;
    }
    state = new State;
    State& s = *state;
    s.ready = true;
    if (!Requested()) {
        return nullptr;
    }
    const VkbVulkan::Instance& instance = Core::Vkb::Device::Instance();
    s.fits = DeviceFits(instance);
    if (!s.fits) {
        return nullptr;
    }
    s.device = instance.GetDevice();
    const auto setup = [&](u32 index, u32 binding, vk::DescriptorType type, u32 capacity) {
        Table& t = s.tables[index];
        t.binding = binding;
        t.type = type;
        t.capacity = capacity;
        t.owner.assign(capacity, 0);
        t.free.reserve(capacity);
        for (u32 slot = capacity - 1; slot >= 1; --slot) {
            t.free.push_back(slot);
        }
        t.owner[0] = ~0ull; // slot 0: the null buffer / the default sampler / unused for images
    };
    setup(BuffersTable, I::BufferBinding, vk::DescriptorType::eStorageBuffer, I::MaxBuffers);
    setup(SamplersTable, I::SamplerBinding, vk::DescriptorType::eSampler, I::MaxSamplers);
    for (u32 key = 0; key < I::NumImageArrays; ++key) {
        const bool storage = key >= I::NumImageKeys;
        const bool atomic = key >= I::FirstAtomicKey;
        setup(FirstImageTable + key, I::ImageBinding + key, storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
              atomic ? I::MaxAtomicImages : storage ? I::MaxStorageImages : I::MaxImages);
    }
    std::array<vk::DescriptorSetLayoutBinding, 1 + NumTables> bindings{};
    std::array<vk::DescriptorBindingFlags, 1 + NumTables> flags{};
    const vk::ShaderStageFlags stages = vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute;
    const vk::DescriptorBindingFlags pending = vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
    bindings[0] = {.binding = I::TableBinding, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = stages};
    flags[0] = vk::DescriptorBindingFlagBits::eUpdateAfterBind | pending;
    for (u32 i = 0; i < NumTables; ++i) {
        bindings[1 + i] = {.binding = s.tables[i].binding, .descriptorType = s.tables[i].type, .descriptorCount = s.tables[i].capacity, .stageFlags = stages};
        flags[1 + i] = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eUpdateAfterBind | pending;
    }
    const vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_ci{.bindingCount = static_cast<u32>(flags.size()), .pBindingFlags = flags.data()};
    const vk::DescriptorSetLayoutCreateInfo layout_ci{.pNext = &flags_ci, .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
                                                      .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()};
    const auto [lr, layout] = s.device.createDescriptorSetLayout(layout_ci);
    if (lr != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "vkb record mode: the global set layout failed: {}", vk::to_string(lr));
        s.fits = false;
        return nullptr;
    }
    s.layout = layout;
    const std::array<vk::DescriptorPoolSize, 4> sizes{{
        {vk::DescriptorType::eStorageBuffer, 1 + I::MaxBuffers},
        {vk::DescriptorType::eSampledImage, I::NumImageKeys * I::MaxImages},
        {vk::DescriptorType::eStorageImage, I::NumImageKeys * I::MaxStorageImages + 4 * I::NumAtomicFormats * I::MaxAtomicImages},
        {vk::DescriptorType::eSampler, I::MaxSamplers},
    }};
    const auto [pr, pool] = s.device.createDescriptorPool({.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind, .maxSets = 1,
                                                           .poolSizeCount = static_cast<u32>(sizes.size()), .pPoolSizes = sizes.data()});
    if (pr != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "vkb record mode: the global pool failed: {}", vk::to_string(pr));
        s.fits = false;
        return nullptr;
    }
    s.pool = pool;
    const vk::DescriptorSetAllocateInfo alloc{.descriptorPool = s.pool, .descriptorSetCount = 1, .pSetLayouts = &s.layout};
    vk::DescriptorSet set{};
    if (s.device.allocateDescriptorSets(&alloc, &set) != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "vkb record mode: the global set failed");
        s.fits = false;
        return nullptr;
    }
    s.set = set;
    const auto [sr, sampler] = s.device.createSampler({.magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear, .mipmapMode = vk::SamplerMipmapMode::eLinear,
                                                       .addressModeU = vk::SamplerAddressMode::eClampToEdge, .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                                       .addressModeW = vk::SamplerAddressMode::eClampToEdge, .maxLod = VK_LOD_CLAMP_NONE});
    s.default_sampler = sr == vk::Result::eSuccess ? sampler : vk::Sampler{};
    // the row table: the whole host ring (the rows are host ring slices, the row base is a slice's offset in dwords)
    const vk::DescriptorBufferInfo table{Stream::HostRingBuffer(), 0, VK_WHOLE_SIZE};
    s.device.updateDescriptorSets(vk::WriteDescriptorSet{.dstSet = s.set, .dstBinding = I::TableBinding, .descriptorCount = 1,
                                                          .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &table},
                                  {});
    WriteBuffer(s, 0, Stream::NullBuffer());
    if (s.default_sampler) {
        WriteImage(s, SamplersTable, 0, {}, s.default_sampler);
    }
    LOG_INFO(Render_Vulkan, "vkb record mode: global set made ({} buffer slots, {} per sampled image array, {} sampler slots)", I::MaxBuffers, I::MaxImages, I::MaxSamplers);
    return state;
}

u32 SlotOf(State& s, u32 table, u64 handle, bool& made) {
    Table& t = s.tables[table];
    made = false;
    if (const auto it = t.map.find(handle); it != t.map.end()) {
        s.hits.fetch_add(1, std::memory_order_relaxed);
        return it->second;
    }
    if (t.free.empty()) {
        s.exhausted.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const u32 slot = t.free.back();
    t.free.pop_back();
    t.owner[slot] = handle;
    t.map.emplace(handle, slot);
    made = true;
    return slot;
}

void Forget(u32 first, u32 last, u64 handle) {
    std::scoped_lock lock{mutex};
    State* s = state != nullptr && state->fits ? state : nullptr;
    if (s == nullptr || handle == 0) {
        return;
    }
    for (u32 table = first; table <= last; ++table) {
        Table& t = s->tables[table];
        if (const auto it = t.map.find(handle); it != t.map.end()) {
            t.owner[it->second] = 0;
            t.free.push_back(it->second);
            t.map.erase(it);
            s->freed.fetch_add(1, std::memory_order_relaxed);
            g_epoch.fetch_add(1, std::memory_order_release);
        }
    }
}
} // namespace

bool On() {
    std::scoped_lock lock{mutex};
    return Live() != nullptr;
}

vk::DescriptorSetLayout Layout() {
    std::scoped_lock lock{mutex};
    const State* s = Live();
    return s != nullptr ? s->layout : vk::DescriptorSetLayout{};
}

vk::DescriptorSet Set() {
    std::scoped_lock lock{mutex};
    const State* s = Live();
    return s != nullptr ? s->set : vk::DescriptorSet{};
}

u32 BufferSlotLocked(vk::Buffer buffer);
u32 BufferSlot(vk::Buffer buffer) {
    if (!buffer) {
        return 0;
    }
    const u64 handle = reinterpret_cast<u64>(static_cast<VkBuffer>(buffer));
    const u64 epoch = g_epoch.load(std::memory_order_acquire);
    MemoEntry& m = MemoFor(0, handle, 0);
    if (m.handle == handle && m.epoch == epoch) {
        return m.slot;
    }
    const u32 slot = BufferSlotLocked(buffer);
    if (slot != 0) {
        m = {handle, epoch, slot};
    }
    return slot;
}
u32 BufferSlotLocked(vk::Buffer buffer) {
    std::scoped_lock lock{mutex};
    State* s = Live();
    if (s == nullptr) {
        return 0;
    }
    bool made;
    const u32 slot = SlotOf(*s, BuffersTable, reinterpret_cast<u64>(static_cast<VkBuffer>(buffer)), made);
    if (made) {
        WriteBuffer(*s, slot, buffer);
        s->buffer_new.fetch_add(1, std::memory_order_relaxed);
    }
    return slot;
}

u32 ImageSlotLocked(u32 key, vk::ImageView view);
u32 ImageSlot(u32 key, vk::ImageView view) {
    if (!view || key >= I::NumImageArrays) {
        return 0;
    }
    const u64 handle = reinterpret_cast<u64>(static_cast<VkImageView>(view)) ^ (u64{key} << 56);
    const u64 epoch = g_epoch.load(std::memory_order_acquire);
    MemoEntry& m = MemoFor(2, handle, key);
    if (m.handle == handle && m.epoch == epoch) {
        return m.slot;
    }
    const u32 slot = ImageSlotLocked(key, view);
    if (slot != 0) {
        m = {handle, epoch, slot};
    }
    return slot;
}
u32 ImageSlotLocked(u32 key, vk::ImageView view) {
    std::scoped_lock lock{mutex};
    State* s = Live();
    if (s == nullptr) {
        return 0;
    }
    bool made;
    const u32 table = FirstImageTable + key;
    const u32 slot = SlotOf(*s, table, reinterpret_cast<u64>(static_cast<VkImageView>(view)), made);
    if (made) {
        WriteImage(*s, table, slot, view, {});
        s->image_new.fetch_add(1, std::memory_order_relaxed);
    }
    return slot;
}

u32 SamplerSlotLocked(vk::Sampler sampler);
u32 SamplerSlot(vk::Sampler sampler) {
    if (!sampler) {
        return 0;
    }
    const u64 handle = reinterpret_cast<u64>(static_cast<VkSampler>(sampler));
    const u64 epoch = g_epoch.load(std::memory_order_acquire);
    MemoEntry& m = MemoFor(1, handle, 0);
    if (m.handle == handle && m.epoch == epoch) {
        return m.slot;
    }
    const u32 slot = SamplerSlotLocked(sampler);
    if (slot != 0) {
        m = {handle, epoch, slot};
    }
    return slot;
}
u32 SamplerSlotLocked(vk::Sampler sampler) {
    std::scoped_lock lock{mutex};
    State* s = Live();
    if (s == nullptr) {
        return 0;
    }
    bool made;
    const u32 slot = SlotOf(*s, SamplersTable, reinterpret_cast<u64>(static_cast<VkSampler>(sampler)), made);
    if (made) {
        WriteImage(*s, SamplersTable, slot, {}, sampler);
        s->sampler_new.fetch_add(1, std::memory_order_relaxed);
    }
    return slot;
}

void ForgetBuffer(vk::Buffer buffer) {
    Forget(BuffersTable, BuffersTable, reinterpret_cast<u64>(static_cast<VkBuffer>(buffer)));
}

void ForgetView(vk::ImageView view) {
    Forget(FirstImageTable, NumTables - 1, reinterpret_cast<u64>(static_cast<VkImageView>(view)));
}

nlohmann::json Report() {
    std::scoped_lock lock{mutex};
    if (state == nullptr || !state->fits) {
        return {{"on", false}};
    }
    const State& s = *state;
    return {{"on", true},
            {"slots_new_buffer_image_sampler", {s.buffer_new.load(), s.image_new.load(), s.sampler_new.load()}},
            {"hits", s.hits.load()},
            {"freed", s.freed.load()},
            {"exhausted", s.exhausted.load()},
            {"live_buffers", s.tables[BuffersTable].map.size()}};
}

} // namespace Core::Vkb::Indexed
