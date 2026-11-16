// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native/shadps4/shader_recompiler/profile.h"

namespace VkbVulkan {
class Instance;
class Scheduler;
class DescriptorHeap;
} // namespace VkbVulkan

/// The backend's device (merge note 5): its own Vulkan instance and device, made by its presenter (native/vkb_core/vkb_presenter), never theirs.
/// The backend's code reaches the device here instead of through their rasterizer.
namespace Core::Vkb::Device {

/// Called once by the backend's presenter when its instance and frame scheduler exist (the descriptor heap and the profile are made here)
void Init(const VkbVulkan::Instance& instance, VkbVulkan::Scheduler& scheduler);
[[nodiscard]] bool Ready() noexcept;
[[nodiscard]] const VkbVulkan::Instance& Instance();
/// The presenter's frame scheduler: its timeline paces the descriptor heap
[[nodiscard]] VkbVulkan::Scheduler& Scheduler();
/// Descriptor sets for pipelines with more than 32 bindings (those cannot use push descriptors)
[[nodiscard]] VkbVulkan::DescriptorHeap& DescHeap();
[[nodiscard]] const VkbShader::Profile& Profile();

} // namespace Core::Vkb::Device
