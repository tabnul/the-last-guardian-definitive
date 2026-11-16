// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "native/shadps4/shader_recompiler/ir/basic_block.h"

namespace VkbShader::IR {

void ComputeDominators(const IR::BlockList& post_order);

void ComputeDominanceFrontiers(const IR::BlockList& post_order);

inline bool Dominates(IR::Block* a, IR::Block* b) {
    IR::Block* current = b;
    while (current) {
        if (current == a) {
            return true;
        }
        if (current->immediate_dominator == current) {
            break;
        }
        current = current->immediate_dominator;
    }
    return false;
}

} // namespace VkbShader::IR