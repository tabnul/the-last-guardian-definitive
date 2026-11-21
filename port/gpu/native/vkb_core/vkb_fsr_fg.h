// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <deque>
#include "common/types.h"
#include "native/native_vkb_aa.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;
struct VmaAllocation_T;

namespace VkbVulkan {

class Instance;
class Scheduler;

/// F2 (TLG_VKB_FG=fsr): AMD FSR 3 frame generation for the engine-to-Vulkan backend, through UniversalScalers-Vulkan by @FireBurn. The same steps
/// as the normal renderer's FsrFrameGen (vk_fsr_frame_gen.cpp): FSR takes only 16-bit color and float depth, so the frame and the depth pass
/// through copies owned here. Camera values: the normal renderer's own fallbacks (near 0.1, vertical field of view 1.0), as the backend reads no camera.
class FsrFg {
public:
    FsrFg(const Instance& instance, Scheduler& scheduler);
    ~FsrFg(); ///< the GPU must be done with its work (device idle)

    /// Records the image between the last frame and this one into `output` on `cmdbuf` (the scheduler's, its tick retires the frame). Both stay in General.
    bool Record(vk::CommandBuffer cmdbuf, vk::Image frame, vk::Image output, u32 width, u32 height, const Core::Vkb::Aa::FgInputs& inputs, float frame_ms);

private:
    struct Image {
        vk::Image image{};
        VmaAllocation_T* allocation{};
    };
    bool Create(u32 width, u32 height, u32 render_width, u32 render_height);
    void Destroy();
    bool MakeImage(Image& image, vk::Format format, u32 width, u32 height, vk::ImageUsageFlags usage);

    const Instance& instance;
    Scheduler& scheduler;
    FfxVkFsr3_3_1_6FrameGenerationContext* context{};
    Image color{}, depth{}, result{};
    vk::Buffer depth_buffer{};
    VmaAllocation_T* depth_buffer_allocation{};
    u32 display_width{}, display_height{}, render_width{}, render_height{};
    u64 frame_id{};
    struct Submitted {
        u64 frame_id;
        u64 tick;
    };
    std::deque<Submitted> submitted;
};

} // namespace VkbVulkan
