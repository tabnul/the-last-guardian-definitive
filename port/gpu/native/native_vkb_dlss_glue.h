// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
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

/// TLG_VKB_AA=dlaa or TLG_VKB_FG=dlss, and the DLLs present (their Dlss::Get): NGX is set up with our instance
[[nodiscard]] bool Wanted();
/// F2: TLG_VKB_FG=dlss and the DLLs present
[[nodiscard]] bool FgWanted();
/// F2: TLG_VKB_FG=fsr (AMD FSR 3 frame generation, vkb_fsr_fg.h; needs no DLSS)
[[nodiscard]] bool FsrFgWanted();
/// F2: either kind of frame generation: the AA keeps the depth and motion for it
[[nodiscard]] bool AnyFgWanted();
/// Issue 99: Wanted, and the bridge and NGX came up with our device (their Dlss::Available, known once Initialize ran). False: `why` says what is wrong.
[[nodiscard]] bool Usable(std::string& why);
/// Our instance and device creation (only when Wanted): what NGX needs
void AppendInstanceExtensions(std::vector<const char*>& enabled);
void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical, std::vector<const char*>& enabled);
void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
/// DLAA (DLSS at the render size): a feature made on first use or a size change (`device` idles first), then evaluated into `output`. False: failed.
bool Run(vk::Device device, vk::CommandBuffer cmd, const Image& color, const Image& depth, const Image& motion, const Image& output, float jitter_x, float jitter_y,
         bool reset, float frame_ms, float sharpness);

/// Issue 99: frees the DLAA feature after DLAA handed over to another AA (`device` idles first: submitted work may still use it)
void Release(vk::Device device);

/// F2: DLSS frame generation: one image between the previous finished frame and `backbuffer` (the finished frame with the interface), from the render depth and the
/// motion (render pixels, previous - current). FgCreate on the first use and a size or format change (`device` idles first).
[[nodiscard]] bool FgAvailable();
bool FgCreate(vk::Device device, vk::CommandBuffer cmd, u32 width, u32 height, vk::Format format);
bool FgEvaluate(vk::CommandBuffer cmd, const Image& backbuffer, const Image& depth, const Image& motion, const Image& output, bool reset);

} // namespace Core::Vkb::DlssGlue
