// SPDX-License-Identifier: GPL-2.0-or-later
// TLG_VKB_SHADER_RECIPES: the new engine's shader recipes and the build process that fills the shader store from them (see the header)

#include "native/native_vkb_shader_recipe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <xxhash.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <csignal>
#include <unistd.h>
#endif

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "native/native_vkb_shader_fields.h"
#include "native/native_vkb_shader_list.h"
#include "native/native_vkb_write_window.h"
#include "native/shadps4/shader_recompiler/backend/spirv/emit_spirv.h"
#include "native/shadps4/shader_recompiler/frontend/fetch_shader.h"
#include "native/shadps4/shader_recompiler/recompiler.h"

namespace Core::Vkb::ShaderRecipe {
namespace {

constexpr u32 Magic = 0x524b5654; // "TVKR"
constexpr u32 Version = 1;

// ---- recording ----

/// Guest memory the translator read on this thread since the recording started, by address (the bytes as it read them)
thread_local std::map<u64, std::vector<u8>> regions;
/// a read at an address already kept with other bytes, or no memory for the copy: this recording gives no recipe (counted)
thread_local const char* recording_broken = nullptr;

void Record(const void* address, const void* bytes, size_t size) noexcept { // called from the translator's noexcept reads
    if (address == nullptr || size == 0) {
        return;
    }
    try {
        auto& kept = regions[reinterpret_cast<u64>(address)];
        const size_t common = std::min(kept.size(), size);
        if (std::memcmp(kept.data(), bytes, common) != 0) { // the game changed it between two reads of one translation
            recording_broken = "the translation read one address twice with different bytes";
        }
        if (kept.size() < size) {
            kept.assign(static_cast<const u8*>(bytes), static_cast<const u8*>(bytes) + size);
        }
    } catch (...) {
        recording_broken = "no memory for the recording";
    }
}

struct Writer {
    std::vector<u8> out;
    void Raw(const void* p, size_t n) {
        const auto* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    }
    template <typename T>
    void Pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        Raw(&v, sizeof(T));
    }
    void Bytes(const void* p, size_t n) {
        Pod(static_cast<u64>(n));
        Raw(p, n);
    }
};

struct Reader {
    const u8* p;
    size_t left;
    bool ok = true;
    void Raw(void* dst, size_t n) {
        if (!ok || n > left) {
            ok = false;
            std::memset(dst, 0, n);
            return;
        }
        std::memcpy(dst, p, n);
        p += n;
        left -= n;
    }
    template <typename T>
    T Pod() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v;
        Raw(&v, sizeof(T));
        return v;
    }
    std::vector<u8> Bytes() {
        const u64 n = Pod<u64>();
        if (!ok || n > left) {
            ok = false;
            return {};
        }
        std::vector<u8> v(p, p + n);
        p += n;
        left -= n;
        return v;
    }
};

std::filesystem::path RecipeFolder() {
    static const std::filesystem::path dir = [] {
        auto d = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / (MemoryPatcher::g_game_serial + "-vkb-recipes");
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        if (ec) {
            LOG_ERROR(Render_Vulkan, "vkb shader recipes: could not create {}: {}; no recipes are kept", d.string(), ec.message());
            return std::filesystem::path{};
        }
        return d;
    }();
    return dir;
}

std::atomic<u64> c_recorded{0}, c_not_taken{0}, c_no_folder{0}, c_broken{0};

// ---- the build process ----

struct Recipe {
    u32 kind{}; ///< 0 graphics, 1 compute
    VkbShader::HwStage hw_stage{};
    VkbShader::SwStage sw_stage{};
    u64 pgm_hash{};
    u64 pgm_base{};
    u64 code_words{};
    std::array<u32, VkbShader::ShaderParams::NumShaderUserData> user_data{};
    std::vector<u32> flat;
    VkbShader::RuntimeInfo runtime{};
    VkbShader::Backend::Bindings start{};
    u64 salt{};
    u32 local_x{};
    std::vector<std::pair<u64, std::vector<u8>>> regions;
    /// a shader list entry: the vertex input layout in place of the fetch shader's code (VkbShader::Gcn::replay_fetch)
    std::optional<VkbShader::Gcn::FetchShaderData> fetch;
};

/// A recipe file; nullopt with the reason in `why` when this build cannot read it
std::optional<Recipe> Parse(const std::vector<u8>& bytes, const char*& why) {
    if (bytes.size() < 8 + 24 || XXH3_64bits(bytes.data(), bytes.size() - 8) != [&] { u64 v; std::memcpy(&v, bytes.data() + bytes.size() - 8, 8); return v; }()) {
        why = "is damaged or cut";
        return std::nullopt;
    }
    Reader r{bytes.data(), bytes.size() - 8};
    const u32 magic = r.Pod<u32>(), version = r.Pod<u32>(), runtime_size = r.Pod<u32>(), bindings_size = r.Pod<u32>();
    const u64 layout = r.Pod<u64>();
    if (magic != Magic) {
        why = "is not a new-engine recipe";
        return std::nullopt;
    }
    if (version != Version) {
        why = "has a recipe version this build does not read";
        return std::nullopt;
    }
    if (runtime_size != sizeof(VkbShader::RuntimeInfo) || bindings_size != sizeof(VkbShader::Backend::Bindings) || layout != ShaderFields::LayoutId()) {
        why = "was made by a build that lays its runtime info or bindings out otherwise (layout id differs)";
        return std::nullopt;
    }
    Recipe rc;
    rc.kind = r.Pod<u32>();
    rc.hw_stage = static_cast<VkbShader::HwStage>(r.Pod<u32>());
    rc.sw_stage = static_cast<VkbShader::SwStage>(r.Pod<u32>());
    rc.pgm_hash = r.Pod<u64>();
    rc.pgm_base = r.Pod<u64>();
    rc.code_words = r.Pod<u64>();
    r.Raw(rc.user_data.data(), sizeof(rc.user_data));
    const std::vector<u8> flat = r.Bytes();
    rc.flat.resize(flat.size() / sizeof(u32));
    std::memcpy(rc.flat.data(), flat.data(), rc.flat.size() * sizeof(u32));
    r.Raw(&rc.runtime, sizeof(rc.runtime));
    r.Raw(&rc.start, sizeof(rc.start));
    rc.salt = r.Pod<u64>();
    rc.local_x = r.Pod<u32>();
    const u64 count = r.Pod<u64>();
    bool addresses_ok = true;
    for (u64 i = 0; r.ok && i < count; ++i) {
        const u64 address = r.Pod<u64>();
        rc.regions.emplace_back(address, r.Bytes());
        // a guest address: above the first 64 KB and below the 40-bit range the PS4's graphics chip addresses (no wrap-around)
        addresses_ok &= address >= 0x10000 && address < (u64{1} << 40) && rc.regions.back().second.size() <= (u64{1} << 40) - address;
    }
    if (r.ok && !addresses_ok) {
        why = "names memory outside the game's address range";
        return std::nullopt;
    }
    if (r.ok && (static_cast<u32>(rc.hw_stage) > static_cast<u32>(VkbShader::HwStage::Compute) || static_cast<u32>(rc.sw_stage) >= VkbShader::MaxStageTypes)) {
        why = "names a stage this build does not know";
        return std::nullopt;
    }
    // the recording keeps them by address, in order: overlaps must agree, or the replay is not what the game read
    for (size_t i = 1; r.ok && i < rc.regions.size(); ++i) {
        const auto& [b, bbytes] = rc.regions[i];
        if (b < rc.regions[i - 1].first) {
            why = "is damaged (regions out of order)";
            return std::nullopt;
        }
        for (size_t j = 0; j < i; ++j) {
            const auto& [a, abytes] = rc.regions[j];
            if (b < a + abytes.size() && std::memcmp(abytes.data() + (b - a), bbytes.data(), std::min<u64>(a + abytes.size() - b, bbytes.size())) != 0) {
                why = "holds two overlapping reads with different bytes";
                return std::nullopt;
            }
        }
    }
    if (!r.ok || r.left != 0 || rc.kind > 1 || flat.size() % sizeof(u32) != 0) {
        why = "is damaged";
        return std::nullopt;
    }
    const auto program = std::ranges::find(rc.regions, rc.pgm_base, &std::pair<u64, std::vector<u8>>::first);
    if (program == rc.regions.end() || program->second.size() != rc.code_words * sizeof(u32) || rc.code_words == 0) {
        why = "holds no program code";
        return std::nullopt;
    }
    return rc;
}

/// Guest pages mapped at their own addresses in the build process, for one recipe (the normal renderer's GuestMapping,
/// video_core/renderer_vulkan/shader_recipe.cpp, copied: the engine does not use the normal renderer's code)
class GuestMapping {
public:
    explicit GuestMapping(const std::vector<std::pair<u64, std::vector<u8>>>& regions) {
        constexpr u64 Page = 0x10000; // Windows' allocation granularity, also fine on Linux
        std::map<u64, u64> spans;     // start -> end, page aligned and merged
        for (const auto& [address, bytes] : regions) {
            u64 start = address & ~(Page - 1);
            u64 end = (address + bytes.size() + Page - 1) & ~(Page - 1);
            auto it = spans.upper_bound(start);
            if (it != spans.begin() && std::prev(it)->second >= start) {
                --it;
                start = it->first;
            }
            while (it != spans.end() && it->first <= end) {
                end = std::max(end, it->second);
                it = spans.erase(it);
            }
            spans.emplace(start, end);
        }
        for (const auto& [start, end] : spans) {
            if (!Map(start, end - start)) {
                ok = false;
                failed_at = start;
                return;
            }
            mapped.emplace_back(start, end - start);
        }
        for (const auto& [address, bytes] : regions) {
            std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
        }
    }
    ~GuestMapping() {
        for (const auto& [address, size] : mapped) {
#ifdef _WIN32
            VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE);
#else
            munmap(reinterpret_cast<void*>(address), size);
#endif
        }
    }
    GuestMapping(const GuestMapping&) = delete;
    GuestMapping& operator=(const GuestMapping&) = delete;
    bool ok = true;
    u64 failed_at = 0;

private:
    static bool Map(u64 address, u64 size) {
#ifdef _WIN32
        return VirtualAlloc(reinterpret_cast<void*>(address), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == reinterpret_cast<void*>(address);
#else
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
        void* p = mmap(reinterpret_cast<void*>(address), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) {
            return false;
        }
        if (p != reinterpret_cast<void*>(address)) { // a kernel without MAP_FIXED_NOREPLACE
            munmap(p, size);
            return false;
        }
        return true;
#endif
    }
    std::vector<std::pair<u64, u64>> mapped;
};

/// The recorded memory of the recipe being built (guest_valid_hook): an address the game's translator could read is in it
thread_local const std::vector<std::pair<u64, std::vector<u8>>>* replay_regions = nullptr;
bool Recorded(const void* address, size_t size) {
    const u64 a = reinterpret_cast<u64>(address);
    for (const auto& [start, bytes] : *replay_regions) {
        if (a >= start && a + size <= start + bytes.size()) {
            return true;
        }
    }
    return false;
}

/// Sets the replay hooks of the translator for one recipe, and clears them at the end
struct Replay {
    Replay(const Recipe& rc) {
        replay_regions = &rc.regions;
        VkbShader::replay_flat = &rc.flat;
        VkbShader::guest_valid_hook = &Recorded;
        VkbShader::Gcn::replay_fetch = rc.fetch ? &*rc.fetch : nullptr;
    }
    ~Replay() {
        VkbShader::Gcn::replay_fetch = nullptr;
        VkbShader::replay_flat = nullptr;
        VkbShader::guest_valid_hook = nullptr;
        replay_regions = nullptr;
    }
    Replay(const Replay&) = delete;
    Replay& operator=(const Replay&) = delete;
};

/// One translation of a recipe (its memory mapped, its replay hooks set): the result in `out`, or the reason it gave none
std::string Translate(const Recipe& rc, const VkbShader::ShaderParams& params, VkbShader::Pools& pools, const VkbShader::Profile& profile,
                      ShaderStore::Translation& out) {
    out.info = std::make_unique<VkbShader::Info>(rc.hw_stage, rc.sw_stage, params);
    VkbShader::Info& info = *out.info;
    info.pgm_base = params.Base();
    out.runtime = rc.runtime;
    out.end = rc.start;
    out.spv.clear();
    out.windows.clear();
    std::string failed;
    try {
        const auto program = VkbShader::TranslateProgram(params.code, pools, info, out.runtime, profile);
        if (info.translation_failed) {
            failed = "the translation failed";
        } else {
            out.spv = VkbShader::Backend::SPIRV::EmitSPIRV(profile, out.runtime, program, out.end);
            if (rc.kind == 1) {
                out.windows = WriteWindow::Analyze(program, info, rc.local_x);
            }
        }
    } catch (const std::exception& e) {
        failed = fmt::format("the translation stopped: {}", e.what());
    }
    if (failed.empty() && out.spv.empty()) {
        failed = "the translation gave no module";
    }
    return failed; // the caller releases the pools' contents (after Keep, as before)
}

VkbShader::ShaderParams ParamsOf(const Recipe& rc) {
    return VkbShader::ShaderParams{
        .user_data = std::span<const u32, VkbShader::ShaderParams::NumShaderUserData>{rc.user_data.data(), VkbShader::ShaderParams::NumShaderUserData},
        .code = std::span<const u32>{reinterpret_cast<const u32*>(rc.pgm_base), static_cast<size_t>(rc.code_words)},
        .hash = rc.pgm_hash,
    };
}

// ---- the shader list's entry form (native_vkb_shader_list.h: an item's body) ----

/// Names every part of the body below, in order: changing the form changes this text and so the list's portable id
constexpr const char* EntryForm = "body 1: kind hw sw base words ud16 flat runtime start salt local_x fetch(size vofs iofs n; dest_vgpr num_elements sgpr_base "
                                  "dword_offset instance_data inst_offset data_format num_format) regions(address bytes)";


struct BodyWriter {
    std::vector<u8> out;
    template <typename T>
    void Pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* b = reinterpret_cast<const u8*>(&v);
        out.insert(out.end(), b, b + sizeof(T));
    }
    void Blob(const void* p, size_t n) {
        Pod(static_cast<u32>(n));
        const auto* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    }
};

/// A list entry's body from a recipe: everything but the program (named by the list item) and the fetch shader's code (`fetch` instead).
/// `regions`: the recorded memory without those two.
std::vector<u8> EncodeBody(const Recipe& rc, const std::optional<VkbShader::Gcn::FetchShaderData>& fetch,
                           const std::vector<std::pair<u64, std::vector<u8>>>& regions) {
    BodyWriter w;
    w.Pod(rc.kind);
    w.Pod(static_cast<u32>(rc.hw_stage));
    w.Pod(static_cast<u32>(rc.sw_stage));
    w.Pod(rc.pgm_base);
    w.Pod(rc.code_words);
    for (const u32 word : rc.user_data) {
        w.Pod(word);
    }
    w.Blob(rc.flat.data(), rc.flat.size() * sizeof(u32));
    const auto runtime = ShaderFields::PortableRuntime(rc.runtime);
    w.Blob(runtime.data(), runtime.size());
    const auto start = ShaderFields::PortableBindings(rc.start);
    w.Blob(start.data(), start.size());
    w.Pod(rc.salt);
    w.Pod(rc.local_x);
    w.Pod(static_cast<u8>(fetch ? 1 : 0));
    if (fetch) {
        w.Pod(fetch->size);
        w.Pod(fetch->vertex_offset_sgpr);
        w.Pod(fetch->instance_offset_sgpr);
        w.Pod(static_cast<u8>(fetch->attributes.size()));
        for (const auto& a : fetch->attributes) {
            for (const u8 b : {a.dest_vgpr, a.num_elements, a.sgpr_base, a.dword_offset, a.instance_data, a.inst_offset, a.data_format, a.num_format}) {
                w.Pod(b);
            }
        }
    }
    w.Pod(static_cast<u32>(regions.size()));
    for (const auto& [address, bytes] : regions) {
        w.Pod(address);
        w.Blob(bytes.data(), bytes.size());
    }
    return std::move(w.out);
}

/// Reads a list entry's body
struct BodyReader {
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
    std::span<const u8> Blob() {
        const u32 n = Pod<u32>();
        if (!ok || n > in.size() - pos) {
            ok = false;
            return {};
        }
        const std::span<const u8> v{in.data() + pos, n};
        pos += n;
        return v;
    }
};

/// A recipe from a list entry's body and its program (read from the game files); nullopt with the reason in `why` when the body is damaged
std::optional<Recipe> DecodeBody(u64 pgm_hash, const std::vector<u8>& body, std::vector<u8> program, const char*& why) {
    BodyReader r{body};
    Recipe rc;
    rc.pgm_hash = pgm_hash;
    rc.kind = r.Pod<u32>();
    rc.hw_stage = static_cast<VkbShader::HwStage>(r.Pod<u32>());
    rc.sw_stage = static_cast<VkbShader::SwStage>(r.Pod<u32>());
    rc.pgm_base = r.Pod<u64>();
    rc.code_words = r.Pod<u64>();
    for (u32& word : rc.user_data) {
        word = r.Pod<u32>();
    }
    const auto flat = r.Blob();
    rc.flat.resize(flat.size() / sizeof(u32));
    if (!rc.flat.empty()) {
        std::memcpy(rc.flat.data(), flat.data(), rc.flat.size() * sizeof(u32));
    }
    const auto runtime = r.Blob();
    const auto start = r.Blob();
    const bool runtime_ok = r.ok && ShaderFields::RuntimeFromPortable(runtime, rc.runtime);
    const bool start_ok = r.ok && ShaderFields::BindingsFromPortable(start, rc.start);
    rc.salt = r.Pod<u64>();
    rc.local_x = r.Pod<u32>();
    if (r.Pod<u8>() != 0) {
        auto& fetch = rc.fetch.emplace();
        fetch.size = r.Pod<u32>();
        fetch.vertex_offset_sgpr = r.Pod<s8>();
        fetch.instance_offset_sgpr = r.Pod<s8>();
        const u8 n = r.Pod<u8>();
        for (u8 i = 0; r.ok && i < n; ++i) {
            auto& a = fetch.attributes.emplace_back();
            for (u8* b : {&a.dest_vgpr, &a.num_elements, &a.sgpr_base, &a.dword_offset, &a.instance_data, &a.inst_offset, &a.data_format, &a.num_format}) {
                *b = r.Pod<u8>();
            }
        }
    }
    const u32 count = r.Pod<u32>();
    bool addresses_ok = true;
    for (u32 i = 0; r.ok && i < count; ++i) {
        const u64 address = r.Pod<u64>();
        const auto bytes = r.Blob();
        rc.regions.emplace_back(address, std::vector<u8>{bytes.begin(), bytes.end()});
        addresses_ok &= address >= 0x10000 && address < (u64{1} << 40) && bytes.size() <= (u64{1} << 40) - address;
    }
    if (!r.ok || r.pos != body.size() || rc.kind > 1 || flat.size() % sizeof(u32) != 0) {
        why = "is damaged";
        return std::nullopt;
    }
    if (!runtime_ok || !start_ok) {
        why = "holds a runtime info or bindings this build does not read";
        return std::nullopt;
    }
    if (!addresses_ok || rc.pgm_base < 0x10000 || rc.pgm_base >= (u64{1} << 40) || program.size() > (u64{1} << 40) - rc.pgm_base) {
        why = "names memory outside the game's address range";
        return std::nullopt;
    }
    if (static_cast<u32>(rc.hw_stage) > static_cast<u32>(VkbShader::HwStage::Compute) || static_cast<u32>(rc.sw_stage) >= VkbShader::MaxStageTypes) {
        why = "names a stage this build does not know";
        return std::nullopt;
    }
    if (rc.code_words == 0 || program.size() != rc.code_words * sizeof(u32)) {
        why = "names a program of another length";
        return std::nullopt;
    }
    rc.regions.emplace_back(rc.pgm_base, std::move(program));
    std::ranges::sort(rc.regions, {}, &std::pair<u64, std::vector<u8>>::first);
    for (size_t i = 1; i < rc.regions.size(); ++i) {
        if (rc.regions[i].first == rc.regions[i - 1].first) {
            why = "names its program's address twice";
            return std::nullopt;
        }
        // the build maps the regions as they are: overlaps must agree, or the replay is not what the game read
        for (size_t j = 0; j < i; ++j) {
            const auto& [a, abytes] = rc.regions[j];
            const auto& [b, bbytes] = rc.regions[i];
            if (b < a + abytes.size() && std::memcmp(abytes.data() + (b - a), bbytes.data(), std::min<u64>(a + abytes.size() - b, bbytes.size())) != 0) {
                why = "holds two overlapping memory reads with different bytes";
                return std::nullopt;
            }
        }
    }
    return rc;
}

/// Recorded memory that may hold game code: never copied into a list (the fetch shader and the program are taken out before)
bool LooksLikeCode(const std::vector<u8>& bytes) {
    static constexpr std::string_view Header = "OrbShdr";
    if (bytes.size() > 256 || std::search(bytes.begin(), bytes.end(), Header.begin(), Header.end()) != bytes.end()) {
        return true;
    }
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        u32 word;
        std::memcpy(&word, bytes.data() + i, 4);
        if ((word & 0xFFFFFF00u) == 0xBE802000u) { // s_setpc_b64: the end of a fetch shader
            return true;
        }
    }
    return false;
}

/// Linux: a build process maps the recorded guest memory at its own addresses, some of them in the first gigabyte (the game's own data).
/// This kernel places the heap of a program like tlg-probe (not position independent) anywhere in that gigabyte at random, so a mapping
/// failed now and then (2 of 12 runs of one list part on this box). The process therefore starts itself again once without address
/// randomization (its heap then starts right after the program, below 0x1000000): the first thing each build entry does. When that is not
/// possible it goes on as it is (a mapping that fails is counted, as before).
void FixedLayout() {
#ifndef _WIN32
    const int persona = personality(0xffffffff);
    if (persona != -1 && (persona & ADDR_NO_RANDOMIZE) != 0) {
        return;
    }
    if (persona == -1) { // a sandbox may refuse the call
        std::printf("vkb build process: address randomization cannot be turned off here; guest memory that the heap holds is counted as not mapped\n");
        return;
    }
    std::ifstream f{"/proc/self/cmdline", std::ios::binary};
    const std::string cmdline{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    std::vector<char*> argv;
    for (size_t at = 0; at < cmdline.size(); at += std::strlen(cmdline.data() + at) + 1) {
        argv.push_back(const_cast<char*>(cmdline.data() + at));
    }
    if (argv.empty() || personality(static_cast<unsigned long>(persona) | ADDR_NO_RANDOMIZE) == -1) {
        std::printf("vkb build process: address randomization cannot be turned off here; guest memory that the heap holds is counted as not mapped\n");
        return;
    }
    argv.push_back(nullptr);
    std::fflush(stdout);
    execv("/proc/self/exe", argv.data());
    personality(static_cast<unsigned long>(persona)); // exec failed: on as it is
    std::printf("vkb build process: could not start again without address randomization; guest memory that the heap holds is counted as not mapped\n");
#endif
}

} // namespace

u64 ListPortableId() {
    static const u64 id = ShaderFields::PortableId(EntryForm);
    return id;
}

bool On() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_SHADER_RECIPES"); return e != nullptr && e[0] == '1'; }();
    return on;
}

Recording::Recording(bool active) : active_{active} {
    if (active_) {
        regions.clear();
        recording_broken = nullptr;
        VkbShader::guest_read_hook = &Record;
    }
}

Recording::~Recording() {
    if (active_) {
        VkbShader::guest_read_hook = nullptr;
        regions.clear();
        c_not_taken.fetch_add(1, std::memory_order_relaxed);
    }
}

void Recording::Take(const ShaderStore::Inputs& in, const VkbShader::Info& info, u32 local_x) {
    if (!active_) {
        return;
    }
    active_ = false;
    VkbShader::guest_read_hook = nullptr;
    auto read = std::move(regions);
    regions.clear();
    const auto folder = RecipeFolder();
    if (folder.empty()) { // said once when the folder could not be made; every recipe lost so is counted (report: dropped_no_folder)
        c_no_folder.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // the program itself, at its guest address (the translation reads it there: its base, inline constants)
    auto& program = read[in.params.Base()];
    if (program.size() > in.params.code.size_bytes()) {
        recording_broken = "the translation read past the end of the program's code";
    } else if (std::memcmp(program.data(), in.params.code.data(), program.size()) != 0) {
        recording_broken = "the program's bytes changed during the translation";
    }
    program.assign(reinterpret_cast<const u8*>(in.params.code.data()), reinterpret_cast<const u8*>(in.params.code.data()) + in.params.code.size_bytes());
    if (recording_broken != nullptr) {
        if (c_broken.fetch_add(1) < 3) { // all are counted (report: dropped_recording_broken)
            LOG_WARNING(Render_Vulkan, "vkb shader recipes: no recipe for program {:#x}: {}", in.params.hash, recording_broken);
        }
        return;
    }

    Writer w;
    w.Pod(Magic);
    w.Pod(Version);
    w.Pod(static_cast<u32>(sizeof(VkbShader::RuntimeInfo)));
    w.Pod(static_cast<u32>(sizeof(VkbShader::Backend::Bindings)));
    w.Pod(ShaderFields::LayoutId());
    w.Pod(static_cast<u32>(in.runtime.hw_stage == VkbShader::HwStage::Compute ? 1 : 0));
    w.Pod(static_cast<u32>(in.runtime.hw_stage));
    w.Pod(static_cast<u32>(in.runtime.sw_stage));
    w.Pod(static_cast<u64>(in.params.hash));
    w.Pod(static_cast<u64>(in.params.Base()));
    w.Pod(static_cast<u64>(in.params.code.size()));
    w.Raw(in.params.user_data.data(), in.params.user_data.size_bytes());
    const auto flat = info.FlatUserData();
    w.Bytes(flat.data(), flat.size_bytes());
    w.Pod(in.runtime);
    w.Pod(in.start);
    w.Pod(in.salt);
    w.Pod(local_x);
    w.Pod(static_cast<u64>(read.size()));
    for (const auto& [address, bytes] : read) {
        w.Pod(address);
        w.Bytes(bytes.data(), bytes.size());
    }
    w.Pod(static_cast<u64>(XXH3_64bits(w.out.data(), w.out.size())));
    const u64 digest = XXH3_64bits(w.out.data(), w.out.size());
    if (ShaderStore::QueueWrite(folder / fmt::format("{:016x}_{}_{}_{:016x}.vkr", in.params.hash, static_cast<u32>(in.runtime.hw_stage), static_cast<u32>(in.runtime.sw_stage), digest),
                            std::move(w.out))) {
        c_recorded.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {

enum class Built { Yes, Already, No };

/// Builds one recipe into the opened store, by the store's own rule: its memory mapped at its own addresses, Find first (same inputs and
/// resources of the same kinds: Already), else a translation kept through Keep. No: the reason in `why`.
Built BuildOne(const Recipe& rc, VkbShader::Pools& pools, const VkbShader::Profile& profile, std::string& why) {
    GuestMapping mapping{rc.regions};
    if (!mapping.ok) {
        why = fmt::format("its guest memory cannot be mapped at its own address (taken in this process near {:#x})", mapping.failed_at);
        return Built::No;
    }
    const auto params = ParamsOf(rc);
    const Replay replay{rc}; // also for Find: its resource-kind check refreshes the flattened user data, from the recipe here
    const ShaderStore::Inputs in{.params = params, .runtime = rc.runtime, .start = rc.start, .salt = rc.salt};
    ShaderStore::Translation stored;
    if (ShaderStore::Find(in, stored)) {
        return Built::Already;
    }
    const auto t0 = std::chrono::steady_clock::now();
    ShaderStore::Translation t;
    why = Translate(rc, params, pools, profile, t);
    const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (why.empty() && !ShaderStore::Keep(in, profile, *t.info, t.runtime, t.end, t.spv, t.windows, ns)) {
        why = "the store does not keep this stage (a geometry stage)";
    }
    pools.ReleaseContents();
    return why.empty() ? Built::Yes : Built::No;
}

} // namespace

int BuildStore(const std::filesystem::path& store_folder, const std::filesystem::path& recipe_dir) {
    FixedLayout();
    const auto t0 = std::chrono::steady_clock::now();
    VkbShader::Profile profile{};
    std::string why;
    if (!ShaderStore::OpenForBuild(store_folder, profile, why)) {
        std::printf("vkb-build-store: refused: %s\n", why.c_str());
        return 2;
    }
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{recipe_dir, ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".vkr") {
            files.push_back(it->path());
        }
    }
    if (ec) {
        std::printf("vkb-build-store: refused: the recipe folder %s cannot be read: %s\n", recipe_dir.string().c_str(), ec.message().c_str());
        return 3;
    }
    std::sort(files.begin(), files.end());
    VkbShader::Pools pools;
    u32 built = 0, already = 0, skipped = 0;
    std::map<std::string, u32> reasons; // skipped recipes, by reason
    const auto skip = [&](const std::filesystem::path& file, const std::string& reason) {
        ++skipped;
        if (reasons[reason]++ < 3) { // every one is counted; the first three of a reason are named
            std::printf("vkb-build-store: %s skipped: %s\n", file.filename().string().c_str(), reason.c_str());
        }
    };
    for (const auto& file : files) {
        std::ifstream f{file, std::ios::binary};
        const std::vector<u8> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        const char* parse_why = "";
        auto rc = Parse(bytes, parse_why);
        if (!rc) {
            skip(file, fmt::format("the recipe {}", parse_why));
            continue;
        }
        std::string failed_reason;
        switch (BuildOne(*rc, pools, profile, failed_reason)) {
        case Built::Yes:
            ++built;
            break;
        case Built::Already:
            ++already;
            break;
        case Built::No:
            skip(file, failed_reason);
            break;
        }
    }
    ShaderStore::Flush();
    const auto report = ShaderStore::Report();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& [reason, n] : reasons) {
        std::printf("vkb-build-store: %u recipes skipped: %s\n", n, reason.c_str());
    }
    std::printf("vkb-build-store: %zu recipes, %u stages built, %u already in the store, %u skipped; %llu files written, %llu writes failed; %s, %lld ms\n",
                files.size(), built, already, skipped, static_cast<unsigned long long>(report.value("files_written", u64{0})),
                static_cast<unsigned long long>(report.value("files_write_failed", u64{0})), store_folder.string().c_str(), static_cast<long long>(ms));
    return report.value("files_write_failed", u64{0}) != 0 ? 4 : 0;
}

int ListRecipes(const std::filesystem::path& store_folder, const std::filesystem::path& recipe_dir, const std::filesystem::path& out_path) {
    FixedLayout();
    const auto t0 = std::chrono::steady_clock::now();
    VkbShader::Profile profile{};
    std::string why;
    if (!ShaderStore::OpenForBuild(store_folder, profile, why)) { // only its profile: nothing is written to it
        std::printf("vkb-list-recipes: refused: %s\n", why.c_str());
        return 2;
    }
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{recipe_dir, ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".vkr") {
            files.push_back(it->path());
        }
    }
    if (ec) {
        std::printf("vkb-list-recipes: the recipe folder %s cannot be read: %s\n", recipe_dir.string().c_str(), ec.message().c_str());
        return 3;
    }
    std::sort(files.begin(), files.end());
    BodyWriter out;
    out.out.assign({'T', 'L', 'G', 'V', 'K', 'B', 'P', 'T'});
    out.Pod(u32{1});
    out.Pod(ListPortableId());
    const size_t count_at = out.out.size();
    out.Pod(u32{0});
    VkbShader::Pools pools;
    u32 written = 0, with_fetch = 0, left_out = 0;
    std::map<std::string, u32> reasons;
    const auto leave_out = [&](const std::filesystem::path& file, const std::string& reason) {
        ++left_out;
        if (reasons[reason]++ < 3) {
            std::printf("vkb-list-recipes: %s left out: %s\n", file.filename().string().c_str(), reason.c_str());
        }
    };
    for (const auto& file : files) {
        std::ifstream f{file, std::ios::binary};
        const std::vector<u8> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        const char* parse_why = "";
        auto rc = Parse(bytes, parse_why);
        if (!rc) {
            leave_out(file, fmt::format("the recipe {}", parse_why));
            continue;
        }
        // 1: the recipe as recorded (the program and the fetch shader's code at their addresses)
        ShaderStore::Translation first;
        std::optional<VkbShader::Gcn::FetchShaderData> fetch;
        u64 fetch_at = 0;
        {
            GuestMapping mapping{rc->regions};
            if (!mapping.ok) {
                leave_out(file, "its guest memory cannot be mapped at its own address");
                continue;
            }
            const auto params = ParamsOf(*rc);
            const Replay replay{*rc};
            if (const auto failed = Translate(*rc, params, pools, profile, first); !failed.empty()) {
                pools.ReleaseContents();
                leave_out(file, fmt::format("the recipe does not translate: {}", failed));
                continue;
            }
            pools.ReleaseContents();
            if (first.info->has_fetch_shader) { // the layout the translation read from the fetch shader, by the translator's own parser
                VkbShader::Gcn::FetchShaderData data;
                if (VkbShader::Gcn::ParseFetchShader(*first.info, data)) {
                    fetch = std::move(data);
                    fetch_at = reinterpret_cast<u64>(VkbShader::Gcn::GetFetchShaderCode(*first.info, first.info->fetch_shader_sgpr_base));
                }
            }
        }
        // 2: the list's form: the state without the program and the fetch shader's code
        std::vector<std::pair<u64, std::vector<u8>>> state;
        std::vector<u8> program;
        std::string code_like;
        for (const auto& [address, region] : rc->regions) {
            if (address == rc->pgm_base) {
                program = region;
            } else if (fetch && address == fetch_at) {
                continue;
            } else if (LooksLikeCode(region)) {
                code_like = fmt::format("recorded memory that may be code ({} bytes)", region.size());
            } else {
                state.emplace_back(address, region);
            }
        }
        if (!code_like.empty()) {
            leave_out(file, code_like);
            continue;
        }
        auto body = EncodeBody(*rc, fetch, state);
        const char* body_why = "";
        auto listed = DecodeBody(rc->pgm_hash, body, program, body_why);
        if (!listed) {
            leave_out(file, fmt::format("its list entry does not read back: it {}", body_why));
            continue;
        }
        if (std::memcmp(&listed->runtime, &rc->runtime, sizeof(rc->runtime)) != 0 || !(listed->start == rc->start)) {
            leave_out(file, "its runtime info or bindings do not come back the same from the portable form");
            continue;
        }
        // 3: the check of the entry: the list's form translates to the same stage as the recording
        {
            GuestMapping mapping{listed->regions};
            if (!mapping.ok) {
                leave_out(file, "its list entry's memory cannot be mapped at its own address");
                continue;
            }
            const auto params = ParamsOf(*listed);
            const Replay replay{*listed};
            ShaderStore::Translation second;
            const auto failed = Translate(*listed, params, pools, profile, second);
            pools.ReleaseContents();
            if (!failed.empty()) {
                leave_out(file, fmt::format("its list entry does not translate: {}", failed));
                continue;
            }
            if (const char* differs = ShaderStore::Differs(first, *second.info, second.runtime, second.end, second.spv, second.windows); differs != nullptr) {
                leave_out(file, fmt::format("its list entry translates otherwise than the recording ({})", differs));
                continue;
            }
        }
        const auto name = file.stem().string();
        out.Blob(name.data(), name.size());
        out.Pod(rc->pgm_hash);
        out.Blob(program.data(), program.size());
        out.Blob(body.data(), body.size());
        ++written;
        with_fetch += fetch.has_value();
    }
    std::memcpy(out.out.data() + count_at, &written, sizeof(written));
    {
        std::ofstream f{out_path, std::ios::binary | std::ios::trunc};
        f.write(reinterpret_cast<const char*>(out.out.data()), static_cast<std::streamsize>(out.out.size()));
        if (!f) {
            std::printf("vkb-list-recipes: could not write %s\n", out_path.string().c_str());
            return 4;
        }
    }
    for (const auto& [reason, n] : reasons) {
        std::printf("vkb-list-recipes: %u recipes left out: %s\n", n, reason.c_str());
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("vkb-list-recipes: %zu recipes, %u list entries written (%u with a vertex input layout), each translated in its list form the same as "
                "recorded; %u left out; portable id %#018llx; %s, %lld ms\n",
                files.size(), written, with_fetch, left_out, static_cast<unsigned long long>(ListPortableId()), out_path.string().c_str(),
                static_cast<long long>(ms));
    return 0;
}

int BuildList(const std::filesystem::path& store_folder, const std::filesystem::path& list_path, const std::filesystem::path& game_dir, u32 part, u32 parts) {
#ifndef _WIN32
    prctl(PR_SET_PDEATHSIG, SIGKILL); // started by the game: it ends with the game (Windows: the game's job object, native_vkb_shader_prebuild.cpp)
#endif
    FixedLayout();
    const auto t0 = std::chrono::steady_clock::now();
    VkbShader::Profile profile{};
    std::string why;
    if (parts == 0 || part >= parts) {
        std::printf("vkb-build-list: part %u of %u: no such part\n", part, parts);
        return 2;
    }
    if (!ShaderStore::OpenForBuild(store_folder, profile, why)) {
        std::printf("vkb-build-list: refused: %s\n", why.c_str());
        return 2;
    }
    const auto store = ShaderStore::Folder();
    auto list = ShaderList::Read(list_path, ListPortableId(), why);
    if (!list) {
        std::printf("vkb-build-list: the list %s is not read: %s\n", list_path.string().c_str(), why.c_str());
        return 3;
    }
    const char* guesses_env = std::getenv("TLG_VKB_SHADER_LIST_GUESSES");
    const bool guesses = !(guesses_env != nullptr && std::strcmp(guesses_env, "0") == 0);
    std::map<u64, std::string> left; // entries a build process stopped on before (crash, hang): the parent wrote them
    {
        std::ifstream f{ShaderList::LeftOutFile(store)};
        std::string line;
        std::getline(f, line); // "list <id>": the game's parent removes a file of another list before it starts a build
        unsigned long long index = 0;
        std::string how;
        while (f >> index && std::getline(f, how)) {
            left.emplace(index, how);
        }
    }
    ShaderList::Programs programs{game_dir, *list};
    std::FILE* progress = nullptr;
#ifdef _WIN32
    _wfopen_s(&progress, ShaderList::ProgressFile(store, part).c_str(), L"wb");
#else
    progress = std::fopen(ShaderList::ProgressFile(store, part).c_str(), "wb");
#endif
    // test only (the proof that a build process that stops never stops the game): TLG_VKB_SHADER_PREBUILD_TEST_STOP=<entry>:crash|hang
    long long test_stop_at = -1;
    bool test_hang = false;
    if (const char* env = std::getenv("TLG_VKB_SHADER_PREBUILD_TEST_STOP"); env != nullptr && *env != '\0') {
        char* end = nullptr;
        test_stop_at = std::strtoll(env, &end, 10);
        test_hang = end != nullptr && std::strcmp(end, ":hang") == 0;
    }
    VkbShader::Pools pools;
    u32 mine = 0, built = 0, already = 0, skipped = 0;
    std::map<std::string, u32> reasons;
    const auto skip = [&](size_t index, const ShaderList::Item& item, const std::string& reason) {
        ++skipped;
        if (reasons[reason]++ < 3) {
            std::printf("vkb-build-list: entry %zu (program %#018llx) skipped: %s\n", index, static_cast<unsigned long long>(item.pgm_hash), reason.c_str());
        }
    };
    for (size_t index = 0; index < list->items.size(); ++index) {
        const auto& item = list->items[index];
        if (item.pgm_hash % parts != part) { // a program's entries stay in one part (its archive entry is read once)
            continue;
        }
        ++mine;
        if (const auto it = left.find(index); it != left.end()) {
            skip(index, item, fmt::format("a build process stopped on it before ({})", it->second.empty() ? "?" : it->second.substr(1)));
            continue;
        }
        if (!guesses && std::ranges::binary_search(list->guessed, item.pgm_hash)) {
            skip(index, item, "a guess, and guesses are off (TLG_VKB_SHADER_LIST_GUESSES=0)");
            continue;
        }
        if (progress != nullptr) { // the parent reads which entry a process that stopped was building (before its program is read: a crash there names this entry too)
            std::fseek(progress, 0, SEEK_SET);
            std::fprintf(progress, "%20zu\n", index);
            std::fflush(progress);
        }
        auto program = programs.Get(item);
        if (!program) {
            skip(index, item, "its program is not in the game files (another version of the game, or a packed game folder)");
            continue;
        }
        const char* body_why = "";
        auto rc = DecodeBody(item.pgm_hash, item.body, std::move(*program), body_why);
        if (!rc) {
            skip(index, item, fmt::format("the entry {}", body_why));
            continue;
        }
        if (static_cast<long long>(index) == test_stop_at) [[unlikely]] {
            std::printf("vkb-build-list: test stop at entry %zu (%s)\n", index, test_hang ? "hang" : "crash");
            if (test_hang) {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds{1});
                }
            }
            std::abort();
        }
        std::string failed;
        switch (BuildOne(*rc, pools, profile, failed)) {
        case Built::Yes:
            ++built;
            break;
        case Built::Already:
            ++already;
            break;
        case Built::No:
            skip(index, item, failed);
            break;
        }
    }
    if (progress != nullptr) {
        std::fclose(progress);
    }
    std::error_code ec;
    std::filesystem::remove(ShaderList::ProgressFile(store, part), ec);
    ShaderStore::Flush();
    const auto report = ShaderStore::Report();
    const u64 write_failed = report.value("files_write_failed", u64{0});
    {
        std::ofstream f{ShaderList::ResultFile(store, part), std::ios::trunc};
        f << "entries " << mine << "\nbuilt " << built << "\nalready " << already << "\nwrite_failed " << write_failed << "\n";
        for (const auto& [reason, n] : reasons) {
            f << "skipped " << n << " " << reason << "\n";
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& [reason, n] : reasons) {
        std::printf("vkb-build-list: part %u: %u entries skipped: %s\n", part, n, reason.c_str());
    }
    std::printf("vkb-build-list: part %u of %u: %u entries of %zu, %u stages built, %u already in the store, %u skipped; %llu files written, %llu writes failed; %lld ms\n",
                part, parts, mine, list->items.size(), built, already, skipped, static_cast<unsigned long long>(report.value("files_written", u64{0})),
                static_cast<unsigned long long>(write_failed), static_cast<long long>(ms));
    return write_failed != 0 ? 4 : 0;
}

nlohmann::json Report() {
    return {{"on", On()},
            {"kept", c_recorded.load()},
            {"dropped_translation_failed_or_stage_not_kept", c_not_taken.load()},
            {"dropped_no_folder", c_no_folder.load()},
            {"dropped_recording_broken", c_broken.load()},
            {"layout_id", fmt::format("{:016x}", ShaderFields::LayoutId())}};
}

} // namespace Core::Vkb::ShaderRecipe

extern "C" int tlg_vkb_build_store(const char* store_folder, const char* recipe_dir) {
    return Core::Vkb::ShaderRecipe::BuildStore(std::filesystem::u8path(store_folder), std::filesystem::u8path(recipe_dir));
}

extern "C" int tlg_vkb_list_recipes(const char* store_folder, const char* recipe_dir, const char* out) {
    return Core::Vkb::ShaderRecipe::ListRecipes(std::filesystem::u8path(store_folder), std::filesystem::u8path(recipe_dir), std::filesystem::u8path(out));
}

extern "C" int tlg_vkb_build_list(const char* store_folder, const char* list, const char* game_dir, unsigned part, unsigned parts) {
    return Core::Vkb::ShaderRecipe::BuildList(std::filesystem::u8path(store_folder), std::filesystem::u8path(list), std::filesystem::u8path(game_dir), part, parts);
}
