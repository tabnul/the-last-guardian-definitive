// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <nlohmann/json_fwd.hpp>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace VkbVulkan {
class Instance;
}

/// F1 (engine-vulkan-backend.md, note 92/93): native anti-aliasing on the game's own targets and motion vectors, as their renderer's "game motion" path.
/// The motion pass (pixel shader 5fce13a4) gives the scene colour (target 0), the game's motion vectors (target 2) and the scene depth; right before the
/// post-processing combine (compute 252f2091) the AA runs and writes its result back into the scene colour, so the game's post, tonemap and interface go on
/// unchanged; the game's SMAA (vertex shader 0a449aef) is cleared instead of drawn while it runs. TLG_VKB_AA=fsr3 turns it on (off by default).
namespace Core::Vkb::Aa {

constexpr u64 MotionPassPs = 0x5fce13a4, CombineCs = 0x252f2091, SmaaVs = 0x0a449aef;

void Init(const VkbVulkan::Instance& instance);
/// The chosen AA (none: false); one relaxed load
[[nodiscard]] bool On();
/// On, not failed, and run at least once (the game's SMAA is cleared only then)
[[nodiscard]] bool Running();
/// Recording thread, recording lock held: a draw of the motion pass with its pass's attachments (colours, then depth)
void NoteMotionPass(std::span<const vk::Image> attachments, u32 color_count, bool has_depth);
/// Recording thread, recording lock held, outside a pass: right before the combine dispatch. Records the AA when this frame's inputs are known.
/// `jitter`: the sub-pixel offset this frame's scene geometry was drawn with (OnCombineQueued)
void BeforeCombine(vk::CommandBuffer cmd, std::array<float, 2> jitter);
/// F1c, the game's render thread (in game order): the motion pass's depth view (the scene depth), a draw's jitter (scene geometry writing that depth, and
/// later draws of a mesh drawn so in this frame: water over its own pre-pass), and the combine queued: returns the frame's jitter, then advances it (Halton 2, 3)
void NoteSceneDepth(u64 depth_view_id);
[[nodiscard]] bool DrawJitter(u64 depth_view_id, bool depth_write, bool geometry, u64 mesh_key, std::array<float, 2>& jitter);
[[nodiscard]] std::array<float, 2> OnCombineQueued();
[[nodiscard]] nlohmann::json Report();

} // namespace Core::Vkb::Aa
