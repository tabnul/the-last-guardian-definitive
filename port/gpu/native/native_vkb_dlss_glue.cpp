// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_dlss_glue.h"

#include <mutex>
#include <cstdlib>
#include <cstring>

#include "native/shadps4/video_core/renderer_vulkan/vk_scheduler.h"
#include "common/logging/log.h"
#include "../shadps4/video_core/renderer_vulkan/vk_dlss.h"

namespace {
/// a device-wide wait: every queue must be left alone meanwhile (Vulkan), so the backend's recording and upload threads wait at the queue lock, as the
/// presenter's swapchain recreate does (a wait without it lost the device, owner play 2026-11-16)
void IdleDevice(vk::Device device) {
    std::scoped_lock queue_lock{VkbVulkan::Scheduler::submit_mutex};
    (void)device.waitIdle();
}
} // namespace

namespace Core::Vkb::DlssGlue {
namespace {

Vulkan::Dlss::Resource Res(const Image& i) {
    return {.image = i.image, .view = i.view, .format = i.format, .aspect = i.aspect, .width = i.width, .height = i.height};
}

} // namespace

bool FgWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("TLG_VKB_FG");
        return e != nullptr && std::strcmp(e, "dlss") == 0 && Vulkan::Dlss::Get() != nullptr;
    }();
    return wanted;
}

bool FsrFgWanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("TLG_VKB_FG");
        return e != nullptr && std::strcmp(e, "fsr") == 0;
    }();
    return wanted;
}

bool AnyFgWanted() {
    return FgWanted() || FsrFgWanted();
}

bool Wanted() {
    static const bool wanted = [] {
        const char* e = std::getenv("TLG_VKB_AA");
        return (e != nullptr && std::strcmp(e, "dlaa") == 0 && Vulkan::Dlss::Get() != nullptr) || FgWanted();
    }();
    return wanted;
}

bool FgAvailable() {
    Vulkan::Dlss* dlss = Vulkan::Dlss::Get();
    return FgWanted() && dlss != nullptr && dlss->FgAvailable();
}

bool FgCreate(vk::Device device, vk::CommandBuffer cmd, u32 width, u32 height, vk::Format format) {
    Vulkan::Dlss* dlss = Vulkan::Dlss::Get();
    if (dlss == nullptr) {
        return false;
    }
    IdleDevice(device);
    dlss->FgRelease();
    return dlss->FgCreate(cmd, width, height, format);
}

bool FgEvaluate(vk::CommandBuffer cmd, const Image& backbuffer, const Image& depth, const Image& motion, const Image& output, bool reset) {
    Vulkan::Dlss* dlss = Vulkan::Dlss::Get();
    return dlss != nullptr && dlss->FgEvaluate(cmd, {.backbuffer = Res(backbuffer), .depth = Res(depth), .motion = Res(motion), .output = Res(output), .reset = reset, .hdr = false});
}

void AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (Wanted()) {
        Vulkan::Dlss::Get()->AppendInstanceExtensions(enabled);
    }
}

void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical, std::vector<const char*>& enabled) {
    if (Wanted()) {
        Vulkan::Dlss::Get()->AppendDeviceExtensions(instance, physical, enabled);
    }
}

void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device) {
    if (Wanted()) {
        Vulkan::Dlss::Get()->Initialize(instance, physical, device);
    }
}

bool Run(vk::Device device, vk::CommandBuffer cmd, const Image& color, const Image& depth, const Image& motion, const Image& output, float jitter_x, float jitter_y,
         bool reset, float frame_ms, float sharpness) {
    Vulkan::Dlss* dlss = Vulkan::Dlss::Get();
    if (dlss == nullptr) {
        return false;
    }
    const Vulkan::Dlss::FeatureDesc desc{color.width, color.height, color.width, color.height, Vulkan::Dlss::QualityForScale(1.0f), true, 0, true};
    if (!dlss->HasFeature(desc)) {
        IdleDevice(device); // the previous feature may still be in use by submitted work
        dlss->ReleaseFeature();
        if (!dlss->CreateFeature(cmd, desc)) {
            LOG_ERROR(Render_Vulkan, "vkb aa: DLSS feature creation failed");
            return false;
        }
        LOG_INFO(Render_Vulkan, "vkb aa: DLAA {}x{} on the game's motion vectors", color.width, color.height);
        reset = true;
    }
    return dlss->Evaluate(cmd, {.color = Res(color), .depth = Res(depth), .motion = Res(motion), .output = Res(output), .jitter_x = jitter_x, .jitter_y = jitter_y,
                                .reset = reset, .frame_ms = frame_ms, .sharpness = sharpness});
}

} // namespace Core::Vkb::DlssGlue
