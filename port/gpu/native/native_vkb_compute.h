// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <nlohmann/json_fwd.hpp>
#include "common/types.h"

/// V3 of the engine-to-Vulkan backend (documents/tlg/native plan/engine-vulkan-backend-notes.md, note 9): the engine's compute shaders as our own pipelines.
/// V3a: a dispatch of the device context finds (or compiles, once per variant) our pipeline for the bound compute shader: the program from the shader's Gnm block, the user data its
/// input usage table describes, our pipeline layout, and the slot map (which engine slot every resource the module reads is). Nothing is dispatched yet.
namespace Core::Vkb::Compute {

/// V3b: a compiled variant is bound from the engine slots (buffers in place, our image views and samplers) and recorded into the frame stream (native_vkb_stream.h).
/// A dispatch of the device context `ctx` (game thread): group counts, or the guest address of the indirect arguments (x, y, z then unused)
void Dispatch(u64 ctx, u32 x, u32 y, u32 z, u64 indirect_address);
/// V3a part 2: a dispatch of the async compute context `jcc` (its own slot tables and offset-table layout; its append counters' initial values go into our GDS first)
void DispatchAsync(u64 jcc, u32 x, u32 y, u32 z);

/// After the frame's submission has finished (SubmitGraphics): the GPU time of its dispatches, by shader
void FrameDone();

/// Debug (tint hunt, 2026-11-16; /vkb_dump_frame): over the next `submits` graphics submissions, every dispatch dumps each image of at least `min_width` wide
/// it reads or writes, once per image, right after the dispatch, at most `max_images` (log/vkb_image_dump, names in dispatch order). Returns what was armed.
[[nodiscard]] nlohmann::json ArmFrameDump(u32 submits, u32 min_width, u32 max_images);

[[nodiscard]] nlohmann::json Report();

bool LanternCensusActive();
void LanternDrawRead(u64 address, u64 size, u64 vs_hash, u64 ps_hash, u32 stage, u32 slot);
} // namespace Core::Vkb::Compute
