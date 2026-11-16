// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VKB: our RenderDoc helpers (native_renderdoc.cpp); their video_core/renderdoc.h keeps its own.
#include <string>
#include "common/types.h"

namespace VideoCore {

/// TLG_RENDERDOC=1: loads renderdoc.dll before any Vulkan instance exists (called at start-up, for either renderer)
void LoadRenderDoc();
/// Counts a present; a capture asked for with RenderDocCaptureNext starts at the next one.
void NoteRenderDocPresent(u64 index);
/// TLG_RENDERDOC=1: RenderDoc captures the next frame; false when RenderDoc is not loaded.
bool RenderDocCaptureNext();
/// The state as JSON text.
std::string RenderDocState();

} // namespace VideoCore
