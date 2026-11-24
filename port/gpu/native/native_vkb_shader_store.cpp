// SPDX-License-Identifier: GPL-2.0-or-later
// TLG_VKB_SHADER_PREBUILD: the new engine's translated shader stages, kept on disk (see the header)

#include "native/native_vkb_shader_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <xxhash.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "native/shadps4/shader_recompiler/specialization.h"

namespace Core::Vkb::ShaderStore {
namespace {

// ---- the encoding of an entry (raw bytes: the folder id names the build, so only this build reads them) ----

constexpr u32 Magic = 0x5653474c; // "LGSV"
constexpr u32 Version = 1;

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
    template <typename List>
    void Items(const List& list) { // a static_vector or small_vector of plain structs
        static_assert(std::is_trivially_copyable_v<typename List::value_type>);
        Pod(static_cast<u32>(list.size()));
        Raw(list.data(), list.size() * sizeof(typename List::value_type));
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
    void Pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        Raw(&v, sizeof(T));
    }
    template <typename List>
    void Items(List& list) {
        static_assert(std::is_trivially_copyable_v<typename List::value_type>);
        u32 n = 0;
        Pod(n);
        bool fits = true;
        if constexpr (requires { list.max_size(); }) {
            fits = n <= list.max_size();
        }
        if (!ok || !fits || static_cast<size_t>(n) * sizeof(typename List::value_type) > left) {
            ok = false;
            return;
        }
        list.resize(n);
        Raw(list.data(), n * sizeof(typename List::value_type));
    }
    template <typename T>
    void Vector(std::vector<T>& v) {
        u32 n = 0;
        Pod(n);
        if (!ok || static_cast<size_t>(n) * sizeof(T) > left) {
            ok = false;
            return;
        }
        v.resize(n);
        Raw(v.data(), n * sizeof(T));
    }
};

// The members of the translator's Info, in declaration order. The mirror structs below repeat them: a member the engine's author adds,
// removes or moves makes the sizes differ and stops the build here, so the encoding is never silently short of a member.
#define TLG_STORE_PERSISTENT(X)                                                                                                            \
    X(buffers) X(images) X(samplers) X(fmasks) X(fetch_shader_sgpr_base) X(shared_memory_scratch_size) X(pgm_hash) X(tess_consts_dword_offset) \
    X(tess_consts_ptr_base) X(hw_stage) X(sw_stage) X(mrt_mask) X(has_fetch_shader) X(has_bitwise_xor) X(uses_dma) X(indexed) X(constant_offsets)
#define TLG_STORE_INFO(X)                                                                                                                  \
    X(user_data) X(flattened_ud_buf) X(srt_info) X(loads) X(stores) X(readconst_types) X(gs_copy_data) X(uses_patches) X(pgm_base)              \
    X(has_storage_images) X(has_discard) X(has_image_gather) X(has_image_query) X(has_flatbuf) X(uses_buffer_atomic_float_min_max)            \
    X(uses_image_atomic_float_min_max) X(uses_lane_id) X(uses_shader_clock) X(uses_group_quad) X(uses_group_shuffle) X(uses_group_ballot)     \
    X(shared_types) X(uses_fp16) X(uses_fp64) X(uses_pack_10_11_11) X(uses_unpack_10_11_11) X(uses_buffer_int64_atomics)                     \
    X(uses_shared_int64_atomics) X(stores_tess_level_outer) X(stores_tess_level_inner) X(translation_failed) X(fs_interpolation)
#define TLG_STORE_SPEC(X) X(info) X(runtime_info) X(bitset) X(fetch_shader_data) X(vs_attribs) X(buffers) X(images) X(fmasks) X(samplers) X(start)

#define TLG_MIRROR_P(m) decltype(VkbShader::InfoPersistent::m) m;
#define TLG_MIRROR_I(m) decltype(VkbShader::Info::m) m;
#define TLG_MIRROR_S(m) decltype(VkbShader::StageSpecialization::m) m;
struct PersistentMirror {
    TLG_STORE_PERSISTENT(TLG_MIRROR_P)
};
struct InfoMirror : PersistentMirror {
    TLG_STORE_INFO(TLG_MIRROR_I)
};
struct SpecMirror {
    TLG_STORE_SPEC(TLG_MIRROR_S)
};
static_assert(sizeof(PersistentMirror) == sizeof(VkbShader::InfoPersistent), "VkbShader::InfoPersistent changed: update TLG_STORE_PERSISTENT");
static_assert(sizeof(InfoMirror) == sizeof(VkbShader::Info), "VkbShader::Info changed: update TLG_STORE_INFO");
static_assert(sizeof(SpecMirror) == sizeof(VkbShader::StageSpecialization), "VkbShader::StageSpecialization changed: update TLG_STORE_SPEC");
static_assert(std::is_trivially_copyable_v<VkbShader::RuntimeInfo> && std::is_trivially_copyable_v<VkbShader::Backend::Bindings>);
static_assert(std::is_trivially_copyable_v<WriteWindow::Window>);

// One member each. Members that belong to a draw and not to the translation are not kept: the user data (set at the use), the flattened
// user data (made again from the user data), the program base (set at the use).
template <typename T>
void Put(Writer& w, const T& v) {
    w.Pod(v);
}
template <typename T, std::size_t N>
void Put(Writer& w, const boost::container::static_vector<T, N>& v) {
    w.Items(v);
}
template <typename T, std::size_t N, typename S>
void Put(Writer& w, const SmallVector<T, N, S>& v) {
    w.Items(v);
}
void Put(Writer&, const std::span<const u32>&) {}
void Put(Writer&, const std::vector<u32>&) {}
void Put(Writer& w, const VkbShader::PersistentSrtInfo& srt) {
    VkbShader::PersistentSrtInfo copy = srt;
    copy.walker_func = nullptr;
    w.Pod(copy);
    std::vector<u8> code;
    if (srt.walker_func != nullptr && !VkbShader::PristineWalkerCode(srt.walker_func, code)) {
        // not the walker made by this thread's last translation: the bytes in the code buffer (a fault may have patched them)
        code.assign(reinterpret_cast<const u8*>(srt.walker_func), reinterpret_cast<const u8*>(srt.walker_func) + srt.walker_func_size);
    }
    w.Pod(static_cast<u32>(code.size()));
    w.Raw(code.data(), code.size());
}
void Put(Writer&, const VkbShader::CopyShaderData&) {} // geometry stages only (Keep refuses a stage that has one)
void Put(Writer& w, const VkbShader::Gcn::FetchShaderData& f) {
    w.Pod(f.size);
    w.Items(f.attributes);
    w.Pod(f.vertex_offset_sgpr);
    w.Pod(f.instance_offset_sgpr);
}
void Put(Writer&, const VkbShader::Info* const&) {}

template <typename T>
void Get(Reader& r, T& v) {
    r.Pod(v);
}
template <typename T, std::size_t N>
void Get(Reader& r, boost::container::static_vector<T, N>& v) {
    r.Items(v);
}
template <typename T, std::size_t N, typename S>
void Get(Reader& r, SmallVector<T, N, S>& v) {
    r.Items(v);
}
void Get(Reader&, std::span<const u32>&) {}
void Get(Reader&, std::vector<u32>&) {}
void Get(Reader& r, VkbShader::PersistentSrtInfo& srt) {
    r.Pod(srt);
    u32 size = 0;
    r.Pod(size);
    srt.walker_func = nullptr;
    if (!r.ok || size > r.left || size != srt.walker_func_size) {
        r.ok = false;
        return;
    }
    if (size != 0) {
        srt.walker_func = VkbShader::RegisterWalkerCode(r.p, size);
        r.p += size;
        r.left -= size;
    }
}
void Get(Reader&, VkbShader::CopyShaderData&) {}
void Get(Reader& r, VkbShader::Gcn::FetchShaderData& f) {
    r.Pod(f.size);
    r.Items(f.attributes);
    r.Pod(f.vertex_offset_sgpr);
    r.Pod(f.instance_offset_sgpr);
}
void Get(Reader&, const VkbShader::Info*&) {}

void PutInfo(Writer& w, const VkbShader::Info& info) {
#define TLG_PUT(m) Put(w, info.m);
    TLG_STORE_PERSISTENT(TLG_PUT)
    TLG_STORE_INFO(TLG_PUT)
#undef TLG_PUT
}
void GetInfo(Reader& r, VkbShader::Info& info) {
#define TLG_GET(m) Get(r, info.m);
    TLG_STORE_PERSISTENT(TLG_GET)
    TLG_STORE_INFO(TLG_GET)
#undef TLG_GET
}
// The check mode's comparison, member by member (the encoding also holds padding bytes, which differ between two equal translations)
template <typename T>
bool Eq(const T& a, const T& b) {
    if constexpr (requires { a == b; }) {
        return a == b;
    } else {
        static_assert(std::has_unique_object_representations_v<T>, "a member without == and with padding: give it a comparison here");
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }
}
template <typename List>
bool EqItems(const List& a, const List& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!Eq(a[i], b[i])) {
            return false;
        }
    }
    return true;
}
template <typename T, std::size_t N>
bool Eq(const boost::container::static_vector<T, N>& a, const boost::container::static_vector<T, N>& b) {
    return EqItems(a, b);
}
bool Eq(const std::span<const u32>&, const std::span<const u32>&) {
    return true; // the draw's, set at the use
}
bool Eq(const VkbShader::PersistentSrtInfo& a, const VkbShader::PersistentSrtInfo& b) {
    Writer wa, wb; // the walker by its bytes, as kept
    Put(wa, a);
    Put(wb, b);
    const size_t head = sizeof(VkbShader::PersistentSrtInfo);
    return a.walker_func_size == b.walker_func_size && a.flattened_bufsize_dw == b.flattened_bufsize_dw &&
           std::memcmp(a.table_root_plus1, b.table_root_plus1, sizeof(a.table_root_plus1)) == 0 && std::memcmp(a.table_src, b.table_src, sizeof(a.table_src)) == 0 &&
           std::equal(wa.out.begin() + head, wa.out.end(), wb.out.begin() + head, wb.out.end());
}
template <std::size_t N>
bool Eq(const std::array<VkbShader::Info::Interpolation, N>& a, const std::array<VkbShader::Info::Interpolation, N>& b) {
    static_assert(std::has_unique_object_representations_v<VkbShader::Info::Interpolation>);
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
bool Eq(const VkbShader::CopyShaderData&, const VkbShader::CopyShaderData&) {
    return true; // geometry stages only, never kept
}
/// The first member in which two translations' infos differ (nullptr: none)
const char* FirstDifference(const VkbShader::Info& a, const VkbShader::Info& b) {
    const char* first = nullptr;
#define TLG_EQ(m)                                                                                                                          \
    if (first == nullptr && !Eq(a.m, b.m)) {                                                                                               \
        first = #m;                                                                                                                        \
    }
    TLG_STORE_PERSISTENT(TLG_EQ)
    TLG_STORE_INFO(TLG_EQ)
#undef TLG_EQ
    return first;
}

void PutSpec(Writer& w, const VkbShader::StageSpecialization& spec) {
#define TLG_PUT(m) Put(w, spec.m);
    TLG_STORE_SPEC(TLG_PUT)
#undef TLG_PUT
}
void GetSpec(Reader& r, VkbShader::StageSpecialization& spec) {
#define TLG_GET(m) Get(r, spec.m);
    TLG_STORE_SPEC(TLG_GET)
#undef TLG_GET
}

// ---- the entries ----

struct CodeId {
    u64 words{}, lo{}, hi{};
    bool operator==(const CodeId&) const = default;
};
CodeId CodeOf(std::span<const u32> code) {
    const XXH128_hash_t h = XXH3_128bits(code.data(), code.size_bytes());
    return {code.size(), h.low64, h.high64};
}

struct Entry {
    CodeId code;
    VkbShader::RuntimeInfo runtime_in{}; ///< compared byte by byte (the translator's settings start zeroed, so their padding is zero too)
    VkbShader::Backend::Bindings start{};
    u64 salt{};
    VkbShader::StageSpecialization spec; ///< the kinds of the resources the stage read (the normal renderer's rule)
    Translation result;                  ///< result.info: no user data between lookups
    u64 translate_ns{};
    bool used = false;                   ///< build stats: its first use was logged
    bool from_disk = false;
};

u64 KeyOf(u64 pgm_hash, const VkbShader::RuntimeInfo& runtime) {
    return pgm_hash ^ (static_cast<u64>(runtime.hw_stage) << 56) ^ (static_cast<u64>(runtime.sw_stage) << 60);
}

bool SameInputs(const Entry& e, const Inputs& in, const CodeId& code) {
    return e.salt == in.salt && e.start == in.start && e.code == code && std::memcmp(&e.runtime_in, &in.runtime, sizeof(VkbShader::RuntimeInfo)) == 0;
}

std::vector<u8> Encode(const Entry& e) {
    Writer w;
    w.Pod(Magic);
    w.Pod(Version);
    w.Pod(e.code);
    w.Pod(e.runtime_in);
    w.Pod(e.start);
    w.Pod(e.salt);
    w.Pod(e.translate_ns);
    PutSpec(w, e.spec);
    PutInfo(w, *e.result.info);
    w.Pod(e.result.runtime);
    w.Pod(e.result.end);
    w.Pod(static_cast<u32>(e.result.spv.size()));
    w.Raw(e.result.spv.data(), e.result.spv.size() * sizeof(u32));
    w.Pod(static_cast<u32>(e.result.windows.size()));
    w.Raw(e.result.windows.data(), e.result.windows.size() * sizeof(WriteWindow::Window));
    w.Pod(static_cast<u64>(XXH3_64bits(w.out.data(), w.out.size()))); // a damaged or cut file is never read as an entry
    return std::move(w.out);
}

std::unique_ptr<Entry> Decode(const std::vector<u8>& bytes) {
    if (bytes.size() < 16 || XXH3_64bits(bytes.data(), bytes.size() - 8) != [&] { u64 v; std::memcpy(&v, bytes.data() + bytes.size() - 8, 8); return v; }()) {
        return nullptr;
    }
    Reader r{bytes.data(), bytes.size() - 8};
    u32 magic = 0, version = 0;
    r.Pod(magic);
    r.Pod(version);
    if (!r.ok || magic != Magic || version != Version) {
        return nullptr;
    }
    auto e = std::make_unique<Entry>();
    r.Pod(e->code);
    r.Pod(e->runtime_in);
    r.Pod(e->start);
    r.Pod(e->salt);
    r.Pod(e->translate_ns);
    GetSpec(r, e->spec);
    e->result.info = std::make_unique<VkbShader::Info>();
    GetInfo(r, *e->result.info);
    e->spec.info = e->result.info.get();
    r.Pod(e->result.runtime);
    r.Pod(e->result.end);
    r.Vector(e->result.spv);
    r.Vector(e->result.windows);
    if (!r.ok || r.left != 0 || e->result.spv.empty()) {
        return nullptr;
    }
    e->from_disk = true;
    return e;
}

// ---- the store ----

// The store's state lives as long as the process: never destroyed, so the writer thread may still run at exit.
struct State {
    std::mutex mutex;
    std::unordered_map<u64, std::vector<std::unique_ptr<Entry>>> entries; ///< by program hash and stage
    std::unordered_map<u64, std::vector<std::filesystem::path>> on_disk;  ///< files not read yet, by the same key
    std::filesystem::path folder;
    std::unordered_set<u64> different_logged;
    std::string open_error;
    // the writer: files are written on a thread of their own, as "<name>.tmp" first and then renamed, so a reader never sees half a file
    std::mutex write_mutex;
    std::condition_variable write_cv;
    std::deque<std::pair<std::string, std::vector<u8>>> write_queue;
};
State& S() {
    static State* const state = new State;
    return *state;
}
std::atomic<bool> ready{false};
std::atomic<u64> c_lookups{0}, c_hits{0}, c_disk_hits{0}, c_kept{0}, c_saved_ns{0}, c_check_same{0}, c_check_different{0}, c_spv_words{0}, c_files_indexed{0},
    c_files_read{0}, c_files_bad{0}, c_written{0}, c_write_failed{0}, c_not_kept_geometry{0}, c_open_ms{0}, c_bypassed{0}, c_kind_misses{0};

std::string FileName(u64 pgm_hash, const VkbShader::RuntimeInfo& runtime, const std::vector<u8>& bytes) {
    return fmt::format("{:016x}_{}_{}_{:016x}.vst", pgm_hash, static_cast<u32>(runtime.hw_stage), static_cast<u32>(runtime.sw_stage), XXH3_64bits(bytes.data(), bytes.size()));
}

void WriterLoop() {
    State& st = S();
    for (;;) {
        std::pair<std::string, std::vector<u8>> item;
        {
            std::unique_lock lock{st.write_mutex};
            st.write_cv.wait(lock, [&] { return !st.write_queue.empty(); });
            item = std::move(st.write_queue.front());
            st.write_queue.pop_front();
        }
        const auto path = st.folder / item.first, tmp = st.folder / (item.first + ".tmp");
        std::error_code ec;
        bool ok = false;
        if (std::ofstream f{tmp, std::ios::binary | std::ios::trunc}) {
            f.write(reinterpret_cast<const char*>(item.second.data()), static_cast<std::streamsize>(item.second.size()));
            ok = static_cast<bool>(f);
        }
        if (ok) {
            std::filesystem::rename(tmp, path, ec);
            ok = !ec;
        }
        if (!ok) {
            std::filesystem::remove(tmp, ec);
            if (c_write_failed.fetch_add(1) == 0) {
                LOG_ERROR(Render_Vulkan, "vkb shader store: could not write {} (the stage stays translated in memory for this session; failed writes are counted)",
                          path.string());
            }
        } else {
            c_written.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

/// The file of the module that holds this code (libtlggpu.so / tlggpu.dll): its bytes name the build.
std::filesystem::path ModulePath() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ModulePath), &module)) {
        return {};
    }
    std::wstring name(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
    if (n == 0 || n >= name.size()) {
        return {};
    }
    name.resize(n);
    return std::filesystem::path{name};
#else
    Dl_info dl{};
    if (dladdr(reinterpret_cast<void*>(&ModulePath), &dl) == 0 || dl.dli_fname == nullptr) {
        return {};
    }
    return std::filesystem::path{dl.dli_fname};
#endif
}

/// The build's id: the module file's size and time of its last change (any rebuild changes the time; a copy that keeps the time keeps the
/// id, which is right: the same bytes). Reading the whole file is too slow at start (it holds the debug information too, hundreds of MB).
bool BuildId(const std::filesystem::path& path, u64& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    const u64 parts[2] = {static_cast<u64>(size), static_cast<u64>(time.time_since_epoch().count())};
    out = XXH3_64bits(parts, sizeof(parts));
    return true;
}

/// A hash of the bytes of `v` that hold its members, without its padding bytes (they can differ between two equal values). A byte is a
/// member's when flipping its lowest bit makes the value compare different (a bool stays a valid bool); `T`'s == compares every member.
template <typename T>
u64 HashMembers(const T& v, u64 seed) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<u8, sizeof(T)> bytes;
    std::memcpy(bytes.data(), &v, sizeof(T));
    for (size_t i = 0; i < sizeof(T); ++i) {
        T flipped;
        std::memcpy(&flipped, &v, sizeof(T));
        reinterpret_cast<u8*>(&flipped)[i] ^= 1;
        if (flipped == v) {
            bytes[i] = 0; // padding
        }
    }
    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
}

/// Every setting the translator reads besides its profile; each value is part of the folder id, so a start with another value never takes
/// a stage translated with the old one. port/tests/test_vkb_store_settings.py reads these three lists and fails when the translator reads a
/// setting none of them names.
constexpr const char* TranslatorEnv[] = {"TLG_CONST_OFFSET_FLOATRULE", "TLG_BLOCK1D"};          ///< environment, by value
constexpr const char* TranslatorEmulatorSettings[] = {"IsDirectMemoryAccessEnabled"};            ///< EmulatorSettings, in TranslatorSettings()
constexpr const char* TranslatorWithoutEffect[] = {"IsDumpShaders"};                              ///< read, but only to write debug files
std::string TranslatorSettings() {
    static_assert(std::size(TranslatorEmulatorSettings) == 1 && std::size(TranslatorWithoutEffect) == 1, "name each EmulatorSettings value below");
    std::string out = fmt::format("IsDirectMemoryAccessEnabled={}", EmulatorSettings.IsDirectMemoryAccessEnabled() ? 1 : 0);
    for (const char* name : TranslatorEnv) {
        const char* value = std::getenv(name);
        out += fmt::format(";{}={}", name, value != nullptr ? value : "<unset>");
    }
    return out;
}

void OpenFolder(const VkbShader::Profile& profile) {
    State& st = S();
    const auto t0 = std::chrono::steady_clock::now();
    const auto module = ModulePath();
    u64 build = 0;
    if (module.empty() || !BuildId(module, build)) {
        std::scoped_lock lock{st.mutex};
        st.open_error = fmt::format("the engine's module file {} could not be read; the store stays in memory for this session", module.string());
        LOG_ERROR(Render_Vulkan, "vkb shader store: {}", st.open_error);
        return; // never ready: nothing is read or written
    }
    const std::string settings = TranslatorSettings();
    const u64 id = XXH3_64bits_withSeed(settings.data(), settings.size(), HashMembers(profile, build));
    const auto parent = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / (MemoryPatcher::g_game_serial + "-vkb-shaders");
    const auto dir = parent / fmt::format("{:016x}", id);
    std::error_code ec;
    u32 removed = 0;
    for (const auto& other : std::filesystem::directory_iterator{parent, ec}) { // another build, card or setting: never read again
        if (other.path().filename() != dir.filename()) {
            std::filesystem::remove_all(other.path(), ec);
            ++removed;
        }
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::scoped_lock lock{st.mutex};
        st.open_error = fmt::format("could not create {}: {}; the store stays in memory for this session", dir.string(), ec.message());
        LOG_ERROR(Render_Vulkan, "vkb shader store: {}", st.open_error);
        return;
    }
    std::unordered_map<u64, std::vector<std::filesystem::path>> found;
    u64 files = 0, stale = 0;
    for (const auto& item : std::filesystem::directory_iterator{dir, ec}) {
        const auto name = item.path().filename().string();
        if (name.ends_with(".tmp")) { // a write cut by the end of a session
            std::filesystem::remove(item.path(), ec);
            ++stale;
            continue;
        }
        unsigned long long pgm = 0;
        u32 hw = 0, sw = 0;
        if (std::sscanf(name.c_str(), "%16llx_%u_%u_", &pgm, &hw, &sw) != 3 || !name.ends_with(".vst")) {
            continue;
        }
        VkbShader::RuntimeInfo key_runtime{};
        key_runtime.hw_stage = static_cast<VkbShader::HwStage>(hw);
        key_runtime.sw_stage = static_cast<VkbShader::SwStage>(sw);
        found[KeyOf(pgm, key_runtime)].push_back(item.path());
        ++files;
    }
    {
        std::scoped_lock lock{st.mutex};
        st.folder = dir;
        st.on_disk = std::move(found);
    }
    c_files_indexed.store(files);
    c_open_ms.store(static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    ready.store(true, std::memory_order_release);
    std::printf("Shader store (new engine): %s, %llu stored stages, %llu cut writes removed, %u folders of other builds or settings removed, %llu ms; "
                "translator settings %s\n",
                dir.string().c_str(), static_cast<unsigned long long>(files), static_cast<unsigned long long>(stale), removed,
                static_cast<unsigned long long>(c_open_ms.load()), settings.c_str());
}

/// Reads the files of `key` not read yet. The files are taken from the index under the lock and read without it (another thread that
/// looks up the same stage meanwhile does not find them and translates).
void LoadKey(u64 key) {
    State& st = S();
    std::vector<std::filesystem::path> files;
    {
        std::scoped_lock lock{st.mutex};
        const auto it = st.on_disk.find(key);
        if (it == st.on_disk.end()) {
            return;
        }
        files = std::move(it->second);
        st.on_disk.erase(it);
    }
    std::vector<std::unique_ptr<Entry>> loaded;
    for (const auto& path : files) {
        std::ifstream f{path, std::ios::binary};
        std::vector<u8> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        auto e = f.bad() ? nullptr : Decode(bytes);
        if (!e) {
            if (c_files_bad.fetch_add(1) < 5) { // all are counted (report: files_damaged_removed)
                LOG_ERROR(Render_Vulkan, "vkb shader store: {} is damaged or of another store version; removed (the stage is translated again)", path.string());
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
            continue;
        }
        c_files_read.fetch_add(1, std::memory_order_relaxed);
        loaded.push_back(std::move(e));
    }
    std::scoped_lock lock{st.mutex};
    auto& list = st.entries[key];
    for (auto& e : loaded) {
        list.push_back(std::move(e));
    }
}

// ---- build statistics ----

bool StatsOn() {
    static const bool on = [] { const char* e = std::getenv("TLG_SHADER_BUILD_STATS"); return e != nullptr && e[0] == '1'; }();
    return on;
}
struct Stats {
    std::mutex mutex;
    std::unordered_map<u64, u32> per_program;
    u32 builds = 0, frame_builds = 0, before_frame = 0, slow = 0, in_slow = 0;
    std::vector<std::pair<u32, u32>> frames; ///< microseconds, builds
    size_t judged = 0;
    std::chrono::steady_clock::time_point last_frame{}, last_report{};
};
Stats& GetStats() {
    static Stats s;
    return s;
}
void ReportLocked(Stats& s) { // the rule and the text of the normal renderer's ReportLocked (shader_recipe.cpp)
    constexpr size_t Window = 30;
    std::vector<u32> around;
    for (size_t& i = s.judged; i < s.frames.size() && i + Window < s.frames.size(); ++i) {
        around.clear();
        for (size_t j = i > Window ? i - Window : 0; j < std::min(s.frames.size(), i + Window + 1); ++j) {
            around.push_back(s.frames[j].first);
        }
        std::ranges::nth_element(around, around.begin() + around.size() / 2);
        const u32 median = around[around.size() / 2];
        if (s.frames[i].first > 2 * median && s.frames[i].first > median + 16'667) {
            ++s.slow;
            s.in_slow += s.frames[i].second;
        }
    }
    u32 pending = 0;
    for (size_t i = s.judged; i < s.frames.size(); ++i) {
        pending += s.frames[i].second;
    }
    std::printf("Shader builds while playing: %u (%u inside slow frames; %u before the first frame, %u while the background build ran, %u of programs in the shader "
                "list); %zu frames, %u slow (over twice the median of the 61 frames around them), %u builds in the last frames not judged yet; %u waits for the build "
                "gave up; new engine\n",
                s.builds, s.in_slow, s.before_frame, 0u, 0u, s.frames.size(), s.slow, pending, 0u);
}

void NoteUse(u64 pgm_hash, u32 hw_stage, Entry& e) {
    if (!StatsOn() || e.used) {
        return;
    }
    e.used = true;
    std::printf("Shader used from the cache: %#018llx_0 (hw stage %u), found by the lookup\n", static_cast<unsigned long long>(pgm_hash), hw_stage);
}

} // namespace

Mode GetMode() {
    static const Mode mode = [] {
        const char* e = std::getenv("TLG_VKB_SHADER_PREBUILD");
        if (e == nullptr || std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0) {
            return Mode::Off;
        }
        if (std::strcmp(e, "1") == 0 || std::strcmp(e, "on") == 0) {
            return Mode::On;
        }
        if (std::strcmp(e, "check") == 0) {
            return Mode::Check;
        }
        LOG_WARNING(Render_Vulkan, "TLG_VKB_SHADER_PREBUILD={}: not 0, off, 1, on or check; the store is off (every stage is translated)", e);
        return Mode::Off;
    }();
    return mode;
}

void Open(const VkbShader::Profile& profile) {
    static std::once_flag once;
    if (GetMode() == Mode::Off) {
        return;
    }
    std::call_once(once, [&] {
        std::thread{WriterLoop}.detach();
        OpenFolder(profile); // at the engine's start, before its first draw: a stat, a folder listing (a few ms)
    });
}

bool Find(const Inputs& in, Translation& out) {
    c_lookups.fetch_add(1, std::memory_order_relaxed);
    const u64 key = KeyOf(in.params.hash, in.runtime);
    const CodeId code = CodeOf(in.params.code);
    if (ready.load(std::memory_order_acquire)) {
        LoadKey(key);
    }
    State& st = S();
    std::scoped_lock lock{st.mutex};
    const auto it = st.entries.find(key);
    if (it == st.entries.end()) {
        return false;
    }
    bool same_inputs = false;
    for (const auto& e : it->second) {
        if (!SameInputs(*e, in, code)) {
            continue;
        }
        same_inputs = true;
        // the resource kinds are read through this draw's user data (and the tables it points to), as the normal renderer does
        VkbShader::Info& info = *e->result.info;
        info.user_data = in.params.user_data;
        info.pgm_base = in.params.Base();
        info.RefreshFlatBuf();
        const bool match = e->spec.Matches(info, in.runtime, in.start);
        if (match) {
            out.info = std::make_unique<VkbShader::Info>(info);
            out.runtime = e->result.runtime;
            out.end = e->result.end;
            out.spv = e->result.spv;
            out.windows = e->result.windows;
        }
        info.user_data = {};
        if (match) {
            c_hits.fetch_add(1, std::memory_order_relaxed);
            c_disk_hits.fetch_add(e->from_disk ? 1 : 0, std::memory_order_relaxed);
            if (GetMode() != Mode::Check) { // check mode translates anyway
                c_saved_ns.fetch_add(e->translate_ns, std::memory_order_relaxed);
            }
            NoteUse(in.params.hash, static_cast<u32>(in.runtime.hw_stage), *e);
            return true;
        }
    }
    if (same_inputs) { // stored with the same settings, but the draw's resources are of other kinds: a new variant, translated
        c_kind_misses.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

void Keep(const Inputs& in, const VkbShader::Profile& profile, const VkbShader::Info& info, const VkbShader::RuntimeInfo& runtime, const VkbShader::Backend::Bindings& end,
          const std::vector<u32>& spv, const std::vector<WriteWindow::Window>& windows, u64 translate_ns) {
    if (!info.gs_copy_data.attr_map.empty() || in.runtime.hw_stage == VkbShader::HwStage::Geometry) { // the new engine has no geometry stages; never kept, counted
        c_not_kept_geometry.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto e = std::make_unique<Entry>();
    e->code = CodeOf(in.params.code);
    std::memcpy(&e->runtime_in, &in.runtime, sizeof(VkbShader::RuntimeInfo));
    e->start = in.start;
    e->salt = in.salt;
    e->result.info = std::make_unique<VkbShader::Info>(info);
    e->result.info->user_data = in.params.user_data;
    e->result.info->pgm_base = in.params.Base();
    e->result.info->RefreshFlatBuf();
    e->spec = VkbShader::StageSpecialization(*e->result.info, in.runtime, profile, in.start);
    e->result.info->user_data = {};
    e->result.info->flattened_ud_buf.clear();
    e->result.runtime = runtime;
    e->result.end = end;
    e->result.spv = spv;
    e->result.windows = windows;
    e->translate_ns = translate_ns;
    e->used = true; // its first use is this draw: logged as a build
    c_kept.fetch_add(1, std::memory_order_relaxed);
    c_spv_words.fetch_add(spv.size(), std::memory_order_relaxed);
    std::vector<u8> bytes = Encode(*e);
    std::string name = FileName(in.params.hash, in.runtime, bytes);
    State& st = S();
    {
        std::scoped_lock lock{st.mutex};
        st.entries[KeyOf(in.params.hash, in.runtime)].push_back(std::move(e));
    }
    if (!ready.load(std::memory_order_acquire)) { // the folder could not be opened (said at the start): kept in memory only
        return;
    }
    {
        std::scoped_lock lock{st.write_mutex};
        st.write_queue.emplace_back(std::move(name), std::move(bytes));
    }
    st.write_cv.notify_one();
}

bool Same(const Inputs& in, const Translation& stored, const VkbShader::Info& info, const VkbShader::RuntimeInfo& runtime, const VkbShader::Backend::Bindings& end,
          const std::vector<u32>& spv, const std::vector<WriteWindow::Window>& windows) {
    const char* differs = FirstDifference(*stored.info, info);
    const bool spv_same = stored.spv == spv, end_same = stored.end == end, windows_same = stored.windows == windows,
               runtime_same = std::memcmp(&stored.runtime, &runtime, sizeof(VkbShader::RuntimeInfo)) == 0;
    const bool same = differs == nullptr && spv_same && end_same && windows_same && runtime_same;
    (same ? c_check_same : c_check_different).fetch_add(1, std::memory_order_relaxed);
    if (!same) {
        State& st = S();
        std::scoped_lock lock{st.mutex};
        if (st.different_logged.insert(in.params.hash).second) {
            LOG_ERROR(Render_Vulkan, "vkb shader store: the stored translation of program {:#x} (stage {}) differs from a fresh one: module {} / {} words ({}), bindings {}, "
                      "runtime {}, windows {}, first info member that differs: {}",
                      in.params.hash, static_cast<u32>(in.runtime.sw_stage), stored.spv.size(), spv.size(), spv_same ? "same" : "differ", end_same ? "same" : "differ",
                      runtime_same ? "same" : "differ", windows_same ? "same" : "differ", differs != nullptr ? differs : "none");
        }
    }
    return same;
}

std::filesystem::path Folder() {
    State& st = S();
    std::scoped_lock lock{st.mutex};
    return st.folder;
}

void NoteBypass() {
    c_bypassed.fetch_add(1, std::memory_order_relaxed);
}

void NoteBuild(u64 pgm_hash, u32 hw_stage) {
    if (!StatsOn()) {
        return;
    }
    auto& s = GetStats();
    std::scoped_lock lock{s.mutex};
    ++s.builds;
    ++s.frame_builds;
    s.before_frame += s.last_frame == std::chrono::steady_clock::time_point{};
    std::printf("Shader built while playing: %#018llx_%u (hw stage %u)\n", static_cast<unsigned long long>(pgm_hash), s.per_program[pgm_hash]++, hw_stage);
}

void NoteFrame() {
    if (!StatsOn()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& s = GetStats();
    std::scoped_lock lock{s.mutex};
    if (s.last_frame != std::chrono::steady_clock::time_point{}) {
        const u32 us = static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(now - s.last_frame).count());
        std::printf("Frame time: %.1f ms, %u shader builds\n", us / 1000.0, s.frame_builds);
        s.frames.emplace_back(us, std::exchange(s.frame_builds, 0));
    } else {
        s.frame_builds = 0;
    }
    s.last_frame = now;
    if (now - s.last_report >= std::chrono::seconds{5}) {
        s.last_report = now;
        ReportLocked(s);
    }
}

nlohmann::json Report() {
    const char* mode = GetMode() == Mode::Off ? "off" : GetMode() == Mode::Check ? "check" : "on";
    size_t programs = 0, waiting = 0;
    std::string error;
    std::string dir;
    State& st = S();
    {
        std::scoped_lock lock{st.mutex};
        programs = st.entries.size();
        error = st.open_error;
        dir = st.folder.string();
    }
    {
        std::scoped_lock lock{st.write_mutex};
        waiting = st.write_queue.size();
    }
    return {{"mode", mode},
            {"folder", dir},
            {"open_error", error},
            {"open_ms", c_open_ms.load()},
            {"lookups", c_lookups.load()},
            {"hits", c_hits.load()},
            {"hits_from_disk", c_disk_hits.load()},
            {"kept", c_kept.load()},
            {"programs_in_memory", programs},
            {"files_indexed_at_open", c_files_indexed.load()},
            {"files_read", c_files_read.load()},
            {"files_damaged_removed", c_files_bad.load()},
            {"files_written", c_written.load()},
            {"files_write_failed", c_write_failed.load()},
            {"files_waiting", waiting},
            {"not_kept_geometry", c_not_kept_geometry.load()},
            {"misses_other_resource_kinds", c_kind_misses.load()},
            {"bypassed_for_debug_tools", c_bypassed.load()},
            {"module_mb_kept", static_cast<double>(c_spv_words.load()) * 4.0 / 1048576.0},
            {"translate_ms_saved", static_cast<double>(c_saved_ns.load()) / 1e6},
            {"check_same", c_check_same.load()},
            {"check_different", c_check_different.load()}};
}

} // namespace Core::Vkb::ShaderStore
