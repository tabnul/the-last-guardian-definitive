// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// TLG_VKB_SHADER_PREBUILD: the new engine's shader list. A shipped file (shader-list/<serial>.vkb.tlgsl next to the executable) that names
// the shader stages the game needs without holding any program bytes: each entry is the recipe of one translation (native_vkb_shader_recipe.h)
// with its program replaced by where the player's own game files hold it (an entry of a .psarc archive or a game file, an offset, the length
// and a CRC-32), and its fetch shader (code too) replaced by the vertex input layout the translator reads from it. The rest is state: the
// user data words, the descriptor bytes the translator read, the flattened user data, the runtime info and binding start (in the portable
// form of native_vkb_shader_fields.h, so a Windows build reads a list made on Linux), the salt and the local size.
//
// The same container as the normal renderer's list (video_core/renderer_vulkan/shader_list.cpp: "TLGSLIST", a version, the payload size, an
// id, the payload packed with zlib) and the same game-file reader (its GameFiles, which reads trico_bootp/trico_boot.psarc and files of the
// game folder): port/tools/shader-list/make_list.py --vkb writes it, check_no_code.py proves it holds no program bytes. The version is the
// new engine's own (ListVersion), so neither renderer reads the other's list.
//
// Payload: u32 sources, per source (u8 kind, u32 n, n bytes ident); u32 entries, per entry (u64 program hash, u32 source, u64 offset, u32
// length, u32 crc, u32 n, n bytes body: the recipe's state, written and read by native_vkb_shader_recipe.cpp); u32 guessed, u64 program hash
// each (the programs whose entries are guesses, sorted; empty until the list has guesses).

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Vkb::ShaderList {

inline constexpr u32 ListVersion = 101; ///< the new engine's list, form 1 (the normal renderer's lists are versions 1 to 4)

/// The list of this game: TLG_VKB_SHADER_LIST (a path; "0": none), else <serial>.vkb.tlgsl in shader-list/ next to the executable or one folder
/// above it (a build in port/out). Empty when there is none.
[[nodiscard]] std::filesystem::path Path();

struct Item {
    u64 pgm_hash{};
    u32 source{};
    u64 offset{};
    u32 length{}, crc{};
    std::vector<u8> body;
};

struct List {
    std::vector<std::pair<u8, std::vector<u8>>> sources; ///< kind (1: .psarc entry by the MD5 of its path, 2: game file by name), ident
    std::vector<Item> items;
    std::vector<u64> guessed; ///< sorted
};

/// Reads a list; nullopt with the reason in `error` when it is not one, is damaged, or carries another portable id than `portable_id` (made
/// with other field lists or another entry form).
std::optional<List> Read(const std::filesystem::path& path, u64 portable_id, std::string& error);

/// An id of the list file's contents (its size and a hash of its bytes): the shader pre-build's "built" marker names it. 0 when unreadable.
[[nodiscard]] u64 FileId(const std::filesystem::path& path);

/// The files of the list build in the store folder (they go with it: the launcher's "clear shader cache" removes them, and a new build of
/// the engine, card or setting has a new folder without them). Progress: the entry a build process works on. Result: its counts. Left out: a
/// first line "list <id>", then one line per entry a build process stopped on (its number, how it stopped). Built: the marker of a finished
/// build, first line "list <id> guesses <0|1>": a start that finds it with the same first line builds nothing.
inline std::filesystem::path ProgressFile(const std::filesystem::path& store, u32 part) {
    return store / ("list-build-progress-" + std::to_string(part) + ".txt");
}
inline std::filesystem::path ResultFile(const std::filesystem::path& store, u32 part) {
    return store / ("list-build-result-" + std::to_string(part) + ".txt");
}
inline std::filesystem::path LeftOutFile(const std::filesystem::path& store) {
    return store / "list-build-left-out.txt";
}
inline std::filesystem::path BuiltMarker(const std::filesystem::path& store) {
    return store / "list-built.txt";
}

/// The game folder on the host; empty for a packed game (.zar), which the list cannot read yet.
[[nodiscard]] std::filesystem::path HostGameDir();

/// Reads programs out of the game folder for the list's items: the normal renderer's GameFiles (renderer-neutral: it only reads files).
class Programs {
public:
    Programs(std::filesystem::path game_dir, const List& list);
    ~Programs();
    Programs(const Programs&) = delete;
    Programs& operator=(const Programs&) = delete;
    /// The program of `item`, checked by its length and CRC-32; nullopt when the game files do not have it.
    std::optional<std::vector<u8>> Get(const Item& item);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Core::Vkb::ShaderList
