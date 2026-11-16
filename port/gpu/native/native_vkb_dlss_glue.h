// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

/// F1d: their DLSS wrapper (gpu/shadps4/video_core/renderer_vulkan/vk_dlss.h: tlg_dlss.dll + NVIDIA's nvngx_dlss.dll next to the executable) used by our
/// backend. Its header lives in their renderer's include tree, so only native_vkb_dlss_glue.cpp includes it; their files are not edited.
namespace Core::Vkb::DlssGlue {

struct Image {
    vk::Image image{};
    vk::ImageView view{};
    vk::Format format{};
    vk::ImageAspectFlags aspect{};
    u32 width{}, height{};
};

/// TLG_VKB_AA=dlaa and the DLLs present (their Dlss::Get)
[[nodiscard]] bool Wanted();
/// Our instance and device creation (only when Wanted): what NGX needs
void AppendInstanceExtensions(std::vector<const char*>& enabled);
void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical, std::vector<const char*>& enabled);
void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
/// DLAA (DLSS at the render size): a feature made on first use or a size change (`device` idles first), then evaluated into `output`. False: failed.
bool Run(vk::Device device, vk::CommandBuffer cmd, const Image& color, const Image& depth, const Image& motion, const Image& output, float jitter_x, float jitter_y,
         bool reset, float frame_ms, float sharpness);

} // namespace Core::Vkb::DlssGlue
