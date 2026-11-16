// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "native/shadps4/shader_recompiler/frontend/control_flow_graph.h"
#include "native/shadps4/shader_recompiler/ir/abstract_syntax_list.h"
#include "native/shadps4/shader_recompiler/ir/basic_block.h"
#include "native/shadps4/shader_recompiler/ir/value.h"

namespace VkbShader {
struct Info;
struct Profile;
struct RuntimeInfo;
struct Pools;
} // namespace VkbShader

namespace VkbShader::Gcn {

[[nodiscard]] IR::AbstractSyntaxList BuildASL(Pools& pools, CFG& cfg, Info& info);

} // namespace VkbShader::Gcn
