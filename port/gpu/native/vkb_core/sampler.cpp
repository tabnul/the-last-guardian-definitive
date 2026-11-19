// VKB: the backend's own copy (from tlg-pc-port-hayato texture_cache/sampler.cpp); their renderer keeps video_core/texture_cache/sampler.cpp.
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "native/vkb_core/liverpool_to_vk.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"
#include "native/vkb_core/sampler.h"

namespace VideoCore::Vkb {

Core::NativeDesc::SamplerDesc SamplerDescFromSharp(const VkbAmdGpu::Sampler& sampler) noexcept {
    Core::NativeDesc::SamplerDesc d;
    d.valid = sampler.Valid();
    d.mag_filter = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::Filter(sampler.xy_mag_filter));
    d.min_filter = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::Filter(sampler.xy_min_filter));
    d.mip_mode = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::MipFilter(sampler.mip_filter));
    d.address_u = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::ClampMode(sampler.clamp_x));
    d.address_v = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::ClampMode(sampler.clamp_y));
    d.address_w = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::ClampMode(sampler.clamp_z));
    d.compare_op = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::DepthCompare(sampler.depth_compare_func));
    d.border = static_cast<u32>(VkbVulkan::Vkb::LiverpoolToVK::BorderColor(sampler.border_color_type));
    d.border_index = static_cast<u32>(sampler.border_color_ptr.Value());
    d.aniso_filter = (VkbAmdGpu::IsAnisoFilter(sampler.xy_mag_filter) || VkbAmdGpu::IsAnisoFilter(sampler.xy_min_filter)) ? 1 : 0;
    d.max_aniso = d.valid ? sampler.MaxAniso() : 1.0f;
    d.lod_bias = sampler.LodBias();
    d.min_lod = sampler.MinLod();
    d.max_lod = sampler.MaxLod();
    d.mipped = sampler.mip_filter != VkbAmdGpu::MipFilter::None ? 1 : 0; // issue 42
    return d;
}

u32 ForcedAnisotropy(const Core::NativeDesc::SamplerDesc& d, const u32 setting, const bool is_depth) noexcept {
    return Core::NativeDesc::ForcedAnisotropy(setting, d.min_filter == static_cast<u32>(vk::Filter::eLinear), d.mipped != 0, d.max_lod, is_depth);
}

Sampler::Sampler(const VkbVulkan::Instance& instance, const Core::NativeDesc::SamplerDesc& sampler,
                 const VkbAmdGpu::BorderColorBuffer border_color_base, const bool is_depth, float extra_lod_bias, u32 aniso_setting) {
    using namespace VkbVulkan;
    using namespace VkbVulkan::Vkb;
    // issue 42 (part a): the anisotropic filtering setting, with the normal renderer's rule (Core::NativeDesc::ForcedAnisotropy)
    const u32 forced_aniso = ForcedAnisotropy(sampler, aniso_setting, is_depth);
    const bool force = forced_aniso != 0;
    const bool anisotropy_enable = instance.IsAnisotropicFilteringSupported() && (force || sampler.aniso_filter != 0);
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(force ? std::max(sampler.max_aniso, static_cast<float>(forced_aniso)) : sampler.max_aniso, 1.0f,
                                       instance.MaxSamplerAnisotropy())
                          : 1.0f;
    auto border_color = static_cast<vk::BorderColor>(sampler.border);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_index;
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = static_cast<vk::Filter>(sampler.mag_filter),
        .minFilter = static_cast<vk::Filter>(sampler.min_filter),
        .mipmapMode = static_cast<vk::SamplerMipmapMode>(sampler.mip_mode),
        .addressModeU = static_cast<vk::SamplerAddressMode>(sampler.address_u),
        .addressModeV = static_cast<vk::SamplerAddressMode>(sampler.address_v),
        .addressModeW = static_cast<vk::SamplerAddressMode>(sampler.address_w),
        .mipLodBias = std::clamp(sampler.lod_bias + extra_lod_bias, -instance.MaxSamplerLodBias(),
                                 instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = static_cast<vk::CompareOp>(sampler.compare_op),
        .minLod = sampler.min_lod,
        .maxLod = sampler.max_lod,
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() {
}

} // namespace VideoCore::Vkb
