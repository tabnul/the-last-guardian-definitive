// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <nlohmann/json_fwd.hpp>
#include "common/types.h"

/// V4 of the engine-to-Vulkan backend (documents/tlg/native plan/engine-vulkan-backend.md): the engine's draws as our own graphics pipelines and passes.
/// V4a: a draw of the device context finds (or compiles, once per variant) our pipeline for the bound vertex and pixel shaders: the programs from their Gnm blocks, the shader
/// environments from the shader objects, the user data their input usage tables describe (the fetch shader and the vertex buffer table included), the fixed-function state from the
/// engine's state objects, the colour and depth target descriptions from the bound views; the slot map of both stages and of the vertex attributes. Nothing is drawn yet.
namespace Core::Vkb::Graphics {

/// A draw of the device context `ctx` (game thread): kind 0 indexed, 1 auto, 2 indirect; the engine's arguments
void Draw(u64 ctx, u32 kind, u64 a1, u64 a2, u64 a3);

[[nodiscard]] nlohmann::json Report();

} // namespace Core::Vkb::Graphics
