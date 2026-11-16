// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VKB: the backend's own pipeline base (see pipeline_common.h).

#include <fmt/format.h>
#include "native/shadps4/shader_recompiler/info.h"
#include "native/vkb_core/pipeline_common.h"

namespace VkbVulkan::Vkb {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const VkbShader::Profile& profile_, vk::PipelineCache, bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_}, is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = fmt::format("{}_{:#018x}", static_cast<u32>(stage->hw_stage), stage->pgm_hash);
            stage_desc = stage_desc.empty() ? shader_name : fmt::format("{},{}", stage_desc, shader_name);
        }
    }
    return stage_desc;
}

} // namespace VkbVulkan::Vkb
