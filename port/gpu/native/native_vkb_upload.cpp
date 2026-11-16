// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_flight.h"
#include "native/native_vkb_upload.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "native/native_desc.h"
#include "native/native_image_desc.h"
#include "native/ps4_data_layout/tiling.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_macro_128_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_macro_16_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_macro_32_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_macro_64_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_macro_8_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_macro_96_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_micro_128_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_micro_16_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_micro_32_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_micro_64_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_micro_8_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_tiling_micro_96_comp.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_host_import.h"
#include "native/native_vkb_objects.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_scheduler.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_shader_util.h"
#include "native/vkb_core/image_info.h"
#include "native/vkb_core/tile.h"

#include <vulkan/vulkan_format_traits.hpp>
#include <vk_mem_alloc.h>

namespace Core::Vkb::Upload {

namespace {

/// The tiling shaders' parameters (the same layout as VideoCore::TileManager's)
struct TilingInfo {
    u32 bank_swizzle;
    u32 micro_tiled_mips;
    u32 num_mips;
    std::array<VideoCore::Vkb::MipInfo, VideoCore::Vkb::MAX_MIPS> mips;
};

struct OwnedBuffer {
    VkBuffer buffer{};
    VmaAllocation allocation{};
};
struct Batch {
    vk::CommandBuffer cmd{};
    u64 value{};
    std::vector<OwnedBuffer> buffers;
    u32 uploads{};
};

std::mutex mutex;
const VkbVulkan::Instance* instance = nullptr;
VkbVulkan::HostImport* host_import = nullptr;
vk::CommandPool pool{};
vk::Semaphore timeline{};
u64 next_value = 0;
Batch current;
std::deque<Batch> in_flight;
vk::DescriptorSetLayout set_layout{};
vk::PipelineLayout pipeline_layout{};
std::unordered_map<u64, vk::Pipeline> pipelines; ///< by (tile mode, bits, samples)

std::atomic<u64> c_uploads{0}, c_tiled{0}, c_linear{0}, c_bytes{0}, c_no_desc{0}, c_not_in_place{0}, c_texel_mismatch{0}, c_samples{0}, c_unsupported_bits{0}, c_batches{0},
    c_record_ns{0}, c_pipelines{0}, c_alloc_failed{0};

u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool MakeBuffer(u64 size, vk::BufferUsageFlags usage, bool host, OwnedBuffer& out, void** mapped = nullptr) {
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = static_cast<VkBufferUsageFlags>(usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc{};
    alloc.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance->GetAllocator(), &ci, &alloc, &out.buffer, &out.allocation, &info) != VK_SUCCESS) {
        c_alloc_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (mapped != nullptr) {
        *mapped = info.pMappedData;
    }
    return true;
}

/// The detiling pipeline of a layout (what VideoCore::TileManager::GetTilingPipeline builds, with our own layout); null when the bit width has no shader
vk::Pipeline DetilePipeline(const VideoCore::Vkb::ImageInfo& info) {
    const u64 key = u64(info.tile_mode) | u64(info.num_bits) << 16 | u64(info.num_samples) << 32;
    if (const auto it = pipelines.find(key); it != pipelines.end()) {
        return it->second;
    }
    std::span<const u32> code;
    const bool is_macro = Ps4Layout::IsMacroTiled(info.array_mode);
    switch (info.num_bits) {
    case 8: code = is_macro ? std::span<const u32>{VKB_TILING_MACRO_8_COMP} : std::span<const u32>{VKB_TILING_MICRO_8_COMP}; break;
    case 16: code = is_macro ? std::span<const u32>{VKB_TILING_MACRO_16_COMP} : std::span<const u32>{VKB_TILING_MICRO_16_COMP}; break;
    case 32: code = is_macro ? std::span<const u32>{VKB_TILING_MACRO_32_COMP} : std::span<const u32>{VKB_TILING_MICRO_32_COMP}; break;
    case 64: code = is_macro ? std::span<const u32>{VKB_TILING_MACRO_64_COMP} : std::span<const u32>{VKB_TILING_MICRO_64_COMP}; break;
    case 96: code = is_macro ? std::span<const u32>{VKB_TILING_MACRO_96_COMP} : std::span<const u32>{VKB_TILING_MICRO_96_COMP}; break;
    case 128: code = is_macro ? std::span<const u32>{VKB_TILING_MACRO_128_COMP} : std::span<const u32>{VKB_TILING_MICRO_128_COMP}; break;
    default: return {};
    }
    const auto micro_tile_mode = Ps4Layout::GetMicroTileMode(info.tile_mode);
    std::array<u32, 12> spec{info.num_samples, u32(micro_tile_mode), Ps4Layout::GetMicroTileThickness(info.array_mode), 0u};
    if (is_macro) {
        const auto macro_tile_mode = Ps4Layout::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec[4] = u32(info.array_mode);
        spec[5] = u32(Ps4Layout::GetPipeConfig(info.tile_mode));
        spec[6] = Ps4Layout::GetBankWidth(macro_tile_mode);
        spec[7] = Ps4Layout::GetBankHeight(macro_tile_mode);
        spec[8] = Ps4Layout::GetNumBanks(macro_tile_mode);
        spec[9] = std::bit_width(spec[8]) - 1;
        spec[10] = Ps4Layout::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode, info.num_bits);
        spec[11] = Ps4Layout::GetMacrotileAspect(macro_tile_mode);
    }
    std::array<vk::SpecializationMapEntry, 12> entries{};
    for (u32 i = 0; i < entries.size(); ++i) {
        entries[i] = vk::SpecializationMapEntry{i, i * u32(sizeof(u32)), sizeof(u32)};
    }
    const u32 count = is_macro ? 12 : 4;
    const vk::SpecializationInfo specialization{.mapEntryCount = count, .pMapEntries = entries.data(), .dataSize = count * sizeof(u32), .pData = spec.data()};
    const vk::Device device = instance->GetDevice();
    const vk::ShaderModule module = VkbVulkan::CompileSPV(code, device);
    const vk::ComputePipelineCreateInfo ci{
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main", .pSpecializationInfo = &specialization},
        .layout = pipeline_layout,
    };
    const auto [result, pipeline] = device.createComputePipeline(VK_NULL_HANDLE, ci);
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) {
        return {};
    }
    pipelines.emplace(key, pipeline);
    c_pipelines.fetch_add(1, std::memory_order_relaxed);
    return pipeline;
}

vk::CommandBuffer Cmd() {
    if (!current.cmd) {
        const vk::CommandBufferAllocateInfo ai{.commandPool = pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1};
        current.cmd = instance->GetDevice().allocateCommandBuffers(ai).value[0];
        (void)current.cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    }
    return current.cmd;
}

void CollectLocked() {
    if (in_flight.empty()) {
        return;
    }
    const u64 done = instance->GetDevice().getSemaphoreCounterValue(timeline).value;
    while (!in_flight.empty() && in_flight.front().value <= done) {
        Batch& b = in_flight.front();
        for (const OwnedBuffer& o : b.buffers) {
            vmaDestroyBuffer(instance->GetAllocator(), o.buffer, o.allocation);
        }
        instance->GetDevice().freeCommandBuffers(pool, b.cmd);
        in_flight.pop_front();
    }
}

} // namespace

void Init(const VkbVulkan::Instance& instance_, VkbVulkan::HostImport& host_import_) {
    std::scoped_lock lock{mutex};
    if (instance != nullptr) {
        return;
    }
    instance = &instance_;
    host_import = &host_import_;
    const vk::Device device = instance->GetDevice();
    pool = device.createCommandPool({.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer, .queueFamilyIndex = instance->GetGraphicsQueueFamilyIndex()}).value;
    const vk::SemaphoreTypeCreateInfo type{.semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0};
    timeline = device.createSemaphore({.pNext = &type}).value;
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {.binding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    set_layout = device.createDescriptorSetLayout({.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR, .bindingCount = 3, .pBindings = bindings.data()}).value;
    pipeline_layout = device.createPipelineLayout({.setLayoutCount = 1, .pSetLayouts = &set_layout}).value;
}

bool TextureContents(u64 texture_id, vk::Image image, vk::Format format, u32 mips, u32 layers, vk::ImageLayout final_layout) {
    const u64 t0 = NowNs();
    NativeDesc::ImageDesc desc;
    if (instance == nullptr || !NativeImageDesc::DeriveDesc(texture_id, 0, desc)) {
        c_no_desc.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const VideoCore::Vkb::ImageInfo info{desc, false};
    if (info.num_samples > 1) {
        c_samples.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (u32(vk::blockSize(format)) * 8 != info.num_bits) {
        c_texel_mismatch.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const BufferWindow window = SourceWindow(info.guest_address, info.guest_size); // R3a: a dynamic texture's Map block can be in our device ring
    if (!window.buffer || info.guest_size == 0) {
        c_not_in_place.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u32 levels = std::min<u32>(mips, info.resources.levels);
    std::vector<vk::BufferImageCopy> copies;
    for (u32 m = 0; m < levels; ++m) {
        const u32 width = std::max(info.size.width >> m, 1u);
        const u32 height = std::max(info.size.height >> m, 1u);
        const u32 depth = info.props.is_volume ? std::max(info.size.depth >> m, 1u) : 1u;
        const auto& mip = info.mips_layout[m];
        copies.push_back({
            .bufferOffset = mip.offset,
            .bufferRowLength = mip.pitch,
            .bufferImageHeight = mip.height,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, m, 0, std::min<u32>(layers, info.resources.layers)},
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip.pitch ? std::min<u32>(mip.pitch, width) : width, mip.height ? std::min<u32>(mip.height, height) : height, depth},
        });
    }

    std::scoped_lock lock{mutex};
    vk::Buffer source = window.buffer;
    u64 source_offset = window.offset;
    OwnedBuffer linear{}, params{};
    const vk::CommandBuffer cmd = Cmd();
    if (info.props.is_tiled) {
        const vk::Pipeline pipeline = DetilePipeline(info);
        if (!pipeline) {
            c_unsupported_bits.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        void* mapped = nullptr;
        if (!MakeBuffer(info.guest_size, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc, false, linear) ||
            !MakeBuffer(sizeof(TilingInfo), vk::BufferUsageFlagBits::eUniformBuffer, true, params, &mapped) || mapped == nullptr) {
            if (linear.buffer) {
                vmaDestroyBuffer(instance->GetAllocator(), linear.buffer, linear.allocation);
            }
            return false;
        }
        TilingInfo p{};
        p.bank_swizzle = info.bank_swizzle;
        p.micro_tiled_mips = info.micro_tiled_mips;
        p.num_mips = info.resources.levels;
        for (u32 m = 0; m < p.num_mips && m < VideoCore::Vkb::MAX_MIPS; ++m) {
            p.mips[m] = info.mips_layout[m];
            if (info.props.is_block) {
                p.mips[m].pitch = std::max<u16>((p.mips[m].pitch + 3) / 4, 1U);
                p.mips[m].height = std::max<u16>((p.mips[m].height + 3) / 4, 1U);
            }
        }
        std::memcpy(mapped, &p, sizeof(p));
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        const vk::DescriptorBufferInfo tiled_info{.buffer = window.buffer, .offset = window.offset, .range = info.guest_size};
        const vk::DescriptorBufferInfo linear_info{.buffer = vk::Buffer{linear.buffer}, .offset = 0, .range = info.guest_size};
        const vk::DescriptorBufferInfo params_info{.buffer = vk::Buffer{params.buffer}, .offset = 0, .range = sizeof(TilingInfo)};
        const std::array<vk::WriteDescriptorSet, 3> writes{{
            {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &tiled_info},
            {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &linear_info},
            {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &params_info},
        }};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout, 0, writes);
        cmd.dispatch((info.guest_size / (info.num_bits / 8)) / 64, 1, 1);
        const vk::MemoryBarrier2 written{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader, .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eCopy, .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        source = vk::Buffer{linear.buffer};
        source_offset = 0;
        current.buffers.push_back(linear);
        current.buffers.push_back(params);
        c_tiled.fetch_add(1, std::memory_order_relaxed);
    } else {
        c_linear.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& copy : copies) {
        copy.bufferOffset += source_offset;
    }
    const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    const vk::ImageMemoryBarrier2 to_dst{.srcStageMask = vk::PipelineStageFlagBits2::eNone, .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                                         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite, .oldLayout = vk::ImageLayout::eUndefined,
                                         .newLayout = vk::ImageLayout::eTransferDstOptimal, .image = image, .subresourceRange = all};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
    cmd.copyBufferToImage(source, image, vk::ImageLayout::eTransferDstOptimal, copies);
    const vk::ImageMemoryBarrier2 to_final{.srcStageMask = vk::PipelineStageFlagBits2::eCopy, .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                           .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                           .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                                           .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = final_layout, .image = image, .subresourceRange = all};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_final});
    ++current.uploads;
    c_uploads.fetch_add(1, std::memory_order_relaxed);
    c_bytes.fetch_add(info.guest_size, std::memory_order_relaxed);
    c_record_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    return true;
}

void CopyBuffer(vk::Buffer source, u64 source_offset, vk::Buffer destination, u64 destination_offset, u64 bytes) {
    std::scoped_lock lock{mutex};
    if (instance == nullptr) {
        return;
    }
    Cmd().copyBuffer(source, destination, vk::BufferCopy{source_offset, destination_offset, bytes});
}

void InitLayout(vk::Image image) {
    std::scoped_lock lock{mutex};
    if (instance == nullptr) {
        return;
    }
    const vk::ImageMemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eNone,
                                          .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                          .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                                          .oldLayout = vk::ImageLayout::eUndefined,
                                          .newLayout = vk::ImageLayout::eGeneral,
                                          .image = image,
                                          .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    Cmd().pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

void Submit() {
    std::scoped_lock lock{mutex};
    if (instance == nullptr) {
        return;
    }
    CollectLocked();
    if (!current.cmd) {
        return;
    }
    (void)current.cmd.end();
    current.value = ++next_value;
    const vk::CommandBufferSubmitInfo cmd_info{.commandBuffer = current.cmd};
    const vk::SemaphoreSubmitInfo signal{.semaphore = timeline, .value = current.value, .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
    const vk::SubmitInfo2 submit{.commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_info, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
    {
        std::scoped_lock queue_lock{VkbVulkan::Scheduler::submit_mutex};
        Core::Flight::Add(Core::Flight::Kind::QueueSubmit, 0, current.value, 2); // flight recorder: an upload batch (b = 2: the upload stream)
        const vk::Result result = instance->GetGraphicsQueue().submit2(submit);
        if (result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "vkb: upload batch submit failed: {}", vk::to_string(result));
        }
    }
    c_batches.fetch_add(1, std::memory_order_relaxed);
    in_flight.push_back(std::move(current));
    current = {};
}

nlohmann::json Report() {
    const u64 n = c_uploads.load();
    return {{"textures_uploaded", n},
            {"tiled", c_tiled.load()},
            {"linear", c_linear.load()},
            {"mb_uploaded", c_bytes.load() >> 20},
            {"record_us_mean", n ? static_cast<double>(c_record_ns.load()) / 1e3 / static_cast<double>(n) : 0.0},
            {"batches_submitted", c_batches.load()},
            {"detile_pipelines", c_pipelines.load()},
            {"skipped_no_engine_description", c_no_desc.load()},
            {"skipped_memory_not_in_one_chunk", c_not_in_place.load()},
            {"skipped_texel_size_differs", c_texel_mismatch.load()},
            {"skipped_multisampled", c_samples.load()},
            {"skipped_bit_width_without_shader", c_unsupported_bits.load()},
            {"staging_allocations_failed", c_alloc_failed.load()}};
}

} // namespace Core::Vkb::Upload
