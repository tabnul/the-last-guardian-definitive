// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "native/shadps4/shader_recompiler/ir/abstract_syntax_list.h"
#include "native/shadps4/shader_recompiler/ir/basic_block.h"

namespace VkbShader::IR {

BlockList PostOrder(Block* const first_block);

} // namespace VkbShader::IR
