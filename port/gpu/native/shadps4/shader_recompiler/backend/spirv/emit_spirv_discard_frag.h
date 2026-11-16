// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"
#include "native/shadps4/shader_recompiler/runtime_info.h"

namespace VkbShader::Backend::SPIRV {

[[nodiscard]] std::vector<u32> EmitDiscardFragmentShader(std::array<VkbShader::OutputMap, 3> outputs);

} // namespace VkbShader::Backend::SPIRV
