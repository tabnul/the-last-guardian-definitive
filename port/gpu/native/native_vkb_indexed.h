// SPDX-License-Identifier: GPL-2.0-or-later
// Scene on the GPU, R1 (documents/tlg/native plan/scene-on-gpu-plan.md): the global descriptor set of record-mode pipelines. One set (set 0) holds the row table (binding 0: the
// stream's host ring, where each draw's row of slot numbers, offsets and sizes is written), one array of storage buffer descriptors, one of samplers, and one per image array key
// (the layout the recompiler's indexed mode declares, shader_recompiler/resource.h Indexed). A slot stands for one of our Vulkan objects: a whole buffer (a read-only guest
// buffer's offset and size are in the row), an image view of an array key, a sampler. Slots are made at first use and written at once (update after bind, unused while pending);
// a slot is freed when its object is destroyed, which happens only after the GPU is done with every submission that could name it.
// Switch: TLG_VKB_RECORD (default on since 2026-11-21; =0 off).
#pragma once

#include <nlohmann/json_fwd.hpp>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace Core::Vkb::Indexed {

/// TLG_VKB_RECORD=1 and the device has the room (checked once, at the first call on a thread with the device up)
[[nodiscard]] bool On();
[[nodiscard]] vk::DescriptorSetLayout Layout();
[[nodiscard]] vk::DescriptorSet Set();
/// The slot of a whole buffer (0: the null buffer)
[[nodiscard]] u32 BufferSlot(vk::Buffer buffer);
/// The slot of an image view in the array of `key` (0: none)
[[nodiscard]] u32 ImageSlot(u32 key, vk::ImageView view);
/// The slot of a sampler (0: the default sampler)
[[nodiscard]] u32 SamplerSlot(vk::Sampler sampler);
/// Our object is being destroyed (the GPU is done with it): its slots are free again
void ForgetBuffer(vk::Buffer buffer);
void ForgetView(vk::ImageView view);
/// Changes whenever a slot is freed (G3: a row kept by pointer is remade then)
[[nodiscard]] u64 Epoch();
[[nodiscard]] nlohmann::json Report();

} // namespace Core::Vkb::Indexed
