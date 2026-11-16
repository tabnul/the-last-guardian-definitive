// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VKB: the backend's own copy (from tlg-pc-port-hayato texture_cache/sampler.h); their renderer keeps video_core/texture_cache/sampler.h.

#include "native/shadps4/video_core/amdgpu/regs_texture.h"
#include "native/native_desc.h"
#include "native/shadps4/video_core/amdgpu/resource.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace VkbVulkan {
class Instance;
}

namespace VideoCore::Vkb {

/// F4.1: the plain description read from an S# with the emulator's own helpers (the reference of the shadow compare)
[[nodiscard]] Core::NativeDesc::SamplerDesc SamplerDescFromSharp(const VkbAmdGpu::Sampler& sampler) noexcept;

class Sampler {
public:
    explicit Sampler(const VkbVulkan::Instance& instance, const Core::NativeDesc::SamplerDesc& sampler,
                     const VkbAmdGpu::BorderColorBuffer border_color_base, bool is_depth,
                     float extra_lod_bias = 0.0f);
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    Sampler(Sampler&&) = default;
    Sampler& operator=(Sampler&&) = default;

    vk::Sampler Handle() const noexcept {
        return *handle;
    }

    u64 hash;

private:
    vk::UniqueSampler handle;
};

} // namespace VideoCore::Vkb
