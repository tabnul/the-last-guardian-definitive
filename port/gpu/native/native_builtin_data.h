// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

/// The backend's per game files, built into the binary (merge plan M3 (5); native/data): the guest hooks (CUSA03627.hooks.json) and the Lua load-time patches
/// (CUSA03627.lua_patches.json). A file of the same name in the user folder's custom_configs overrides the built-in one (development).
namespace Core::BuiltinData {

/// `serial` + `suffix` (".hooks.json", ".lua_patches.json"): the user's file when it exists, else the built-in text; empty when neither exists
[[nodiscard]] std::string Read(const std::string& serial, std::string_view suffix, std::string& source);

} // namespace Core::BuiltinData
