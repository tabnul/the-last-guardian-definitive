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
#include <vector>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <xxhash.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "native/native_vkb_shader_fields.h"
#include "native/native_vkb_write_window.h"
#include "native/shadps4/shader_recompiler/backend/spirv/emit_spirv.h"
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

std::atomic<u64> c_recorded{0}, c_not_kept{0}, c_no_folder{0}, c_broken{0};

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
    }
    ~Replay() {
        VkbShader::replay_flat = nullptr;
        VkbShader::guest_valid_hook = nullptr;
        replay_regions = nullptr;
    }
    Replay(const Replay&) = delete;
    Replay& operator=(const Replay&) = delete;
};

} // namespace

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
        c_not_kept.fetch_add(1, std::memory_order_relaxed);
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
    if (recording_broken != nullptr) {
        if (c_broken.fetch_add(1) < 3) { // all are counted (report: dropped_recording_broken)
            LOG_WARNING(Render_Vulkan, "vkb shader recipes: no recipe for program {:#x}: {}", in.params.hash, recording_broken);
        }
        return;
    }
    const auto folder = RecipeFolder();
    if (folder.empty()) { // said once when the folder could not be made; every recipe lost so is counted (report: dropped_no_folder)
        c_no_folder.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // the program itself, at its guest address (the translation reads it there: its base, inline constants)
    auto& program = read[in.params.Base()];
    program.assign(reinterpret_cast<const u8*>(in.params.code.data()), reinterpret_cast<const u8*>(in.params.code.data()) + in.params.code.size_bytes());

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
    c_recorded.fetch_add(1, std::memory_order_relaxed);
    ShaderStore::QueueWrite(folder / fmt::format("{:016x}_{}_{}_{:016x}.vkr", in.params.hash, static_cast<u32>(in.runtime.hw_stage), static_cast<u32>(in.runtime.sw_stage), digest),
                            std::move(w.out));
}

int BuildStore(const std::filesystem::path& store_folder, const std::filesystem::path& recipe_dir) {
    const auto t0 = std::chrono::steady_clock::now();
    VkbShader::Profile profile{};
    std::string why;
    if (!ShaderStore::OpenForBuild(store_folder, profile, why)) {
        std::printf("vkb-build-store: refused: %s\n", why.c_str());
        return 2;
    }
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator{recipe_dir, ec}) {
        if (item.path().extension() == ".vkr") {
            files.push_back(item.path());
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
        GuestMapping mapping{rc->regions};
        if (!mapping.ok) {
            skip(file, fmt::format("its guest memory cannot be mapped at its own address (taken in this process near {:#x})", mapping.failed_at));
            continue;
        }
        const VkbShader::ShaderParams params{
            .user_data = std::span<const u32, VkbShader::ShaderParams::NumShaderUserData>{rc->user_data.data(), VkbShader::ShaderParams::NumShaderUserData},
            .code = std::span<const u32>{reinterpret_cast<const u32*>(rc->pgm_base), static_cast<size_t>(rc->code_words)},
            .hash = rc->pgm_hash,
        };
        const Replay replay{*rc}; // also for Find: its resource-kind check refreshes the flattened user data, from the recipe here
        const ShaderStore::Inputs in{.params = params, .runtime = rc->runtime, .start = rc->start, .salt = rc->salt};
        ShaderStore::Translation stored;
        if (ShaderStore::Find(in, stored)) { // the store's own rule: same inputs, and the resources of the same kinds
            ++already;
            continue;
        }
        const auto translate_t0 = std::chrono::steady_clock::now();
        VkbShader::Info info{rc->hw_stage, rc->sw_stage, params};
        info.pgm_base = params.Base();
        VkbShader::RuntimeInfo runtime = rc->runtime;
        VkbShader::Backend::Bindings binding = rc->start;
        std::vector<u32> spv;
        std::vector<WriteWindow::Window> windows;
        std::string failed;
        try {
            const auto program = VkbShader::TranslateProgram(params.code, pools, info, runtime, profile);
            if (info.translation_failed) {
                failed = "the translation failed";
            } else {
                spv = VkbShader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
                if (rc->kind == 1) {
                    windows = WriteWindow::Analyze(program, info, rc->local_x);
                }
            }
        } catch (const std::exception& e) {
            failed = fmt::format("the translation stopped: {}", e.what());
        }
        if (failed.empty() && spv.empty()) {
            failed = "the translation gave no module";
        }
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - translate_t0).count());
        if (failed.empty() && !ShaderStore::Keep(in, profile, info, runtime, binding, spv, windows, ns)) {
            failed = "the store does not keep this stage (a geometry stage)";
        }
        pools.ReleaseContents();
        if (!failed.empty()) {
            skip(file, failed);
            continue;
        }
        ++built;
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
    return 0;
}

nlohmann::json Report() {
    return {{"on", On()},
            {"kept", c_recorded.load()},
            {"dropped_not_kept_by_store", c_not_kept.load()},
            {"dropped_no_folder", c_no_folder.load()},
            {"dropped_recording_broken", c_broken.load()},
            {"layout_id", fmt::format("{:016x}", ShaderFields::LayoutId())}};
}

} // namespace Core::Vkb::ShaderRecipe

extern "C" int tlg_vkb_build_store(const char* store_folder, const char* recipe_dir) {
    return Core::Vkb::ShaderRecipe::BuildStore(std::filesystem::u8path(store_folder), std::filesystem::u8path(recipe_dir));
}
