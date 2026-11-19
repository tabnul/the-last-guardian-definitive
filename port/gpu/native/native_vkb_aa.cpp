// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_aa.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>

#include <nlohmann/json.hpp>
#include <vk_mem_alloc.h>

#include "native/shadps4/video_core/renderer_vulkan/vk_scheduler.h"
#include "common/logging/log.h"
#include "ffx_vk_portable.h"
#include "native/native_vkb_dlss_glue.h"
#include "native/native_vkb_stream.h"
#include "native/native_vkb_objects.h"
#include "native/shadps4/video_core/host_shaders/vkb_aa_motion_comp.h"
#include "native/shadps4/video_core/host_shaders/vkb_rcas_comp.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"
#include "tlg_settings.h"

namespace {
/// a device-wide wait: every queue must be left alone meanwhile (Vulkan), so the backend's recording and upload threads wait at the queue lock, as the
/// presenter's swapchain recreate does (a wait without it lost the device, owner play 2026-11-16)
void IdleDevice(vk::Device device) {
    std::scoped_lock queue_lock{VkbVulkan::Scheduler::submit_mutex};
    (void)device.waitIdle();
}
} // namespace

namespace Core::Vkb::Aa {
namespace {

enum class Kind : u32 { Off, Fsr3, Dlaa };

} // namespace

bool PicturePart(char part) {
    static const std::string parts = [] {
        const char* e = std::getenv("TLG_VKB_PICTURE_SETTINGS");
        const std::string v = e != nullptr ? e : "";
        return v.empty() || v == "1" ? std::string("fsba") : v == "0" ? std::string() : v;
    }();
    return parts.find(part) != std::string::npos;
}

namespace {

Kind Chosen() {
    static const Kind k = [] {
        const char* e = std::getenv("TLG_VKB_AA");
        if (e != nullptr && (std::strcmp(e, "fsr3") == 0 || std::strcmp(e, "1") == 0)) {
            return Kind::Fsr3;
        }
        if (e != nullptr && std::strcmp(e, "dlaa") == 0) {
            if (Core::Vkb::DlssGlue::Wanted()) {
                return Kind::Dlaa;
            }
            if (PicturePart('f')) {
                // the normal renderer's rule (TlgSettings::ConfigureDlssSupport): DLAA without DLSS is FSR 3.1 native AA, on every machine
                LOG_INFO(Render_Vulkan, "vkb aa: DLAA asked for, but tlg_dlss.dll / nvngx_dlss.dll are not next to the executable: FSR 3.1 native AA instead");
                return Kind::Fsr3;
            }
            LOG_WARNING(Render_Vulkan, "vkb aa: DLAA asked for, but tlg_dlss.dll / nvngx_dlss.dll are not next to the executable: AA off");
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
std::atomic<u64> c_rcas_passes{0}, c_scene_bias_draws{0};

// Issue 42 (part s): RCAS passes after the AA, as the normal renderer's ExtraSharpen / DlssSharpen; the result ping-pongs between `output` and this image
VkImage sharpen_img = VK_NULL_HANDLE;
VmaAllocation sharpen_alloc = nullptr;
vk::ImageView sharpen_view{};
vk::DescriptorSetLayout rcas_set_layout{};
vk::PipelineLayout rcas_layout{};
vk::Pipeline rcas_pipeline{};
bool rcas_broken = false; ///< the pipeline could not be made (logged once, in the report): the AA's own sharpening only

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

// F1d (DLAA): the motion in pixels (vkb_aa_motion.comp), views of the inputs (by image), the conversion pipeline
/// F2: the motion and depth copies rotate over this many frames, each published with the timeline value of its command buffer; the presenter takes the newest whose
/// value the GPU has passed (the frame being flipped; the recorder may already record the next ones into other slots). Upstream records frame generation on the frame's
/// own scheduler; ours is the presenter's, so this stands in for that order.
constexpr u32 FgSlots = 4;
std::array<VkImage, FgSlots> motion_pxs{};
std::array<VmaAllocation, FgSlots> motion_px_allocs{};
std::array<vk::ImageView, FgSlots> motion_px_views{};
u32 px_index = 0; ///< the slot of this run
VkImage motion_px = VK_NULL_HANDLE;
vk::ImageView motion_px_view{}, output_view{};
// F2: the newest frame's inputs for frame generation
std::mutex fg_mutex;
struct FgSlot {
    FgInputs inputs{};
    u64 value = 0;  ///< the stream timeline value the slot's commands go out with
    u64 serial = 0; ///< 0: empty
};
std::array<FgSlot, FgSlots> fg_slots{};
u64 fg_serial = 0, fg_taken = 0;
// F2: our copy of the scene depth (double-buffered with the motion): the presenter reads only images the AA owns, the backend frees the game's (loading)
std::array<VkImage, FgSlots> depth_copies{};
std::array<VmaAllocation, FgSlots> depth_copy_allocs{};
std::array<vk::ImageView, FgSlots> depth_copy_views{};
vk::Format depth_copy_format{};
u32 depth_copy_w = 0, depth_copy_h = 0;
vk::DescriptorSetLayout motion_set_layout{};
vk::PipelineLayout motion_layout{};
vk::Pipeline motion_pipeline{};
u32 dl_w = 0, dl_h = 0;

/// A view of one of the game's images for this run only: retired through the backend once the submit is done (a cache by handle went stale when the backend
/// freed an image during loading and a new one got the same handle: device lost, owner play 2026-11-16)
vk::ImageView ViewOf(VkImage image, vk::Format format, vk::ImageAspectFlags aspect) {
    const auto [r, view] = instance->GetDevice().createImageView(
        {.image = vk::Image{image}, .viewType = vk::ImageViewType::e2D, .format = format, .subresourceRange = {aspect, 0, 1, 0, 1}});
    if (r != vk::Result::eSuccess) {
        return {};
    }
    Core::Vkb::RetireView(view);
    return view;
}

void DestroyDepthCopies() {
    {
        std::scoped_lock fg_lock{fg_mutex};
        fg_slots = {};
    }
    for (u32 i = 0; i < FgSlots; ++i) {
        if (depth_copies[i] != VK_NULL_HANDLE) {
            instance->GetDevice().destroyImageView(depth_copy_views[i]);
            vmaDestroyImage(instance->GetAllocator(), depth_copies[i], depth_copy_allocs[i]);
            depth_copies[i] = VK_NULL_HANDLE;
        }
    }
}

/// F2: our depth images at the render size and the game's depth format (destroyed only after a device-wide wait)
bool EnsureDepthCopies(u32 w, u32 h, vk::Format format) {
    if (depth_copies[0] != VK_NULL_HANDLE && depth_copy_format == format && depth_copy_w == w && depth_copy_h == h) {
        return true;
    }
    DestroyDepthCopies(); // EnsureDlaa has idled the device on a size change; a format change idles here
    const VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = static_cast<VkFormat>(format),
                               .extent = {w, h, 1},
                               .mipLevels = 1,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    for (u32 i = 0; i < FgSlots; ++i) {
        if (vmaCreateImage(instance->GetAllocator(), &ci, &alloc, &depth_copies[i], &depth_copy_allocs[i], nullptr) != VK_SUCCESS) {
            return false;
        }
        depth_copy_views[i] = instance->GetDevice().createImageView({.image = vk::Image{depth_copies[i]}, .viewType = vk::ImageViewType::e2D, .format = format,
                                                                     .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}}).value;
    }
    depth_copy_format = format, depth_copy_w = w, depth_copy_h = h;
    return true;
}

bool EnsureDlaa(u32 w, u32 h) {
    const vk::Device device = instance->GetDevice();
    if (!motion_pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
            {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        motion_set_layout = device.createDescriptorSetLayout({.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                                                              .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()}).value;
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = 8};
        motion_layout = device.createPipelineLayout({.setLayoutCount = 1, .pSetLayouts = &motion_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range}).value;
        const std::span<const u32> code{VKB_AA_MOTION_COMP};
        const vk::ShaderModule module = device.createShaderModule({.codeSize = code.size_bytes(), .pCode = code.data()}).value;
        const auto [r, pipeline] = device.createComputePipeline({}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"}, .layout = motion_layout});
        device.destroyShaderModule(module);
        if (r != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "vkb aa: motion pipeline failed");
            return false;
        }
        motion_pipeline = pipeline;
    }
    if (motion_pxs[0] != VK_NULL_HANDLE && dl_w == w && dl_h == h) {
        px_index = (px_index + 1) % FgSlots;
        motion_px = motion_pxs[px_index], motion_px_view = motion_px_views[px_index];
        return true;
    }
    if (motion_pxs[0] != VK_NULL_HANDLE) {
        IdleDevice(device);
        for (u32 i = 0; i < FgSlots; ++i) {
            device.destroyImageView(motion_px_views[i]);
            vmaDestroyImage(instance->GetAllocator(), motion_pxs[i], motion_px_allocs[i]);
            motion_pxs[i] = VK_NULL_HANDLE;
        }
    }
    const VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = VK_FORMAT_R16G16_SFLOAT,
                               .extent = {w, h, 1},
                               .mipLevels = 1,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    for (u32 i = 0; i < FgSlots; ++i) {
        if (vmaCreateImage(instance->GetAllocator(), &ci, &alloc, &motion_pxs[i], &motion_px_allocs[i], nullptr) != VK_SUCCESS) {
            return false;
        }
        motion_px_views[i] = device.createImageView({.image = vk::Image{motion_pxs[i]}, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR16G16Sfloat,
                                                     .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}).value;
    }
    px_index = 0;
    motion_px = motion_pxs[0], motion_px_view = motion_px_views[0];
    {
        std::scoped_lock fg_lock{fg_mutex};
        fg_slots = {};
    }
    dl_w = w, dl_h = h;
    return true;
}

void DestroyContext() {
    if (context != nullptr) {
        IdleDevice(instance->GetDevice());
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    if (output != VK_NULL_HANDLE) {
        if (output_view) {
            instance->GetDevice().destroyImageView(output_view);
            output_view = vk::ImageView{};
        }
        vmaDestroyImage(instance->GetAllocator(), output, output_alloc);
        output = VK_NULL_HANDLE, output_alloc = nullptr;
    }
    if (sharpen_img != VK_NULL_HANDLE) {
        instance->GetDevice().destroyImageView(sharpen_view);
        sharpen_view = vk::ImageView{};
        vmaDestroyImage(instance->GetAllocator(), sharpen_img, sharpen_alloc);
        sharpen_img = VK_NULL_HANDLE, sharpen_alloc = nullptr;
    }
}

bool EnsureContext(u32 w, u32 h) {
    if ((context != nullptr || (Chosen() == Kind::Dlaa && output != VK_NULL_HANDLE)) && ctx_w == w && ctx_h == h) {
        return true;
    }
    DestroyContext();
    if (Chosen() == Kind::Dlaa) {
        goto make_output; // DLAA has its own feature (DlssGlue); only the output image is shared
    }
    {
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
    }
make_output:
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
        if (context != nullptr) {
            ffxVkPortableUpscaleContextDestroy(context);
            context = nullptr;
        }
        return false;
    }
    output_view = instance->GetDevice().createImageView({.image = vk::Image{output}, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR16G16B16A16Sfloat,
                                                         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}).value;
    if (PicturePart('s')) { // issue 42: the second image of the RCAS passes (same format and use as the output)
        if (vmaCreateImage(instance->GetAllocator(), &ci, &alloc, &sharpen_img, &sharpen_alloc, nullptr) != VK_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "vkb aa: sharpening image failed");
            sharpen_img = VK_NULL_HANDLE, sharpen_alloc = nullptr;
            DestroyContext();
            return false;
        }
        sharpen_view = instance->GetDevice().createImageView({.image = vk::Image{sharpen_img}, .viewType = vk::ImageViewType::e2D,
                                                              .format = vk::Format::eR16G16B16A16Sfloat,
                                                              .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}).value;
    }
    ctx_w = w, ctx_h = h;
    frame_id = 0;
    if (Chosen() == Kind::Fsr3) {
        LOG_INFO(Render_Vulkan, "vkb aa: FSR 3.1 native AA {}x{} on the game's motion vectors", w, h);
    }
    return true;
}

void Barrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                               .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands, .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
}

bool EnsureRcas() {
    if (rcas_pipeline || rcas_broken) {
        return !rcas_broken;
    }
    rcas_broken = true; // until made
    const vk::Device device = instance->GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding = 0, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    rcas_set_layout = device.createDescriptorSetLayout({.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                                                        .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()}).value;
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = 8};
    rcas_layout = device.createPipelineLayout({.setLayoutCount = 1, .pSetLayouts = &rcas_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range}).value;
    const std::span<const u32> code{VKB_RCAS_COMP};
    const vk::ShaderModule module = device.createShaderModule({.codeSize = code.size_bytes(), .pCode = code.data()}).value;
    const auto [r, pipeline] = device.createComputePipeline({}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"}, .layout = rcas_layout});
    device.destroyShaderModule(module);
    if (r != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "vkb aa: sharpening pipeline failed");
        return false;
    }
    rcas_pipeline = pipeline;
    rcas_broken = false;
    return true;
}

/// One RCAS pass (vkb_rcas.comp) from `src` into `dst`, both RGBA16F in GENERAL
void RcasPass(vk::CommandBuffer cmd, vk::ImageView src, vk::ImageView dst, u32 w, u32 h, float strength) {
    const vk::DescriptorImageInfo src_info{.imageView = src, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo dst_info{.imageView = dst, .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &src_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &dst_info},
    }};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, rcas_pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, rcas_layout, 0, writes);
    const std::array<float, 2> params{strength, 1.0f};
    cmd.pushConstants(rcas_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), params.data());
    cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Barrier(cmd);
    c_rcas_passes.fetch_add(1, std::memory_order_relaxed);
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

bool IsSceneDepth(u64 depth_view_id) {
    return depth_view_id != 0 && (depth_view_id == scene_depth_view || depth_view_id == scene_depth_other);
}

bool DrawJitter(u64 depth_view_id, bool depth_write, bool geometry, bool samples_depth, u64 mesh_key, std::array<float, 2>& jitter) {
    if (CensusOn()) {
        auto& c = depth_census[depth_view_id];
        c[0] += depth_write && geometry, c[1] += depth_write, c[2] += !depth_write, c[3] += 1;
    }
    if (!JitterWanted() || !Running() || depth_view_id == 0 || (depth_view_id != scene_depth_view && depth_view_id != scene_depth_other)) {
        return false;
    }
    bool on = false;
    if (geometry && !samples_depth) {
        on = true;
        if (depth_write) {
            jittered_meshes.insert(mesh_key);
        }
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
    const bool want_px = Chosen() == Kind::Dlaa || Core::Vkb::DlssGlue::FgWanted();
    if (depth_copies[0] != VK_NULL_HANDLE && depth_copy_format != d.format) {
        IdleDevice(instance->GetDevice()); // the presenter may read the old copies
        DestroyDepthCopies();
    }
    if (!EnsureContext(w, h) || (want_px && !EnsureDlaa(w, h)) || (Core::Vkb::DlssGlue::FgWanted() && !EnsureDepthCopies(w, h, d.format))) {
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

    static const float sharpness = [] { const char* e = std::getenv("TLG_VKB_AA_SHARPNESS"); return e != nullptr ? static_cast<float>(std::atof(e)) : 0.5f; }();
    // issue 42 (part s): the player's Sharpening setting, read each frame (the menu changes it live), as the normal renderer: 0..100% is the AA's own
    // RCAS (FSR 3.1's; DLAA has none, so one RCAS pass stands in), every 100% above adds one RCAS pass, the last one partial (their ExtraSharpen)
    const bool setting_sharpen = PicturePart('s') && sharpen_img != VK_NULL_HANDLE && EnsureRcas();
    const auto& settings = TlgSettings::Get();
    const float wanted = !setting_sharpen ? std::min(std::max(sharpness, 0.0f), 1.0f) : settings.sharpen ? std::clamp(settings.sharpness.load(), 0.0f, 4.0f) : 0.0f;
    const float own = std::min(wanted, 1.0f); ///< the AA's own share
    if (want_px) {
        // the game's motion as DLSS reads it: render pixels, previous - current (vkb_aa_motion.comp)
        const vk::DescriptorImageInfo game_info{.imageView = ViewOf(m.image, m.format, vk::ImageAspectFlagBits::eColor), .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo px_info{.imageView = motion_px_view, .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 2> writes{{
            {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &game_info},
            {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &px_info},
        }};
        const vk::ImageMemoryBarrier2 px_barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands, .srcAccessMask = vk::AccessFlagBits2::eNone,
                                                 .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                                 .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral, .image = vk::Image{motion_px},
                                                 .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &px_barrier});
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, motion_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, motion_layout, 0, writes);
        const std::array<float, 2> size{static_cast<float>(w), static_cast<float>(h)};
        cmd.pushConstants(motion_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size), size.data());
        cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(cmd);
        if (Core::Vkb::DlssGlue::FgWanted()) { // F2: this frame's depth (our copy) and motion for the presenter's frame generation
            const vk::Image copy{depth_copies[px_index]};
            const vk::ImageMemoryBarrier2 to_dst{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands, .srcAccessMask = vk::AccessFlagBits2::eNone,
                                                 .dstStageMask = vk::PipelineStageFlagBits2::eTransfer, .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                                 .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral, .image = copy,
                                                 .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
            const vk::ImageCopy region{.srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1}, .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
                                       .extent = {w, h, 1}};
            cmd.copyImage(d.image, vk::ImageLayout::eGeneral, copy, vk::ImageLayout::eGeneral, region);
            Barrier(cmd);
            std::scoped_lock fg_lock{fg_mutex};
            fg_slots[px_index] = {.inputs = {.depth = copy, .motion = vk::Image{motion_px}, .depth_view = depth_copy_views[px_index], .motion_view = motion_px_view,
                                             .depth_format = d.format, .width = w, .height = h, .reset = reset},
                                  .value = Core::Vkb::Stream::RecordingValue(),
                                  .serial = ++fg_serial};
        }
    }
    if (Chosen() == Kind::Dlaa) {
        using Core::Vkb::DlssGlue::Image;
        const Image color{s.image, ViewOf(s.image, s.format, vk::ImageAspectFlagBits::eColor), s.format, vk::ImageAspectFlagBits::eColor, w, h};
        const Image dimg{d.image, ViewOf(d.image, d.format, vk::ImageAspectFlagBits::eDepth), d.format, vk::ImageAspectFlagBits::eDepth, w, h};
        const Image mimg{vk::Image{motion_px}, motion_px_view, vk::Format::eR16G16Sfloat, vk::ImageAspectFlagBits::eColor, w, h};
        const Image oimg{vk::Image{output}, output_view, vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor, w, h};
        ++frame_id; // the history counter both paths share (a reset only after a gap)
        if (!Core::Vkb::DlssGlue::Run(instance->GetDevice(), cmd, color, dimg, mimg, oimg, jitter[0], jitter[1], reset, frame_ms, setting_sharpen ? 0.0f : own)) {
            LOG_ERROR(Render_Vulkan, "vkb aa: DLAA failed; AA off");
            failed = true;
            c_failed.fetch_add(1, std::memory_order_relaxed);
            Barrier(cmd);
            return;
        }
        goto copy_back;
    }
    {
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
    info.sharpness = own;
    info.enableSharpening = own > 0.0f ? VK_TRUE : VK_FALSE;
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
    }
copy_back:
    Barrier(cmd);
    VkImage result = output;
    if (setting_sharpen) {
        // the passes: DLAA's own share first (it has no RCAS of its own), then one per 100% above
        std::array<float, 5> passes{};
        u32 count = 0;
        if (Chosen() == Kind::Dlaa && own > 0.0f) {
            passes[count++] = own;
        }
        for (float extra = wanted - 1.0f; extra > 0.0f && count < passes.size(); extra -= 1.0f) {
            passes[count++] = std::min(extra, 1.0f);
        }
        if (count != 0) {
            const vk::ImageMemoryBarrier2 to_general{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands, .srcAccessMask = vk::AccessFlagBits2::eNone,
                                                     .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                                     .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                                                     .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral, .image = vk::Image{sharpen_img},
                                                     .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
        }
        vk::ImageView from = output_view, to = sharpen_view;
        for (u32 i = 0; i < count; ++i) {
            RcasPass(cmd, from, to, w, h, passes[i]);
            std::swap(from, to);
            result = result == output ? sharpen_img : output;
        }
    }
    // the result replaces the scene colour (B10G11R11: no alpha to keep); both images stay in GENERAL (note 66)
    const vk::ImageBlit region{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                               .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(w), static_cast<s32>(h), 1}},
                               .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                               .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(w), static_cast<s32>(h), 1}}};
    cmd.blitImage(vk::Image{result}, vk::ImageLayout::eGeneral, s.image, vk::ImageLayout::eGeneral, region, vk::Filter::eNearest);
    Barrier(cmd);
    c_runs.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<u64> c_fg_made{0}, c_fg_presented{0}, c_fg_failed{0};
void NoteFg(bool made, bool presented, bool failed) {
    (made ? c_fg_made : presented ? c_fg_presented : c_fg_failed).fetch_add(1, std::memory_order_relaxed);
    (void)failed;
}

bool TakeFgInputs(FgInputs& out) {
    const u64 done = Core::Vkb::Stream::CompletedValue();
    std::scoped_lock lock{fg_mutex};
    const FgSlot* best = nullptr;
    for (const FgSlot& slot : fg_slots) {
        if (slot.serial > fg_taken && slot.value != 0 && slot.value <= done && (best == nullptr || slot.serial > best->serial)) {
            best = &slot;
        }
    }
    if (best == nullptr) {
        return false;
    }
    fg_taken = best->serial;
    out = best->inputs;
    return true;
}

SamplerLook LookFor(u32 mrt_mask) {
    SamplerLook look{};
    const auto& settings = TlgSettings::Get();
    if (PicturePart('a')) {
        look.aniso = static_cast<u32>(std::max(0, settings.aniso.load())); // 0, 2, 4, 8 or 16 (tlg_settings.cpp)
    }
    // their Rasterizer::BindResources + TemporalUpscaler::SceneMipBias: a graphics pipeline with 5 or more colour outputs (the scene's material pass)
    // samples with the Lab tab's bias in quarter steps while DLAA or FSR native AA runs; at native size the render/output term is 0
    if (PicturePart('b') && std::popcount(mrt_mask & 0xffu) >= 5 && Running()) {
        look.bias_q = settings.lod_bias.load();
        if (look.bias_q != 0) {
            c_scene_bias_draws.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return look;
}

nlohmann::json Report() {
    return {{"on", On()},
            {"kind", Chosen() == Kind::Fsr3 ? "fsr3" : Chosen() == Kind::Dlaa ? "dlaa" : "off"},
            {"size", fmt::format("{}x{}", ctx_w, ctx_h)},
            {"motion_passes", c_motion_passes.load()},
            {"runs", c_runs.load()},
            {"combines_without_inputs", c_no_inputs.load()},
            {"unusable", c_unusable.load()},
            {"failed", c_failed.load()},
            {"history_resets", c_resets.load()},
            {"jitter", JitterWanted()},
            {"jittered_draws", c_jittered.load()},
            {"jittered_mesh_redraws", c_jittered_meshes.load()},
            {"picture_settings", {{"fallback_fsr3", PicturePart('f')}, {"sharpening", PicturePart('s')}, {"scene_bias", PicturePart('b')}, {"aniso", PicturePart('a')},
                                  {"rcas_passes", c_rcas_passes.load()}, {"rcas_broken", rcas_broken},
                                  {"scene_bias_draws", c_scene_bias_draws.load()}}},
            {"frame_generation", {{"made", c_fg_made.load()}, {"presented", c_fg_presented.load()}, {"failed", c_fg_failed.load()}}}};
}

} // namespace Core::Vkb::Aa
