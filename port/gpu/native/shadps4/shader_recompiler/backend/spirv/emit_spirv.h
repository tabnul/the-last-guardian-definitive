// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "native/shadps4/shader_recompiler/backend/bindings.h"
#include "native/shadps4/shader_recompiler/ir/program.h"
#include "native/shadps4/shader_recompiler/profile.h"

namespace VkbShader::Backend::SPIRV {

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                                         const IR::Program& program, Bindings& binding);

} // namespace VkbShader::Backend::SPIRV
