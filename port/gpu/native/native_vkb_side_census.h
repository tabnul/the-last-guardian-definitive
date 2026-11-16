// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <initializer_list>
#include <nlohmann/json_fwd.hpp>
#include "common/types.h"

/// Note 76 (debug only, TLG_VKB_SIDE_CENSUS=1): what the engine's PS4 functions that still run under TLG_VKB (clears, CopyResource, the target eliminate,
/// the clear quad, the HTILE clear, ResTransition) leave behind in the engine context and in their argument objects. Before the original runs, the context
/// (0x13200 bytes) and every heap object among the arguments (0x200 bytes) are copied; after it, every 32-bit word that changed is counted by
/// (function, region, offset) with a few of its old -> new values. Those are the fields a skip of the original has to write itself (as ExCsDepthCopy does).
namespace Core::Vkb::SideCensus {

[[nodiscard]] bool On();
/// Starts a snapshot on this thread (nested calls stack). `ctx` 0: no context; `args`: up to 6 values, heap pointers among them are snapshotted.
void Begin(const char* function, u64 ctx, std::initializer_list<u64> args);
/// Compares with the innermost snapshot of this thread and files the differences.
void End();
/// RAII form of Begin / End.
class Scope {
public:
    Scope(const char* function, u64 ctx, std::initializer_list<u64> args) : on{On()} {
        if (on) {
            Begin(function, ctx, args);
        }
    }
    ~Scope() {
        if (on) {
            End();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool on;
};
[[nodiscard]] nlohmann::json Report();

} // namespace Core::Vkb::SideCensus

namespace VkbShader::IR {
struct Program;
}
/// Note 77 (debug only, TLG_VKB_FLAT_CENSUS=1): what a compiled shader reads from its flat user data at run time. Every GetUserData and ReadConst left in the final
/// program, by the user-data input (the engine shader's usage table at `gnm + inputs_offset`) whose registers it reads: descriptor words, table pointers or plain data.
namespace Core::Vkb::FlatCensus {
[[nodiscard]] bool On();
void Note(const VkbShader::IR::Program& program, u64 gnm, u32 inputs_offset, const char* stage, u64 hash);
[[nodiscard]] nlohmann::json Report();
} // namespace Core::Vkb::FlatCensus
