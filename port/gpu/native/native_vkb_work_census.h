// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <nlohmann/json_fwd.hpp>
#include "common/types.h"

/// D2 census (engine-vulkan-backend plan, review order D2; `TLG_VKB_WORK_CENSUS=1`, debug only, off by default): under the backend, every engine work call our hooks see
/// (fills, copies, clears, transitions, eliminate passes, draws, dispatches), counted by kind, by the engine's call site (Ghidra address of the hooked function's return)
/// and by the hooked engine function it ran inside (a scope set around the engine code our hooks let run), with the bytes it moved. The question it answers: which of
/// our Vulkan work, and which engine PS4 code, runs only because the engine's PS4 implementation of an API call does it (note 50's ClearRenderTargetView fills).
namespace Core::Vkb::WorkCensus {

[[nodiscard]] bool On();
/// One call of `kind` from engine return address `ret` (a guest address; 0 when unknown), moving `bytes`
void Note(const char* kind, u64 ret, u64 bytes = 0);
/// The engine function `name` runs on this thread while the scope lives: work noted meanwhile is filed under it (scopes nest; the innermost names it)
class Scope {
public:
    explicit Scope(const char* name);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* previous{};
    bool active{};
};
[[nodiscard]] nlohmann::json Report();

} // namespace Core::Vkb::WorkCensus
