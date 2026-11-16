// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "native/shadps4/shader_recompiler/frontend/instruction.h"
#include "native/shadps4/shader_recompiler/info.h"
#include "native/shadps4/shader_recompiler/ir/abstract_syntax_list.h"
#include "native/shadps4/shader_recompiler/ir/basic_block.h"

namespace VkbShader::IR {

struct Program {
    explicit Program(Info& info_) : info{info_} {}

    AbstractSyntaxList syntax_list;
    BlockList blocks;
    BlockList post_order_blocks;
    std::vector<Gcn::GcnInst> ins_list;
    u32 next_reg_index{};
    Info& info;
};

void DumpProgram(const Program& program, const Info& info, const std::string& type = "");

} // namespace VkbShader::IR
