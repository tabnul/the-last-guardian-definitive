// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_builtin_data.h"

#include <fstream>
#include <sstream>
#include "common/path_util.h"

// The files of native/data, embedded (the paths come from CMake: TLG_VKB_HOOKS_PATH, TLG_VKB_LUA_PATCHES_PATH)
#ifdef _WIN32
#define TLG_VKB_SECTION ".section .rdata,\"dr\"\n"
#define TLG_VKB_HIDE(name) ""
#define TLG_VKB_END ".text\n"
#else
#define TLG_VKB_SECTION ".section .rodata\n"
#define TLG_VKB_HIDE(name) ".hidden " name "\n"
#define TLG_VKB_END ".previous\n"
#endif
asm(TLG_VKB_SECTION ".balign 16\n" TLG_VKB_HIDE("tlg_vkb_hooks_json") ".global tlg_vkb_hooks_json\n"
    "tlg_vkb_hooks_json:\n"
    ".incbin \"" TLG_VKB_HOOKS_PATH "\"\n" TLG_VKB_HIDE("tlg_vkb_hooks_json_end") ".global tlg_vkb_hooks_json_end\n"
    "tlg_vkb_hooks_json_end:\n"
    ".balign 16\n" TLG_VKB_HIDE("tlg_vkb_lua_patches_json") ".global tlg_vkb_lua_patches_json\n"
    "tlg_vkb_lua_patches_json:\n"
    ".incbin \"" TLG_VKB_LUA_PATCHES_PATH "\"\n" TLG_VKB_HIDE("tlg_vkb_lua_patches_json_end") ".global tlg_vkb_lua_patches_json_end\n"
    "tlg_vkb_lua_patches_json_end:\n" TLG_VKB_END);
extern "C" const char tlg_vkb_hooks_json[];
extern "C" const char tlg_vkb_hooks_json_end[];
extern "C" const char tlg_vkb_lua_patches_json[];
extern "C" const char tlg_vkb_lua_patches_json_end[];

namespace Core::BuiltinData {

std::string Read(const std::string& serial, std::string_view suffix, std::string& source) {
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / (serial + std::string{suffix});
    if (std::ifstream in{path, std::ios::binary}) {
        std::ostringstream text;
        text << in.rdbuf();
        source = path.string();
        return text.str();
    }
    // US CUSA03627 and EU CUSA03745 v1.03 ship the same code: all hook 'expect' bytes match both.
    if (serial != "CUSA03627" && serial != "CUSA03745") {
        return {};
    }
    if (suffix == ".hooks.json") {
        source = "built in (native/data/CUSA03627.hooks.json)";
        return std::string{tlg_vkb_hooks_json, tlg_vkb_hooks_json_end};
    }
    if (suffix == ".lua_patches.json") {
        source = "built in (native/data/CUSA03627.lua_patches.json)";
        return std::string{tlg_vkb_lua_patches_json, tlg_vkb_lua_patches_json_end};
    }
    return {};
}

} // namespace Core::BuiltinData
