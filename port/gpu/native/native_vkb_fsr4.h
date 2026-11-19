// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace VkbVulkan {
class Instance;
}

/// FSR 4 native AA for the new engine (TLG_VKB_AA=fsr4, switch TLG_VKB_FSR4). The same provider, model files and inputs as the normal renderer's FSR 4
/// (their vk_fsr4.cpp, the v07 INT8 path; that file is not touched): the HDR scene colour, the scene depth, the motion in render pixels (previous - current)
/// and the jitter. Only the native model (render size = output size) is loaded: the new engine's AA is native AA only. Its frames are retired on the new
/// engine's timeline (Core::Vkb::Stream) instead of their scheduler. Recording thread only.
namespace Core::Vkb::Fsr4 {

struct Image {
    vk::Image image{};
    vk::ImageView view{}; ///< depth: a view of the depth aspect
    u32 width{}, height{};
};
struct Frame {
    vk::CommandBuffer cmd{};
    Image color, depth, motion, output; ///< all in layout General; the output is written by compute shaders
    std::array<float, 2> jitter{};
    float frame_ms{}, near_plane{}, far_plane{}, vertical_fov{};
    float sharpness{}; ///< 0..1: FSR 4's own RCAS (0: none)
    bool reset{}, auto_exposure{};
};

enum class Start : u32 {
    Ok,
    NoModel, ///< the model files (TLG_FSR4_DIR or fsr4_shaders) are missing: the one case the caller may fall back quietly (owner, pull request 51)
    Fault,   ///< anything else: the device lacks a feature, the backend or provider could not be made
};

/// Makes the provider for a `w` x `h` frame (native model). Problem() says why when not Ok.
Start Create(const VkbVulkan::Instance& instance, u32 w, u32 h);
/// Records one frame into `frame.cmd`. False: Problem() says why (the caller treats it as a fault).
bool Record(const Frame& frame);
/// Frees the provider; the caller has made the device idle first
void Destroy();
[[nodiscard]] const std::string& Problem();

} // namespace Core::Vkb::Fsr4
