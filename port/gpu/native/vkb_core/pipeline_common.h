// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VKB: the backend's own pipeline base (our vk_pipeline_common from tlg-pc-port-hayato, in VkbVulkan::Vkb, without the old path's
// indexed set, split-mode stage adoption and lean replay); their renderer keeps video_core/renderer_vulkan/vk_pipeline_common.h.

#include <array>
#include <span>
#include <string>
#include "native/shadps4/shader_recompiler/profile.h"
#include "native/shadps4/shader_recompiler/runtime_info.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace VkbShader {
struct Info;
} // namespace VkbShader

namespace VkbVulkan {
class Instance;
class Scheduler;
class DescriptorHeap;
} // namespace VkbVulkan

namespace VkbVulkan::Vkb {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const VkbShader::Profile& profile, vk::PipelineCache pipeline_cache, bool is_compute = false);
    virtual ~Pipeline();

    vk::Pipeline Handle() const noexcept {
        return *pipeline;
    }

    /// tlg note 41: false when the layout has more bindings than push descriptors allow (a plain descriptor set then)
    bool UsesPushDescriptors() const noexcept {
        return uses_push_descriptors;
    }
    vk::DescriptorSetLayout GetDescriptorSetLayout() const noexcept {
        return *desc_layout;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(VkbShader::SwStage::Compute) == VkbShader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const VkbShader::Info& GetStage(VkbShader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

protected:
    [[nodiscard]] std::string GetDebugString() const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const VkbShader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const VkbShader::Info*, VkbShader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
};

} // namespace VkbVulkan::Vkb
