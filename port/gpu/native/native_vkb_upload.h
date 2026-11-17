// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <nlohmann/json_fwd.hpp>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace VkbVulkan {
class Instance;
class HostImport;
} // namespace VkbVulkan

/// V2b of the engine-to-Vulkan backend: the backend's own recorder for work outside the frame's passes (uploads), and the one-time upload of a texture's contents.
/// The recorder has its own command pool and timeline semaphore and submits to the graphics queue under the shared submit lock (VkbVulkan::Scheduler::submit_mutex), in batches at each graphics
/// submit; what a batch used (staging buffers) is freed when the GPU has passed its timeline value.
namespace Core::Vkb::Upload {

void Init(const VkbVulkan::Instance& instance, VkbVulkan::HostImport& host_import);

/// Our image of engine texture `texture_id` gets the texture's contents: the engine's memory (its address and size from the engine's own description), converted from the PS4 tiling
/// by the tiling shaders reading the imported backing in place, then copied level by level. The image is left in `final_layout`. False (counted): no description, the memory is not in
/// one imported chunk, or the texel size of the image does not match the layout.
/// The size of the engine texture's memory from its description (its tiled layout); 0 when it cannot be derived
u64 GuestSize(u64 texture_id);
bool TextureContents(u64 texture_id, vk::Image image, vk::Format format, u32 mips, u32 layers, vk::ImageLayout final_layout);
/// api-contract.md W5 / L5 / L6 / L8: the same upload as a job of the frame stream, made where the engine changed the texture's memory (a file read, a fill, UpdateSubresource,
/// a GPU copy into it): the stream's hazard barriers order it after earlier work on the image (and on the memory) and before later reads. The image must already be GENERAL.
/// Its temporary buffers are freed three graphics submits later. False (counted): as TextureContents.
bool TextureContentsInStream(u64 texture_id, vk::Image image, vk::Format format, u32 mips, u32 layers);

/// A buffer copy in the batch (kill-experiment TLG_VKB_HOST_CACHE)
void CopyBuffer(vk::Buffer source, u64 source_offset, vk::Buffer destination, u64 destination_offset, u64 bytes);

/// Submits the recorded batch (graphics submit) and frees what finished batches used
void Submit();
/// note 66: an image whose contents can come through this batch (a colour texture that is not a render target) gets its one layout, GENERAL, here: in order with its uploads
void InitLayout(vk::Image image);

[[nodiscard]] nlohmann::json Report();

} // namespace Core::Vkb::Upload
