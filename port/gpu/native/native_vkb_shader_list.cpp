// SPDX-License-Identifier: GPL-2.0-or-later
// TLG_VKB_SHADER_PREBUILD: the new engine's shader list (see the header)

#include "native/native_vkb_shader_list.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fmt/format.h>
#include <miniz.h>
#include <xxhash.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/memory_patcher.h"
// The normal renderer's list reader: only its game-file reader (GameFiles) is used, which reads the player's game files and nothing of the
// renderer (port/gpu/shadps4 is not changed: principle 8).
#include "video_core/renderer_vulkan/shader_list.h"

extern "C" int runtime_file_translate(const char* guest, char* out, size_t size);

namespace Core::Vkb::ShaderList {
namespace {

constexpr std::array<char, 8> Magic{'T', 'L', 'G', 'S', 'L', 'I', 'S', 'T'};

std::vector<u8> ReadAll(const std::filesystem::path& path) {
    std::ifstream f{path, std::ios::binary};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct Reader {
    const std::vector<u8>& in;
    size_t pos = 0;
    bool ok = true;
    template <typename T>
    T Pod() {
        T v{};
        if (!ok || sizeof(T) > in.size() - pos) {
            ok = false;
            return v;
        }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    std::vector<u8> Blob() {
        const u32 n = Pod<u32>();
        if (!ok || n > in.size() - pos) {
            ok = false;
            return {};
        }
        std::vector<u8> v(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return v;
    }
};

std::filesystem::path ExecutableDir() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return n ? std::filesystem::path{std::wstring{exe, n}}.parent_path() : std::filesystem::path{};
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

} // namespace

std::filesystem::path Path() {
    if (const char* env = std::getenv("TLG_VKB_SHADER_LIST"); env != nullptr && *env != '\0') {
        return std::strcmp(env, "0") == 0 ? std::filesystem::path{} : std::filesystem::u8path(env);
    }
    const auto name = MemoryPatcher::g_game_serial + ".vkb.tlgsl";
    const auto dir = ExecutableDir();
    for (const auto& candidate : {dir / "shader-list" / name, dir.parent_path() / "shader-list" / name}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

std::optional<List> Read(const std::filesystem::path& path, u64 portable_id, std::string& error) {
    const auto file = ReadAll(path);
    constexpr size_t Header = Magic.size() + 4 + 4 + 8 + 8;
    if (file.size() < Header || std::memcmp(file.data(), Magic.data(), Magic.size()) != 0) {
        error = "not a shader list";
        return std::nullopt;
    }
    u32 version = 0;
    u64 size = 0, id = 0;
    std::memcpy(&version, file.data() + Magic.size(), 4);
    std::memcpy(&size, file.data() + Magic.size() + 8, 8);
    std::memcpy(&id, file.data() + Magic.size() + 16, 8);
    if (version != ListVersion) {
        error = fmt::format("list version {} (the new engine reads version {} only; versions 1 to 4 are the normal renderer's)", version, ListVersion);
        return std::nullopt;
    }
    if (id != portable_id) {
        error = fmt::format("list made with other field lists or another entry form (portable id {:#018x}, this build {:#018x})", id, portable_id);
        return std::nullopt;
    }
    if (size > (u64{512} << 20)) {
        error = "damaged list (size)";
        return std::nullopt;
    }
    std::vector<u8> payload(size);
    mz_ulong length = static_cast<mz_ulong>(size);
    if (mz_uncompress(payload.data(), &length, file.data() + Header, static_cast<mz_ulong>(file.size() - Header)) != MZ_OK || length != size) {
        error = "damaged list (it does not unpack)";
        return std::nullopt;
    }
    Reader r{payload};
    List list;
    const u32 sources = r.Pod<u32>();
    for (u32 i = 0; r.ok && i < sources; ++i) {
        const u8 kind = r.Pod<u8>();
        list.sources.emplace_back(kind, r.Blob());
    }
    const u32 items = r.Pod<u32>();
    for (u32 i = 0; r.ok && i < items; ++i) {
        Item item;
        item.pgm_hash = r.Pod<u64>();
        item.source = r.Pod<u32>();
        item.offset = r.Pod<u64>();
        item.length = r.Pod<u32>();
        item.crc = r.Pod<u32>();
        item.body = r.Blob();
        if (r.ok && item.source >= list.sources.size()) {
            error = "damaged list (an entry names no source)";
            return std::nullopt;
        }
        list.items.push_back(std::move(item));
    }
    const u32 guessed = r.Pod<u32>();
    for (u32 i = 0; r.ok && i < guessed; ++i) {
        list.guessed.push_back(r.Pod<u64>());
    }
    if (!r.ok || r.pos != payload.size()) {
        error = "damaged list";
        return std::nullopt;
    }
    std::ranges::sort(list.guessed);
    return list;
}

u64 FileId(const std::filesystem::path& path) {
    const auto bytes = ReadAll(path);
    return bytes.empty() ? 0 : XXH3_64bits_withSeed(bytes.data(), bytes.size(), bytes.size());
}

std::filesystem::path HostGameDir() {
    char host[4096];
    if (runtime_file_translate("/app0/eboot.bin", host, sizeof(host)) != 0) {
        return {};
    }
    return std::filesystem::u8path(host).parent_path();
}

struct Programs::Impl {
    Vulkan::ShaderList::List list; ///< only its sources: GameFiles reads them
    Vulkan::ShaderList::GameFiles files;
    Impl(std::filesystem::path game_dir, const List& in) : list{make(in)}, files{std::move(game_dir), list} {}
    static Vulkan::ShaderList::List make(const List& in) {
        Vulkan::ShaderList::List out;
        for (const auto& [kind, ident] : in.sources) {
            out.sources.push_back({kind, ident});
        }
        return out;
    }
};

Programs::Programs(std::filesystem::path game_dir, const List& list) : impl_{std::make_unique<Impl>(std::move(game_dir), list)} {}
Programs::~Programs() = default;

std::optional<std::vector<u8>> Programs::Get(const Item& item) {
    Vulkan::ShaderList::Entry entry;
    entry.source = item.source;
    entry.offset = item.offset;
    entry.length = item.length;
    entry.crc = item.crc;
    return impl_->files.Program(entry);
}

} // namespace Core::Vkb::ShaderList
