// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_aa.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <vk_mem_alloc.h>

#include "common/logging/log.h"
#include "ffx_vk_portable.h"
#include "native/native_vkb_objects.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"

namespace Core::Vkb::Aa {
namespace {

enum class Kind : u32 { Off, Fsr3 };

Kind Chosen() {
    static const Kind k = [] {
        const char* e = std::getenv("TLG_VKB_AA");
        if (e != nullptr && (std::strcmp(e, "fsr3") == 0 || std::strcmp(e, "1") == 0)) {
            return Kind::Fsr3;
        }
        return Kind::Off;
    }();
    return k;
}

const VkbVulkan::Instance* instance = nullptr;

struct Input {
    vk::Image image{};
    vk::Format format{};
    u32 width{}, height{};
    bool depth{};
};
/// The newest motion pass's attachments (recording threads, under `mutex`)
std::mutex mutex;
Input scene, motion, depth;
u64 inputs_serial = 0, used_serial = 0;

FfxVkPortableUpscaleContext* context = nullptr;
u32 ctx_w = 0, ctx_h = 0;
VkImage output = VK_NULL_HANDLE;
VmaAllocation output_alloc = nullptr;
u64 frame_id = 0;
std::chrono::steady_clock::time_point last_run{};
bool failed = false;

std::atomic<u64> c_motion_passes{0}, c_runs{0}, c_no_inputs{0}, c_unusable{0}, c_failed{0}, c_resets{0};

bool Facts(vk::Image image, Input& out) {
    out.image = image;
    return image && Core::Vkb::ImageFacts(image, out.format, out.width, out.height, out.depth);
}

vk::ImageUsageFlags UsageOf(const Input& in) {
    // the rule our images are made with (native_vkb_objects.cpp): transfer and sampled, plus the attachment use these targets have
    return vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
           (in.depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment : vk::ImageUsageFlagBits::eColorAttachment);
}

FfxVkPortableImage Describe(VkImage image, VkFormat format, u32 w, u32 h, VkImageUsageFlags usage, VkImageAspectFlags aspect, FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = format;
    out.extent = {w, h};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = usage;
    out.aspect = aspect;
    out.state = state;
    return out;
}

void DestroyContext() {
    if (context != nullptr) {
        (void)instance->GetDevice().waitIdle();
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    if (output != VK_NULL_HANDLE) {
        vmaDestroyImage(instance->GetAllocator(), output, output_alloc);
        output = VK_NULL_HANDLE, output_alloc = nullptr;
    }
}

bool EnsureContext(u32 w, u32 h) {
    if (context != nullptr && ctx_w == w && ctx_h == h) {
        return true;
    }
    DestroyContext();
    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance->GetInstance();
    device_info.physicalDevice = instance->GetPhysicalDevice();
    device_info.device = instance->GetDevice();
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance->GetGraphicsQueue();
    device_info.queueFamilyIndex = instance->GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance->IsShaderFloat16Supported() ? VK_TRUE : VK_FALSE;
    device_info.subgroupSizeControlEnabled = VK_TRUE;
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;
    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE; // native AA runs on the HDR scene colour
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {w, h};
    if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info); issues != 0) {
        LOG_ERROR(Render_Vulkan, "vkb aa: FSR 3.1 create info rejected ({:#x})", issues);
        return false;
    }
    if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) != FFX_VK_PORTABLE_OK) {
        LOG_ERROR(Render_Vulkan, "vkb aa: FSR 3.1 context creation failed");
        context = nullptr;
        return false;
    }
    const VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                               .extent = {w, h, 1},
                               .mipLevels = 1,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(instance->GetAllocator(), &ci, &alloc, &output, &output_alloc, nullptr) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "vkb aa: output image failed");
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
        return false;
    }
    ctx_w = w, ctx_h = h;
    frame_id = 0;
    LOG_INFO(Render_Vulkan, "vkb aa: FSR 3.1 native AA {}x{} on the game's motion vectors", w, h);
    return true;
}

void Barrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                               .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands, .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
}

} // namespace

void Init(const VkbVulkan::Instance& instance_) {
    instance = &instance_;
}

bool On() {
    return Chosen() != Kind::Off;
}

bool Running();

bool JitterWanted() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_AA_JITTER"); return e == nullptr || e[0] != '0'; }();
    return on;
}

// F1c state: the game's render thread only
u64 scene_depth_view = 0;
std::array<float, 2> frame_jitter{};
u32 jitter_index = 0;
std::unordered_set<u64> jittered_meshes;
std::atomic<u64> c_jittered{0}, c_jittered_meshes{0};

float Halton(u32 index, u32 base) {
    float f = 1.0f, r = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
    }
    return r;
}

u64 scene_depth_other = 0; ///< the scene depth is double-buffered (frames alternate two textures, census 2026-11-16): both recent ones count
void NoteSceneDepth(u64 depth_view_id) {
    if (depth_view_id != scene_depth_view) {
        scene_depth_other = scene_depth_view;
        scene_depth_view = depth_view_id;
    }
}

std::unordered_map<u64, std::array<u64, 4>> depth_census; ///< TLG_VKB_AA_CENSUS=1: depth id -> draws [write+geometry, write, test only, total] (render thread)
bool CensusOn() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_AA_CENSUS"); return e != nullptr && e[0] == '1'; }();
    return on;
}

bool DrawJitter(u64 depth_view_id, bool depth_write, bool geometry, u64 mesh_key, std::array<float, 2>& jitter) {
    if (CensusOn()) {
        auto& c = depth_census[depth_view_id];
        c[0] += depth_write && geometry, c[1] += depth_write, c[2] += !depth_write, c[3] += 1;
    }
    if (!JitterWanted() || !Running() || depth_view_id == 0 || (depth_view_id != scene_depth_view && depth_view_id != scene_depth_other)) {
        return false;
    }
    bool on = false;
    if (geometry && depth_write) {
        on = true;
        jittered_meshes.insert(mesh_key);
    } else if (mesh_key != 0 && jittered_meshes.contains(mesh_key)) {
        on = true; // a redraw of a mesh drawn jittered into this depth takes the same shift (water over its pre-pass)
        c_jittered_meshes.fetch_add(1, std::memory_order_relaxed);
    }
    if (on) {
        jitter = frame_jitter;
        c_jittered.fetch_add(1, std::memory_order_relaxed);
    }
    return on;
}

std::array<float, 2> OnCombineQueued() {
    const std::array<float, 2> used = frame_jitter;
    if (CensusOn()) {
        static u32 frames = 0;
        if (++frames % 300 == 0) {
            std::string line;
            for (const auto& [id, c] : depth_census) {
                line += fmt::format(" [{:#x}: wg {} w {} t {} all {}]", id, c[0], c[1], c[2], c[3]);
            }
            LOG_WARNING(Render_Vulkan, "vkb aa census (300 frames): scene depth {:#x}, running {}:{}", scene_depth_view, Running(), line);
        }
        if (frames % 300 == 0) {
            depth_census.clear();
        }
    }
    jittered_meshes.clear();
    if (JitterWanted() && Running()) {
        jitter_index = jitter_index % 8 + 1; // native size: 8 phases (their JitterPhases at a ratio of 1)
        frame_jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    } else {
        frame_jitter = {};
    }
    return used;
}

bool Running() {
    return On() && !failed && c_runs.load(std::memory_order_relaxed) != 0;
}

void NoteMotionPass(std::span<const vk::Image> attachments, u32 color_count, bool has_depth) {
    c_motion_passes.fetch_add(1, std::memory_order_relaxed);
    if (color_count < 3 || !has_depth || attachments.size() < color_count + 1) {
        return;
    }
    Input s, m, d;
    if (!Facts(attachments[0], s) || !Facts(attachments[2], m) || !Facts(attachments[color_count], d)) {
        return;
    }
    std::scoped_lock lock{mutex};
    scene = s, motion = m, depth = d;
    ++inputs_serial;
}

void BeforeCombine(vk::CommandBuffer cmd, std::array<float, 2> jitter) {
    if (!On() || instance == nullptr || failed) {
        return;
    }
    Input s, m, d;
    {
        std::scoped_lock lock{mutex};
        if (inputs_serial == used_serial) {
            c_no_inputs.fetch_add(1, std::memory_order_relaxed);
            return; // no motion pass since the last run: a menu, a loading screen
        }
        used_serial = inputs_serial;
        s = scene, m = motion, d = depth;
    }
    const u32 w = s.width, h = s.height;
    if (s.format != vk::Format::eB10G11R11UfloatPack32 || m.format != vk::Format::eR16G16Sfloat || m.width != w || m.height != h || d.width != w || d.height != h ||
        !d.depth) {
        if (c_unusable.fetch_add(1, std::memory_order_relaxed) == 0) {
            LOG_WARNING(Render_Vulkan, "vkb aa: inputs unusable (scene {} {}x{}, motion {} {}x{}, depth {} {}x{})", vk::to_string(s.format), s.width, s.height,
                        vk::to_string(m.format), m.width, m.height, vk::to_string(d.format), d.width, d.height);
        }
        return;
    }
    if (!EnsureContext(w, h)) {
        failed = true;
        c_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_run).count();
    const bool reset = frame_id == 0 || frame_ms > 100.0f; // fresh history after a gap (menu, load, cut)
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_run = now;
    if (reset) {
        c_resets.fetch_add(1, std::memory_order_relaxed);
    }

    Barrier(cmd);
    // the output starts each frame in GENERAL (FSR writes all of it)
    const vk::ImageMemoryBarrier2 out_barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands, .srcAccessMask = vk::AccessFlagBits2::eNone,
                                              .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                              .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                                              .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral, .image = vk::Image{output},
                                              .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &out_barrier});

    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.commandBuffer = cmd;
    info.color = Describe(s.image, static_cast<VkFormat>(s.format), w, h, static_cast<VkImageUsageFlags>(UsageOf(s)), VK_IMAGE_ASPECT_COLOR_BIT,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(d.image, static_cast<VkFormat>(d.format), w, h, static_cast<VkImageUsageFlags>(UsageOf(d)), VK_IMAGE_ASPECT_DEPTH_BIT,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    // the game's motion target: UV units, current - previous, unjittered (their camera_motion.comp mode 2); FSR wants pixels toward the previous frame
    info.motionVectors = Describe(m.image, static_cast<VkFormat>(m.format), w, h, static_cast<VkImageUsageFlags>(UsageOf(m)), VK_IMAGE_ASPECT_COLOR_BIT,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(output, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    info.jitterOffset = {jitter[0], jitter[1]}; // F1c: the offset this frame's geometry had (pixels; their sign convention)
    info.motionVectorScale = {-static_cast<float>(w), -static_cast<float>(h)};
    info.renderSize = {w, h};
    info.outputSize = {w, h};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = 0.1f; // their fallback: the game's camera is not read yet
    info.cameraFar = 3000.0f;
    info.cameraVerticalFovRadians = 1.0f;
    info.viewSpaceToMeters = 1.0f;
    static const float sharpness = [] { const char* e = std::getenv("TLG_VKB_AA_SHARPNESS"); return e != nullptr ? static_cast<float>(std::atof(e)) : 0.5f; }();
    info.sharpness = std::min(std::max(sharpness, 0.0f), 1.0f);
    info.enableSharpening = sharpness > 0.0f ? VK_TRUE : VK_FALSE;
    info.reset = reset ? VK_TRUE : VK_FALSE;
    info.frameId = frame_id++;
    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {w, h};
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info); issues != 0) {
        LOG_ERROR(Render_Vulkan, "vkb aa: FSR 3.1 dispatch rejected ({:#x}); AA off", issues);
        failed = true;
        c_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) != FFX_VK_PORTABLE_OK) {
        LOG_ERROR(Render_Vulkan, "vkb aa: FSR 3.1 dispatch failed; AA off");
        failed = true;
        c_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Barrier(cmd);
    // the result replaces the scene colour (B10G11R11: no alpha to keep); both images stay in GENERAL (note 66)
    const vk::ImageBlit region{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                               .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(w), static_cast<s32>(h), 1}},
                               .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                               .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(w), static_cast<s32>(h), 1}}};
    cmd.blitImage(vk::Image{output}, vk::ImageLayout::eGeneral, s.image, vk::ImageLayout::eGeneral, region, vk::Filter::eNearest);
    Barrier(cmd);
    c_runs.fetch_add(1, std::memory_order_relaxed);
}

nlohmann::json Report() {
    return {{"on", On()},
            {"kind", Chosen() == Kind::Fsr3 ? "fsr3" : "off"},
            {"size", fmt::format("{}x{}", ctx_w, ctx_h)},
            {"motion_passes", c_motion_passes.load()},
            {"runs", c_runs.load()},
            {"combines_without_inputs", c_no_inputs.load()},
            {"unusable", c_unusable.load()},
            {"failed", c_failed.load()},
            {"history_resets", c_resets.load()},
            {"jitter", JitterWanted()},
            {"jittered_draws", c_jittered.load()},
            {"jittered_mesh_redraws", c_jittered_meshes.load()}};
}

} // namespace Core::Vkb::Aa
