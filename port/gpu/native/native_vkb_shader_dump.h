// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <span>
#include <nlohmann/json_fwd.hpp>
#include "common/types.h"

/// note 89: every shader of the scene at the frame walk freeze. Each compiled variant keeps a copy of its SPIR-V and its GCN program (Keep, at compile time); while
/// debugger mode records (Shift+F5 .. freeze) every draw and dispatch notes the shaders it uses (one relaxed load when debugger mode is off); the freeze writes the
/// used ones to <freeze folder>/shaders/ (<stage>_<hash>_<n>.spv / .gcn) with index.json (uses, the vertex / pixel pairs, the files).
namespace Core::Vkb::ShaderDump {

enum class Stage : u8 { Vertex, Pixel, Compute, AsyncCompute };

/// Compile time (under the variant lock): a copy of the module and the program it came from (distinct modules of one hash kept apart: specialisations differ).
void Keep(Stage stage, u64 hash, std::span<const u32> spv, std::span<const u32> gcn);

/// Debugger mode only (callers check Core::FrameWalk::DebuggerMode()): a draw with this vertex / pixel shader pair (pixel 0: depth only), or a dispatch.
void NoteDraw(u64 vs, u64 ps);
void NoteDispatch(u64 cs, bool async);

/// Debugger mode starts: the use counts start over.
void Reset();

/// The freeze: writes the shaders used since debugger mode started into `dir` and returns the index (also written as dir/index.json).
nlohmann::json Write(const std::filesystem::path& dir);

} // namespace Core::Vkb::ShaderDump
