// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_debug_server.h"
#include "native/native_marker.h"
#include "native/native_vkb_graphics.h"

#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <bit>
#include <type_traits>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>
#include <boost/container/small_vector.hpp>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include <deque>
#include <filesystem>
#include "common/path_util.h"
#include "native/native_vkb.h"
#include "native/shadps4/core/guest_heap.h"
#include "native/native_desc.h"
#include "native/native_device.h"
#include "native/native_draw.h"
#include "native/native_shader_env.h"
#include "native/native_target_inputs.h"
#include "native/native_vkb_objects.h"
#include "native/native_vkb_stream.h"
#include "native/native_vkb_util.h"
#include "native/shadps4/shader_recompiler/backend/bindings.h"
#include "native/shadps4/shader_recompiler/backend/spirv/emit_spirv.h"
#include "native/shadps4/shader_recompiler/frontend/fetch_shader.h"
#include "native/shadps4/shader_recompiler/info.h"
#include "native/shadps4/shader_recompiler/profile.h"
#include "native/shadps4/shader_recompiler/recompiler.h"
#include "native/shadps4/shader_recompiler/runtime_info.h"
#include "native/shadps4/video_core/amdgpu/regs_shader.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_fixed_state.h"
#include "native/native_vkb_pipeline.h"
#include "native/vkb_core/vkb_device.h"
#include "native/vkb_core/liverpool_to_vk.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"
#include "native/native_vkb_side_census.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_shader_boundary.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_shader_util.h"
#include "native/vkb_core/sampler.h"


namespace Core::Vkb::Graphics {

namespace {

using Util::Mix;
using Util::ObjectClass;
using Util::Rd;
using Util::Source;
using Util::SourceOf;
using VkbShader::HwStage;
using VkbShader::SwStage;

// the device context (Ctx in native_draw.cpp)
constexpr u64 CtxInputLayout = 0x68, CtxRenderTargetCount = 0x4484, CtxRenderTargets = 0x4488, CtxDepthStencilView = 0x44d0, CtxPixelShader = 0x4690,
              CtxVertexShader = 0x4698, CtxTarget7 = 0x130e0;
constexpr u64 SrvTable(u32 stage) {
    return 0x580 + stage * 0x400;
}

/// One stage of a variant: its program, the user data it was specialized with (kept: the info's spans point into it), its module and its slot map
struct Stage {
    u32 index{};                          ///< 0 vertex, 1 pixel (the engine's stage numbers)
    u64 shader{};                         ///< the shader object it was compiled from
    Core::NativeDraw::UserLayout layout;
    Core::NativeDraw::UserLayout live;    ///< the user data of the current draw (the flat user data buffer only)
    std::vector<vk::ImageViewType> image_types;
    std::vector<std::unique_ptr<VideoCore::Vkb::Sampler>> constant_samplers; ///< per sampler: ours from the S# the shader holds as a constant (null: from a slot)
    bool has_flat{};
    std::unique_ptr<VkbShader::Info> info;
    VkbShader::RuntimeInfo runtime{};
    vk::ShaderModule module{};
    std::vector<Source> buffers, images, samplers;
};

struct Variant {
    Stage vs, ps;
    bool has_ps{};
    VkbShader::Gcn::FetchShaderData fetch{};
    bool has_fetch{};
    std::vector<Source> attributes; ///< per fetch attribute: the vertex buffer table element it reads (tag kind 5)
    VkbVulkan::Vkb::GraphicsPipelineKey key{};
    std::array<vk::Format, 8> color_formats{}; ///< the pipeline's attachment formats (our views are made in them)
    vk::Format depth_format{};                 ///< our depth image's format (the pipeline's depth format)
    std::unique_ptr<VkbVulkan::Vkb::GraphicsPipeline> pipeline;
    bool ok{};
};

std::mutex mutex;
std::unordered_map<u64, std::unique_ptr<Variant>> variants;
std::unique_ptr<VkbShader::Pools> pools;
VkbShader::Profile profile{};
const VkbVulkan::Instance* instance = nullptr;

enum Fail : u32 { FailLayout, FailNoCode, FailTranslation, FailFetch, FailNoDynamicVertexInput, FailPipeline, FailCount };
constexpr std::array<const char*, FailCount> FailNames{"user_data_layout", "no_program", "translation", "fetch_shader", "no_dynamic_vertex_input", "pipeline"};
std::array<std::atomic<u64>, FailCount> c_fail{};
std::atomic<u64> c_draws{0}, c_by_kind[3]{}, c_no_vs{0}, c_special{0}, c_no_fixed{0}, c_found{0}, c_compiled{0}, c_failed{0}, c_compile_ns{0}, c_key_ns{0};
std::atomic<u64> c_depth_only{0}, c_targets_seen{0}, c_unsupported_layout_bits{0};
/// D3 census (note 77, TLG_VKB_TARGET_CENSUS=1, debug only): at each variant compile, every bound target's engine description (the view's format and dimension at
/// + 0x40 / + 0x44, its texture's sample count at + 0x194) against what the PS4 register block in the view gives (format, number type, swap, blend bypass, samples)
bool TargetCensusOn() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_TARGET_CENSUS"); return e != nullptr && e[0] == '1'; }();
    return on;
}
std::mutex target_census_mutex;
std::map<std::string, u64> target_census;
void TargetCensusNote(std::string line) {
    std::scoped_lock lock{target_census_mutex};
    ++target_census[std::move(line)];
}
std::atomic<u64> c_src[2][3][2]{}; ///< [stage vs/ps][buffers/images/samplers][mapped/unmapped]
std::atomic<u64> c_attr_mapped{0}, c_attr_unmapped{0};
std::atomic<u64> c_recorded{0}, c_record_ns{0}, c_skip_not_compiled{0}, c_skip_buffer{0}, c_skip_image{0}, c_skip_sampler{0}, c_skip_uav{0}, c_skip_special{0}, c_skip_target{0},
    c_skip_depth{0}, c_late_views{0}, c_skip_vertex{0}, c_skip_index{0}, c_skip_indirect{0}, c_skip_no_attachment{0}, c_indirect_recorded{0}, c_new_passes{0}, c_flat_rebuilt{0};

u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Ready() {
    if (instance != nullptr) {
        return true;
    }
    if (!Core::Vkb::Device::Ready()) { // merge note 5: the backend's own device, made by its presenter
        return false;
    }
    instance = &Core::Vkb::Device::Instance();
    profile = Core::Vkb::Device::Profile();
    pools = std::make_unique<VkbShader::Pools>();
    return true;
}

/// The colour target words of a bound render target view (the 14 words of its block at + 0x54; word 0 zero: not a target)
bool ColorWords(u64 view, std::array<u32, Core::NativeTarget::RawTargetWords::ColorWords>& out) {
    out = {};
    if (view == 0 || Rd<u32>(view + 0x54, 0) == 0) {
        return false;
    }
    for (u32 k = 0; k < 14; ++k) {
        out[k] = Rd<u32>(view + 0x54, k * 4);
    }
    return true;
}

/// The render target views of the draw, slots 0 to 7 (slot 7 is the object at ctx + 0x130e0)
std::array<u64, 8> TargetViews(u64 ctx) {
    std::array<u64, 8> views{};
    const u32 count = Rd<u32>(ctx, CtxRenderTargetCount);
    for (u32 slot = 0; slot < 7; ++slot) {
        views[slot] = slot < count ? Rd<u64>(ctx, CtxRenderTargets + slot * 8) : 0;
    }
    views[7] = Rd<u64>(ctx, CtxTarget7);
    return views;
}

Core::NativeTarget::DepthTarget DepthOf(u64 ctx) {
    std::array<u32, Core::NativeTarget::RawTargetWords::DepthWords> words{};
    if (const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView); dsv != 0) {
        for (u32 k = 0; k < words.size(); ++k) {
            words[k] = Rd<u32>(dsv + 0x58, k * 4);
        }
    }
    return Core::NativeTarget::DepthFromWords(words.data());
}

/// D3 (note 77): the targets described from the engine's own objects (the view's DXGI format at + 0x40, its texture's sample count at + 0x194) instead of the PS4
/// register block the engine keeps in each view (+ 0x54 colour, + 0x58 depth). The values stay in the recompiler's terms (its export conversion needs them).
/// A format missing from the table below falls back to the words (counted).
/// TLG_VKB_TARGET_CHECK=1 (debug): both descriptions at every variant compile, differences counted and the first ones listed.
bool TargetCheckOn() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_TARGET_CHECK"); return e != nullptr && e[0] == '1'; }();
    return on;
}
std::atomic<u64> c_targets_engine{0}, c_targets_unmapped{0}, c_target_checks{0}, c_target_check_differences{0};
std::mutex target_check_mutex;
std::vector<std::string> target_check_lines;
void TargetCheckNote(std::string line) {
    c_target_check_differences.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock{target_check_mutex};
    if (target_check_lines.size() < 16) {
        target_check_lines.push_back(std::move(line));
    }
}

/// A DXGI render target format in colour buffer terms: format, number type, component swap (note 77 census, heaviest save; the other rows follow the same rules)
struct CbFormat {
    u32 format{}, number_type{};
    Core::NativeTarget::ColorTarget::SwapMode swap{};
};
std::optional<CbFormat> CbFormatOf(u32 dxgi) {
    using Swap = Core::NativeTarget::ColorTarget::SwapMode;
    constexpr u32 Unorm = 0, Snorm = 1, Uint = 4, Sint = 5, Srgb = 6, Float = 7;
    switch (dxgi) {
    case 2: return CbFormat{14, Float};                 // R32G32B32A32_FLOAT
    case 1: case 3: return CbFormat{14, Uint};          // R32G32B32A32_TYPELESS / _UINT
    case 4: return CbFormat{14, Sint};
    case 9: case 10: return CbFormat{12, Float};        // R16G16B16A16_TYPELESS / _FLOAT (seen)
    case 11: return CbFormat{12, Unorm};
    case 12: return CbFormat{12, Uint};
    case 13: return CbFormat{12, Snorm};
    case 14: return CbFormat{12, Sint};
    case 16: return CbFormat{11, Float};                // R32G32_FLOAT
    case 15: case 17: return CbFormat{11, Uint};
    case 18: return CbFormat{11, Sint};
    case 23: case 24: return CbFormat{9, Unorm};        // R10G10B10A2_TYPELESS / _UNORM (seen)
    case 25: return CbFormat{9, Uint};
    case 26: return CbFormat{6, Float};                 // R11G11B10_FLOAT (seen)
    case 27: case 28: return CbFormat{10, Unorm};       // R8G8B8A8_TYPELESS / _UNORM (seen)
    case 29: return CbFormat{10, Srgb};                 // R8G8B8A8_UNORM_SRGB (seen)
    case 30: return CbFormat{10, Uint};
    case 31: return CbFormat{10, Snorm};
    case 32: return CbFormat{10, Sint};
    case 33: case 34: return CbFormat{5, Float};        // R16G16_TYPELESS / _FLOAT (seen)
    case 35: return CbFormat{5, Unorm};
    case 36: return CbFormat{5, Uint};
    case 37: return CbFormat{5, Snorm};
    case 38: return CbFormat{5, Sint};
    case 41: return CbFormat{4, Float};                 // R32_FLOAT
    case 39: case 42: return CbFormat{4, Uint};
    case 43: return CbFormat{4, Sint};
    case 48: case 49: return CbFormat{3, Unorm};        // R8G8_TYPELESS / _UNORM
    case 50: return CbFormat{3, Uint};
    case 51: return CbFormat{3, Snorm};
    case 52: return CbFormat{3, Sint};
    case 53: case 54: return CbFormat{2, Float};        // R16_TYPELESS / _FLOAT
    case 56: return CbFormat{2, Unorm};
    case 57: return CbFormat{2, Uint};
    case 58: return CbFormat{2, Snorm};
    case 59: return CbFormat{2, Sint};
    case 60: case 61: return CbFormat{1, Unorm};        // R8_TYPELESS / _UNORM (seen)
    case 62: return CbFormat{1, Uint};
    case 63: return CbFormat{1, Snorm};
    case 64: return CbFormat{1, Sint};
    case 87: case 88: case 90: return CbFormat{10, Unorm, Swap::Alternate}; // B8G8R8A8 / B8G8R8X8 / _TYPELESS (seen)
    case 91: return CbFormat{10, Srgb, Swap::Alternate};
    default: return std::nullopt;
    }
}
u32 TextureSamples(u64 view) {
    const u64 resource = Rd<u64>(view, 0x30);
    return std::max(resource != 0 && IsHeapPointer(resource) ? Rd<u32>(resource, 0x194) : 1u, 1u);
}

/// A bound colour target in the recompiler's terms (false: none bound)
bool ColorOf(u64 view, Core::NativeTarget::ColorTarget& out) {
    out = {};
    if (view == 0) {
        return false;
    }
    if (const auto f = CbFormatOf(Rd<u32>(view, 0x40))) {
        out.base_address = 1; // ColorTarget's "bound" test is base_address && format; the backend never uses the address
        out.info.format = f->format;
        out.info.number_type = f->number_type;
        out.info.comp_swap = f->swap;
        out.attrib.num_samples_log2 = out.attrib.num_fragments_log2 = static_cast<u32>(std::bit_width(TextureSamples(view)) - 1);
        c_targets_engine.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    c_targets_unmapped.fetch_add(1, std::memory_order_relaxed);
    std::array<u32, Core::NativeTarget::RawTargetWords::ColorWords> words;
    if (!ColorWords(view, words)) {
        return false;
    }
    out = Core::NativeTarget::ColorFromWords(words.data());
    return true;
}

/// The depth target in the recompiler's terms
struct DepthDesc {
    Core::NativeTarget::DepthTarget::ZFormat z{Core::NativeTarget::DepthTarget::ZFormat::Invalid};
    Core::NativeTarget::DepthTarget::StencilFormat stencil{Core::NativeTarget::DepthTarget::StencilFormat::Invalid};
    u32 samples{1};
};
DepthDesc DepthWordsDesc(u64 ctx) {
    const Core::NativeTarget::DepthTarget depth = DepthOf(ctx);
    DepthDesc d;
    d.z = depth.DepthValid() ? depth.z_info.format : Core::NativeTarget::DepthTarget::ZFormat::Invalid;
    d.stencil = depth.StencilValid() ? depth.stencil_info.format : Core::NativeTarget::DepthTarget::StencilFormat::Invalid;
    d.samples = depth.NumSamples();
    return d;
}
DepthDesc DepthDescOf(u64 ctx) {
    using Z = Core::NativeTarget::DepthTarget::ZFormat;
    using S = Core::NativeTarget::DepthTarget::StencilFormat;
    const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView);
    if (dsv == 0) {
        return {};
    }
    DepthDesc d;
    switch (Rd<u32>(dsv, 0x40)) {
    case 39: case 40: case 41: d.z = Z::Z32Float; break;                                                          // D32_FLOAT (seen) and its typeless forms
    case 19: case 20: case 21: case 22: case 44: case 45: case 46: case 47: d.z = Z::Z32Float, d.stencil = S::Stencil8; break; // D32S8 / D24S8 (seen; kept as 32 bit float)
    case 53: case 55: case 56: d.z = Z::Z16; break;                                                               // D16_UNORM
    default: c_targets_unmapped.fetch_add(1, std::memory_order_relaxed); return DepthWordsDesc(ctx);
    }
    d.samples = TextureSamples(dsv);
    c_targets_engine.fetch_add(1, std::memory_order_relaxed);
    return d;
}

/// What the variant key takes from a bound target (false: none): the engine's format and sample count, remembered per thread by the view and its unique id (+ 0x10,
/// never reused); with the legacy words, CB_COLOR_INFO and CB_COLOR_ATTRIB (or DB_Z_INFO and DB_STENCIL_INFO for the depth view)
bool TargetKey(u64 view, bool depth, u64& a, u64& b) {
    if (view == 0) {
        return false;
    }
    const u64 block = view + (depth ? 0x58 : 0x54);
    const auto words = [&] {
        if (!depth && Rd<u32>(block, 0) == 0) {
            return false;
        }
        a = Rd<u32>(block, depth ? 0 : 16), b = Rd<u32>(block, depth ? 4 : 20);
        return true;
    };
    struct Entry {
        u64 view{}, id{~0ull}, a{}, b{};
        bool mapped{};
    };
    thread_local std::array<Entry, 256> memo{};
    const u64 id = Rd<u64>(view, 0x10);
    Entry& e = memo[((view >> 4) ^ (view >> 12) ^ id) & 255];
    if (e.view != view || e.id != id) {
        const u32 dxgi = Rd<u32>(view, 0x40);
        e = {view, id, 0x100000000ull | dxgi, TextureSamples(view), depth || CbFormatOf(dxgi).has_value()};
    }
    if (!e.mapped) {
        return words();
    }
    a = e.a, b = e.b;
    return true;
}

/// The variant of the draw: the shader ids, the fetch object, the fixed state (cached per state, so its address names it), the target words that describe formats and samples,
/// and the class of every object in the shader resource slots the two shaders use
/// The user data offsets (SH register offsets, 0: none) where the console's indirect draw puts the base vertex and the base instance for this vertex shader
u32 IndirectSgprs(u64 vs) {
    const u8 locations = Rd<u8>(Rd<u64>(vs, 0x120), 0x27);
    constexpr u32 VertexStageSgprBase = 0x4c;
    const u32 vertex = (locations & 0xf) == 0 ? 0 : (locations & 0xf) + VertexStageSgprBase;
    const u32 instance = (locations >> 4) == 0 ? 0 : (locations >> 4) + VertexStageSgprBase;
    return vertex | instance << 16;
}

/// A3 (note 47; TLG_VKB_FAST_KEY, default on, =0 the full key): the same key value as VariantKeyFull, made cheaper. Only the set bits of the
/// shader-resource masks are visited; of a target view's words only the three the key uses are read; a bound object's class is remembered per thread by the object
/// and its unique id (+0x10, never reused), since a view's format, kind and resource and the resource's description do not change for its life.
/// TLG_VKB_KEY_CHECK=1 computes both keys at every draw and counts the differences (must be 0).
u64 ObjectClassMemo(u64 object) {
    if (object == 0 || !IsHeapPointer(object)) { // note 75: an unused slot in the shader's mask can hold anything (0x100000001 crashed here); never dereferenced
        return ObjectClass(object);
    }
    struct Entry {
        u64 object{}, id{~0ull}, cls{};
    };
    thread_local std::unique_ptr<std::array<Entry, 4096>> table;
    if (!table) {
        table = std::make_unique<std::array<Entry, 4096>>();
    }
    const u64 id = Rd<u64>(object, 0x10);
    Entry& e = (*table)[((object >> 3) ^ (object >> 15) ^ id) & 4095];
    if (e.object == object && e.id == id) {
        return e.cls;
    }
    e = {object, id, ObjectClass(object)};
    return e.cls;
}
u64 VariantKeyFull(u64 ctx, u64 vs, u64 ps, u64 fetch, const VkbVulkan::FixedState* fixed, bool indirect);
u64 VariantKeyFast(u64 ctx, u64 vs, u64 ps, u64 fetch, const VkbVulkan::FixedState* fixed, bool indirect) {
    u64 h = Mix(0x6a, Rd<u64>(vs, 0x10));
    if (indirect) {
        h = Mix(h, 0x1d00000000ull | IndirectSgprs(vs));
    }
    h = Mix(h, ps != 0 ? Rd<u64>(ps, 0x10) : 0);
    h = Mix(h, fetch);
    h = Mix(h, Rd<u64>(ctx, CtxInputLayout));
    h = Mix(h, reinterpret_cast<u64>(fixed));
    const auto views = TargetViews(ctx);
    for (u32 slot = 0; slot < 8; ++slot) {
        if (u64 a, b; TargetKey(views[slot], false, a, b)) { // D3: the engine's format and samples
            h = Mix(Mix(Mix(h, slot), a), b);
        }
    }
    if (const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView); dsv != 0) {
        if (u64 a, b; TargetKey(dsv, true, a, b)) {
            h = Mix(Mix(h, a), b);
        }
        h = Mix(h, static_cast<u64>(Core::Vkb::ViewFormat(Rd<u64>(dsv, 0x10))));
    }
    for (u32 stage = 0; stage < 2; ++stage) {
        const u64 shader = stage == 0 ? vs : ps;
        if (shader == 0) {
            continue;
        }
        const u64 masks[2] = {Rd<u64>(shader, 0x30 + 0xc0), Rd<u64>(shader, 0x30 + 0xc8)};
        for (u32 half = 0; half < 2; ++half) {
            for (u64 bits = masks[half]; bits != 0; bits &= bits - 1) {
                const u32 s = half * 64 + static_cast<u32>(std::countr_zero(bits));
                h = Mix(Mix(h, stage << 8 | s), ObjectClassMemo(Rd<u64>(ctx, SrvTable(stage) + s * 8)));
            }
        }
    }
    return h;
}
std::atomic<u64> c_key_checks{0}, c_key_differences{0};
u64 VariantKey(u64 ctx, u64 vs, u64 ps, u64 fetch, const VkbVulkan::FixedState* fixed, bool indirect) {
    static const bool fast = [] { const char* e = std::getenv("TLG_VKB_FAST_KEY"); return e == nullptr || e[0] != '0'; }();
    static const bool check = [] { const char* e = std::getenv("TLG_VKB_KEY_CHECK"); return e != nullptr && e[0] == '1'; }();
    if (check) {
        const u64 a = VariantKeyFast(ctx, vs, ps, fetch, fixed, indirect), b = VariantKeyFull(ctx, vs, ps, fetch, fixed, indirect);
        c_key_checks.fetch_add(1, std::memory_order_relaxed);
        if (a != b) {
            c_key_differences.fetch_add(1, std::memory_order_relaxed);
        }
        return b;
    }
    return fast ? VariantKeyFast(ctx, vs, ps, fetch, fixed, indirect) : VariantKeyFull(ctx, vs, ps, fetch, fixed, indirect);
}
u64 VariantKeyFull(u64 ctx, u64 vs, u64 ps, u64 fetch, const VkbVulkan::FixedState* fixed, bool indirect) {
    u64 h = Mix(0x6a, Rd<u64>(vs, 0x10));
    if (indirect) {
        h = Mix(h, 0x1d00000000ull | IndirectSgprs(vs));
    }
    h = Mix(h, ps != 0 ? Rd<u64>(ps, 0x10) : 0);
    h = Mix(h, fetch);
    h = Mix(h, Rd<u64>(ctx, CtxInputLayout));
    h = Mix(h, reinterpret_cast<u64>(fixed));
    const auto views = TargetViews(ctx);
    for (u32 slot = 0; slot < 8; ++slot) {
        if (u64 a, b; TargetKey(views[slot], false, a, b)) { // D3: the engine's format and samples (legacy: CB_COLOR_INFO, CB_COLOR_ATTRIB)
            h = Mix(Mix(Mix(h, slot), a), b);
        }
    }
    if (const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView); dsv != 0) {
        if (u64 a, b; TargetKey(dsv, true, a, b)) { // legacy: DB_Z_INFO, DB_STENCIL_INFO
            h = Mix(Mix(h, a), b);
        }
        h = Mix(h, static_cast<u64>(Core::Vkb::ViewFormat(Rd<u64>(dsv, 0x10))));
    }
    for (u32 stage = 0; stage < 2; ++stage) {
        const u64 shader = stage == 0 ? vs : ps;
        if (shader == 0) {
            continue;
        }
        const u64 lo = Rd<u64>(shader, 0x30 + 0xc0), hi = Rd<u64>(shader, 0x30 + 0xc8);
        for (u32 s = 0; s < 128; ++s) {
            if ((((s < 64 ? lo : hi) >> (s & 63)) & 1) != 0) {
                h = Mix(Mix(h, stage << 8 | s), ObjectClass(Rd<u64>(ctx, SrvTable(stage) + s * 8)));
            }
        }
    }
    return h;
}

vk::ImageViewType ViewTypeOf(VkbAmdGpu::ImageType type) {
    switch (type) {
    case VkbAmdGpu::ImageType::Color1D: return vk::ImageViewType::e2D; // note 41: the module declares 1D images as 2D
    case VkbAmdGpu::ImageType::Color1DArray: return vk::ImageViewType::e2DArray;
    case VkbAmdGpu::ImageType::Color2DArray:
    case VkbAmdGpu::ImageType::Color2DMsaaArray: return vk::ImageViewType::e2DArray;
    case VkbAmdGpu::ImageType::Color3D: return vk::ImageViewType::e3D;
    case VkbAmdGpu::ImageType::Cube: return vk::ImageViewType::eCube;
    default: return vk::ImageViewType::e2D;
    }
}

/// Compiles one stage (mutex held): its user data, program, translation, module and slot map
bool CompileStage(u64 ctx, u64 shader, u32 stage, u64 code_address, Stage& out, VkbShader::Backend::Bindings& binding) {
    const HwStage hw = stage == 0 ? HwStage::Vertex : HwStage::Fragment;
    const SwStage sw = stage == 0 ? SwStage::Vertex : SwStage::Fragment;
    const u32 inputs_offset = stage == 0 ? 0x28 : 0x3c;
    Core::NativeDraw::BuildUserLayout(ctx, stage, shader, inputs_offset, false, out.layout);
    if (out.layout.unsupported_types != 0) {
        c_unsupported_layout_bits.fetch_or(out.layout.unsupported_types, std::memory_order_relaxed);
        c_fail[FailLayout].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (code_address == 0 || !IsGuestAddress(code_address)) {
        c_fail[FailNoCode].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u32* code = reinterpret_cast<const u32*>(code_address);
    const auto& binary = VkbAmdGpu::SearchBinaryInfo(code);
    const VkbShader::ShaderParams params{
        .user_data = std::span<const u32, VkbShader::ShaderParams::NumShaderUserData>{out.layout.sgpr.data(), VkbShader::ShaderParams::NumShaderUserData},
        .code = std::span<const u32>{code, binary.length / sizeof(u32)},
        .hash = binary.shader_hash,
    };
    out.info = std::make_unique<VkbShader::Info>(hw, sw, params);
    out.info->pgm_base = params.Base();
    std::vector<u32> spv;
    {
        const auto program = VkbShader::TranslateProgram(params.code, *pools, *out.info, out.runtime, profile);
        if (out.info->translation_failed) {
            c_fail[FailTranslation].fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (Core::Vkb::FlatCensus::On()) { // note 77 census (debug)
            Core::Vkb::FlatCensus::Note(program, shader ? Rd<u64>(shader, 0x120) : 0, inputs_offset, stage == 0 ? "vs" : "ps", out.info->pgm_hash);
        }
        // note 41, test only: TLG_VKB_TEST_PS_PROBE=<pixel shader hash>: render target 3 gets the coordinates of the shader's first 2D texture sample (x, y) and the first
        // barycentric weight they use (z), so a G-buffer dump shows what the shader receives
        static const u64 probe = [] { const char* e = std::getenv("TLG_VKB_TEST_PS_PROBE"); return e != nullptr ? std::strtoull(e, nullptr, 16) : 0ull; }();
        if (probe != 0 && sw == VkbShader::SwStage::Fragment && out.info->pgm_hash == probe) {
            VkbShader::IR::Inst* coords = nullptr;
            for (VkbShader::IR::Block* block : program.blocks) {
                for (VkbShader::IR::Inst& inst : block->Instructions()) {
                    if (coords == nullptr && inst.GetOpcode() == VkbShader::IR::Opcode::ImageSampleImplicitLod && inst.Arg(1).TryInst() != nullptr &&
                        inst.Arg(1).TryInst()->GetOpcode() == VkbShader::IR::Opcode::CompositeConstructF32x2) {
                        coords = inst.Arg(1).TryInst();
                    }
                }
            }
            u32 patched = 0;
            if (coords != nullptr) {
                const VkbShader::IR::Value u = coords->Arg(0), v = coords->Arg(1);
                VkbShader::IR::Value w = u;
                if (const VkbShader::IR::Inst* fma = u.TryInst(); fma != nullptr && fma->GetOpcode() == VkbShader::IR::Opcode::FPFma32) {
                    w = fma->Arg(1);
                }
                for (VkbShader::IR::Block* block : program.blocks) {
                    for (VkbShader::IR::Inst& inst : block->Instructions()) {
                        if (inst.GetOpcode() == VkbShader::IR::Opcode::SetAttribute && inst.Arg(0).Attribute() == VkbShader::IR::Attribute::RenderTarget3) {
                            const u32 comp = inst.Arg(2).U32();
                            inst.SetArg(1, comp == 0 ? u : comp == 1 ? v : comp == 2 ? w : VkbShader::IR::Value{1.0f});
                            ++patched;
                        }
                    }
                }
            }
            LOG_WARNING(Render_Vulkan, "vkb ps probe {:#x}: coordinates {} found, {} exports patched", probe, coords != nullptr, patched);
        }
        spv = VkbShader::Backend::SPIRV::EmitSPIRV(profile, out.runtime, program, binding);
    }
    // test: TLG_VKB_PS_DIR=<folder>: every pixel shader is written there as <hash>.spv; a file <hash>.new.spv in that folder takes the shader's place
    if (sw == VkbShader::SwStage::Fragment) {
        static const char* dir = std::getenv("TLG_VKB_PS_DIR");
        if (dir != nullptr) {
            const std::string base = fmt::format("{}/{:08x}", dir, static_cast<u32>(out.info->pgm_hash));
            if (std::FILE* f = std::fopen((base + ".new.spv").c_str(), "rb")) {
                std::vector<u32> repl(1u << 18);
                const size_t n = std::fread(repl.data(), 4, repl.size(), f);
                std::fclose(f);
                if (n > 5) {
                    repl.resize(n);
                    spv = std::move(repl);
                    LOG_ERROR(Render_Vulkan, "vkb ps {:#x}: replaced from the test folder ({} words)", out.info->pgm_hash, n);
                }
            } else if (std::FILE* f = std::fopen((base + ".spv").c_str(), "wb")) {
                std::fwrite(spv.data(), 4, spv.size(), f);
                std::fclose(f);
            }
        }
    }
    out.module = VkbVulkan::CompileSPV(spv, instance->GetDevice());
    // the slot map: the same layout with a tag per slot, flattened by the shader's own table walker
    Core::NativeDraw::UserLayout tagged;
    Core::NativeDraw::BuildUserLayout(ctx, stage, shader, inputs_offset, true, tagged);
    const std::span<const u32> real = out.info->user_data;
    out.info->user_data = std::span<const u32>{tagged.sgpr.data(), tagged.sgpr.size()};
    out.info->RefreshFlatBuf();
    const std::vector<u32> flat = out.info->flattened_ud_buf;
    out.info->user_data = real;
    out.info->RefreshFlatBuf();
    const auto count = [&](u32 kind, const Source& s) {
        c_src[stage][kind][s.cls == Source::Class::Unmapped ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    };
    for (const auto& b : out.info->buffers) {
        Source s = SourceOf(b.sharp_fetch, flat, b.IsSpecial(), b.post_op == VkbShader::SharpFetchPostOp::OffsetByProgramBase);
        if (s.cls == Source::Class::ProgramConstant) {
            const Core::NativeDesc::BufferDesc d = Core::NativeDesc::BufferDescOf(b.GetSharp(*out.info));
            s.address = d.address;
            s.size = d.size;
        }
        out.buffers.push_back(s);
        count(0, s);
    }
    for (const auto& image : out.info->images) {
        out.images.push_back(SourceOf(image.sharp_fetch, flat, false, false));
        count(1, out.images.back());
        out.image_types.push_back(ViewTypeOf(image.GetSharp(*out.info).GetHostViewType(image.is_array)));
    }
    for (const auto& b : out.info->buffers) {
        out.has_flat |= b.buffer_type == VkbShader::BufferType::Flatbuf;
    }
    out.index = stage;
    out.shader = shader;
    for (const auto& sampler : out.info->samplers) {
        Source s = SourceOf(sampler.sharp_fetch, flat, false, false);
        std::unique_ptr<VideoCore::Vkb::Sampler> constant;
        using Summary = std::remove_cvref_t<decltype(sampler.sharp_fetch)>::Summary;
        if (s.cls == Source::Class::Unmapped && sampler.sharp_fetch.summary == Summary::MultiLoad && sampler.sharp_fetch.load_mask == 0) {
            // the S# is a constant of the shader's code (no table load): our sampler made from it, once
            constant = std::make_unique<VideoCore::Vkb::Sampler>(*instance, VideoCore::Vkb::SamplerDescFromSharp(sampler.GetSharp(*out.info)), VkbAmdGpu::BorderColorBuffer{},
                                                             sampler.is_depth);
            s.cls = Source::Class::ProgramConstant;
        }
        out.samplers.push_back(s);
        out.constant_samplers.push_back(std::move(constant));
        count(2, out.samplers.back());
    }
    return true;
}

/// Compiles the variant of the draw (mutex held)
void Compile(u64 ctx, u64 vs, u64 ps, u64 fetch_object, const VkbVulkan::FixedState& fixed, Variant& v, bool indirect) {
    const u64 t0 = NowNs();
    const u64 vs_gnm = Rd<u64>(vs, 0x120);
    const u64 ps_gnm = ps != 0 ? Rd<u64>(ps, 0x120) : 0;
    Core::NativeShaderEnv::DrawEnv env;
    {
        const Core::NativeShaderEnv::Scope scope{vs_gnm, ps_gnm, fetch_object};
        (void)Core::NativeShaderEnv::Current(env);
    }
    auto& key = v.key;
    // the targets: depth from the depth view's words, colour from each render target view's words (the PS4 description the engine keeps in its views; stated in the notes)
    const DepthDesc depth_desc = DepthDescOf(ctx); // D3 (note 77): from the engine's depth view
    const bool db_enabled = fixed.depth_target || fixed.stencil_target;
    if (TargetCheckOn() && Rd<u64>(ctx, CtxDepthStencilView) != 0) {
        const DepthDesc w = DepthWordsDesc(ctx);
        c_target_checks.fetch_add(1, std::memory_order_relaxed);
        if (w.z != depth_desc.z || w.stencil != depth_desc.stencil || w.samples != depth_desc.samples) {
            TargetCheckNote(fmt::format("depth dxgi {}: words z {} stencil {} samples {}, engine z {} stencil {} samples {}", Rd<u32>(Rd<u64>(ctx, CtxDepthStencilView), 0x40),
                                        static_cast<u32>(w.z), static_cast<u32>(w.stencil), w.samples, static_cast<u32>(depth_desc.z), static_cast<u32>(depth_desc.stencil),
                                        depth_desc.samples));
        }
    }
    const Core::NativeTarget::DepthTarget depth = TargetCensusOn() ? DepthOf(ctx) : Core::NativeTarget::DepthTarget{}; // the census prints the words
    if (const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView); dsv != 0 && TargetCensusOn()) {
        const u64 resource = Rd<u64>(dsv, 0x30);
        TargetCensusNote(fmt::format("depth dxgi {} dim {} flags {} tex_samples {} tex_format {} -> z {} stencil {} samples {} | zvalid {} svalid {} db_enabled {}", Rd<u32>(dsv, 0x40),
                                     Rd<u32>(dsv, 0x44), Rd<u32>(dsv, 0x48), resource != 0 ? Rd<u32>(resource, 0x194) : 0, resource != 0 ? Rd<u32>(resource, 0x190) : 0,
                                     static_cast<u32>(depth.z_info.format), static_cast<u32>(depth.stencil_info.format), depth.NumSamples(), depth.DepthValid() ? 1 : 0,
                                     depth.StencilValid() ? 1 : 0, db_enabled ? 1 : 0));
    }
    key.z_format = depth_desc.z;
    key.stencil_format = depth_desc.stencil;
    if (const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView); dsv != 0) {
        v.depth_format = Core::Vkb::ViewFormat(Rd<u64>(dsv, 0x10));
    }
    if (v.depth_format != vk::Format::eD32SfloatS8Uint) {
        key.stencil_format = Core::NativeTarget::DepthTarget::StencilFormat::Invalid; // our depth image has no stencil
    }
    if (v.depth_format == vk::Format::eUndefined) {
        key.z_format = Core::NativeTarget::DepthTarget::ZFormat::Invalid;
    }
    key.depth_clamp_enable = fixed.depth_clamp;
    key.depth_clip_enable = fixed.depth_clip;
    key.negative_one_to_one = fixed.negative_one_to_one;
    key.provoking_first = fixed.provoking_first;
    key.prim_type = fixed.primitive_type;
    key.polygon_mode = fixed.polygon_mode;
    key.logic_op = fixed.logic_op;
    key.logic_op_enable = fixed.logic_op_enabled;
    key.depth_samples = db_enabled && (depth_desc.z != Core::NativeTarget::DepthTarget::ZFormat::Invalid ||
                                       depth_desc.stencil != Core::NativeTarget::DepthTarget::StencilFormat::Invalid) ? depth_desc.samples : 1;
    key.num_samples = key.depth_samples;
    key.alpha_not_exported = fixed.alpha_not_exported;
    const auto views = TargetViews(ctx);
    std::array<Core::NativeTarget::ColorTarget, 8> colors{};
    std::array<u32, Core::NativeTarget::RawTargetWords::ColorWords> words;
    for (u32 cb = 0; cb < 8 && fixed.color_enabled; ++cb) {
        if (!ColorOf(views[cb], colors[cb])) { // D3 (note 77): from the engine's view
            continue;
        }
        c_targets_seen.fetch_add(1, std::memory_order_relaxed);
        if (TargetCheckOn()) {
            c_target_checks.fetch_add(1, std::memory_order_relaxed);
            const auto& e = colors[cb];
            const auto w = ColorWords(views[cb], words) ? Core::NativeTarget::ColorFromWords(words.data()) : Core::NativeTarget::ColorTarget{};
            if (static_cast<bool>(w) != static_cast<bool>(e) || w.GetDataFmt() != e.GetDataFmt() || w.GetNumberFmt() != e.GetNumberFmt() ||
                w.GetNumberConversion() != e.GetNumberConversion() || w.info.comp_swap != e.info.comp_swap || w.info.blend_bypass != e.info.blend_bypass ||
                w.NumSamples() != e.NumSamples()) {
                TargetCheckNote(fmt::format("color dxgi {}: words fmt {} num {} swap {} bypass {} samples {} valid {}, engine fmt {} num {} swap {} samples {}",
                                            Rd<u32>(views[cb], 0x40), w.info.format, w.info.number_type, static_cast<u32>(w.info.comp_swap), w.info.blend_bypass, w.NumSamples(),
                                            static_cast<bool>(w) ? 1 : 0, e.info.format, e.info.number_type, static_cast<u32>(e.info.comp_swap), e.NumSamples()));
            }
        }
        if (TargetCensusOn() && ColorWords(views[cb], words)) {
            const u64 resource = Rd<u64>(views[cb], 0x30);
            const auto c = Core::NativeTarget::ColorFromWords(words.data());
            TargetCensusNote(fmt::format("color dxgi {} dim {} tex_samples {} -> fmt {} num {} swap {} bypass {} frag_log2 {} samp_log2 {} | data {} numfmt {} conv {} valid {}",
                                         Rd<u32>(views[cb], 0x40), Rd<u32>(views[cb], 0x44), resource != 0 ? Rd<u32>(resource, 0x194) : 0, c.info.format, c.info.number_type,
                                         static_cast<u32>(c.info.comp_swap), c.info.blend_bypass, c.attrib.num_fragments_log2, c.attrib.num_samples_log2,
                                         static_cast<u32>(c.GetDataFmt()), static_cast<u32>(c.GetNumberFmt()), static_cast<u32>(c.GetNumberConversion()), c ? 1 : 0));
        }
        if (!colors[cb] || fixed.target_write_mask[cb] == 0) {
            continue;
        }
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = colors[cb].GetDataFmt();
        color_buffer.num_format = colors[cb].GetNumberFmt();
        color_buffer.num_conversion = colors[cb].GetNumberConversion();
        if (env.has_pixel) {
            VkbVulkan::ShaderBoundary::SetExportFormat(color_buffer, env.pixel.fragment.color_export_format[cb]);
        } else {
            VkbVulkan::ShaderBoundary::SetNoExport(color_buffer);
        }
        color_buffer.swizzle = colors[cb].Swizzle();
        const auto& bc = fixed.blend[cb];
        color_buffer.blend_self_scale = bc.enable && !colors[cb].info.blend_bypass && (bc.color_op == vk::BlendOp::eMin || bc.color_op == vk::BlendOp::eMax) &&
                                        bc.src_color == vk::BlendFactor::eSrcColor && bc.dst_color == vk::BlendFactor::eDstColor;
    }
    u32 samples = key.num_samples;
    for (u32 cb = 0; cb < 8; ++cb) {
        if (colors[cb]) {
            samples = std::max(samples, colors[cb].NumSamples());
        }
    }
    // the pixel stage first, then the vertex stage, with one binding counter (the pipeline layout walks the stages in that order)
    VkbShader::Backend::Bindings binding{};
    v.has_ps = env.has_pixel && ps != 0;
    if (v.has_ps) {
        auto& rt = v.ps.runtime;
        rt.Initialize(HwStage::Fragment, SwStage::Fragment);
        rt.hw.fs.dual_source_blending = fixed.DualSourceBlending();
        for (u32 i = 0; i < VkbShader::MaxColorBuffers; ++i) {
            rt.hw.fs.color_buffers[i] = key.color_buffers[i];
        }
        rt.hw.fs.clip_distance_emulation =
            ((env.has_vertex && VkbVulkan::ShaderBoundary::GuestClipDistances(env.vertex)) || fixed.user_clip_plane_mask != 0) && profile.needs_clip_distance_emulation;
        VkbVulkan::ShaderBoundary::ApplyShaderEnv(rt, env, HwStage::Fragment, samples);
        if (!CompileStage(ctx, ps, 1, env.pixel.program.address, v.ps, binding)) {
            c_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } else {
        c_depth_only.fetch_add(1, std::memory_order_relaxed);
    }
    key.mrt_mask = v.has_ps ? v.ps.info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);
    {
        auto& rt = v.vs.runtime;
        rt.Initialize(HwStage::Vertex, SwStage::Vertex);
        rt.hw.vs.user_clip_plane_mask = fixed.user_clip_plane_mask;
        rt.hw.vs.emulate_depth_negative_one_to_one = !instance->IsDepthClipControlSupported() && fixed.negative_one_to_one;
        rt.hw.vs.clip_disable = fixed.clip_disabled;
        rt.sw.vs.tess_emulated_primitive =
            fixed.primitive_type == Core::NativePrimitive::Kind::RectList || fixed.primitive_type == Core::NativePrimitive::Kind::QuadList;
        VkbVulkan::ShaderBoundary::ApplyShaderEnv(rt, env, HwStage::Vertex, samples);
        if (indirect) {
            // V5a: the indirect draw's base vertex and instance come from its arguments (gl_BaseVertex / gl_BaseInstance where the console's user data had them)
            const u32 sgprs = IndirectSgprs(vs);
            rt.sw.vs.vertex_sgpr_offset = static_cast<u16>(sgprs & 0xffff);
            rt.sw.vs.instance_sgpr_offset = static_cast<u16>(sgprs >> 16);
            rt.sw.vs.is_indirect = true;
        }
        if (!CompileStage(ctx, vs, 0, env.vertex.program.address, v.vs, binding)) {
            c_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // the vertex attributes: which element of the vertex buffer table each reads (the tagged walk of the fetch shader's table)
    if (v.vs.info->has_fetch_shader) {
        if (!VkbShader::Gcn::ParseFetchShader(*v.vs.info, v.fetch)) {
            c_fail[FailFetch].fetch_add(1, std::memory_order_relaxed);
            c_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        v.has_fetch = true;
        Core::NativeDraw::UserLayout tagged;
        Core::NativeDraw::BuildUserLayout(ctx, 0, vs, 0x28, true, tagged);
        for (const auto& attribute : v.fetch.attributes) {
            Source s;
            const u32 pointer_lo = attribute.sgpr_base < 16 ? tagged.sgpr[attribute.sgpr_base] : 0;
            const u32 pointer_hi = attribute.sgpr_base + 1 < 16 ? tagged.sgpr[attribute.sgpr_base + 1] : 0;
            const u64 table = static_cast<u64>(pointer_hi) << 32 | pointer_lo;
            u32 kind = 0, slot = 0;
            if (table != 0 && Core::NativeDraw::UserLayout::TagOf(*reinterpret_cast<const u32*>(table + attribute.dword_offset * 4ull), kind, slot) && kind == 5) {
                s.cls = Source::Class::Slot;
                s.kind = 5;
                s.slot = static_cast<u16>(slot);
                c_attr_mapped.fetch_add(1, std::memory_order_relaxed);
            } else {
                c_attr_unmapped.fetch_add(1, std::memory_order_relaxed);
            }
            v.attributes.push_back(s);
        }
    }
    if (!instance->IsVertexInputDynamicState()) {
        c_fail[FailNoDynamicVertexInput].fetch_add(1, std::memory_order_relaxed);
        c_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // the second pass of the key: targets the pixel shader does not write are dropped; blend, write masks, samples
    u8 color_samples = 0;
    for (u32 cb = 0; cb < key.num_color_attachments && fixed.color_enabled; ++cb) {
        const u32 target_mask = fixed.target_write_mask[cb];
        if (!colors[cb] || target_mask == 0) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(VkbShader::PsColorBuffer));
            continue;
        }
        if (fixed.blend[cb].enable && !colors[cb].info.blend_bypass) {
            key.blend[cb] = fixed.blend[cb];
        }
        key.write_masks[cb] = vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};
        color_samples = static_cast<u8>(colors[cb].NumSamples());
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max<u8>(key.num_samples, color_samples);
    }
    key.stage_hashes[static_cast<u32>(SwStage::Vertex)] = v.vs.info->pgm_hash;
    key.stage_hashes[static_cast<u32>(SwStage::Fragment)] = v.has_ps ? v.ps.info->pgm_hash : 0;
    // our pipeline
    std::array<const VkbShader::Info*, VkbVulkan::Vkb::MaxShaderStages> infos{};
    std::array<VkbShader::RuntimeInfo, VkbVulkan::Vkb::MaxShaderStages> runtimes{};
    std::array<vk::ShaderModule, VkbVulkan::Vkb::MaxShaderStages> modules{};
    infos[static_cast<u32>(SwStage::Vertex)] = v.vs.info.get();
    runtimes[static_cast<u32>(SwStage::Vertex)] = v.vs.runtime;
    modules[static_cast<u32>(SwStage::Vertex)] = v.vs.module;
    if (v.has_ps) {
        infos[static_cast<u32>(SwStage::Fragment)] = v.ps.info.get();
        runtimes[static_cast<u32>(SwStage::Fragment)] = v.ps.runtime;
        modules[static_cast<u32>(SwStage::Fragment)] = v.ps.module;
    }
    VkbVulkan::Vkb::GraphicsPipeline::SerializationSupport sdata{};
    for (u32 i = 0; i < key.num_color_attachments; ++i) {
        v.color_formats[i] = instance->GetSupportedFormat(VkbVulkan::Vkb::LiverpoolToVK::SurfaceFormat(key.color_buffers[i].data_format, key.color_buffers[i].num_format),
                                                          vk::FormatFeatureFlagBits2::eColorAttachment);
    }
    v.pipeline = std::make_unique<VkbVulkan::Vkb::GraphicsPipeline>(*instance, Core::Vkb::Device::Scheduler(), Core::Vkb::Device::DescHeap(), profile, key, vk::PipelineCache{},
                                                             std::span<const VkbShader::Info*, VkbVulkan::Vkb::MaxShaderStages>{infos},
                                                             std::span<const VkbShader::RuntimeInfo, VkbVulkan::Vkb::MaxShaderStages>{runtimes}, v.has_fetch ? &v.fetch : nullptr,
                                                             std::span<const vk::ShaderModule>{modules}, sdata, false, v.depth_format);
    if (!v.pipeline->Handle()) {
        c_fail[FailPipeline].fetch_add(1, std::memory_order_relaxed);
        c_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    v.ok = true;
    c_compiled.fetch_add(1, std::memory_order_relaxed);
    Core::Vkb::FrameNote(fmt::format("new graphics variant: vs {:#x} ps {:#x} colors {} depth {}", v.vs.info ? v.vs.info->pgm_hash : 0ull,
                                     v.has_ps && v.ps.info ? v.ps.info->pgm_hash : 0ull, key.num_color_attachments, v.depth_format != vk::Format::eUndefined ? 1 : 0)); // note 44
    c_compile_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
}

/// The engine object in slot `slot` of graphics stage `stage`'s table of `kind` (1 resource, 2 sampler, 3 constant buffer)
u64 SlotObject(u64 ctx, u32 stage, u32 kind, u32 slot) {
    u64 table = 0, limit = 0;
    switch (kind) {
    case 1: table = 0x580 + stage * 0x400, limit = 128; break;
    case 2: table = 0x1d80 + stage * 0x80, limit = 16; break;
    case 3: table = 0x280 + stage * 0x80, limit = 16; break;
    default: return 0;
    }
    if (slot >= limit) {
        return 0;
    }
    const u64 object = Rd<u64>(ctx, table + slot * 8);
    return object != 0 && IsHeapPointer(object) && (object & 7) == 0 ? object : 0;
}

/// What one stage binds: descriptor writes (in the pipeline layout's order) are appended; buffer offsets go into the push data
struct Bound {
    boost::container::small_vector<vk::DescriptorBufferInfo, 32> buffers;
    boost::container::small_vector<vk::DescriptorImageInfo, 32> images;
    boost::container::small_vector<vk::DescriptorImageInfo, 16> samplers;
    boost::container::small_vector<u8, 32> stageable; ///< V4h: per buffer, read only in place
    boost::container::small_vector<std::pair<u64, u64>, 32> guest; ///< note 41, debug dumps: per buffer, the guest range it stands for (0: none)
};

/// A4 (note 48): a draw's record for the recording thread, taken from the job pool and filled in place by Record (before: locals moved into a lambda, the lambda
/// moved into a heap job, and a copy of the whole fixed state in it)
using Ranges = boost::container::small_vector<std::pair<u32, u32>, 6>;
/// R2b (note 64): a buffer the capture leaves to the recording thread: the engine object's fields copied at the draw (the object is never read again: it may be gone
/// when the job runs), or (object 0) a range by address in `offset`. Frame ring windows are never late (they are arithmetic on the capture's placement).
struct LateBuffer {
    enum What : u8 { Stage, Vertex, Index, Args };
    u64 object{}, id{}, memory{}, bytes{}, offset{}, size{};
    u16 slot{};      ///< Stage: index into Bound::buffers; Vertex: the binding
    u8 push_index{}; ///< Stage: its push data buffer offset
    What what{};
    bool written{};
};
using LateList = boost::container::small_vector<LateBuffer, 16>;
/// R2b part 2b (note 74, TLG_VKB_LATE_IMAGES=1, off by default): the capture keeps only the engine view ids of the draw's shader images and targets; our views,
/// attachments and the pass size are found by the resolver (stream order: deletions reach our tables in order, R2a; images keep one layout, part 2a)
struct LateImage {
    u64 view_id{};
    vk::ImageViewType type{};
    u16 slot{}; ///< index into Bound::images
    bool written{};
};
struct LateTarget {
    u64 view_id{};
    vk::Format format{};
    u8 cb{}; ///< colour target; 0xff: the depth target
};
using LateImages = boost::container::small_vector<LateImage, 16>;
using LateTargets = boost::container::small_vector<LateTarget, 9>;
bool ResolveOnRecorder();
bool LateImagesOn();
struct DrawJob final : Stream::Job {
    LateList late; ///< R2b: resolved first in Run, on the recording thread (R4a: in Prepare, on the resolver)
    LateImages late_images;   ///< R2b part 2b
    LateTargets late_targets; ///< R2b part 2b
    bool want_stencil{};      ///< R2b part 2b: the variant has a stencil format (the depth attachment's stencil is used)
    bool AnyLate() const {
        return !late.empty() || !late_images.empty() || !late_targets.empty();
    }
    bool ResolveLate();
    bool prepared{}, prepared_ok{};
    void Prepare() override {
        prepared = true;
        prepared_ok = !AnyLate() || ResolveLate();
    }
    std::vector<std::pair<u64, std::string>> dump_views;
    bool dump_buffers{};
    float rt_sun[3]{};
    bool rt_sun_set{};
    boost::container::small_vector<Stream::Use, 64> uses;
    boost::container::small_vector<Stream::Use, 9> attachment_uses;
    Stream::Pass pass{};
    Bound b;
    Ranges ranges;
    boost::container::small_vector<vk::DescriptorType, 32> image_types;
    vk::Pipeline pipeline{};
    vk::PipelineLayout layout{};
    bool push_descriptors{};
    vk::DescriptorSetLayout set_layout{};
    VkbShader::PushData push{};
    const VkbVulkan::FixedState* fixed_state{};
    std::array<vk::ColorComponentFlags, VkbAmdGpu::NUM_COLOR_BUFFERS> masks{};
    boost::container::small_vector<vk::VertexInputBindingDescription2EXT, 16> input_bindings;
    boost::container::small_vector<vk::VertexInputAttributeDescription2EXT, 16> input_attributes;
    boost::container::small_vector<vk::Buffer, 16> vertex_buffers;
    boost::container::small_vector<vk::DeviceSize, 16> vertex_offsets;
    boost::container::small_vector<u64, 16> vertex_sizes;
    vk::Buffer index_buffer{};
    u64 index_offset{};
    vk::IndexType index_type{vk::IndexType::eUint16};
    u64 index_size{};
    vk::Buffer args_buffer{};
    u64 args_offset{};
    u32 kind{};
    u64 a1{}, a2{}, a3{};
    u64 guest_key{}; ///< RT: the guest index address + counts (the same object in the sun view and the camera view)
    u32 instances{};
    u64 ps_hash{};
    bool ps_discard{}; ///< the pixel shader cuts pixels out (leaves, grass): solid in a ray trace, so left out of it
    u32 color_format0{};
    void Run(vk::CommandBuffer cmd) override;
    static void* operator new(size_t) {
        return Stream::JobBlocks<sizeof(DrawJob)>::Get();
    }
    static void operator delete(void* p) {
        Stream::JobBlocks<sizeof(DrawJob)>::Put(p);
    }
};

/// Binds one stage (stream recording held, outside a pass: images may be moved to the general layout). False: the draw cannot run (counted).
/// R5 TLG_VKB_DRAW_DUMP: one line into log/vkb_draw_dump.txt (its own file: the game log drops lines when it is busy)
void DrawDumpLine(const std::string& line) {
    static std::mutex m;
    std::scoped_lock lock{m};
    if (FILE* f = std::fopen((Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "vkb_draw_dump.txt").string().c_str(), "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
}

std::atomic<u64> c_cb_at_draw{0};

/// note 41: descriptor sets for the pipelines whose bindings exceed the push descriptor limit (32 on this GPU; the boy's G-buffer pipeline has 35). Pools are reused once
/// the stream's timeline passes the last submission that used them. Recording thread only.
std::atomic<u64> c_big_sets{0}, c_big_pools{0};
vk::DescriptorSet BigDescriptorSet(vk::DescriptorSetLayout set_layout) {
    struct Pool {
        vk::DescriptorPool pool;
        u64 last_value;
    };
    thread_local std::deque<Pool> used; // R4a: per recording thread
    thread_local vk::DescriptorPool current{};
    const auto device = instance->GetDevice();
    const auto fresh = [&]() -> vk::DescriptorPool {
        if (!used.empty() && Stream::CompletedValue() >= used.front().last_value) {
            const vk::DescriptorPool pool = used.front().pool;
            used.pop_front();
            (void)device.resetDescriptorPool(pool);
            return pool;
        }
        static constexpr std::array<vk::DescriptorPoolSize, 5> sizes{{{vk::DescriptorType::eStorageBuffer, 8192}, {vk::DescriptorType::eSampledImage, 8192},
                                                                       {vk::DescriptorType::eStorageImage, 2048}, {vk::DescriptorType::eSampler, 2048},
                                                                       {vk::DescriptorType::eUniformBuffer, 1024}}};
        const auto [result, pool] = device.createDescriptorPool({.maxSets = 256, .poolSizeCount = static_cast<u32>(sizes.size()), .pPoolSizes = sizes.data()});
        c_big_pools.fetch_add(1, std::memory_order_relaxed);
        return result == vk::Result::eSuccess ? pool : vk::DescriptorPool{};
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!current) {
            current = fresh();
            if (!current) {
                return {};
            }
        }
        vk::DescriptorSet set{};
        const vk::DescriptorSetAllocateInfo info{.descriptorPool = current, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
        if (device.allocateDescriptorSets(&info, &set) == vk::Result::eSuccess) {
            c_big_sets.fetch_add(1, std::memory_order_relaxed);
            return set;
        }
        used.push_back({current, Stream::RecordingValue()}); // full: its sets go out with this submission at the latest
        current = fresh();
    }
    return {};
}
std::atomic<u64> c_state_sent{0}, c_state_skipped{0}, c_state_pipelines{0}, c_state_vertex_inputs{0}; ///< note 60: the redundant-state filter
/// note 52 census (TLG_VKB_STAGE_CENSUS=1): does a stage resolve to exactly what it resolved at the previous draw (same shader, same buffer windows, views, samplers)?
bool StageCensusOn() { static const bool on = [] { const char* e = std::getenv("TLG_VKB_STAGE_CENSUS"); return e != nullptr && e[0] == '1'; }(); return on; }
std::atomic<u64> c_stage_same[2]{}, c_stage_changed[2]{}, c_stage_buffers_same[2]{}, c_stage_images_same[2]{}, c_stage_samplers_same[2]{};
thread_local u64 t_stage_hash[3]{}, t_stage_prev[2][3]{};
inline void StageMix(u32 part, u64 v) {
    t_stage_hash[part] = (t_stage_hash[part] ^ v) * 0x100000001b3ull;
}
bool BindStage(u64 ctx, Stage& st, vk::CommandBuffer cmd, VkbShader::PushData& push, u32& buffer_index, Bound& b, Ranges& ranges,
               boost::container::small_vector<Stream::Use, 64>& uses, LateList* late_list, LateImages* late_images = nullptr) {
    if (StageCensusOn()) {
        for (u32 k = 0; k < 3; ++k) {
            t_stage_hash[k] = 0xcbf29ce484222325ull ^ reinterpret_cast<u64>(st.info.get());
        }
    }
    const u64 alignment = instance->StorageMinAlignment();
    const auto& info = *st.info;
    const u32 first_buffer = static_cast<u32>(b.buffers.size());
    // R5 TLG_VKB_DRAW_DUMP=<pixel or vertex shader hash, hex>: its 100th, 400th and 1,000th bind write what it samples to log/vkb_draw_dump.txt
    static const u64 dump_hash = [] { const char* e = std::getenv("TLG_VKB_DRAW_DUMP"); return e != nullptr ? std::strtoull(e, nullptr, 16) : 0ull; }();
    static std::atomic<u32> dump_uses{0};
    const bool dumping = dump_hash != 0 && st.info != nullptr && (st.info->pgm_hash == dump_hash || (st.info->pgm_hash & 0xffffffffull) == dump_hash) &&
                         [] { const u32 n = dump_uses.fetch_add(1, std::memory_order_relaxed); return n == 100 || n == 400 || n == 1000; }();
    std::string dump_text;
    LateList* const late = dumping ? nullptr : late_list; // R2b: a dumped draw resolves here (its text needs the windows)
    LateImages* const late_img = dumping ? nullptr : late_images; // R2b part 2b
    for (u32 i = 0; i < st.buffers.size(); ++i) {
        const Source& src = st.buffers[i];
        const auto& desc = info.buffers[i];
        // R7 (note 56): a constant buffer the engine renamed outside the frame ring gets its bytes copied now, at the draw (a D3D11 driver's per-draw version)
        const auto copy_at_draw = [&](u64 address, u64 size) {
            const Stream::Slice slice = Stream::HostRing(size, alignment);
            std::memcpy(slice.data, reinterpret_cast<const void*>(address), size);
            push.buf_offsets[buffer_index] = 0;
            b.buffers.push_back({slice.buffer, slice.offset, size});
            b.guest.resize(b.buffers.size());
            b.guest.back() = {address, size};
            b.stageable.resize(b.buffers.size());
            b.stageable.back() = 0;
            if (StageCensusOn()) {
                StageMix(0, address), StageMix(0, size);
            }
            return true;
        };
        const auto bind_window = [&](u64 address, u64 size, const Core::Vkb::BufferWindow& w) {
            if (dumping) { // note 41
                dump_text += fmt::format("\n  buffer {} class {} kind {} slot {} type {}{}: guest {:#x} {} B, ours offset {} {}", i, static_cast<u32>(src.cls), src.kind, src.slot,
                                         static_cast<u32>(desc.buffer_type), desc.is_written ? " written" : "", address, size, w.offset, w.device ? "device" : "host");
                if (src.kind == 3 && size <= 1024) { // its bytes now (floats)
                    dump_text += "\n   ";
                    for (u64 k = 0; k + 4 <= size; k += 4) {
                        dump_text += fmt::format(" {:.4g}", *reinterpret_cast<const float*>(address + k));
                    }
                }
            }
            // note 41 (test, TLG_VKB_CB_AT_DRAW=1): a constant buffer read in place gets its bytes now, at the engine's draw call (D3D's meaning); the game writes some
            // of them (the scene slot matrices) straight into ring memory it reuses, so a copy made later, at the submission, can hold another draw's data
            static const bool cb_at_draw = [] { const char* e = std::getenv("TLG_VKB_CB_AT_DRAW"); return e != nullptr && e[0] == '1'; }();
            if (cb_at_draw && src.cls == Source::Class::Slot && src.kind == 3 && !w.device && !desc.is_written && size <= 65536) {
                const Stream::Slice slice = Stream::HostRing(size, alignment);
                std::memcpy(slice.data, reinterpret_cast<const void*>(address), size);
                push.buf_offsets[buffer_index] = 0;
                b.buffers.push_back({slice.buffer, slice.offset, size});
                b.guest.resize(b.buffers.size());
                b.guest.back() = {address, size};
                b.stageable.resize(b.buffers.size());
                b.stageable.back() = 0;
                c_cb_at_draw.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            (void)copy_at_draw;
            if (StageCensusOn()) {
                StageMix(0, address), StageMix(0, size), StageMix(0, reinterpret_cast<u64>(static_cast<VkBuffer>(w.buffer))), StageMix(0, w.offset);
            }
            const u64 aligned = w.offset & ~(alignment - 1);
            push.buf_offsets[buffer_index] = static_cast<u8>(w.offset - aligned);
            b.buffers.push_back({w.buffer, aligned, size + (w.offset - aligned)});
            b.guest.resize(b.buffers.size());
            b.guest.back() = {address, size};
            b.stageable.resize(b.buffers.size());
            b.stageable.back() = !w.device && !desc.is_written ? 1 : 0;
            uses.push_back(Stream::BufferUse(w.buffer, w.offset, size, desc.is_written));
            Core::Vkb::NoteBind(1, w, size, desc.is_written);
            return true;
        };
        // R2b (note 64): the window found later, by the recording thread; a placeholder binding now (filled in DrawJob::ResolveLate)
        const auto defer = [&](u64 object, u64 id, u64 memory, u64 bytes, u64 offset, u64 size) {
            late->push_back({.object = object, .id = id, .memory = memory, .bytes = bytes, .offset = offset, .size = size, .slot = static_cast<u16>(b.buffers.size()),
                             .push_index = static_cast<u8>(buffer_index), .what = LateBuffer::Stage, .written = desc.is_written});
            push.buf_offsets[buffer_index] = 0;
            b.buffers.push_back({});
            b.guest.resize(b.buffers.size());
            b.guest.back() = {object != 0 ? memory + offset : offset, size};
            b.stageable.resize(b.buffers.size());
            b.stageable.back() = 0;
            return true;
        };
        const auto bind_range = [&](u64 address, u64 size) {
            if (late != nullptr) {
                return defer(0, 0, 0, 0, address, size);
            }
            Core::Vkb::BufferWindow w;
            return Core::Vkb::FindBuffer(address, size, w) && bind_window(address, size, w);
        };
        bool ok = false;
        if (src.cls == Source::Class::Slot && src.kind != 4) {
            // R7 (note 56): by the engine object in the slot. A constant buffer's current data is where the engine renamed it (its buffer address word, the engine's own
            // pointer to the data; every constant buffer is renamed); a buffer view is its buffer from offset 0, the whole buffer (note 56 census: always so)
            const u64 object = SlotObject(ctx, st.index, src.kind, src.slot);
            if (object != 0 && src.kind == 3) {
                const u64 address = static_cast<u64>(Rd<u32>(object, 0x60)) | static_cast<u64>(Rd<u32>(object, 0x64) & 0xffu) << 32;
                const u64 size = Rd<u32>(object, 0x180);
                Core::Vkb::BufferWindow w;
                if (address != 0 && size != 0) {
                    if (Core::Vkb::ConstantInRing(address, size, w)) {
                        ok = bind_window(address, size, w);
                    } else if (!desc.is_written && size <= 65536 && !Core::Vkb::R7NoCopy()) {
                        Core::Vkb::NoteConstantCopied();
                        ok = copy_at_draw(address, size);
                    } else {
                        ok = bind_range(address, size);
                    }
                }
            } else if (object != 0 && src.kind == 1 && (Rd<u32>(object, 0x44) == 1 || Rd<u32>(object, 0x44) == 0xb)) {
                const u64 buffer = Rd<u64>(object, 0x30);
                const u64 size = buffer != 0 ? Rd<u32>(buffer, 0x180) : 0;
                Core::Vkb::BufferWindow w;
                if (late != nullptr && size != 0) { // R2b: the frame ring now (arithmetic), any other buffer by the recording thread
                    const u64 memory = Rd<u64>(buffer, 0x30);
                    ok = Core::Vkb::BufferInRing(memory, size, w) ? bind_window(memory, size, w) : defer(buffer, Rd<u64>(buffer, 0x10), memory, size, 0, size);
                } else {
                    ok = size != 0 && Core::Vkb::BufferOfObject(buffer, 0, size, w) && bind_window(Rd<u64>(buffer, 0x30), size, w);
                }
            }
        } else if (src.cls == Source::Class::Slot) {
            c_skip_uav.fetch_add(1, std::memory_order_relaxed); // an unordered access view in a graphics stage: not bound yet
            return false;
        } else if (src.cls == Source::Class::ProgramConstant) {
            ok = src.size != 0 && bind_range(src.address, src.size);
        } else if (src.cls == Source::Class::Special) {
            if (desc.buffer_type == VkbShader::BufferType::Flatbuf) {
                // RESIDUAL (as for compute): the flat user data is rebuilt from the engine objects per draw
                Core::NativeDraw::BuildUserLayout(ctx, st.index, st.shader, st.index == 0 ? 0x28 : 0x3c, false, st.live);
                st.info->user_data = std::span<const u32>{st.live.sgpr.data(), st.live.sgpr.size()};
                st.info->RefreshFlatBuf();
                const u64 bytes = std::max<u64>(st.info->flattened_ud_buf.size() * sizeof(u32), 16);
                const Stream::Slice slice = Stream::HostRing(bytes, alignment);
                std::memset(slice.data, 0, bytes);
                std::memcpy(slice.data, st.info->flattened_ud_buf.data(), st.info->flattened_ud_buf.size() * sizeof(u32));
                b.buffers.push_back({slice.buffer, slice.offset, bytes});
                c_flat_rebuilt.fetch_add(1, std::memory_order_relaxed);
                ok = true;
            } else if (desc.buffer_type == VkbShader::BufferType::ClipPlanes || desc.buffer_type == VkbShader::BufferType::GdsBuffer) {
                b.buffers.push_back({desc.buffer_type == VkbShader::BufferType::GdsBuffer ? Stream::GdsBuffer() : Stream::NullBuffer(), 0, VK_WHOLE_SIZE});
                ok = true;
            } else {
                c_skip_special.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        if (!ok) {
            c_skip_buffer.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++buffer_index;
    }
    const u32 first_image = static_cast<u32>(b.images.size());
    for (u32 i = 0; i < st.images.size(); ++i) {
        const Source& src = st.images[i];
        const u64 object = src.cls == Source::Class::Slot && src.kind == 1 ? SlotObject(ctx, st.index, 1, src.slot) : 0;
        if (late_img != nullptr) { // R2b part 2b: the view id now, our view by the resolver
            if (object == 0) {
                c_skip_image.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            late_img->push_back({.view_id = Rd<u64>(object, 0x10), .type = st.image_types[i], .slot = static_cast<u16>(b.images.size()), .written = info.images[i].is_written});
            b.images.push_back({{}, {}, vk::ImageLayout::eGeneral});
            continue;
        }
        vk::ImageView view{};
        vk::Image image{};
        if (object == 0 || !Core::Vkb::ViewForCompute(Rd<u64>(object, 0x10), st.image_types[i], cmd, view, &image)) {
            c_skip_image.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (StageCensusOn()) {
            StageMix(1, object), StageMix(1, reinterpret_cast<u64>(static_cast<VkImageView>(view)));
        }
        b.images.push_back({{}, view, vk::ImageLayout::eGeneral});
        uses.push_back(Stream::ImageUse(image, info.images[i].is_written));
        if (dumping) {
            dump_text += fmt::format("\n  image {} slot {} as {}{}: {}", i, src.slot, vk::to_string(st.image_types[i]), info.images[i].is_written ? " written" : "",
                                     Core::Vkb::DescribeView(Rd<u64>(object, 0x10)));
        }
    }
    const u32 first_sampler = static_cast<u32>(b.samplers.size());
    for (u32 i = 0; i < st.samplers.size(); ++i) {
        const Source& src = st.samplers[i];
        const u64 object = src.cls == Source::Class::Slot && src.kind == 2 ? SlotObject(ctx, st.index, 2, src.slot) : 0;
        const vk::Sampler sampler = st.constant_samplers[i] ? st.constant_samplers[i]->Handle() : object != 0 ? Core::Vkb::SamplerFor(object, i < info.samplers.size() && info.samplers[i].is_depth) : vk::Sampler{};
        if (!sampler) {
            c_skip_sampler.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (StageCensusOn()) {
            StageMix(2, reinterpret_cast<u64>(static_cast<VkSampler>(sampler)));
        }
        b.samplers.push_back({sampler, {}, vk::ImageLayout::eUndefined});
        if (dumping) {
            dump_text += fmt::format("\n  sampler {} {}", i, st.constant_samplers[i] ? std::string("constant (from the shader)") : Core::Vkb::DescribeSampler(object));
        }
    }
    if (dumping) {
        DrawDumpLine(fmt::format("bind: stage {} hash {:#x}, {} images, {} samplers:{}", st.index, st.info->pgm_hash, st.images.size(), st.samplers.size(), dump_text));
    }
    ranges.push_back({first_buffer, static_cast<u32>(b.buffers.size()) - first_buffer});
    ranges.push_back({first_image, static_cast<u32>(b.images.size()) - first_image});
    ranges.push_back({first_sampler, static_cast<u32>(b.samplers.size()) - first_sampler});
    if (StageCensusOn() && st.index < 2) {
        const u32 s = st.index;
        bool all = true;
        std::atomic<u64>* same[3] = {c_stage_buffers_same, c_stage_images_same, c_stage_samplers_same};
        for (u32 k = 0; k < 3; ++k) {
            const bool eq = t_stage_hash[k] == t_stage_prev[s][k];
            same[k][s].fetch_add(eq ? 1 : 0, std::memory_order_relaxed);
            all = all && eq;
            t_stage_prev[s][k] = t_stage_hash[k];
        }
        (all ? c_stage_same : c_stage_changed)[s].fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

/// Binds and records one draw of a compiled variant into the frame stream (mutex held)
/// note 45 (R2 design): Record's phases on the engine thread (0 views + attachments, 1 stage bindings, 2 vertex + index, 3 the rest + the job hand-over) and the job on the recorder
std::atomic<u64> c_phase_ns[4]{}, c_job_ns{0}, c_job_n{0}, c_fixed_ns{0}, c_variant_find_ns{0};
bool TimesOn() { static const bool on = [] { const char* e = std::getenv("TLG_VKB_TIMES"); return e != nullptr && e[0] == '1'; }(); return on; } ///< note 45: these timers only with TLG_VKB_TIMES=1
void Record(u64 ctx, Variant& v, const VkbVulkan::FixedState& fixed, u32 kind, u64 a1, u64 a2, u64 a3) {
    if (Core::Vkb::FillReadersOn()) {
        Core::Vkb::CensusTag(v.has_ps && v.ps.info ? v.ps.info->pgm_hash : 0x10000000000ull); // note 49 census: a draw, its pixel shader
    }
    // note 41, test only: TLG_VKB_TEST_SKIP_DRAW_PS=hex,hex,... skips the draws of those pixel shaders (visual bisect; the picture is wrong)
    static const std::vector<u64> skip_ps = [] {
        std::vector<u64> out;
        if (const char* e = std::getenv("TLG_VKB_TEST_SKIP_DRAW_PS")) {
            for (const char* p = e; *p != 0;) {
                char* end = nullptr;
                const u64 h = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                out.push_back(h);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return out;
    }();
    if (!skip_ps.empty() && v.has_ps && v.ps.info && std::find(skip_ps.begin(), skip_ps.end(), v.ps.info->pgm_hash) != skip_ps.end()) {
        return;
    }
    const u64 t0 = TimesOn() ? NowNs() : 0; // note 60: no clock reads per draw with the timers off
    // views the engine made through a path its view-creation hooks do not see are noted at their first use (their description is kept at + 0x40)
    // R2b part 2b: a view id seen with a format is never checked again on this thread (ids are never reused); the check itself stays here (it reads the engine view)
    thread_local std::array<u64, 1024> known_views{};
    const auto view_known = [](u64 id) {
        if (!LateImagesOn()) {
            return false;
        }
        u64& slot = known_views[(id * 0x9e3779b97f4a7c15ull) >> 54];
        if (slot == id) {
            return true;
        }
        if (Core::Vkb::ViewFormat(id) == vk::Format::eUndefined) {
            return false;
        }
        slot = id;
        return true;
    };
    {
        const auto targets = TargetViews(ctx);
        for (u32 cb = 0; cb < v.key.num_color_attachments; ++cb) {
            if (targets[cb] != 0 && !view_known(Rd<u64>(targets[cb], 0x10)) && Core::Vkb::ViewFormat(Rd<u64>(targets[cb], 0x10)) == vk::Format::eUndefined) {
                Core::NativeDevice::NoteView(Core::NativeDevice::Kind::RenderTargetView, targets[cb], targets[cb] + 0x40);
                c_late_views.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView); dsv != 0 && !view_known(Rd<u64>(dsv, 0x10)) && Core::Vkb::ViewFormat(Rd<u64>(dsv, 0x10)) == vk::Format::eUndefined) {
            Core::NativeDevice::NoteView(Core::NativeDevice::Kind::DepthStencilView, dsv, dsv + 0x40);
            c_late_views.fetch_add(1, std::memory_order_relaxed);
        }
    }
    u64 ph = TimesOn() ? NowNs() : 0;
    const auto phase = [&](u32 i) { if (!TimesOn()) { return; } const u64 n = NowNs(); c_phase_ns[i].fetch_add(n - ph, std::memory_order_relaxed); ph = n; };
    const vk::CommandBuffer cmd{}; // V4g: resolved here; its commands are recorded by the recording thread (the job below)
    // the attachments, in the pipeline's formats (their images move to the general layout here, before the pass)
    std::unique_ptr<DrawJob> job{new DrawJob}; // A4 (note 48): the draw's record, filled in place
    Stream::Pass& pass = job->pass;
    auto& attachment_uses = job->attachment_uses; // V4d: written by the pass
    auto& uses = job->uses;                       // V4d: read by this draw
    pass.color_count = v.key.num_color_attachments;
    u32 width = ~0u, height = ~0u, layers = ~0u;
    const auto views = TargetViews(ctx);
    const bool late_images = LateImagesOn(); // R2b part 2b: the targets by view id, resolved later
    if (late_images) {
        for (u32 cb = 0; cb < pass.color_count; ++cb) {
            if ((v.key.mrt_mask & (1u << cb)) == 0 || v.color_formats[cb] == vk::Format::eUndefined || views[cb] == 0) {
                continue;
            }
            job->late_targets.push_back({.view_id = Rd<u64>(views[cb], 0x10), .format = v.color_formats[cb], .cb = static_cast<u8>(cb)});
        }
        if (v.key.z_format != Core::NativeTarget::DepthTarget::ZFormat::Invalid) {
            const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView);
            if (dsv == 0) {
                c_skip_depth.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            job->late_targets.push_back({.view_id = Rd<u64>(dsv, 0x10), .format = vk::Format::eUndefined, .cb = 0xff});
            job->want_stencil = v.key.stencil_format != Core::NativeTarget::DepthTarget::StencilFormat::Invalid;
        }
        if (job->late_targets.empty()) {
            c_skip_no_attachment.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        width = height = layers = 0; // set by the resolver
    }
    for (u32 cb = 0; !late_images && cb < pass.color_count; ++cb) {
        if ((v.key.mrt_mask & (1u << cb)) == 0 || v.color_formats[cb] == vk::Format::eUndefined || views[cb] == 0) {
            continue;
        }
        Core::Vkb::Attachment a;
        if (!Core::Vkb::AttachmentFor(Rd<u64>(views[cb], 0x10), v.color_formats[cb], cmd, a)) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
                const u64 view = views[cb];
                const u64 resource = Rd<u64>(view, 0x30);
                LOG_WARNING(Render_Vulkan, "vkb draw: no attachment for target slot {}: view id {} tag {} kind {:#x} desc {:#x} {:#x}; resource {:#x} id {} kind {} {}x{} format {} bind {:#x} memory {:#x}",
                            cb, Rd<u64>(view, 0x10), Rd<u32>(view, 0x18), Rd<u32>(view, 0x44), Rd<u32>(view, 0x40), Rd<u32>(view, 0x48), resource,
                            resource ? Rd<u64>(resource, 0x10) : 0, resource ? Rd<u32>(resource, 0x124) : 0, resource ? Rd<u32>(resource, 0x180) : 0,
                            resource ? Rd<u32>(resource, 0x184) : 0, resource ? Rd<u32>(resource, 0x190) : 0, resource ? Rd<u32>(resource, 0x1a0) : 0,
                            resource ? Rd<u64>(resource, 0x30) : 0);
            }
            c_skip_target.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pass.colors[cb] = a.view;
        attachment_uses.push_back(Stream::ImageUse(a.image, true));
        width = std::min(width, a.extent.width), height = std::min(height, a.extent.height), layers = std::min(layers, a.layers);
    }
    if (!late_images && v.key.z_format != Core::NativeTarget::DepthTarget::ZFormat::Invalid) {
        const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView);
        Core::Vkb::Attachment a;
        if (dsv == 0 || !Core::Vkb::AttachmentFor(Rd<u64>(dsv, 0x10), vk::Format::eUndefined, cmd, a)) {
            c_skip_depth.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pass.depth = a.view;
        pass.depth_image = a.image, pass.depth_format = Core::Vkb::ViewFormat(Rd<u64>(dsv, 0x10));
        attachment_uses.push_back(Stream::ImageUse(a.image, true));
        pass.depth_has_stencil = a.has_stencil && v.key.stencil_format != Core::NativeTarget::DepthTarget::StencilFormat::Invalid;
        width = std::min(width, a.extent.width), height = std::min(height, a.extent.height), layers = std::min(layers, a.layers);
    }
    if (width == ~0u) {
        c_skip_no_attachment.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    pass.extent = {width, height};
    pass.layers = layers;
    phase(0);
    // the resources of both stages, pixel first (the pipeline layout's order)
    VkbShader::PushData& push = job->push;
    push.xoffset = fixed.transform.x_offset;
    push.xscale = fixed.transform.x_scale;
    push.yoffset = fixed.transform.y_offset;
    push.yscale = fixed.transform.y_scale;
    Bound& b = job->b;
    Ranges& ranges = job->ranges;
    u32 buffer_index = 0;
    LateList* const late = ResolveOnRecorder() ? &job->late : nullptr; // R2b (note 64)
    LateImages* const late_imgs = late_images ? &job->late_images : nullptr; // R2b part 2b
    if (v.has_ps && !BindStage(ctx, v.ps, cmd, push, buffer_index, b, ranges, uses, late, late_imgs)) {
        return;
    }
    if (!BindStage(ctx, v.vs, cmd, push, buffer_index, b, ranges, uses, late, late_imgs)) {
        return;
    }
    phase(1);
    // the vertex inputs: each fetch attribute from its element of the input layout
    auto& input_bindings = job->input_bindings;
    auto& input_attributes = job->input_attributes;
    auto& vertex_buffers = job->vertex_buffers;
    auto& vertex_offsets = job->vertex_offsets;
    auto& vertex_sizes = job->vertex_sizes; // V4h: in place (staged), 0 on a device copy
    using InstanceIdType = VkbShader::Gcn::VertexAttribute::InstanceIdType;
    for (u32 i = 0; i < v.attributes.size(); ++i) {
        const auto& attribute = v.fetch.attributes[i];
        Core::NativeDraw::VertexElement e;
        Core::Vkb::BufferWindow w;
        bool vertex_late = false;
        if (late != nullptr && v.attributes[i].cls == Source::Class::Slot && Core::NativeDraw::VertexElementOf(ctx, v.attributes[i].slot, e) && e.bytes > attribute.inst_offset) {
            // R2b: the frame ring now, any other buffer by the recording thread
            const u64 size = e.bytes - attribute.inst_offset;
            if (e.buffer != 0) {
                const u64 memory = Rd<u64>(e.buffer, 0x30), offset = e.start + attribute.inst_offset;
                if (!Core::Vkb::BufferInRing(memory + offset, size, w)) {
                    late->push_back({.object = e.buffer, .id = Rd<u64>(e.buffer, 0x10), .memory = memory, .bytes = Rd<u32>(e.buffer, 0x180), .offset = offset, .size = size,
                                     .slot = static_cast<u16>(i), .what = LateBuffer::Vertex});
                    vertex_late = true;
                }
            } else {
                late->push_back({.offset = e.address + attribute.inst_offset, .size = size, .slot = static_cast<u16>(i), .what = LateBuffer::Vertex});
                vertex_late = true;
            }
        } else if (v.attributes[i].cls != Source::Class::Slot || !Core::NativeDraw::VertexElementOf(ctx, v.attributes[i].slot, e) || e.bytes <= attribute.inst_offset ||
            !Core::Vkb::BufferOfObject(e.buffer, e.start + attribute.inst_offset, e.bytes - attribute.inst_offset, w)) { // R7: by the slot's buffer object
            c_skip_vertex.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const u32 data_format = attribute.data_format != 0 ? attribute.data_format : e.data_format;
        const u32 num_format = attribute.data_format != 0 ? attribute.num_format : e.num_format;
        const auto step = attribute.GetStepRate();
        const u32 divisor = step == InstanceIdType::OverStepRate0 ? v.vs.runtime.sw.vs.step_rate_0 : step == InstanceIdType::OverStepRate1 ? v.vs.runtime.sw.vs.step_rate_1 : 1;
        input_bindings.push_back({.binding = i, .stride = e.stride, .inputRate = step == InstanceIdType::None ? vk::VertexInputRate::eVertex : vk::VertexInputRate::eInstance,
                                  .divisor = divisor});
        input_attributes.push_back({.location = i, .binding = i,
                                    .format = VkbVulkan::Vkb::LiverpoolToVK::SurfaceFormat(static_cast<VkbAmdGpu::DataFormat>(data_format), static_cast<VkbAmdGpu::NumberFormat>(num_format)),
                                    .offset = 0});
        if (vertex_late) { // R2b: filled by DrawJob::ResolveLate
            vertex_buffers.push_back({});
            vertex_sizes.push_back(0);
            vertex_offsets.push_back(0);
            continue;
        }
        vertex_buffers.push_back(w.buffer);
        vertex_sizes.push_back(w.device ? 0 : e.bytes - attribute.inst_offset);
        Core::Vkb::NoteBind(2, w, e.bytes - attribute.inst_offset, false);
        vertex_offsets.push_back(w.offset);
        uses.push_back(Stream::BufferUse(w.buffer, w.offset, e.bytes - attribute.inst_offset, false));
    }
    // the index buffer
    vk::Buffer& index_buffer = job->index_buffer;
    u64& index_offset = job->index_offset;
    vk::IndexType& index_type = job->index_type;
    u64& index_size = job->index_size; // V4h: in place (staged), 0 on a device copy
    vk::Buffer& args_buffer = job->args_buffer;
    u64& args_offset = job->args_offset;
    if (kind == 0 || kind == 2) {
        const u64 view = Rd<u64>(ctx, 0x58);
        const u32 type = Rd<u32>(ctx, 0x64);
        const u64 address = view != 0 ? (static_cast<u64>(Rd<u32>(view, 0x64) & 0xff) << 32 | (Rd<u32>(view, 0x60) & 0xfffffffeu)) : 0;
        index_type = type == 1 ? vk::IndexType::eUint32 : vk::IndexType::eUint16;
        job->guest_key = address * 0x9E3779B97F4A7C15ull ^ (a1 << 40) ^ (a2 << 20) ^ a3 ^ (static_cast<u64>(kind) << 62);
        const u64 index_bytes = type == 1 ? 4 : 2;
        // an indirect draw's index count is in its arguments: the whole index range of the view (its max index size, as the console's VGT_INDX_MAX_SIZE)
        const u64 index_range = kind == 0 ? (a2 + a1) * index_bytes : static_cast<u64>(view != 0 ? Rd<u32>(view, 0x68) : 0) * index_bytes;
        Core::Vkb::BufferWindow w;
        bool index_late = false;
        if (late != nullptr && view != 0 && address != 0 && index_range != 0 && Rd<u32>(ctx, 0x60) == 0x39 && type <= 1) {
            // R2b: the frame ring now, any other buffer by the recording thread
            const u64 memory = Rd<u64>(view, 0x30);
            if (memory == 0 || !Core::Vkb::BufferInRing(memory, index_range, w)) {
                if (memory != 0) {
                    late->push_back({.object = view, .id = Rd<u64>(view, 0x10), .memory = memory, .bytes = Rd<u32>(view, 0x180), .offset = 0, .size = index_range, .what = LateBuffer::Index});
                } else {
                    late->push_back({.offset = address, .size = index_range, .what = LateBuffer::Index});
                }
                index_late = true;
            } else if ((w.offset % index_bytes) != 0) {
                c_skip_index.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (!index_late) {
        // R7 (note 56): the index buffer object's own memory from its start (census: always so)
        const bool found = late != nullptr ? static_cast<bool>(w.buffer) : view != 0 && Core::Vkb::BufferOfObject(view, 0, index_range, w);
        if (address == 0 || index_range == 0 || Rd<u32>(ctx, 0x60) != 0x39 || type > 1 || !found || (w.offset % index_bytes) != 0) {
            c_skip_index.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (Core::Vkb::R7CensusOn()) {
            Core::Vkb::R7Census(4, view, address, index_range); // note 56
        }
        index_buffer = w.buffer, index_offset = w.offset;
        index_size = w.device ? 0 : index_range;
        Core::Vkb::NoteBind(3, w, index_range, false);
        uses.push_back(Stream::BufferUse(w.buffer, w.offset, index_range, false));
        }
    }
    if (kind == 2) {
        // V5a: DrawIndexedInstancedIndirect(args buffer a1, offset a2): the arguments (index count, instance count, first index, base vertex, first instance: the layout of
        // VkDrawIndexedIndirectCommand) at the buffer's memory plus the offset (the engine's SET_BASE + data offset)
        const u64 packed = a1 != 0 && IsHeapPointer(a1) ? Rd<u64>(a1, 0x30) : 0;
        const u64 args = ((packed & 0xfffffff8ull) | ((packed >> 32) & 0xff) << 32) + static_cast<u32>(a2);
        Core::Vkb::BufferWindow w;
        if (late != nullptr && packed != 0) { // R2b: by the recording thread
            late->push_back({.object = a1, .id = Rd<u64>(a1, 0x10), .memory = Rd<u64>(a1, 0x30), .bytes = Rd<u32>(a1, 0x180), .offset = static_cast<u32>(a2), .size = 20,
                             .what = LateBuffer::Args});
        } else if (packed == 0 || !Core::Vkb::BufferOfObject(a1, static_cast<u32>(a2), 20, w) || (w.offset & 3) != 0) {
            c_skip_indirect.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (late == nullptr) { // (R2b: else filled by DrawJob::ResolveLate)
            args_buffer = w.buffer, args_offset = w.offset;
            uses.push_back(Stream::BufferUse(w.buffer, w.offset, 20, false));
        }
        c_indirect_recorded.fetch_add(1, std::memory_order_relaxed);
    }
    phase(2);
    // the pass, then the pipeline and its state
    // V4g: the image types of the descriptor writes (in the order the job writes them), then the commands, on the recording thread
    auto& image_types = job->image_types;
    image_types.resize(b.images.size());
    for (u32 r = 0; r < ranges.size(); r += 3) {
        const Stage& st = (v.has_ps && r == 0) ? v.ps : v.vs;
        for (u32 k = 0; k < ranges[r + 1].second; ++k) {
            image_types[ranges[r + 1].first + k] = st.info->images[k].is_written ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        }
    }
    const u32 instances = std::max(Rd<u32>(ctx, 0x4724), 1u);
    job->instances = instances;
    {
        static const u64 dump_hash = [] { const char* e = std::getenv("TLG_VKB_DRAW_DUMP"); return e != nullptr ? std::strtoull(e, nullptr, 16) : 0ull; }();
        static std::atomic<u32> dump_draws{0};
        const u64 ps = v.has_ps && v.ps.info ? v.ps.info->pgm_hash : 0ull;
        if (dump_hash != 0 && (ps == dump_hash || (ps & 0xffffffffull) == dump_hash) && [] { const u32 n = dump_draws.fetch_add(1, std::memory_order_relaxed); return n == 100 || n == 400 || n == 1000; }()) {
            DrawDumpLine(fmt::format("draw: kind {} a1 {} a2 {} a3 {} instances {} (ctx+0x4724 {}); pass {}x{} layers {} colors {}", kind, a1, a2, a3, instances, Rd<u32>(ctx, 0x4724), pass.extent.width, pass.extent.height, pass.layers, pass.color_count));
        }
    }
    // note 41, debug only: TLG_VKB_DRAW_CENSUS=1: every draw of the graphics submissions from TLG_VKB_IMAGE_DUMP_AT (default 2600) on for about one frame (8 submissions),
    // one line each into log/vkb_draw_dump.txt: shaders, counts, pass, and what each pixel shader image slot holds
    {
        static const bool census = [] { const char* e = std::getenv("TLG_VKB_DRAW_CENSUS"); return e != nullptr && e[0] == '1'; }();
        static const u64 census_at = [] { const char* e = std::getenv("TLG_VKB_IMAGE_DUMP_AT"); return e != nullptr ? std::strtoull(e, nullptr, 10) : 2600ull; }();
        if (census && Core::Vkb::GraphicsSubmits() >= census_at && Core::Vkb::GraphicsSubmits() < census_at + 8) {
            std::string text = fmt::format("submit {} vs {:#x} ps {:#x} kind {} a1 {} a2 {} instances {} pass {}x{} colors {} depth {}", Core::Vkb::GraphicsSubmits(),
                                           v.vs.info ? v.vs.info->pgm_hash : 0ull, v.has_ps && v.ps.info ? v.ps.info->pgm_hash : 0ull, kind, a1, a2, instances, pass.extent.width, pass.extent.height, pass.color_count,
                                           pass.depth ? 1 : 0);
            text += fmt::format(" dtest {} dwrite {} dcmp {} cull {}", fixed.depth_test ? 1 : 0, fixed.depth_write ? 1 : 0, vk::to_string(fixed.depth_compare),
                                static_cast<u32>(VkCullModeFlags(fixed.cull_mode)));
            text += fmt::format(" stest {} sref {} cmpmask {:x} wmask {:x} sfunc {} pass {} fail {} zfail {} stencil_attached {}", fixed.stencil_test ? 1 : 0, fixed.stencil_reference[0],
                                fixed.stencil_compare_mask[0], fixed.stencil_write_mask[0], vk::to_string(fixed.stencil_front_ops.compare_op),
                                vk::to_string(fixed.stencil_front_ops.pass_op), vk::to_string(fixed.stencil_front_ops.fail_op), vk::to_string(fixed.stencil_front_ops.depth_fail_op),
                                pass.depth_has_stencil ? 1 : 0);
            text += " bufs";
            for (u32 i = 0; i < b.guest.size(); ++i) {
                text += fmt::format(" {:#x}/{}", b.guest[i].first, b.guest[i].second);
            }
            if (v.has_ps) {
                text += " expfmt";
                for (u32 cb = 0; cb < 8; ++cb) {
                    text += fmt::format(" {}:{}/{}", cb, static_cast<u32>(v.ps.runtime.hw.fs.color_buffers[cb].export_format),
                                        static_cast<u32>(v.ps.runtime.hw.fs.color_buffers[cb].data_format));
                }
            }
            text += fmt::format(" vp {},{} {}x{} z {}..{} sc {},{} {}x{} clipdis {} fs {}", fixed.viewports[0].x, fixed.viewports[0].y, fixed.viewports[0].width,
                                fixed.viewports[0].height, fixed.viewports[0].minDepth, fixed.viewports[0].maxDepth, fixed.scissors[0].offset.x, fixed.scissors[0].offset.y,
                                fixed.scissors[0].extent.width, fixed.scissors[0].extent.height, fixed.clip_disabled ? 1 : 0, static_cast<u32>(fixed.front_face));
            if (const u64 raster = Rd<u64>(ctx, 0x46c8); raster != 0) {
                text += fmt::format(" bias {} clamp {} slope {}", Rd<s32>(raster, 0x3c), Rd<float>(raster, 0x40), Rd<float>(raster, 0x44));
            }
            if (const u64 blend = Rd<u64>(ctx, 0x46f0); blend != 0) { // api map: D3D11_BLEND_DESC AlphaToCoverageEnable (not implemented), blend enable of target 0
                text += fmt::format(" a2c {} blend0 {}", Rd<u8>(blend, 0x30), Rd<u8>(blend, 0x34));
            }
            {
                const auto targets = TargetViews(ctx);
                for (u32 cb = 0; cb < v.key.num_color_attachments; ++cb) {
                    text += fmt::format(" m{}={:x}/{:x}{}", cb, static_cast<u32>(VkColorComponentFlags(v.key.write_masks[cb])), fixed.target_write_mask[cb],
                                        v.key.blend[cb].enable ? "b" : "");
                    text += fmt::format(" o{}={} ({})", cb, targets[cb] != 0 ? Rd<u64>(targets[cb], 0x10) : 0ull, targets[cb] != 0 ? Core::Vkb::DescribeView(Rd<u64>(targets[cb], 0x10)).substr(0, 12) : std::string());
                }
            }
            if (v.has_ps) {
                for (u32 i = 0; i < v.ps.images.size(); ++i) {
                    const Source& src = v.ps.images[i];
                    const u64 object = src.cls == Source::Class::Slot && src.kind == 1 ? SlotObject(ctx, v.ps.index, 1, src.slot) : 0;
                    text += fmt::format(" | t{} {}", src.slot, object != 0 ? fmt::format("view {} {}", Rd<u64>(object, 0x10), Core::Vkb::DescribeView(Rd<u64>(object, 0x10))) : std::string("none"));
                }
            }
            DrawDumpLine(text);
        }
    }
    // note 41, debug only: TLG_VKB_IMAGE_DUMP_PS=<pixel shader hash> (TLG_VKB_IMAGE_DUMP_AT=<graphics submit>, default 2600): the first such draw from then on dumps its
    // images (inputs in0.., the color targets out0.., the depth target depth) right after the draw, once
    auto& dump_views = job->dump_views;
    bool& dump_buffers = job->dump_buffers;
    {
        static const u64 dump_ps = [] { const char* e = std::getenv("TLG_VKB_IMAGE_DUMP_PS"); return e != nullptr ? std::strtoull(e, nullptr, 16) : 0ull; }();
        static const u64 dump_at = [] { const char* e = std::getenv("TLG_VKB_IMAGE_DUMP_AT"); return e != nullptr ? std::strtoull(e, nullptr, 10) : 2600ull; }();
        static const u64 dump_nth = [] { const char* e = std::getenv("TLG_VKB_IMAGE_DUMP_NTH"); return e != nullptr ? std::strtoull(e, nullptr, 10) : 0ull; }();
        static std::atomic<u64> seen{0};
        if (dump_ps != 0 && v.has_ps && v.ps.info && v.ps.info->pgm_hash == dump_ps && Core::Vkb::GraphicsSubmits() >= dump_at && seen.fetch_add(1) == dump_nth) {
            for (const Stage* st : {&v.vs, &v.ps}) {
                for (u32 i = 0; i < st->images.size(); ++i) {
                    const Source& src = st->images[i];
                    const u64 object = src.cls == Source::Class::Slot && src.kind == 1 ? SlotObject(ctx, st->index, 1, src.slot) : 0;
                    if (object != 0) {
                        dump_views.push_back({Rd<u64>(object, 0x10), fmt::format("{}_in{}_slot{}", st == &v.ps ? "ps" : "vs", i, src.slot)});
                    }
                }
            }
            const auto targets = TargetViews(ctx);
            for (u32 cb = 0; cb < v.key.num_color_attachments; ++cb) {
                if (targets[cb] != 0) {
                    dump_views.push_back({Rd<u64>(targets[cb], 0x10), fmt::format("out{}", cb)});
                }
            }
            if (const u64 dsv = Rd<u64>(ctx, CtxDepthStencilView); dsv != 0) {
                dump_views.push_back({Rd<u64>(dsv, 0x10), "depth"});
            }
            // and the buffers: the guest bytes now (at the draw call), the GPU's copy after the draw
            const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "vkb_image_dump";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            b.guest.resize(b.buffers.size());
            for (u32 i = 0; i < b.guest.size(); ++i) {
                const auto [address, size] = b.guest[i];
                if (address != 0 && size != 0 && size <= (64u << 20)) {
                    if (FILE* f = std::fopen((dir / fmt::format("g{}_{:#x}_{}B.raw", i, address, size)).string().c_str(), "wb")) {
                        std::fwrite(reinterpret_cast<const void*>(address), 1, size, f);
                        std::fclose(f);
                    }
                }
            }
            dump_buffers = true;
            LOG_INFO(Render_Vulkan, "vkb image dump: draw of ps {:#x} at graphics submit {}: {} images", dump_ps, Core::Vkb::GraphicsSubmits(), dump_views.size());
        }
    }
    Core::Vkb::Census::Work(0);
    // the glitch marker's and the frame walk's draw table (draws.json): one row per draw, as the PS4 path wrote (F6 / F5; TLG_MARKER=0 off)
    if (Core::Marker::Enabled()) {
        Core::Marker::NoteRow({.vs_hash = v.vs.info ? v.vs.info->pgm_hash : 0ull,
                               .ps_hash = v.has_ps && v.ps.info ? v.ps.info->pgm_hash : 0ull,
                               .color0 = Rd<u64>(ctx, 0x4488), // engine render target view of slot 0 (Ctx::RenderTargets)
                               .depth = Rd<u64>(ctx, 0x44d0),  // engine depth stencil view (Ctx::DepthStencilView)
                               .frame = static_cast<u32>(Core::DebugServer::PresentCount()),
                               .count = static_cast<u32>(a1),
                               .instances = instances,
                               .kind = static_cast<u8>(kind == 2 ? 1 : 0),
                               .draw_class = 15,
                               .flags = 2,
                               .colors = static_cast<u8>(pass.color_count),
                               .width = static_cast<u16>(pass.extent.width),
                               .height = static_cast<u16>(pass.extent.height)});
    }
    job->pipeline = v.pipeline->Handle();
    job->layout = v.pipeline->GetLayout();
    job->push_descriptors = v.pipeline->UsesPushDescriptors();
    job->set_layout = v.pipeline->GetDescriptorSetLayout();
    job->fixed_state = &fixed; // interned by FixedStateOf, never freed
    job->masks = v.key.write_masks;
    job->kind = kind, job->a1 = a1, job->a2 = a2, job->a3 = a3;
    job->ps_hash = v.has_ps && v.ps.info ? v.ps.info->pgm_hash : 0ull;
    job->ps_discard = v.has_ps && v.ps.info && v.ps.info->has_discard;
    job->color_format0 = static_cast<u32>(v.color_formats[0]);
    // RT sun shadow: the game's sun shadow step carries the camera matrix and the shadow matrices; the sun's direction comes out of them
    if (job->ps_hash == 0x85df4ad2ull) {
        const float* vm = nullptr;
        const float* cm = nullptr;
        for (const auto& [address, size] : b.guest) {
            if (address != 0 && size == 688) {
                vm = reinterpret_cast<const float*>(address + 80);
            } else if (address != 0 && size == 848) {
                cm = reinterpret_cast<const float*>(address);
            }
        }
        static u32 said = 0;
        if (vm && cm) {
            float sw[3]{cm[4] * cm[9] - cm[8] * cm[5], cm[8] * cm[1] - cm[0] * cm[9], cm[0] * cm[5] - cm[4] * cm[1]};
            const float len = std::sqrt(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
            const float sg = (sw[1] < 0.0f ? -1.0f : 1.0f) / std::max(len, 1e-20f);
            for (float& c : sw) {
                c *= sg;
            }
            job->rt_sun[0] = sw[0] * vm[0] + sw[1] * vm[4] + sw[2] * vm[8];
            job->rt_sun[1] = sw[0] * vm[1] + sw[1] * vm[5] + sw[2] * vm[9];
            job->rt_sun[2] = -(sw[0] * vm[2] + sw[1] * vm[6] + sw[2] * vm[10]);
            job->rt_sun_set = true;
            if (said++ % 600 == 0) {
                LOG_ERROR(Render_Vulkan, "RT sun: world {:.3f} {:.3f} {:.3f}, view {:.3f} {:.3f} {:.3f}", sw[0], sw[1], sw[2], job->rt_sun[0], job->rt_sun[1], job->rt_sun[2]);
            }
        } else if (said++ % 600 == 0) {
            LOG_ERROR(Render_Vulkan, "RT sun: the shadow step's buffers were not found ({} buffers)", b.guest.size());
        }
    }
    job->work = Stream::Work::Draw;
    Stream::Enqueue(std::move(job));
    phase(3);
    c_recorded.fetch_add(1, std::memory_order_relaxed);
    if (TimesOn()) {
        c_record_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    }
}

/// R2b (note 64): the buffers the capture left are found here, on the recording thread (stream recording held: the objects lock nests inside it, as FindLocked expects).
/// The census tools and TLG_VKB_CB_AT_DRAW resolve on the engine's render thread at the draw (they read engine objects or thread-local tags while they resolve).
std::atomic<u64> c_late_buffers{0}, c_late_failed{0}, c_late_targets{0}, c_late_images{0};
bool LateImagesOn() {
    static const bool on = [] {
        const char* e = std::getenv("TLG_VKB_LATE_IMAGES");
        const auto set = [](const char* name) { const char* v = std::getenv(name); return v != nullptr && v[0] != 0 && v[0] != '0'; };
        // the draw dump and census print the pass at the capture
        return e != nullptr && e[0] == '1' && ResolveOnRecorder() && !set("TLG_VKB_DRAW_DUMP") && !set("TLG_VKB_DRAW_CENSUS") &&
               !set("TLG_VKB_IMAGE_DUMP_PS");
    }();
    return on;
}
bool ResolveOnRecorder() {
    static const bool on = [] {
        const char* cb = std::getenv("TLG_VKB_CB_AT_DRAW");
        return (cb == nullptr || cb[0] != '1') && Stream::RecorderOn() && !StageCensusOn() && !Core::Vkb::R7CensusOn() &&
               !Core::Vkb::FillReadersOn();
    }();
    return on;
}
bool DrawJob::ResolveLate() {
    const u64 alignment = instance->StorageMinAlignment();
    c_late_buffers.fetch_add(late.size(), std::memory_order_relaxed);
    for (const LateBuffer& l : late) {
        Core::Vkb::BufferWindow w;
        const bool found = l.object != 0 ? Core::Vkb::BufferOfObjectFields(l.object, l.id, l.memory, l.bytes, l.offset, l.size, w) : Core::Vkb::FindBuffer(l.offset, l.size, w);
        switch (l.what) {
        case LateBuffer::Stage: {
            if (!found) {
                c_skip_buffer.fetch_add(1, std::memory_order_relaxed);
                c_late_failed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const u64 aligned = w.offset & ~(alignment - 1);
            push.buf_offsets[l.push_index] = static_cast<u8>(w.offset - aligned);
            b.buffers[l.slot] = {w.buffer, aligned, l.size + (w.offset - aligned)};
            b.stageable[l.slot] = !w.device && !l.written ? 1 : 0;
            uses.push_back(Stream::BufferUse(w.buffer, w.offset, l.size, l.written));
            Core::Vkb::NoteBind(1, w, l.size, l.written);
            break;
        }
        case LateBuffer::Vertex:
            if (!found) {
                c_skip_vertex.fetch_add(1, std::memory_order_relaxed);
                c_late_failed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            vertex_buffers[l.slot] = w.buffer;
            vertex_offsets[l.slot] = w.offset;
            vertex_sizes[l.slot] = w.device ? 0 : l.size;
            Core::Vkb::NoteBind(2, w, l.size, false);
            uses.push_back(Stream::BufferUse(w.buffer, w.offset, l.size, false));
            break;
        case LateBuffer::Index:
            if (!found || (w.offset % (index_type == vk::IndexType::eUint32 ? 4 : 2)) != 0) {
                c_skip_index.fetch_add(1, std::memory_order_relaxed);
                c_late_failed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            index_buffer = w.buffer, index_offset = w.offset;
            index_size = w.device ? 0 : l.size;
            Core::Vkb::NoteBind(3, w, l.size, false);
            uses.push_back(Stream::BufferUse(w.buffer, w.offset, l.size, false));
            break;
        case LateBuffer::Args:
            if (!found || (w.offset & 3) != 0) {
                c_skip_indirect.fetch_add(1, std::memory_order_relaxed);
                c_late_failed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            args_buffer = w.buffer, args_offset = w.offset;
            uses.push_back(Stream::BufferUse(w.buffer, w.offset, 20, false));
            break;
        }
    }
    // R2b part 2b: the targets (the pass size comes from them) and the shader images
    if (!late_targets.empty()) {
        c_late_targets.fetch_add(late_targets.size(), std::memory_order_relaxed);
        u32 width = ~0u, height = ~0u, layers = ~0u;
        for (const LateTarget& t : late_targets) {
            Core::Vkb::Attachment a;
            if (!Core::Vkb::AttachmentFor(t.view_id, t.format, vk::CommandBuffer{}, a)) {
                (t.cb == 0xff ? c_skip_depth : c_skip_target).fetch_add(1, std::memory_order_relaxed);
                c_late_failed.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (t.cb == 0xff) {
                pass.depth = a.view;
                pass.depth_image = a.image, pass.depth_format = Core::Vkb::ViewFormat(t.view_id);
                pass.depth_has_stencil = a.has_stencil && want_stencil;
            } else {
                pass.colors[t.cb] = a.view;
            }
            attachment_uses.push_back(Stream::ImageUse(a.image, true));
            width = std::min(width, a.extent.width), height = std::min(height, a.extent.height), layers = std::min(layers, a.layers);
        }
        pass.extent = {width, height};
        pass.layers = layers;
    }
    c_late_images.fetch_add(late_images.size(), std::memory_order_relaxed);
    for (const LateImage& li : late_images) {
        vk::ImageView view{};
        vk::Image image{};
        if (!Core::Vkb::ViewForCompute(li.view_id, li.type, vk::CommandBuffer{}, view, &image)) {
            c_skip_image.fetch_add(1, std::memory_order_relaxed);
            c_late_failed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        b.images[li.slot].imageView = view;
        uses.push_back(Stream::ImageUse(image, li.written));
    }
    return true;
}

// RT prototype (TLG_VKB_RT=1): main-view vertex positions (clip space) of every draw are captured with transform feedback into one buffer per frame
namespace RtCapture {
} // namespace RtCapture
bool RtNoWriteToo() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_RT_ALLDEPTH"); return e == nullptr || e[0] != '0'; }();
    return on;
}
void RtBuildHook(vk::CommandBuffer cmd);
namespace RtCapture {
bool On() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_RT"); return e != nullptr && e[0] == '1' && instance != nullptr && instance->IsRtSupported(); }();
    return on;
}
constexpr u64 kXfbBytes = 256ull << 20; ///< 16M vertices (vec4)
constexpr u32 kSlots = 8;
constexpr u32 kDraws = 256; ///< per-draw slots of the capture buffer
constexpr u64 kSlotBytes = kXfbBytes / kDraws;
std::atomic<u32> slot_next{0};
VkBuffer xfb{}, counter{}, readback{};
VmaAllocation xfb_a{}, counter_a{}, readback_a{};
const u32* readback_ptr = nullptr;
VkBuffer sample{};
VmaAllocation sample_a{};
const float* sample_ptr = nullptr;
constexpr u32 kSampleSlots = 256, kSampleVerts = 16;
constexpr u32 kShadowDraws = 2048; ///< sun-view (shadow map) draws sampled per frame, 16 vertices each
u64 main_keys[8][256]{};
u64 slot_keys[256]{}; ///< this frame's draw keys per capture slot, snapshot with the sample copy
u64 shadow_keys[8][kShadowDraws]{};
struct SunInfo {
    u64 view;
    float vp[4];
    u32 indices, inst, kind, prim;
    bool discard;
};
SunInfo shadow_info[kShadowDraws]{}; ///< diagnosis: what each sampled sun-view draw was
double shadow_fit[kShadowDraws][12]{};
bool shadow_fit_ok[kShadowDraws]{};
std::atomic<u32> shadow_next{0};
std::atomic<u64> shadow_frame{~0ull};
VkBuffer shadow_xfb{};
VmaAllocation shadow_xfb_a{};
const float* shadow_ptr = nullptr;
constexpr u32 kMainDraws = 2048; ///< camera draws beyond the capture slots, 16 vertices each, written straight to host memory (same frame index as the sun view)
u64 cam_keys[8][kMainDraws]{};
std::atomic<u32> cam_next{0};
std::atomic<u64> cam_frame{~0ull};
VkBuffer cam_xfb{};
VmaAllocation cam_xfb_a{};
const float* cam_ptr = nullptr;
// v2: the geometry of the far sun view (the game's own shadow pass) is the ray-trace scene, captured whole into xfb each frame; objects the camera draws too
// give vertex pairs for the camera -> sun view map of the same frame
constexpr u32 kSunVerts = 12u << 20; ///< 201 MB of the capture buffer
constexpr u32 kCutBase = 8u << 20;   ///< from this vertex on: the cut-out draws (leaves, grass), a geometry of their own in the ray scene
u32 cut_cursor = 0;                  ///< vertices of cut-out draws captured since the last trace
constexpr u32 kPairs = 256;
std::mutex sun_mutex;
std::atomic<u64> sun_view{0}; ///< the depth view of the chosen sun pass (0: not chosen yet)
u32 sun_cursor = 0;           ///< vertices captured since the last trace
std::unordered_map<u64, u32> sun_at; ///< draw key -> its first vertex (~0u: drawn more than once, or already paired)
u32 pair_next = 0, pair_ring = 0;
VkBuffer pair_xfb{}, pair_tab{}, fit_out{}, fit_read{};
VmaAllocation pair_xfb_a{}, pair_tab_a{}, fit_out_a{}, fit_read_a{};
u32* pair_tab_ptr = nullptr;
const float* fit_read_ptr = nullptr;
std::atomic<u32> last_tris{0}, last_pairs{0}, last_ring{0}, empty_frames{0}, c_empty{0}, c_built{0};
bool ready = false, failed = false;
u64 recorded_frame = ~0ull;
std::atomic<u64> claimed{~0ull};
u64 draws_frame = 0;
std::atomic<u64> draws_last{0}, c_all{0}, c_dw{0}, c_cap{0};
bool Make(VkDeviceSize size, VkBufferUsageFlags usage, bool host, VkBuffer& b, VmaAllocation& a, void** mapped) {
    const VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo alloc{};
    alloc.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance->GetAllocator(), &ci, &alloc, &b, &a, &info) != VK_SUCCESS) {
        return false;
    }
    if (mapped != nullptr) {
        *mapped = info.pMappedData;
    }
    return true;
}
bool Ready() {
    if (ready || failed) {
        return ready;
    }
    void* m = nullptr;
    const VkBufferUsageFlags xfb_usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!Make(kXfbBytes, xfb_usage, false, xfb, xfb_a, nullptr) ||
        !Make(kDraws * 4, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
              false, counter, counter_a, nullptr) ||
        !Make(kSlots * kDraws * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback, readback_a, &m) || m == nullptr) {
        failed = true;
        LOG_ERROR(Render_Vulkan, "RT prototype: capture buffers failed");
        return false;
    }
    readback_ptr = static_cast<const u32*>(m);
    void* sm = nullptr;
    if (!Make(kSlots * kSampleSlots * kSampleVerts * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, sample, sample_a, &sm) || sm == nullptr) {
        failed = true;
        return false;
    }
    std::memset(sm, 0, kSlots * kSampleSlots * kSampleVerts * 16);
    sample_ptr = static_cast<const float*>(sm);
    void* shm = nullptr;
    if (!Make(8 * kShadowDraws * kSampleVerts * 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT, true, shadow_xfb, shadow_xfb_a, &shm) || shm == nullptr) {
        failed = true;
        return false;
    }
    std::memset(shm, 0, 8 * kShadowDraws * kSampleVerts * 16);
    shadow_ptr = static_cast<const float*>(shm);
    void* cm = nullptr;
    if (!Make(8 * kMainDraws * kSampleVerts * 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT, true, cam_xfb, cam_xfb_a, &cm) || cm == nullptr) {
        failed = true;
        return false;
    }
    std::memset(cm, 0, 8 * kMainDraws * kSampleVerts * 16);
    cam_ptr = static_cast<const float*>(cm);
    void* tm = nullptr;
    void* fm = nullptr;
    if (!Make(kPairs * 256, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, pair_xfb, pair_xfb_a, nullptr) ||
        !Make(8 * kPairs * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, pair_tab, pair_tab_a, &tm) || tm == nullptr ||
        !Make(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, fit_out, fit_out_a, nullptr) ||
        !Make(8 * 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, fit_read, fit_read_a, &fm) || fm == nullptr) {
        failed = true;
        return false;
    }
    std::memset(tm, 0, 8 * kPairs * 8);
    std::memset(fm, 0, 8 * 64);
    pair_tab_ptr = static_cast<u32*>(tm);
    fit_read_ptr = static_cast<const float*>(fm);
    std::memset(m, 0, kSlots * kDraws * 4);
    ready = true;
    LOG_ERROR(Render_Vulkan, "RT prototype: capture buffers made ({} MB)", kXfbBytes >> 20);
    return true;
}
void MemBarrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 mb{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands, .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &mb});
}
/// Called outside a pass (before UsePass): at the first captured draw of a new frame, the old count goes to the host and the counter starts at zero
void FrameStart(vk::CommandBuffer cmd, u64 frame) {
    if (frame == recorded_frame) {
        return;
    }
    if (recorded_frame != ~0ull) {
        draws_last.store(draws_frame, std::memory_order_relaxed);
        MemBarrier(cmd);
        const vk::BufferCopy copy{0, (recorded_frame % kSlots) * kDraws * 4, kDraws * 4};
        cmd.copyBuffer(counter, readback, copy);
        static std::array<vk::BufferCopy, kSampleSlots> sc;
        for (u32 i = 0; i < kSampleSlots; ++i) {
            sc[i] = vk::BufferCopy{i * kSlotBytes, ((recorded_frame % kSlots) * kSampleSlots + i) * kSampleVerts * 16, kSampleVerts * 16};
        }
        std::memcpy(main_keys[recorded_frame % kSlots], slot_keys, sizeof(slot_keys));
        std::memset(slot_keys, 0, sizeof(slot_keys));
    }
    MemBarrier(cmd);
    cmd.fillBuffer(counter, 0, kDraws * 4, 0);
    slot_next.store(0, std::memory_order_relaxed);
    MemBarrier(cmd);
    recorded_frame = frame;
    draws_frame = 0;
}
u32 main_w = 0, main_h = 0;
std::unordered_map<u64, u64> census;
u64 census_draws = 0;
bool census_done = false;
/// once: the depth-writing draws of ~2000 draws by pass size and color count
void Census(u32 w, u32 h, u32 colors) {
    if (census_done) {
        return;
    }
    ++census[(static_cast<u64>(w) << 32) | (static_cast<u64>(h) << 8) | colors];
    if (++census_draws == 200000) {
        census_done = true;
        for (const auto& [k, n] : census) {
            LOG_ERROR(Render_Vulkan, "RT census: pass {}x{} colors {}: {} depth-writing draws", k >> 32, (k >> 8) & 0xffffff, k & 0xff, n);
        }
    }
}
u32 BytesOfFrame(u64 frame) {
    u32 sum = 0;
    for (u32 i = 0; readback_ptr != nullptr && i < kDraws; ++i) {
        sum += readback_ptr[(frame % kSlots) * kDraws + i];
    }
    return sum;
}
} // namespace RtCapture
std::atomic<u64> rt_present_frame{0};
// RT prototype, part 2: pack the captured triangles, build the acceleration structures, trace one shadow ray per pixel toward the sun
namespace RtTrace {
static const u32 kPackSpv[] =
#include "rt_shaders/rt_fit.comp.inc"
;
static const u32 kFullSpv[] =
#include "rt_shaders/rt_full.vert.inc"
;
static const u32 kShadowSpv[] =
#include "rt_shaders/rt_gi.frag.inc"
;
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int);
static const u32 kBeamSpv[] =
#include "rt_shaders/rt_beam.frag.inc"
    ;
static const u32 kSunSpv[] =
#include "rt_shaders/rt_sun.frag.inc"
    ;
static const u32 kBlurSpv[] =
#include "rt_shaders/rt_blur.frag.inc"
;
constexpr u32 kMaxTris = RtCapture::kSunVerts / 3;
bool ready = false, failed = false, built = false;
VkBuffer verts{}, blas_buf{}, tlas_buf{}, scratch{}, inst{};
VmaAllocation verts_a{}, blas_a{}, tlas_a{}, scratch_a{}, inst_a{};
vk::AccelerationStructureKHR blas{}, tlas{};
vk::DeviceAddress verts_addr{}, scratch_addr{}, inst_addr{};
vk::DeviceSize blas_scratch{}, tlas_scratch{};
vk::DescriptorSetLayout pack_dsl{}, shadow_dsl{};
vk::PipelineLayout pack_layout{}, shadow_layout{};
vk::Pipeline pack_pipe{}, shadow_pipe{}, raw_pipe{};
VkImage ao_image{};
VmaAllocation ao_alloc{};
vk::ImageView ao_view{};
u32 ao_w = 0, ao_h = 0;
vk::Format shadow_format{};
vk::Sampler sampler{};
std::atomic<u64> traced{~0ull};
std::atomic<u64> c_traces{0};
// the main pass of the frame (from a captured draw)
vk::ImageView main_colors[8]{};
vk::Format main_formats[8]{};
vk::ImageView main_depth{};
bool main_stencil = false;
float fit_a = 1.0f, fit_b = 0.0f;
bool fit_ok = false;
std::atomic<bool> strips_on{false}; // off at the start: the first run with strips froze in the driver while a scene loaded
std::atomic<u32> c_strip{0};
std::atomic<u32> c_skip_ind{0}, c_skip_prim{0}, c_skip_nowrite{0}; ///< main pass draws that did not go into the ray scene, by reason
u32 cut_tris = 0;                 ///< cut-out triangles for the next Build
bool skip_as = false;             ///< Build: the ray scene of this frame is already there, only the bookkeeping
bool sun_built = false, sun_seen = false;
float beam_sun[3]{};
void NoteSun(const float* s) {
    beam_sun[0] = s[0], beam_sun[1] = s[1], beam_sun[2] = s[2];
    sun_seen = true;
}
float sun_dir[3]{0.3f, 1.0f, -0.3f};
double sun_rms[3]{};
double sun_rows[6]{};
bool sun_ok = false;

vk::DeviceAddress Addr(VkBuffer b) {
    return instance->GetDevice().getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = b});
}
std::array<float, 4> EnvVec(const char* name, std::array<float, 4> def) {
    if (const char* e = std::getenv(name)) {
        std::sscanf(e, "%f,%f,%f,%f", &def[0], &def[1], &def[2], &def[3]);
    }
    return def;
}
vk::ShaderModule Module(const u32* code, size_t bytes) {
    return instance->GetDevice().createShaderModule(vk::ShaderModuleCreateInfo{.codeSize = bytes, .pCode = code}).value;
}
bool Init() {
    if (ready || failed) {
        return ready;
    }
    failed = true;
    const vk::Device dev = instance->GetDevice();
    const VkBufferUsageFlags build_in = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    verts_addr = Addr(RtCapture::xfb); // the capture buffer itself: vec4 per vertex, xyz used
    // sizes
    const vk::AccelerationStructureGeometryKHR geo{.geometryType = vk::GeometryTypeKHR::eTriangles,
                                                   .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                                                                    .vertexFormat = vk::Format::eR32G32B32Sfloat, .vertexData = {.deviceAddress = verts_addr}, .vertexStride = 16,
                                                                    .maxVertex = kMaxTris * 3 - 1, .indexType = vk::IndexType::eNoneKHR}},
                                                   .flags = vk::GeometryFlagBitsKHR::eOpaque};
    vk::AccelerationStructureGeometryKHR geo_cut = geo; // the cut-out draws: not opaque, the shader decides per hit
    geo_cut.geometry.triangles.vertexData.deviceAddress = verts_addr + u64{RtCapture::kCutBase} * 16;
    geo_cut.flags = {};
    const std::array<vk::AccelerationStructureGeometryKHR, 2> geos{geo, geo_cut};
    vk::AccelerationStructureBuildGeometryInfoKHR bi{.type = vk::AccelerationStructureTypeKHR::eBottomLevel, .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
                                                    .mode = vk::BuildAccelerationStructureModeKHR::eBuild, .geometryCount = 2, .pGeometries = geos.data()};
    const std::array<u32, 2> max_prims{RtCapture::kCutBase / 3, (RtCapture::kSunVerts - RtCapture::kCutBase) / 3};
    const auto bs = dev.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, bi, max_prims);
    const VkBufferUsageFlags as_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (!RtCapture::Make(bs.accelerationStructureSize, as_usage, false, blas_buf, blas_a, nullptr)) {
        return false;
    }
    blas = dev.createAccelerationStructureKHR(vk::AccelerationStructureCreateInfoKHR{.buffer = blas_buf, .size = bs.accelerationStructureSize, .type = vk::AccelerationStructureTypeKHR::eBottomLevel}).value;
    blas_scratch = bs.buildScratchSize;
    // the instance (identity, host written once)
    void* m = nullptr;
    if (!RtCapture::Make(sizeof(VkAccelerationStructureInstanceKHR), build_in, true, inst, inst_a, &m) || m == nullptr) {
        return false;
    }
    VkAccelerationStructureInstanceKHR in{};
    in.transform.matrix[0][0] = in.transform.matrix[1][1] = in.transform.matrix[2][2] = 1.0f;
    in.mask = 0xff;
    in.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    in.accelerationStructureReference = dev.getAccelerationStructureAddressKHR(vk::AccelerationStructureDeviceAddressInfoKHR{.accelerationStructure = blas});
    std::memcpy(m, &in, sizeof(in));
    inst_addr = Addr(inst);
    const vk::AccelerationStructureGeometryKHR tgeo{.geometryType = vk::GeometryTypeKHR::eInstances,
                                                    .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{.data = {.deviceAddress = inst_addr}}}};
    vk::AccelerationStructureBuildGeometryInfoKHR ti{.type = vk::AccelerationStructureTypeKHR::eTopLevel, .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                                                    .mode = vk::BuildAccelerationStructureModeKHR::eBuild, .geometryCount = 1, .pGeometries = &tgeo};
    const u32 one = 1;
    const auto ts = dev.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, ti, one);
    if (!RtCapture::Make(ts.accelerationStructureSize, as_usage, false, tlas_buf, tlas_a, nullptr)) {
        return false;
    }
    tlas = dev.createAccelerationStructureKHR(vk::AccelerationStructureCreateInfoKHR{.buffer = tlas_buf, .size = ts.accelerationStructureSize, .type = vk::AccelerationStructureTypeKHR::eTopLevel}).value;
    tlas_scratch = ts.buildScratchSize;
    const u64 align = 256;
    if (!RtCapture::Make(blas_scratch + tlas_scratch + 2 * align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, scratch, scratch_a, nullptr)) {
        return false;
    }
    scratch_addr = (Addr(scratch) + align - 1) & ~(align - 1);
    // fit pipeline (camera -> sun view map)
    const std::array<vk::DescriptorSetLayoutBinding, 4> pb{{{0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
                                                            {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
                                                            {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
                                                            {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
    pack_dsl = dev.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR, .bindingCount = 4, .pBindings = pb.data()}).value;
    const vk::PushConstantRange ppc{vk::ShaderStageFlagBits::eCompute, 0, 8};
    pack_layout = dev.createPipelineLayout(vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &pack_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &ppc}).value;
    const vk::ShaderModule pm = Module(kPackSpv, sizeof(kPackSpv));
    const auto pr = dev.createComputePipeline({}, vk::ComputePipelineCreateInfo{.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = pm, .pName = "main"}, .layout = pack_layout});
    if (pr.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "RT: fit pipeline failed ({})", vk::to_string(pr.result));
        return false;
    }
    pack_pipe = pr.value;
    // shadow layout (pipeline per color format, made at the first trace)
    const std::array<vk::DescriptorSetLayoutBinding, 4> sb{{{0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
                                                            {1, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eFragment},
                                                            {2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment},
                                                            {3, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment}}};
    shadow_dsl = dev.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR, .bindingCount = 4, .pBindings = sb.data()}).value;
    const vk::PushConstantRange spc{vk::ShaderStageFlagBits::eFragment, 0, 64};
    shadow_layout = dev.createPipelineLayout(vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &shadow_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &spc}).value;
    sampler = dev.createSampler(vk::SamplerCreateInfo{.magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest}).value;
    LOG_ERROR(Render_Vulkan, "RT prototype: acceleration structures made (BLAS {} MB, scratch {} MB, max {} triangles)", bs.accelerationStructureSize >> 20,
              (blas_scratch + tlas_scratch) >> 20, kMaxTris);
    failed = false;
    ready = true;
    return true;
}
std::mutex depth_mutex;
vk::Image depth_image{};
vk::Format depth_format{};
std::unordered_map<VkImage, vk::ImageView> depth_views;
VkBuffer probe{};
VmaAllocation probe_a{};
const u32* probe_ptr = nullptr;
std::atomic<u32> logged_format{0};
void NoteDepth(vk::Image image, vk::Format format, bool stencil) {
    if (!image) {
        return;
    }
    std::scoped_lock lock{depth_mutex};
    depth_image = image, depth_format = format, main_stencil = stencil;
}
/// a view of the depth aspect only (a depth+stencil view cannot be sampled)
vk::ImageView DepthView(vk::Image& image_out) {
    std::scoped_lock lock{depth_mutex};
    image_out = depth_image;
    if (!depth_image || depth_format == vk::Format::eUndefined) {
        return {};
    }
    auto it = depth_views.find(depth_image);
    if (it != depth_views.end()) {
        return it->second;
    }
    const auto r = instance->GetDevice().createImageView(vk::ImageViewCreateInfo{.image = depth_image, .viewType = vk::ImageViewType::e2D, .format = depth_format,
                                                                                .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}});
    if (r.result != vk::Result::eSuccess) {
        return {};
    }
    if (logged_format.exchange(1) == 0) {
        LOG_ERROR(Render_Vulkan, "RT prototype: depth view format {} (stencil {})", vk::to_string(depth_format), main_stencil);
    }
    depth_views.emplace(depth_image, r.value);
    return r.value;
}
bool MaskMode() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_RT_VIEW"); return e != nullptr && e[0] == '1'; }();
    return on;
}
u32 full_pipe_sun = 0;
vk::Pipeline FullPipe(const u32* spv, size_t size, vk::Format format, bool multiply) {
    const vk::Device dev = instance->GetDevice();
    const vk::ShaderModule vm = Module(kFullSpv, sizeof(kFullSpv)), fm = Module(spv, size);
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{{.stage = vk::ShaderStageFlagBits::eVertex, .module = vm, .pName = "main"},
                                                                   {.stage = vk::ShaderStageFlagBits::eFragment, .module = fm, .pName = "main"}}};
    const vk::PipelineVertexInputStateCreateInfo vi{};
    const vk::PipelineInputAssemblyStateCreateInfo ia{.topology = vk::PrimitiveTopology::eTriangleList};
    const vk::PipelineViewportStateCreateInfo vp{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo rs{.cullMode = vk::CullModeFlagBits::eNone, .lineWidth = 1.0f};
    const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineDepthStencilStateCreateInfo ds{};
    vk::PipelineColorBlendAttachmentState att{.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB};
    float history = 0.0f;
    if (format == vk::Format::eR8G8B8A8Unorm) {
        // the raw result is mixed into the results of the frames before it (takes out frame-to-frame jumps)
        history = std::clamp(EnvVec("TLG_VKB_RT_BOUNCE", {0.4f, 0.85f, 2.0f, 0.0f})[1], 0.0f, 0.98f);
        att.colorWriteMask |= vk::ColorComponentFlagBits::eA;
        att.blendEnable = true;
        att.srcColorBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha, att.dstColorBlendFactor = vk::BlendFactor::eConstantAlpha, att.colorBlendOp = vk::BlendOp::eAdd;
        att.srcAlphaBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha, att.dstAlphaBlendFactor = vk::BlendFactor::eConstantAlpha, att.alphaBlendOp = vk::BlendOp::eAdd;
    }
    if (multiply) {
        att.blendEnable = true;
        att.srcColorBlendFactor = vk::BlendFactor::eDstColor, att.dstColorBlendFactor = vk::BlendFactor::eSrcColor, att.colorBlendOp = vk::BlendOp::eAdd;
        att.srcAlphaBlendFactor = vk::BlendFactor::eZero, att.dstAlphaBlendFactor = vk::BlendFactor::eOne, att.alphaBlendOp = vk::BlendOp::eAdd;
    }
    if (full_pipe_sun != 0) { // the sun shadow: only the green channel, the darker of the game's value and ours (1) or ours alone (2)
        att = vk::PipelineColorBlendAttachmentState{.colorWriteMask = vk::ColorComponentFlagBits::eG};
        if (full_pipe_sun == 1) {
            att.blendEnable = true;
            att.srcColorBlendFactor = vk::BlendFactor::eOne, att.dstColorBlendFactor = vk::BlendFactor::eOne, att.colorBlendOp = vk::BlendOp::eMin;
            att.srcAlphaBlendFactor = vk::BlendFactor::eOne, att.dstAlphaBlendFactor = vk::BlendFactor::eOne, att.alphaBlendOp = vk::BlendOp::eMin;
        }
    }
    if (full_pipe_sun == 3) { // the sun beams: light added to the picture
        att = vk::PipelineColorBlendAttachmentState{.blendEnable = true, .srcColorBlendFactor = vk::BlendFactor::eOne, .dstColorBlendFactor = vk::BlendFactor::eOne,
                                                    .colorBlendOp = vk::BlendOp::eAdd, .srcAlphaBlendFactor = vk::BlendFactor::eZero, .dstAlphaBlendFactor = vk::BlendFactor::eOne,
                                                    .alphaBlendOp = vk::BlendOp::eAdd,
                                                    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB};
    }
    vk::PipelineColorBlendStateCreateInfo cb{.attachmentCount = 1, .pAttachments = &att};
    cb.blendConstants[3] = history;
    const std::array<vk::DynamicState, 2> dyn{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dy{.dynamicStateCount = 2, .pDynamicStates = dyn.data()};
    const vk::PipelineRenderingCreateInfo ri{.colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    const vk::GraphicsPipelineCreateInfo ci{.pNext = &ri, .stageCount = 2, .pStages = stages.data(), .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
                                            .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
                                            .layout = shadow_layout};
    const auto r = dev.createGraphicsPipeline({}, ci);
    if (r.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "RT: pipeline failed ({})", vk::to_string(r.result));
        return {};
    }
    return r.value;
}
bool ShadowPipe(vk::Format format) {
    if (shadow_pipe && raw_pipe && shadow_format == format) {
        return true;
    }
    if (!raw_pipe) {
        raw_pipe = FullPipe(kShadowSpv, sizeof(kShadowSpv), vk::Format::eR8G8B8A8Unorm, false);
    }
    shadow_pipe = FullPipe(kBlurSpv, sizeof(kBlurSpv), format, !MaskMode());
    shadow_format = format;
    LOG_ERROR(Render_Vulkan, "RT: pipelines made for format {} (mode {})", vk::to_string(format), MaskMode() ? "check view" : "multiply");
    return shadow_pipe && raw_pipe;
}
/// the picture the raw (unsmoothed) ambient shading is drawn into; a new one when the size changes (the old one stays behind: rare, prototype)
bool AoTarget(vk::CommandBuffer cmd, u32 w, u32 h) {
    if (ao_view && ao_w == w && ao_h == h) {
        return true;
    }
    const VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ao_view = vk::ImageView{};
    if (vmaCreateImage(instance->GetAllocator(), &ici, &alloc, &ao_image, &ao_alloc, nullptr) != VK_SUCCESS) {
        return false;
    }
    const auto r = instance->GetDevice().createImageView(vk::ImageViewCreateInfo{.image = vk::Image{ao_image}, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
                                                                                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
    if (r.result != vk::Result::eSuccess) {
        return false;
    }
    ao_view = r.value, ao_w = w, ao_h = h;
    const vk::ImageMemoryBarrier ib{.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead, .oldLayout = vk::ImageLayout::eUndefined,
                                    .newLayout = vk::ImageLayout::eGeneral, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = vk::Image{ao_image}, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, ib);
    return true;
}
/// Outside a pass: the whole RT work of a frame
void Trace(vk::CommandBuffer cmd, vk::ImageView target_view, vk::Format target_format, u32 extent_w, u32 extent_h) {
    static const u32 target = [] { const char* e = std::getenv("TLG_VKB_RT_TARGET"); return e != nullptr ? static_cast<u32>(std::atoi(e)) : 0u; }();
    (void)target, (void)target_view, (void)target_format, (void)extent_w, (void)extent_h, (void)cmd;
}
void Build(vk::CommandBuffer cmd, u32 tris, u32 pairs, u32 ring) {
    RtCapture::MemBarrier(cmd);
    // 1. the camera -> sun view map of this frame
    const vk::DescriptorBufferInfo b0{RtCapture::xfb, 0, VK_WHOLE_SIZE}, b1{RtCapture::pair_xfb, 0, VK_WHOLE_SIZE}, b2{RtCapture::pair_tab, 0, VK_WHOLE_SIZE}, b3{RtCapture::fit_out, 0, VK_WHOLE_SIZE};
    const std::array<vk::WriteDescriptorSet, 4> pw{{{.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &b0},
                                                    {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &b1},
                                                    {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &b2},
                                                    {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &b3}}};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pack_pipe);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pack_layout, 0, pw);
    const std::array<u32, 2> ppc{pairs, ring * RtCapture::kPairs};
    cmd.pushConstants(pack_layout, vk::ShaderStageFlagBits::eCompute, 0, 8, ppc.data());
    cmd.dispatch(1, 1, 1);
    RtCapture::MemBarrier(cmd);
    cmd.copyBuffer(RtCapture::fit_out, RtCapture::fit_read, vk::BufferCopy{0, ring * 64, 64});
    if (skip_as && built) {
        return;
    }
    // 2. BLAS, then TLAS
    const vk::AccelerationStructureGeometryKHR geo{.geometryType = vk::GeometryTypeKHR::eTriangles,
                                                   .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                                                                    .vertexFormat = vk::Format::eR32G32B32Sfloat, .vertexData = {.deviceAddress = verts_addr}, .vertexStride = 16,
                                                                    .maxVertex = kMaxTris * 3 - 1, .indexType = vk::IndexType::eNoneKHR}},
                                                   .flags = vk::GeometryFlagBitsKHR::eOpaque};
    vk::AccelerationStructureGeometryKHR geo_cut = geo;
    geo_cut.geometry.triangles.vertexData.deviceAddress = verts_addr + u64{RtCapture::kCutBase} * 16;
    geo_cut.flags = {};
    const std::array<vk::AccelerationStructureGeometryKHR, 2> geos{geo, geo_cut};
    const vk::AccelerationStructureBuildGeometryInfoKHR bi{.type = vk::AccelerationStructureTypeKHR::eBottomLevel, .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
                                                          .mode = vk::BuildAccelerationStructureModeKHR::eBuild, .dstAccelerationStructure = blas, .geometryCount = 2, .pGeometries = geos.data(),
                                                          .scratchData = {.deviceAddress = scratch_addr}};
    const std::array<vk::AccelerationStructureBuildRangeInfoKHR, 2> br{{{.primitiveCount = std::min(tris, RtCapture::kCutBase / 3)}, {.primitiveCount = cut_tris}}};
    const vk::AccelerationStructureBuildRangeInfoKHR* brp = br.data();
    cmd.buildAccelerationStructuresKHR(1, &bi, &brp);
    RtCapture::MemBarrier(cmd);
    const vk::AccelerationStructureGeometryKHR tgeo{.geometryType = vk::GeometryTypeKHR::eInstances,
                                                    .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{.data = {.deviceAddress = inst_addr}}}};
    const vk::AccelerationStructureBuildGeometryInfoKHR ti{.type = vk::AccelerationStructureTypeKHR::eTopLevel, .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                                                          .mode = vk::BuildAccelerationStructureModeKHR::eBuild, .dstAccelerationStructure = tlas, .geometryCount = 1, .pGeometries = &tgeo,
                                                          .scratchData = {.deviceAddress = (scratch_addr + blas_scratch + 255) & ~u64{255}}};
    const vk::AccelerationStructureBuildRangeInfoKHR tr{.primitiveCount = 1};
    const vk::AccelerationStructureBuildRangeInfoKHR* trp = &tr;
    cmd.buildAccelerationStructuresKHR(1, &ti, &trp);
    RtCapture::MemBarrier(cmd);
    built = true;
}
/// the sun beams: lit air between the camera and the surface, added to the final picture. TLG_VKB_RT_BEAM=strength,density,steps,reach; live file
/// TLG_VKB_RT_BEAMLIVE "strength,density,steps,reach,forward"; F4 switches it off and on.
void Beams(vk::CommandBuffer cmd, vk::ImageView target_view, vk::Format format, u32 w, u32 h, vk::ImageView dview) {
    static std::array<float, 4> opt = EnvVec("TLG_VKB_RT_BEAM", {2.0f, 0.02f, 32.0f, 150.0f});
    static float forward = 0.5f;
    static const char* live_path = std::getenv("TLG_VKB_RT_BEAMLIVE");
    static u32 live_tick = 0;
    if (live_path && live_tick++ % 30 == 0) {
        if (std::FILE* lf = std::fopen(live_path, "r")) {
            float v[5];
            const int n = std::fscanf(lf, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
            if (n >= 4) {
                opt = {v[0], v[1], v[2], v[3]};
            }
            if (n >= 5) {
                forward = std::clamp(v[4], 0.0f, 0.95f);
            }
            std::fclose(lf);
        }
    }
    static bool on = true, was = false;
    const bool down = (GetAsyncKeyState(0x73) & 0x8000) != 0;
    if (down && !was) {
        on = !on;
        LOG_ERROR(Render_Vulkan, "RT: sun beams {}", on ? "on" : "off");
    }
    was = down;
    const bool seen = sun_seen;
    sun_seen = false;
    if (!on || !seen || opt[0] <= 0.0f || !built) {
        return;
    }
    static const std::array<float, 4> sgn = EnvVec("TLG_VKB_RT_SUNSIGN", {1.0f, 1.0f, 1.0f, 0.0f});
    static const std::array<float, 4> lens = EnvVec("TLG_VKB_RT_LENS", {1.06f, 2.54f, -1.0f, 3.0f});
    // a picture of its own at half size (the ambient shading's picture stays its own)
    static decltype(ao_image) b_image{};
    static decltype(ao_alloc) b_alloc{};
    static vk::ImageView b_view{};
    static u32 b_w = 0, b_h = 0;
    const u32 rw = std::max(w / 2, 1u), rh = std::max(h / 2, 1u);
    std::swap(ao_image, b_image), std::swap(ao_alloc, b_alloc), std::swap(ao_view, b_view), std::swap(ao_w, b_w), std::swap(ao_h, b_h);
    const bool target_ok = AoTarget(cmd, rw, rh);
    std::swap(ao_image, b_image), std::swap(ao_alloc, b_alloc), std::swap(ao_view, b_view), std::swap(ao_w, b_w), std::swap(ao_h, b_h);
    static vk::Pipeline raw{}, add{};
    static vk::Format add_format{};
    if (!raw) {
        raw = FullPipe(kBeamSpv, sizeof(kBeamSpv), vk::Format::eR8G8B8A8Unorm, false);
    }
    if (!add || add_format != format) {
        full_pipe_sun = 3;
        add = FullPipe(kBlurSpv, sizeof(kBlurSpv), format, false);
        full_pipe_sun = 0;
        add_format = format;
        LOG_ERROR(Render_Vulkan, "RT beams: pipeline for {} ({}x{})", vk::to_string(format), w, h);
    }
    if (!target_ok || !raw || !add) {
        return;
    }
    const vk::RenderingAttachmentInfo color{.imageView = target_view, .imageLayout = vk::ImageLayout::eGeneral, .loadOp = vk::AttachmentLoadOp::eLoad, .storeOp = vk::AttachmentStoreOp::eStore};
    const vk::RenderingAttachmentInfo rawc{.imageView = b_view, .imageLayout = vk::ImageLayout::eGeneral, .loadOp = vk::AttachmentLoadOp::eLoad, .storeOp = vk::AttachmentStoreOp::eStore};
    const vk::DescriptorImageInfo di{sampler, dview, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo ai{sampler, b_view, vk::ImageLayout::eGeneral};
    const vk::WriteDescriptorSetAccelerationStructureKHR wa{.accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
    const std::array<vk::WriteDescriptorSet, 2> sw{{{.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                                                    {.pNext = &wa, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR}}};
    const std::array<vk::WriteDescriptorSet, 2> bw{{{.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                                                    {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &ai}}};
    static u32 frame = 0;
    const std::array<float, 16> pr{beam_sun[0] * sgn[0], beam_sun[1] * sgn[1], beam_sun[2] * sgn[2], opt[2], fit_a, fit_b, lens[2], opt[3],
                                   static_cast<float>(frame++ % 64), 1.0f / rw, 1.0f / rh, opt[0], lens[0], lens[1], forward, opt[1]};
    const std::array<float, 16> pb{0.0f, 0.0f, 0.0f, 0.0f, fit_a, fit_b, lens[2], 2.0f, 0.0f, 1.0f / w, 1.0f / h, 1.0f, lens[0], lens[1], 0.0f, 2.0f};
    RtCapture::MemBarrier(cmd);
    cmd.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {rw, rh}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &rawc});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, raw);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, shadow_layout, 0, sw);
    cmd.pushConstants(shadow_layout, vk::ShaderStageFlagBits::eFragment, 0, 64, pr.data());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{{0, 0}, {rw, rh}});
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();
    RtCapture::MemBarrier(cmd);
    cmd.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {w, h}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, add);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, shadow_layout, 0, bw);
    cmd.pushConstants(shadow_layout, vk::ShaderStageFlagBits::eFragment, 0, 64, pb.data());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{{0, 0}, {w, h}});
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();
    RtCapture::MemBarrier(cmd);
}
void Shade(vk::CommandBuffer cmd, vk::ImageView target_view, vk::Format target_format, u32 extent_w, u32 extent_h) {
    if (!Init()) {
        return;
    }
    // what was captured since the last trace: this frame's sun view geometry and the pairs
    u32 tris = 0, pairs = 0, ring = 0;
    {
        std::scoped_lock lock{RtCapture::sun_mutex};
        tris = RtCapture::sun_cursor / 3, pairs = RtCapture::pair_next, ring = RtCapture::pair_ring;
        cut_tris = RtCapture::cut_cursor / 3;
        RtCapture::cut_cursor = 0;
        RtCapture::sun_cursor = 0, RtCapture::pair_next = 0, RtCapture::pair_ring = (ring + 1) % 8;
        RtCapture::sun_at.clear();
    }
    // a frame that arrives with only a fraction of the scene (the snapshot fell in the middle of the frame's draws) keeps the scene of the frame before
    static u32 good_tris = 0;
    const bool whole = tris > 0 && u64{tris} * 10 >= u64{good_tris} * 9;
    {
        static u32 lo = ~0u, hi = 0, n = 0, part = 0;
        lo = std::min(lo, tris), hi = std::max(hi, tris), part += whole ? 0 : 1;
        if (++n == 120) {
            LOG_ERROR(Render_Vulkan, "RT scene size over 120 frames: {} .. {} triangles (+{} cut-out now), {} frames held back as incomplete; {} strip draws taken; draws left out: {} indirect, {} not triangle lists, {} without depth write",
                      lo, hi, cut_tris, part, c_strip.exchange(0), c_skip_ind.exchange(0), c_skip_prim.exchange(0), c_skip_nowrite.exchange(0));
            lo = ~0u, hi = 0, n = 0, part = 0;
        }
    }
    good_tris = whole ? tris : good_tris - good_tris / 4;
    if (!whole && tris > 0 && built) {
        RtCapture::c_empty.fetch_add(1, std::memory_order_relaxed);
    } else if (tris > 0) {
        skip_as = sun_built; // the sun shadow step already built this frame's ray scene
        Build(cmd, tris, pairs, ring);
        skip_as = false;
        RtCapture::last_tris.store(tris, std::memory_order_relaxed), RtCapture::last_pairs.store(pairs, std::memory_order_relaxed), RtCapture::last_ring.store(ring, std::memory_order_relaxed);
        RtCapture::empty_frames.store(0, std::memory_order_relaxed);
        RtCapture::c_built.fetch_add(1, std::memory_order_relaxed);
    } else {
        RtCapture::c_empty.fetch_add(1, std::memory_order_relaxed);
        if (RtCapture::empty_frames.fetch_add(1, std::memory_order_relaxed) > 120) { // the chosen sun pass is gone: choose again
            RtCapture::sun_view.store(0, std::memory_order_relaxed);
            RtCapture::empty_frames.store(0, std::memory_order_relaxed);
        }
    }
    sun_built = false;
    {
        static bool was = false;
        const bool down = (GetAsyncKeyState(0x74) & 0x8000) != 0; // F5: strips in the ray scene on / off
        if (down && !was) {
            strips_on.store(!strips_on.load());
            LOG_ERROR(Render_Vulkan, "RT: strips {}", strips_on.load() ? "on" : "off");
        }
        was = down;
    }
    vk::Image dimg{};
    const vk::ImageView dview = DepthView(dimg);
    if (!built || !dview || !fit_ok || !ShadowPipe(target_format)) {
        sun_seen = false;
        return;
    }
    if (probe_ptr == nullptr) {
        void* m = nullptr;
        if (RtCapture::Make(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, probe, probe_a, &m) && m != nullptr) {
            probe_ptr = static_cast<const u32*>(m);
        }
    }
    RtCapture::MemBarrier(cmd);
    if (probe != VkBuffer{}) {
        const vk::BufferImageCopy pc0{.bufferOffset = 0, .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
                                      .imageOffset = {static_cast<s32>(RtCapture::main_w / 2), static_cast<s32>(RtCapture::main_h * 3 / 4), 0}, .imageExtent = {1, 1, 1}};
        cmd.copyImageToBuffer(dimg, vk::ImageLayout::eGeneral, probe, pc0);
        RtCapture::MemBarrier(cmd);
    }
    // live switch: F2 turns the effect off and on
    {
        static bool on = false, was = false; // off at the start: F2 turns the corner shading on
        const bool down = (GetAsyncKeyState(0x71) & 0x8000) != 0;
        if (down && !was) {
            on = !on;
            LOG_ERROR(Render_Vulkan, "RT: effect {}", on ? "on" : "off");
        }
        was = down;
        if (!on) {
            Beams(cmd, target_view, target_format, extent_w, extent_h, dview);
            return;
        }
    }
    // 3. the raw ambient shading into its own picture, then smoothed onto the main pass's color target
    // the rays run at a fraction of the picture's size (2 = half in both directions); the smoothing pass brings it back to full size
    static const u32 div = static_cast<u32>(std::clamp(EnvVec("TLG_VKB_RT_BOUNCE", {0.4f, 0.85f, 2.0f, 0.0f})[2], 1.0f, 4.0f));
    const u32 raw_w = std::max(extent_w / div, 1u), raw_h = std::max(extent_h / div, 1u);
    if (!AoTarget(cmd, raw_w, raw_h)) {
        return;
    }
    const vk::RenderingAttachmentInfo color{.imageView = target_view, .imageLayout = vk::ImageLayout::eGeneral, .loadOp = vk::AttachmentLoadOp::eLoad, .storeOp = vk::AttachmentStoreOp::eStore};
    const vk::RenderingAttachmentInfo raw{.imageView = ao_view, .imageLayout = vk::ImageLayout::eGeneral, .loadOp = vk::AttachmentLoadOp::eLoad, .storeOp = vk::AttachmentStoreOp::eStore};
    const vk::DescriptorImageInfo di{sampler, dview, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo ai{sampler, ao_view, vk::ImageLayout::eGeneral};
    const vk::WriteDescriptorSetAccelerationStructureKHR wa{.accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
    const vk::DescriptorBufferInfo mb{RtCapture::fit_out, 0, 64};
    const vk::DescriptorImageInfo ci{sampler, target_view, vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 4> sw{{{.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &ci},
                                                    {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                                                    {.pNext = &wa, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR},
                                                    {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &mb}}};
    const std::array<vk::WriteDescriptorSet, 2> bw{{{.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                                                    {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &ai}}};
    static std::array<float, 4> gi = EnvVec("TLG_VKB_RT_GI", {1.5f, 0.8f, 4.0f, 0.01f});     // radius, strength, rays per pixel, bias
    static const std::array<float, 4> lens = EnvVec("TLG_VKB_RT_LENS", {1.06f, 2.54f, -1.0f, 3.0f}); // fx, fy, y flip, smoothing reach in pixels
    static u32 gi_frame = 0;
    static float bounce_k = EnvVec("TLG_VKB_RT_BOUNCE", {0.4f, 0.85f, 2.0f, 0.0f})[0];
    // live tuning: a text file "radius,strength,rays,bias,bounce" (path in TLG_VKB_RT_LIVE) is read again twice a second
    static const char* live_path = std::getenv("TLG_VKB_RT_LIVE");
    static u32 live_tick = 0;
    if (live_path && live_tick++ % 30 == 0) {
        if (std::FILE* lf = std::fopen(live_path, "r")) {
            float v[5];
            if (std::fscanf(lf, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
                gi = {v[0], v[1], v[2], v[3]};
                bounce_k = v[4];
            }
            std::fclose(lf);
        }
    }
    static const float bounce_unused_ = 0.0f; // strength of the bounce light, share of the earlier frames
    const std::array<float, 16> pc{gi[0], gi[1], gi[2], gi[3], fit_a, fit_b, lens[2], MaskMode() ? 1.0f : 0.0f,
                                   static_cast<float>(gi_frame++ % 64), 1.0f / extent_w, 1.0f / extent_h, built ? 1.0f : 0.0f, lens[0], lens[1], bounce_k, lens[3]};
    std::array<float, 16> pc_raw = pc;
    pc_raw[7] = 0.0f, pc_raw[9] = 1.0f / raw_w, pc_raw[10] = 1.0f / raw_h;
    cmd.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {raw_w, raw_h}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &raw});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, raw_pipe);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, shadow_layout, 0, sw);
    cmd.pushConstants(shadow_layout, vk::ShaderStageFlagBits::eFragment, 0, 64, pc_raw.data());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(raw_w), static_cast<float>(raw_h), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{{0, 0}, {raw_w, raw_h}});
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();
    RtCapture::MemBarrier(cmd);
    cmd.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {extent_w, extent_h}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, shadow_pipe);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, shadow_layout, 0, bw);
    cmd.pushConstants(shadow_layout, vk::ShaderStageFlagBits::eFragment, 0, 64, pc.data());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent_w), static_cast<float>(extent_h), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{{0, 0}, {extent_w, extent_h}});
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();
    RtCapture::MemBarrier(cmd);
    c_traces.fetch_add(1, std::memory_order_relaxed);
    Beams(cmd, target_view, target_format, extent_w, extent_h, dview);
}
/// the sun shadow, right after the game's own sun shadow step, into that step's picture. TLG_VKB_RT_SUN=cone,rays,bias,mode (mode 0 off, 1 darker of game and
/// ours, 2 ours alone); F3 switches it off and on.
bool SunOn() { // F3: off = the frame is exactly the game's own again
    static bool on = true, was = false;
    const bool down = (GetAsyncKeyState(0x72) & 0x8000) != 0;
    if (down && !was) {
        on = !on;
    }
    was = down;
    return on;
}
void SunShadow(vk::CommandBuffer cmd, vk::ImageView target_view, vk::Format format, u32 w, u32 h, const float* sun) {
    static std::array<float, 4> opt = EnvVec("TLG_VKB_RT_SUN", {0.03f, 12.0f, 0.02f, 1.0f});
    // leaves and grass (cut-out cards): the share of a card that blocks the sun (1 solid, 0 no shadow) and the number of cells per card edge
    static std::array<float, 4> leaf = EnvVec("TLG_VKB_RT_LEAF", {0.6f, 4.0f, 0.0f, 0.0f});
    // live tuning: a text file "cone,rays,bias,mode" (path in TLG_VKB_RT_SUNLIVE) is read again twice a second
    static const char* live_path = std::getenv("TLG_VKB_RT_SUNLIVE");
    static u32 live_tick = 0;
    if (live_path && live_tick++ % 30 == 0) {
        if (std::FILE* lf = std::fopen(live_path, "r")) {
            float v[6];
            const int n = std::fscanf(lf, "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            if (n >= 4) {
                opt = {v[0], v[1], v[2], v[3]};
            }
            if (n >= 5) {
                leaf[0] = v[4];
            }
            if (n >= 6) {
                leaf[1] = v[5];
            }
            std::fclose(lf);
        }
    }
    static const std::array<float, 4> sgn = EnvVec("TLG_VKB_RT_SUNSIGN", {1.0f, 1.0f, 1.0f, 0.0f});
    static const std::array<float, 4> lens = EnvVec("TLG_VKB_RT_LENS", {1.06f, 2.54f, -1.0f, 3.0f});
    if (opt[3] < 0.5f || !Init() || !fit_ok || !target_view) {
        return;
    }
    u32 tris = 0;
    {
        std::scoped_lock lock{RtCapture::sun_mutex};
        tris = RtCapture::sun_cursor / 3;
        cut_tris = RtCapture::cut_cursor / 3;
    }
    vk::Image dimg{};
    const vk::ImageView dview = DepthView(dimg);
    if (tris == 0 || !dview) {
        return;
    }
    static vk::Pipeline pipes[2]{};
    static vk::Format pipe_format{};
    if (!pipes[0] || pipe_format != format) {
        full_pipe_sun = 1;
        pipes[0] = FullPipe(kSunSpv, sizeof(kSunSpv), format, false);
        full_pipe_sun = 2;
        pipes[1] = FullPipe(kSunSpv, sizeof(kSunSpv), format, false);
        full_pipe_sun = 0;
        pipe_format = format;
        LOG_ERROR(Render_Vulkan, "RT sun: pipeline for {} ({}x{}), {} triangles", vk::to_string(format), w, h, tris);
    }
    const vk::Pipeline pipe = pipes[opt[3] > 1.5f ? 1 : 0];
    if (!pipe) {
        return;
    }
    Build(cmd, tris, 0, 0);
    if (!built) {
        return;
    }
    sun_built = true;
    const vk::RenderingAttachmentInfo color{.imageView = target_view, .imageLayout = vk::ImageLayout::eGeneral, .loadOp = vk::AttachmentLoadOp::eLoad, .storeOp = vk::AttachmentStoreOp::eStore};
    const vk::DescriptorImageInfo di{sampler, dview, vk::ImageLayout::eGeneral};
    const vk::WriteDescriptorSetAccelerationStructureKHR wa{.accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
    const std::array<vk::WriteDescriptorSet, 2> sw{{{.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                                                    {.pNext = &wa, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR}}};
    static u32 frame = 0;
    const std::array<float, 16> pc{sun[0] * sgn[0], sun[1] * sgn[1], sun[2] * sgn[2], opt[2], fit_a, fit_b, lens[2], opt[0],
                                   static_cast<float>(frame++ % 64), 1.0f / w, 1.0f / h, opt[1], lens[0], lens[1], leaf[0], std::max(leaf[1], 1.0f)};
    cmd.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {w, h}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, shadow_layout, 0, sw);
    cmd.pushConstants(shadow_layout, vk::ShaderStageFlagBits::eFragment, 0, 64, pc.data());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{{0, 0}, {w, h}});
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();
    RtCapture::MemBarrier(cmd);
}
} // namespace RtTrace
thread_local bool rt_state_dirty = false;
void RtBuildHook(vk::CommandBuffer) {
}


/// test: stone depth. The value goes to every pixel shader (a spare per-draw field); only patched shaders read it. F6 switches it off and on; a text file
/// "depth" (path in TLG_VKB_STONELIVE) is read again about twice a second. 0 = off.
static float StoneKnob() {
    static std::atomic<float> value{0.0f};
    static std::atomic<u32> tick{0};
    static std::mutex m;
    if (tick.fetch_add(1, std::memory_order_relaxed) % 4096 == 0) {
        std::scoped_lock lock{m};
        static const char* live_path = std::getenv("TLG_VKB_STONELIVE");
        static float depth = 0.05f;
        static bool on = true, was = false;
        const bool down = (RtTrace::GetAsyncKeyState(0x78) & 0x8000) != 0; // F9
        if (down && !was) {
            on = !on;
            LOG_ERROR(Render_Vulkan, "stone depth {}", on ? "on" : "off");
        }
        was = down;
        static u32 n = 0;
        if (live_path && n++ % 8 == 0) {
            if (std::FILE* lf = std::fopen(live_path, "r")) {
                float v;
                if (std::fscanf(lf, "%f", &v) == 1) {
                    depth = v;
                }
                std::fclose(lf);
            }
        }
        value.store(on && live_path ? depth : 0.0f, std::memory_order_relaxed);
    }
    return value.load(std::memory_order_relaxed);
}
void DrawJob::Run(vk::CommandBuffer cmd) {
    const VkbVulkan::FixedState& fixed = *fixed_state;
    struct JobTime {
        u64 t0;
        ~JobTime() {
            if (t0 != 0) {
                c_job_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
                c_job_n.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } job_time{TimesOn() ? NowNs() : 0};
    if (prepared ? !prepared_ok : (AnyLate() && !ResolveLate())) { // R2b (note 64): a buffer not found: the draw is skipped, as at the capture before
        return;
    }
    // note 65 bound, kill experiment only (TLG_VKB_TEST_NO_DRAW_COMMANDS=1, nothing is drawn): the draw job resolves, then records nothing (no barrier, no pass,
    // no state, no draw): what the recording thread's own work for draws costs the frame
    static const bool no_commands = [] { const char* e = std::getenv("TLG_VKB_TEST_NO_DRAW_COMMANDS"); return e != nullptr && e[0] == '1'; }();
    if (no_commands) {
        return;
    }
    // V4h: engine memory read in place gets a copy in our device ring in front of the submission
    for (u32 i = 0; i < b.buffers.size() && i < b.stageable.size(); ++i) {
        if (b.stageable[i] != 0) {
            (void)Stream::StageRead(b.buffers[i].buffer, b.buffers[i].offset, b.buffers[i].range);
        }
    }
    for (u32 i = 0; i < vertex_buffers.size() && i < vertex_sizes.size(); ++i) {
        if (vertex_sizes[i] != 0) {
            (void)Stream::StageRead(vertex_buffers[i], vertex_offsets[i], vertex_sizes[i]);
        }
    }
    if (index_size != 0 && index_buffer) {
        (void)Stream::StageRead(index_buffer, index_offset, index_size);
    }
    // V4d: a barrier only when this draw reads what earlier work since the last barrier wrote (that ends the open pass; it begins again below), or its pass's attachments need one
    static const bool no_draw_barrier = [] { const char* e = std::getenv("TLG_VKB_NO_DRAW_BARRIER"); return e != nullptr && e[0] == '1'; }(); // R5 kill-experiment (note 32)
    if (!Stream::LegacyBarriersOn() && !no_draw_barrier) {
        Stream::SetHazardTag(ps_hash);
        (void)Stream::Hazard(cmd, std::span<const Stream::Use>{uses.data(), uses.size()});
    }
    const bool rt_dw = RtCapture::On() && fixed.depth_test && fixed.depth_target && pass.depth && (fixed.depth_write || RtNoWriteToo()) && !fixed.depth_clear && !fixed.rasterizer_discard;
    if (RtCapture::On()) {
        RtCapture::c_all.fetch_add(1, std::memory_order_relaxed);
    }
    if (rt_dw) {
        RtCapture::c_dw.fetch_add(1, std::memory_order_relaxed);
        RtCapture::Census(pass.extent.width, pass.extent.height, pass.color_count);
        if (pass.color_count > 0) {
            RtCapture::main_w = pass.extent.width, RtCapture::main_h = pass.extent.height;
        }
    }
    const bool rt_capture = rt_dw && pass.extent.width == RtCapture::main_w && pass.extent.height == RtCapture::main_h && RtCapture::Ready();
    if (rt_capture && pass.color_count > 0) {
        for (u32 i = 0; i < 8; ++i) {
            RtTrace::main_colors[i] = i < pass.color_count ? pass.colors[i] : vk::ImageView{};
        }
        RtTrace::main_formats[0] = static_cast<vk::Format>(color_format0);
        RtTrace::NoteDepth(pass.depth_image, pass.depth_format, pass.depth_has_stencil);
    }
    if (rt_capture) {
        const u64 now = rt_present_frame.load(std::memory_order_relaxed);
        u64 seen = RtCapture::claimed.load(std::memory_order_relaxed);
        if (seen != now && RtCapture::claimed.compare_exchange_strong(seen, now)) {
            Stream::BreakPass(cmd);
            RtCapture::FrameStart(cmd, now);
        }
    }
    if (Stream::UsePass(cmd, pass, std::span<const Stream::Use>{attachment_uses.data(), attachment_uses.size()})) {
        c_new_passes.fetch_add(1, std::memory_order_relaxed);
    }
    // note 60: what this recording already holds (a D3D11 driver's redundant-state filter): the pipeline, the dynamic state, the vertex input, the vertex and index
    // buffers are sent only when they differ from the previous draw's in the same command buffer. TLG_VKB_ALL_STATE=1 sends everything every draw, as before.
    struct DrawStateCache {
        u64 generation{~0ull};
        VkCommandBuffer cmd{};
        VkPipeline pipeline{};
        const VkbVulkan::FixedState* fixed{};
        u64 pass_key{~0ull};
        std::array<vk::ColorComponentFlags, VkbAmdGpu::NUM_COLOR_BUFFERS> masks{};
        boost::container::small_vector<vk::VertexInputBindingDescription2EXT, 16> bindings;
        boost::container::small_vector<vk::VertexInputAttributeDescription2EXT, 16> attributes;
        bool vertex_input{};
        boost::container::small_vector<vk::Buffer, 16> vertex_buffers;
        boost::container::small_vector<vk::DeviceSize, 16> vertex_offsets;
        VkBuffer index_buffer{};
        u64 index_offset{~0ull};
        vk::IndexType index_type{};
    };
    thread_local DrawStateCache state;
    static const bool all_state = [] { const char* e = std::getenv("TLG_VKB_ALL_STATE"); return e != nullptr && e[0] == '1'; }();
    if (all_state || rt_state_dirty || state.generation != Stream::RecordingGeneration() || state.cmd != static_cast<VkCommandBuffer>(cmd)) {
        rt_state_dirty = false;
        state = {};
        state.generation = Stream::RecordingGeneration();
        state.cmd = static_cast<VkCommandBuffer>(cmd);
    }
    if (state.pipeline != static_cast<VkPipeline>(pipeline)) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        state.pipeline = static_cast<VkPipeline>(pipeline);
        c_state_pipelines.fetch_add(1, std::memory_order_relaxed);
    }
    // A2 (note 46): the descriptor writes are made here, on the recording thread, in its own reused list (they were 4 KB of every draw's job)
    thread_local boost::container::small_vector<vk::WriteDescriptorSet, 64> writes;
    writes.clear();
    u32 binding = 0;
    for (u32 r = 0; r < ranges.size(); r += 3) {
        for (u32 k = 0; k < ranges[r].second; ++k) {
            writes.push_back({.dstBinding = binding++, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &b.buffers[ranges[r].first + k]});
        }
        for (u32 k = 0; k < ranges[r + 1].second; ++k) {
            writes.push_back({.dstBinding = binding++, .descriptorCount = 1,
                                .descriptorType = image_types[ranges[r + 1].first + k],
                                .pImageInfo = &b.images[ranges[r + 1].first + k]});
        }
        for (u32 k = 0; k < ranges[r + 2].second; ++k) {
            writes.push_back({.dstBinding = binding++, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampler, .pImageInfo = &b.samplers[ranges[r + 2].first + k]});
        }
    }
    if (!writes.empty() && push_descriptors) {
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0, writes);
    } else if (!writes.empty()) {
        // note 41: more bindings than push descriptors allow (the layout is a plain one, as the old path's): a descriptor set of ours, written now
        const vk::DescriptorSet set = BigDescriptorSet(set_layout);
        if (set) {
            for (auto& write : writes) {
                write.dstSet = set;
            }
            instance->GetDevice().updateDescriptorSets(writes, {});
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, set, {});
        }
    }
    push.tlg_test = StoneKnob();
    cmd.pushConstants(layout, VkbVulkan::Vkb::AllGraphicsStageBits, 0, sizeof(push), &push);
    // note 59 bound, kill experiment only (TLG_VKB_TEST_STATE_ONCE=1, the picture is wrong): the dynamic state sent once per command buffer, not every draw
    static const bool state_once = [] { const char* e = std::getenv("TLG_VKB_TEST_STATE_ONCE"); return e != nullptr && e[0] == '1'; }();
    thread_local VkCommandBuffer t_state_cmd{};
    const bool first_in_cmd = t_state_cmd != static_cast<VkCommandBuffer>(cmd);
    t_state_cmd = static_cast<VkCommandBuffer>(cmd);
    // note 60: the dynamic state comes from the interned fixed state, the pass's depth / stencil / colour count and the write masks (the two note 41 test switches
    // aside): when all of them equal the previous draw's in this recording, nothing is sent
    const u64 pass_key = (pass.depth ? 1ull : 0ull) | (pass.depth_has_stencil ? 2ull : 0ull) | static_cast<u64>(pass.color_count) << 2; // (the note 41 test switches need TLG_VKB_ALL_STATE=1)
    const bool same_state = state.fixed == fixed_state && state.pass_key == pass_key && state.masks == masks;
    const bool send_state = state_once ? first_in_cmd : (!same_state || !Stream::StampsLean());
    state.fixed = fixed_state, state.pass_key = pass_key, state.masks = masks;
    (same_state ? c_state_skipped : c_state_sent).fetch_add(1, std::memory_order_relaxed);
    if (send_state) {
    // the dynamic state the pipeline declares, all of it, every draw
    boost::container::small_vector<vk::Viewport, 16> viewports;
    boost::container::small_vector<vk::Rect2D, 16> scissors;
    for (u32 i = 0; i < std::max(fixed.viewport_count, 1u); ++i) {
        vk::Viewport viewport = fixed.viewports[i];
        if (!instance->IsDepthRangeUnrestrictedSupported()) {
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }
        if (fixed.clip_disabled && !fixed.viewport_empty) {
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance->GetMaxViewportWidth(), 16384));
            viewport.height = float(std::min<u32>(instance->GetMaxViewportHeight(), 16384));
        }
        viewports.push_back(viewport);
        scissors.push_back(fixed.scissors[i]);
    }
    cmd.setViewportWithCount(viewports);
    cmd.setScissorWithCount(scissors);
    {
        const bool dt = fixed.depth_test && fixed.depth_target && pass.depth;
        const bool st = fixed.stencil_test && fixed.stencil_target && pass.depth_has_stencil;
        Stream::NotePassDraw((pass.depth ? 1u : 0u) | (pass.depth_has_stencil ? 2u : 0u) | (dt ? 4u : 0u) | (st ? 8u : 0u) |
                                 (fixed.depth_bounds_test && pass.depth ? 16u : 0u) | (dt && fixed.depth_write && !fixed.depth_clear ? 32u : 0u),
                             ps_hash, color_format0, viewports[0], scissors[0]);
    }
    static const u64 test_nodepth_ps = [] { const char* e = std::getenv("TLG_VKB_TEST_NODEPTH_PS"); return e != nullptr ? std::strtoull(e, nullptr, 16) : 0ull; }(); // note 41, test
    const bool depth_test = fixed.depth_test && fixed.depth_target && pass.depth && !(test_nodepth_ps != 0 && ps_hash == test_nodepth_ps);
    cmd.setDepthTestEnable(depth_test);
    cmd.setDepthWriteEnable(depth_test && fixed.depth_write && !fixed.depth_clear);
    cmd.setDepthCompareOp(depth_test ? fixed.depth_compare : vk::CompareOp::eAlways);
    if (instance->IsDepthBoundsSupported()) {
        cmd.setDepthBoundsTestEnable(fixed.depth_bounds_test && pass.depth);
        cmd.setDepthBounds(fixed.depth_bounds_min, fixed.depth_bounds_max);
    }
    // note 41, test only: TLG_VKB_TEST_SHADOW_BIAS=<constant>,<slope>: that bias on every draw into a pass without color targets
    static const std::array<float, 2> test_bias = [] {
        std::array<float, 2> r{0.0f, 0.0f};
        if (const char* e = std::getenv("TLG_VKB_TEST_SHADOW_BIAS")) {
            std::sscanf(e, "%f,%f", &r[0], &r[1]);
        }
        return r;
    }();
    static const float test_eq_bias = [] { const char* e = std::getenv("TLG_VKB_TEST_EQ_BIAS"); return e != nullptr ? static_cast<float>(std::atof(e)) : 0.0f; }();
    if (test_eq_bias != 0.0f && pass.color_count > 0 && fixed.depth_test && !fixed.depth_write) {
        cmd.setDepthBiasEnable(true);
        cmd.setDepthBias(test_eq_bias, 0.0f, 0.0f);
    } else if ((test_bias[0] != 0.0f || test_bias[1] != 0.0f) && pass.color_count == 0) {
        cmd.setDepthBiasEnable(true);
        cmd.setDepthBias(test_bias[0], 0.0f, test_bias[1]);
    } else {
        cmd.setDepthBiasEnable(fixed.depth_bias);
        cmd.setDepthBias(fixed.depth_bias_constant, fixed.depth_bias_clamp, fixed.depth_bias_slope);
    }
    const bool stencil_test = fixed.stencil_test && fixed.stencil_target && pass.depth_has_stencil;
    cmd.setStencilTestEnable(stencil_test);
    cmd.setStencilOp(vk::StencilFaceFlagBits::eFront, fixed.stencil_front_ops.fail_op, fixed.stencil_front_ops.pass_op, fixed.stencil_front_ops.depth_fail_op,
                     fixed.stencil_front_ops.compare_op);
    cmd.setStencilOp(vk::StencilFaceFlagBits::eBack, fixed.stencil_back_ops.fail_op, fixed.stencil_back_ops.pass_op, fixed.stencil_back_ops.depth_fail_op,
                     fixed.stencil_back_ops.compare_op);
    cmd.setStencilReference(vk::StencilFaceFlagBits::eFront, fixed.stencil_reference[0]);
    cmd.setStencilReference(vk::StencilFaceFlagBits::eBack, fixed.stencil_reference[1]);
    cmd.setStencilWriteMask(vk::StencilFaceFlagBits::eFront, fixed.stencil_clear ? 0u : fixed.stencil_write_mask[0]);
    cmd.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, fixed.stencil_clear ? 0u : fixed.stencil_write_mask[1]);
    cmd.setStencilCompareMask(vk::StencilFaceFlagBits::eFront, fixed.stencil_compare_mask[0]);
    cmd.setStencilCompareMask(vk::StencilFaceFlagBits::eBack, fixed.stencil_compare_mask[1]);
    cmd.setCullMode(fixed.cull_mode);
    cmd.setFrontFace(fixed.front_face);
    const bool restart = fixed.primitive_restart && (instance->IsListRestartSupported() || !fixed.list_topology) &&
                         (instance->IsPatchListRestartSupported() || !fixed.patch_list_topology);
    cmd.setPrimitiveRestartEnable(restart);
    cmd.setRasterizerDiscardEnable(fixed.rasterizer_discard);
    cmd.setLineWidth(1.0f);
    cmd.setBlendConstants(fixed.blend_constants.data());
    if (instance->IsDynamicColorWriteMaskSupported() && pass.color_count != 0) {
        cmd.setColorWriteMaskEXT(0, std::span<const vk::ColorComponentFlags>{masks.data(), pass.color_count});
    }
    }
    const auto same_span = [](const auto& a, const auto& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0; };
    if (!state.vertex_input || !same_span(state.bindings, input_bindings) || !same_span(state.attributes, input_attributes)) {
        cmd.setVertexInputEXT(input_bindings, input_attributes);
        state.bindings.assign(input_bindings.begin(), input_bindings.end());
        state.attributes.assign(input_attributes.begin(), input_attributes.end());
        state.vertex_input = true;
        c_state_vertex_inputs.fetch_add(1, std::memory_order_relaxed);
    }
    if (!vertex_buffers.empty() && (!same_span(state.vertex_buffers, vertex_buffers) || !same_span(state.vertex_offsets, vertex_offsets))) {
        cmd.bindVertexBuffers(0, vertex_buffers, vertex_offsets);
        state.vertex_buffers.assign(vertex_buffers.begin(), vertex_buffers.end());
        state.vertex_offsets.assign(vertex_offsets.begin(), vertex_offsets.end());
    }
    static const bool no_draw = [] { const char* e = std::getenv("TLG_VKB_NO_DRAW"); return e != nullptr && *e == '1'; }(); // kill experiment: everything but the draw
    u32 rt_shadow_slot = ~0u;
    u64 rt_key = 0;
    if (RtCapture::On()) {
        rt_key = guest_key;
    }
    // triangle strips (without restart marks) come out of the capture as loose triangles: 3 * (n - 2) vertices. F5 leaves them out again.
    const bool rt_strip = fixed.primitive_type == Core::NativePrimitive::Kind::TriangleStrip && !fixed.primitive_restart && kind != 2 && a1 >= 3 && instances >= 1 &&
                          RtTrace::strips_on.load(std::memory_order_relaxed);
    const bool rt_tri = (fixed.primitive_type == Core::NativePrimitive::Kind::TriangleList && kind != 2 && a1 >= 3 && instances >= 1) || rt_strip;
    const u64 rt_verts = (rt_strip ? u64{3} * (static_cast<u32>(a1) - 2) : static_cast<u64>(static_cast<u32>(a1))) * instances;
    if (rt_strip) {
        RtTrace::c_strip.fetch_add(1, std::memory_order_relaxed);
    }
    if (false && RtCapture::On() && RtCapture::ready && pass.color_count == 0 && pass.depth && pass.extent.width == 2048 && pass.extent.height == 2048 && fixed.depth_write &&
        !fixed.rasterizer_discard && kind != 2) {
        const u64 now = rt_present_frame.load(std::memory_order_relaxed);
        if (reinterpret_cast<u64>(static_cast<VkImageView>(pass.depth)) == RtCapture::sun_view.load(std::memory_order_relaxed)) {
            // the chosen sun pass: the whole draw goes into the ray-trace scene of this frame (vertex count known here: no readback)
            u32 at = ~0u;
            if (rt_tri) {
                std::scoped_lock lock{RtCapture::sun_mutex};
                if (RtCapture::sun_cursor + rt_verts <= u64{RtCapture::kSunVerts}) {
                    at = RtCapture::sun_cursor;
                    RtCapture::sun_cursor += static_cast<u32>(rt_verts);
                    const auto [it, fresh] = RtCapture::sun_at.try_emplace(rt_key, at);
                    if (!fresh) {
                        it->second = ~0u;
                    }
                }
            }
            if (at != ~0u) {
                const vk::Buffer xb{RtCapture::xfb};
                const vk::DeviceSize off = u64{at} * 16, size = rt_verts * 16;
                cmd.bindTransformFeedbackBuffersEXT(0, 1, &xb, &off, &size);
                cmd.beginTransformFeedbackEXT(0, 0, nullptr, nullptr);
                rt_shadow_slot = 0;
            }
        } else if (now % 8 == 0) {
            u64 seen = RtCapture::shadow_frame.load(std::memory_order_relaxed);
            if (seen != now && RtCapture::shadow_frame.compare_exchange_strong(seen, now)) {
                RtCapture::shadow_next.store(0, std::memory_order_relaxed);
            }
            const u32 sl = RtCapture::shadow_next.fetch_add(1, std::memory_order_relaxed);
            if (sl < RtCapture::kShadowDraws) {
                rt_shadow_slot = sl;
                RtCapture::shadow_keys[now % 8][sl] = rt_key;
                RtCapture::shadow_info[sl] = RtCapture::SunInfo{reinterpret_cast<u64>(static_cast<VkImageView>(pass.depth)), {fixed.viewports[0].x, fixed.viewports[0].y, fixed.viewports[0].width, fixed.viewports[0].height},
                                                                static_cast<u32>(a1), instances, kind, static_cast<u32>(fixed.primitive_type), ps_discard};
                const vk::Buffer xb{RtCapture::shadow_xfb};
                const vk::DeviceSize off = ((now % 8) * RtCapture::kShadowDraws + sl) * RtCapture::kSampleVerts * 16, size = RtCapture::kSampleVerts * 16;
                cmd.bindTransformFeedbackBuffersEXT(0, 1, &xb, &off, &size);
                cmd.beginTransformFeedbackEXT(0, 0, nullptr, nullptr);
            }
        }
    }
    { // test: TLG_VKB_PS_TOP=1: which pixel shaders draw the most triangles into the main picture (passes with 3+ colour targets)
        static const bool top_on = std::getenv("TLG_VKB_PS_TOP") != nullptr;
        if (top_on && pass.color_count >= 3 && fixed.depth_test && pass.depth) {
            static std::mutex m;
            static std::unordered_map<u64, std::pair<u64, u32>> tally;
            static u32 calls = 0;
            std::scoped_lock lock{m};
            auto& t = tally[ps_hash];
            t.first += static_cast<u64>(a1) * std::max<u32>(instances, 1) / 3;
            t.second++;
            if (++calls >= 150000) {
                std::vector<std::pair<u64, std::pair<u64, u32>>> v(tally.begin(), tally.end());
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                std::string line;
                for (size_t i = 0; i < std::min<size_t>(v.size(), 14); ++i) {
                    line += fmt::format(" {:08x}:{}k/{}", static_cast<u32>(v[i].first), v[i].second.first / 1000, v[i].second.second);
                }
                LOG_ERROR(Render_Vulkan, "PS top ({} shaders, {} targets {}x{}):{}", v.size(), pass.color_count, pass.extent.width, pass.extent.height, line);
                tally.clear();
                calls = 0;
            }
        }
    }
    if (rt_capture && !rt_tri) {
        (kind == 2 ? RtTrace::c_skip_ind : RtTrace::c_skip_prim).fetch_add(1, std::memory_order_relaxed);
    } else if (RtCapture::On() && !rt_capture && fixed.depth_test && pass.depth && !fixed.depth_write && pass.extent.width == RtCapture::main_w && pass.extent.height == RtCapture::main_h) {
        RtTrace::c_skip_nowrite.fetch_add(1, std::memory_order_relaxed);
    }
    if (rt_capture) {
        RtCapture::c_cap.fetch_add(1, std::memory_order_relaxed);
        RtCapture::slot_next.fetch_add(1, std::memory_order_relaxed);
        const u64 now = rt_present_frame.load(std::memory_order_relaxed);
        u32 pair = ~0u, at = ~0u;
        if (rt_tri && !fixed.rasterizer_discard && (RtTrace::fit_ok || now % 8 != 0)) {
            std::scoped_lock lock{RtCapture::sun_mutex};
            if (ps_discard) {
                if (RtCapture::cut_cursor + rt_verts <= u64{RtCapture::kSunVerts - RtCapture::kCutBase}) {
                    at = RtCapture::kCutBase + RtCapture::cut_cursor;
                    RtCapture::cut_cursor += static_cast<u32>(rt_verts);
                }
            } else if (RtCapture::sun_cursor + rt_verts <= u64{RtCapture::kCutBase}) {
                at = RtCapture::sun_cursor;
                RtCapture::sun_cursor += static_cast<u32>(rt_verts);
            }
        }
        if (at != ~0u) {
            const vk::Buffer xb{RtCapture::xfb};
            const vk::DeviceSize off = u64{at} * 16, size = rt_verts * 16;
            cmd.bindTransformFeedbackBuffersEXT(0, 1, &xb, &off, &size);
            cmd.beginTransformFeedbackEXT(0, 0, nullptr, nullptr);
            rt_shadow_slot = 0;
        } else if (pair != ~0u) {
            const vk::Buffer xb{RtCapture::pair_xfb};
            const vk::DeviceSize off = pair * 256, size = 240;
            cmd.bindTransformFeedbackBuffersEXT(0, 1, &xb, &off, &size);
            cmd.beginTransformFeedbackEXT(0, 0, nullptr, nullptr);
            rt_shadow_slot = 0;
        } else if (kind != 2 && now % 8 == 0 && !RtTrace::fit_ok) {
            u64 seen = RtCapture::cam_frame.load(std::memory_order_relaxed);
            if (seen != now && RtCapture::cam_frame.compare_exchange_strong(seen, now)) {
                RtCapture::cam_next.store(0, std::memory_order_relaxed);
            }
            const u32 sl = RtCapture::cam_next.fetch_add(1, std::memory_order_relaxed);
            if (sl < RtCapture::kMainDraws) {
                rt_shadow_slot = sl; // ends the same way as a sun-view sample
                RtCapture::cam_keys[now % 8][sl] = rt_key;
                const vk::Buffer xb{RtCapture::cam_xfb};
                const vk::DeviceSize off = ((now % 8) * RtCapture::kMainDraws + sl) * RtCapture::kSampleVerts * 16, size = RtCapture::kSampleVerts * 16;
                cmd.bindTransformFeedbackBuffersEXT(0, 1, &xb, &off, &size);
                cmd.beginTransformFeedbackEXT(0, 0, nullptr, nullptr);
            }
        }
    }
    if (no_draw) {
    } else if (kind == 0) {
        if (state.index_buffer != static_cast<VkBuffer>(index_buffer) || state.index_offset != index_offset || state.index_type != index_type) {
            cmd.bindIndexBuffer(index_buffer, index_offset, index_type);
            state.index_buffer = static_cast<VkBuffer>(index_buffer), state.index_offset = index_offset, state.index_type = index_type;
        }
        cmd.drawIndexed(static_cast<u32>(a1), instances, static_cast<u32>(a2), static_cast<s32>(static_cast<u32>(a3)), 0);
    } else if (kind == 2) {
        if (state.index_buffer != static_cast<VkBuffer>(index_buffer) || state.index_offset != index_offset || state.index_type != index_type) {
            cmd.bindIndexBuffer(index_buffer, index_offset, index_type);
            state.index_buffer = static_cast<VkBuffer>(index_buffer), state.index_offset = index_offset, state.index_type = index_type;
        }
        cmd.drawIndexedIndirect(args_buffer, args_offset, 1, 20);
    } else {
        cmd.draw(static_cast<u32>(a1), instances, static_cast<u32>(a2), 0);
    }
    if (rt_shadow_slot != ~0u) {
        cmd.endTransformFeedbackEXT(0, 0, nullptr, nullptr);
    }
    if (rt_sun_set && RtCapture::On()) {
        RtTrace::NoteSun(rt_sun);
    }
    if (rt_sun_set && RtCapture::On() && pass.color_count > 0 && RtTrace::SunOn()) {
        Stream::BreakPass(cmd);
        RtTrace::SunShadow(cmd, pass.colors[0], static_cast<vk::Format>(color_format0), pass.extent.width, pass.extent.height, rt_sun);
        rt_state_dirty = true;
    }
    if (!dump_views.empty()) {
        Stream::BreakPass(cmd);
        for (const auto& [id, name] : dump_views) {
            Core::Vkb::DumpView(cmd, id, name);
        }
        for (u32 i = 0; dump_buffers && i < b.buffers.size(); ++i) {
            if (b.buffers[i].range != VK_WHOLE_SIZE) {
                Core::Vkb::DumpBuffer(cmd, b.buffers[i].buffer, b.buffers[i].offset, b.buffers[i].range, fmt::format("g{}", i));
            }
        }
    }
}

} // namespace

void Draw(u64 ctx, u32 kind, u64 a1, u64 a2, u64 a3) {
    c_draws.fetch_add(1, std::memory_order_relaxed);
    // sensitivity experiment: TLG_VKB_SPIN_NS=N spends N ns more per draw on the render thread (how much of its time is on the frame's critical path)
    static const u64 spin_ns = [] { const char* e = std::getenv("TLG_VKB_SPIN_NS"); return e != nullptr ? std::strtoull(e, nullptr, 10) : 0ull; }();
    if (spin_ns != 0) {
        const u64 until = NowNs() + spin_ns;
        while (NowNs() < until) {
        }
    }
    c_by_kind[std::min<u32>(kind, 2)].fetch_add(1, std::memory_order_relaxed);
    const u64 vs = Rd<u64>(ctx, CtxVertexShader);
    const u64 ps = Rd<u64>(ctx, CtxPixelShader);
    if (vs == 0 || !IsHeapPointer(vs) || Rd<u64>(vs, 0x120) == 0 || (ps != 0 && (!IsHeapPointer(ps) || Rd<u64>(ps, 0x120) == 0))) {
        c_no_vs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // a vertex shader variant, a hull / domain or geometry stage, a dispatch draw: the pipeline is vertex plus pixel only (counted)
    if (Rd<u64>(vs, 0x420) != 0 || Rd<u64>(ctx, 0x4680) != 0 || Rd<u64>(ctx, 0x4688) != 0 || Rd<u32>(ctx, 0x130c8) == 0x200) {
        c_special.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 fs0 = TimesOn() ? NowNs() : 0;
    const VkbVulkan::FixedState* fixed = Core::NativeDraw::FixedStateOf(ctx);
    if (TimesOn()) {
        c_fixed_ns.fetch_add(NowNs() - fs0, std::memory_order_relaxed);
    }
    if (fixed == nullptr) {
        c_no_fixed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 fetch = Core::NativeDraw::FetchObjectOf(ctx, vs);
    const u64 k0 = TimesOn() ? NowNs() : 0;
    const u64 key = VariantKey(ctx, vs, ps, fetch, fixed, kind == 2);
    if (TimesOn()) {
        c_key_ns.fetch_add(NowNs() - k0, std::memory_order_relaxed);
    }
    const u64 vl0 = TimesOn() ? NowNs() : 0;
    std::scoped_lock lock{mutex};
    if (!Ready()) {
        return;
    }
    auto [it, fresh] = variants.try_emplace(key);
    if (TimesOn()) {
        c_variant_find_ns.fetch_add(NowNs() - vl0, std::memory_order_relaxed);
    }
    if (!fresh) {
        c_found.fetch_add(1, std::memory_order_relaxed);
    } else {
        it->second = std::make_unique<Variant>();
        Compile(ctx, vs, ps, fetch, *fixed, *it->second, kind == 2);
    }
    if (!it->second->ok) {
        c_skip_not_compiled.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static const bool no_draw_record = [] { const char* e = std::getenv("TLG_VKB_NO_DRAW_RECORD"); return e != nullptr && *e == '1'; }(); // kill experiment: variant found, nothing recorded
    // note 62 bound: TLG_VKB_NO_DRAW_RECORD_AFTER=N: the same kill experiment from graphics submit N on (after the load, which the plain switch does not survive)
    static const u64 no_record_after = [] { const char* e = std::getenv("TLG_VKB_NO_DRAW_RECORD_AFTER"); return e != nullptr ? std::strtoull(e, nullptr, 10) : 0ull; }();
    if (!no_draw_record && !(no_record_after != 0 && Core::Vkb::GraphicsSubmits() >= no_record_after)) {
        Record(ctx, *it->second, *fixed, kind, a1, a2, a3);
    }
}

nlohmann::json TargetCensusReport() {
    std::scoped_lock lock{target_census_mutex};
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [line, n] : target_census) {
        j[line] = n;
    }
    return j;
}
nlohmann::json Report() {
    const u64 compiled = c_compiled.load(), draws = c_draws.load();
    nlohmann::json fails = nlohmann::json::object();
    for (u32 i = 0; i < FailCount; ++i) {
        fails[FailNames[i]] = c_fail[i].load();
    }
    const auto src = [&](u32 stage) {
        return nlohmann::json{{"buffers", {c_src[stage][0][0].load(), c_src[stage][0][1].load()}},
                              {"images", {c_src[stage][1][0].load(), c_src[stage][1][1].load()}},
                              {"samplers", {c_src[stage][2][0].load(), c_src[stage][2][1].load()}}};
    };
    return {{"draws", draws},
            {"by_kind_indexed_auto_indirect", {c_by_kind[0].load(), c_by_kind[1].load(), c_by_kind[2].load()}},
            {"no_vertex_shader", c_no_vs.load()},
            {"special_pipeline", c_special.load()},
            {"no_fixed_state", c_no_fixed.load()},
            {"variant_found", c_found.load()},
            {"variants", compiled + c_failed.load()},
            {"compiled", compiled},
            {"failed", c_failed.load()},
            {"failed_by", fails},
            {"unsupported_layout_type_bits", fmt::format("{:#x}", c_unsupported_layout_bits.load())},
            {"depth_only_variants", c_depth_only.load()},
            {"compile_ms_mean", compiled ? static_cast<double>(c_compile_ns.load()) / 1e6 / static_cast<double>(compiled) : 0.0},
            {"key_check_draws_differences", {c_key_checks.load(), c_key_differences.load()}},
            {"variant_key_us_mean", draws ? static_cast<double>(c_key_ns.load()) / 1e3 / static_cast<double>(draws) : 0.0},
            {"slot_map_mapped_unmapped", {{"vertex", src(0)}, {"pixel", src(1)}}},
            {"vertex_attributes_mapped_unmapped", {c_attr_mapped.load(), c_attr_unmapped.load()}},
            {"recorded", c_recorded.load()},
            {"record_us_mean", c_recorded.load() ? static_cast<double>(c_record_ns.load()) / 1e3 / static_cast<double>(c_recorded.load()) : 0.0},
            {"record_phase_us_mean_targets_stages_vertex_handover", [&] {
                 nlohmann::json a = nlohmann::json::array();
                 for (u32 i = 0; i < 4; ++i) {
                     a.push_back(c_recorded.load() ? static_cast<double>(c_phase_ns[i].load()) / 1e3 / static_cast<double>(c_recorded.load()) : 0.0);
                 }
                 return a;
             }()},
            {"fixed_state_us_mean", draws ? static_cast<double>(c_fixed_ns.load()) / 1e3 / static_cast<double>(draws) : 0.0},
            {"r2b_on_late_buffers_failed", {ResolveOnRecorder(), c_late_buffers.load(), c_late_failed.load()}},
            {"r2b2b_on_late_targets_images", {LateImagesOn(), c_late_targets.load(), c_late_images.load()}},
            {"target_census", TargetCensusReport()},
            {"flat_census", Core::Vkb::FlatCensus::Report()},
            {"d3_targets_engine_unmapped", {c_targets_engine.load(), c_targets_unmapped.load()}},
            {"d3_target_check_compares_differences", {c_target_checks.load(), c_target_check_differences.load()}},
            {"d3_target_check_first", [] { std::scoped_lock lock{target_check_mutex}; return nlohmann::json(target_check_lines); }()},
            {"state_filter_sent_skipped_pipelines_vertex_inputs", {c_state_sent.load(), c_state_skipped.load(), c_state_pipelines.load(), c_state_vertex_inputs.load()}},
            {"stage_census_vs_ps_same_changed_buffers_images_samplers_same", {{c_stage_same[0].load(), c_stage_changed[0].load(), c_stage_buffers_same[0].load(), c_stage_images_same[0].load(), c_stage_samplers_same[0].load()},
                                                                               {c_stage_same[1].load(), c_stage_changed[1].load(), c_stage_buffers_same[1].load(), c_stage_images_same[1].load(), c_stage_samplers_same[1].load()}}},
            {"variant_find_us_mean", draws ? static_cast<double>(c_variant_find_ns.load()) / 1e3 / static_cast<double>(draws) : 0.0},
            {"lookups", Core::Vkb::LookupReport()},
            {"recorder_job_us_mean", c_job_n.load() ? static_cast<double>(c_job_ns.load()) / 1e3 / static_cast<double>(c_job_n.load()) : 0.0},
            {"new_passes", c_new_passes.load()},
            {"indirect_recorded", c_indirect_recorded.load()},
            {"views_noted_at_first_use", c_late_views.load()},
            {"flat_user_data_rebuilt", c_flat_rebuilt.load()},
            {"skipped", {{"not_compiled", c_skip_not_compiled.load()}, {"buffer", c_skip_buffer.load()}, {"image", c_skip_image.load()}, {"sampler", c_skip_sampler.load()},
                         {"unordered_access", c_skip_uav.load()}, {"special_buffer", c_skip_special.load()}, {"color_target", c_skip_target.load()},
                         {"depth_target", c_skip_depth.load()}, {"no_attachment", c_skip_no_attachment.load()}, {"vertex_input", c_skip_vertex.load()},
                         {"index_buffer", c_skip_index.load()}, {"indirect", c_skip_indirect.load()}}}};
}

} // namespace Core::Vkb::Graphics

/// RT prototype: the presenter's frame tick; every 120 frames the captured vertex count of a frame a few frames back goes to the log
void TlgVkbRtOnPresent() {
    const u64 f = Core::Vkb::Graphics::rt_present_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool rt_log = f % 30 == 0;
    if (!Core::Vkb::Graphics::RtCapture::ready || f < 4) {
        return;
    }
    const u32 bytes = Core::Vkb::Graphics::RtCapture::BytesOfFrame(f - 4);
    if (rt_log) LOG_ERROR(Render_Vulkan, "RT draws over the last 120 presents: captured {} all {} depth-tested {} (main size {}x{})", Core::Vkb::Graphics::RtCapture::c_cap.exchange(0), Core::Vkb::Graphics::RtCapture::c_all.exchange(0),
              Core::Vkb::Graphics::RtCapture::c_dw.exchange(0), Core::Vkb::Graphics::RtCapture::main_w, Core::Vkb::Graphics::RtCapture::main_h);
    namespace G = Core::Vkb::Graphics;
    {
        const float* v = G::RtCapture::cam_ptr; // the camera samples of the last sampled frame (ring place 0)
        // one fit per sampled draw (draws differ in near plane); the value most draws share wins
        double n = 0;
        std::vector<std::pair<float, float>> fits;
        for (u32 slot = 0; slot < G::RtCapture::kSampleSlots; ++slot) {
            double m = 0, sw = 0, sz = 0, sww = 0, swz = 0;
            for (u32 i = 0; i < G::RtCapture::kSampleVerts; ++i) {
                const float* x = v + (slot * G::RtCapture::kSampleVerts + i) * 4;
                const double z = x[2], w = x[3];
                if (w > 0.001 && std::isfinite(z) && std::isfinite(w)) {
                    m += 1, sw += w, sz += z, sww += w * w, swz += w * z;
                }
            }
            const double det = m * sww - sw * sw;
            if (m >= 3 && std::abs(det) > 1e-9) {
                const double fa = (m * swz - sw * sz) / det;
                fits.emplace_back(static_cast<float>(fa), static_cast<float>((sz - fa * sw) / m));
                n += m;
            }
        }
        u32 best = 0, best_votes = 0;
        for (u32 i = 0; i < fits.size(); ++i) {
            u32 votes = 0;
            for (const auto& f2 : fits) {
                votes += std::abs(f2.second - fits[i].second) < std::abs(fits[i].second) * 0.05f ? 1 : 0;
            }
            if (votes > best_votes) {
                best = i, best_votes = votes;
            }
        }
        static const float fixed_b = [] { const char* e = std::getenv("TLG_VKB_RT_NEAR"); return e != nullptr ? -static_cast<float>(std::atof(e)) : 0.0f; }();
        if (!fits.empty()) {
            G::RtTrace::fit_a = fits[best].first;
            G::RtTrace::fit_b = fixed_b != 0.0f ? fixed_b : fits[best].second;
            G::RtTrace::fit_ok = true;
        }
        {
            // the sun direction in (x_clip, y_clip, w) space, from draws seen both by the camera and by the sun view (shadow map): the affine map camera -> sun view;
            // the direction along which sun-view x and y stay the same is the light direction (cross product of the two rows); sign: toward smaller sun-view depth
            const u64 fr = (f - 4) % 8;
            const u64 fm = (f - 4) % G::RtCapture::kSlots;
            if (fr == 0) {
            const float* sv = G::RtCapture::shadow_ptr + fr * G::RtCapture::kShadowDraws * G::RtCapture::kSampleVerts * 4;
            const float* mv = G::RtCapture::cam_ptr + fr * G::RtCapture::kMainDraws * G::RtCapture::kSampleVerts * 4;
            std::unordered_map<u64, u32> main_slot;
            for (u32 i = 0; i < G::RtCapture::kMainDraws; ++i) {
                if (G::RtCapture::cam_keys[fr][i] != 0) {
                    main_slot.emplace(G::RtCapture::cam_keys[fr][i], i);
                }
            }
            std::unordered_map<u64, u32> main_count;
            for (u32 i = 0; i < G::RtCapture::kMainDraws; ++i) {
                if (G::RtCapture::cam_keys[fr][i] != 0) {
                    ++main_count[G::RtCapture::cam_keys[fr][i]];
                }
            }
            // one fit per matched pair (the sun map holds several cascades: one object, several sun matrices, but the same light direction);
            // objects the camera draws more than once are skipped (which copy pairs with which is unknown); fits that do not close are dropped
            double acc[3]{};
            u32 matched = 0, tried = 0;
            double rms_sum = 0;
            std::memset(G::RtCapture::shadow_fit_ok, 0, sizeof(G::RtCapture::shadow_fit_ok));
            for (u32 k = 0; k < G::RtCapture::kShadowDraws; ++k) {
                const u64 key = G::RtCapture::shadow_keys[fr][k];
                const auto it = main_slot.find(key);
                if (key == 0 || it == main_slot.end() || main_count[key] != 1) {
                    continue;
                }
                double mtx[4][7]{}, so2[3]{};
                u32 used = 0;
                for (u32 i = 0; i < G::RtCapture::kSampleVerts; ++i) {
                    const float* m = mv + (it->second * G::RtCapture::kSampleVerts + i) * 4;
                    const float* t = sv + (k * G::RtCapture::kSampleVerts + i) * 4;
                    if (!(m[3] > 0.001f) || !std::isfinite(m[0]) || !std::isfinite(t[0]) || std::abs(t[3] - 1.0f) > 1e-3f) { // the sun view is orthographic (w = 1); perspective 2048 views are lamps
                        continue;
                    }
                    const double r[4]{m[0], m[1], m[3], 1.0};
                    const double o[3]{t[0], t[1], t[2]};
                    for (u32 x = 0; x < 4; ++x) {
                        for (u32 y = 0; y < 4; ++y) {
                            mtx[x][y] += r[x] * r[y];
                        }
                        for (u32 c = 0; c < 3; ++c) {
                            mtx[x][4 + c] += r[x] * o[c];
                        }
                    }
                    for (u32 c = 0; c < 3; ++c) {
                        so2[c] += o[c] * o[c];
                    }
                    ++used;
                }
                if (used < 8) {
                    continue;
                }
                ++tried;
                double orig[4][7];
                std::memcpy(orig, mtx, sizeof(mtx));
                bool ok = true;
                for (u32 c = 0; c < 4 && ok; ++c) {
                    u32 piv = c;
                    for (u32 r2 = c + 1; r2 < 4; ++r2) {
                        if (std::abs(mtx[r2][c]) > std::abs(mtx[piv][c])) {
                            piv = r2;
                        }
                    }
                    if (std::abs(mtx[piv][c]) < 1e-9 * (1.0 + std::abs(orig[c][c]))) {
                        ok = false;
                        break;
                    }
                    for (u32 c2 = 0; c2 < 7; ++c2) {
                        std::swap(mtx[c][c2], mtx[piv][c2]);
                    }
                    for (u32 r2 = 0; r2 < 4; ++r2) {
                        if (r2 != c) {
                            const double q = mtx[r2][c] / mtx[c][c];
                            for (u32 c2 = 0; c2 < 7; ++c2) {
                                mtx[r2][c2] -= q * mtx[c][c2];
                            }
                        }
                    }
                }
                if (!ok) {
                    continue;
                }
                double row[3][3], rms[3];
                for (u32 c = 0; c < 3; ++c) {
                    double fit = 0;
                    for (u32 r2 = 0; r2 < 4; ++r2) {
                        fit += mtx[r2][4 + c] / mtx[r2][r2] * orig[r2][4 + c];
                    }
                    rms[c] = std::sqrt(std::max(0.0, so2[c] - fit) / used);
                    for (u32 r2 = 0; r2 < 3; ++r2) {
                        row[c][r2] = mtx[r2][4 + c] / mtx[r2][r2];
                    }
                }
                if (!(rms[0] < 0.002 && rms[1] < 0.002)) {
                    continue;
                }
                const double d[3]{row[0][1] * row[1][2] - row[0][2] * row[1][1], row[0][2] * row[1][0] - row[0][0] * row[1][2], row[0][0] * row[1][1] - row[0][1] * row[1][0]};
                const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (!(len > 1e-12)) {
                    continue;
                }
                const double sgn = (d[0] * row[2][0] + d[1] * row[2][1] + d[2] * row[2][2]) > 0 ? -1.0 : 1.0;
                for (u32 c = 0; c < 3; ++c) {
                    acc[c] += sgn * d[c] / len;
                }
                rms_sum += rms[0] + rms[1];
                for (u32 c = 0; c < 3; ++c) {
                    for (u32 r2 = 0; r2 < 4; ++r2) {
                        G::RtCapture::shadow_fit[k][c * 4 + r2] = mtx[r2][4 + c] / mtx[r2][r2];
                    }
                }
                G::RtCapture::shadow_fit_ok[k] = true;
                G::RtTrace::sun_rms[0] = rms[0], G::RtTrace::sun_rms[1] = rms[1], G::RtTrace::sun_rms[2] = rms[2];
                G::RtTrace::sun_rows[0] = row[0][0], G::RtTrace::sun_rows[1] = row[0][1], G::RtTrace::sun_rows[2] = row[0][2];
                G::RtTrace::sun_rows[3] = row[1][0], G::RtTrace::sun_rows[4] = row[1][1], G::RtTrace::sun_rows[5] = row[1][2];
                ++matched;
            }
            if (rt_log) {
                LOG_ERROR(Render_Vulkan, "RT sun per pair: {} tried, {} clean", tried, matched);
            }
            static u32 sun_dumps = 0;
            if (G::RtCapture::sun_view.load(std::memory_order_relaxed) == 0 && G::RtCapture::shadow_next.load(std::memory_order_relaxed) > 20) {
                ++sun_dumps;
                struct Grp {
                    u32 draws = 0, ortho = 0, persp = 0, discard = 0, clean = 0, indirect = 0, first = ~0u;
                    u64 indices = 0;
                    double dev = 0;
                    u32 order = 0;
                };
                std::map<std::tuple<u64, float, float, float, float>, Grp> grp;
                u32 order = 0;
                const u32 total = std::min<u32>(G::RtCapture::shadow_next.load(std::memory_order_relaxed), G::RtCapture::kShadowDraws);
                for (u32 k = 0; k < total; ++k) {
                    const auto& in = G::RtCapture::shadow_info[k];
                    auto [gi, fresh] = grp.try_emplace({in.view, in.vp[0], in.vp[1], in.vp[2], in.vp[3]});
                    Grp& g = gi->second;
                    if (fresh) {
                        g.order = order++;
                    }
                    ++g.draws;
                    g.indices += static_cast<u64>(in.indices) * std::max<u32>(in.inst, 1);
                    g.discard += in.discard;
                    g.indirect += in.kind == 2;
                    const float w0 = sv[k * G::RtCapture::kSampleVerts * 4 + 3];
                    (std::abs(w0 - 1.0f) < 1e-3f ? g.ortho : g.persp) += 1;
                    if (G::RtCapture::shadow_fit_ok[k]) {
                        ++g.clean;
                        if (g.first == ~0u) {
                            g.first = k;
                        } else {
                            for (u32 c = 0; c < 12; ++c) {
                                g.dev = std::max(g.dev, std::abs(G::RtCapture::shadow_fit[k][c] - G::RtCapture::shadow_fit[g.first][c]));
                            }
                        }
                    }
                }
                for (const auto& [key, g] : grp) {
                    std::string m;
                    if (g.first != ~0u) {
                        for (u32 c = 0; c < 12; ++c) {
                            m += fmt::format(" {:.5f}", G::RtCapture::shadow_fit[g.first][c]);
                        }
                    }
                    LOG_ERROR(Render_Vulkan, "RT sunpass #{}: view {:#x} viewport {} {} {} {}: draws {} indices {} ortho {} persp {} cutout {} indirect {} prim {} clean fits {} (spread {:.5f}) M ={}", g.order,
                              std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), std::get<4>(key), g.draws, g.indices, g.ortho, g.persp, g.discard, g.indirect,
                              G::RtCapture::shadow_info[g.first != ~0u ? g.first : 0].prim, g.clean, g.dev, m);
                }
                // the sun pass with the most draws among the parallel-light ones (the far cascade: the whole shadow scene)
                u64 pick = 0;
                u32 pick_draws = 0;
                for (const auto& [key, g] : grp) {
                    if (g.ortho > g.persp && g.draws > pick_draws) {
                        pick = std::get<0>(key), pick_draws = g.draws;
                    }
                }
                if (pick != 0) {
                    G::RtCapture::sun_view.store(pick, std::memory_order_relaxed);
                    LOG_ERROR(Render_Vulkan, "RT sun pass chosen: view {:#x} ({} draws)", pick, pick_draws);
                }
            }
            if (matched < 3) {
                matched = 0;
            }
            const double al = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
            if (matched > 0 && al > 1e-6) {
                // smoothed over time (the matched set changes per frame): about 0.5 s
                const float k = G::RtTrace::sun_ok ? 0.03f : 1.0f;
                float sd[3];
                for (u32 c = 0; c < 3; ++c) {
                    sd[c] = G::RtTrace::sun_dir[c] + k * (static_cast<float>(acc[c] / al) - G::RtTrace::sun_dir[c]);
                }
                const float sl = std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
                for (u32 c = 0; c < 3; ++c) {
                    G::RtTrace::sun_dir[c] = sd[c] / sl;
                }
                G::RtTrace::sun_ok = true;
            }
            if (rt_log) {
                std::unordered_map<u64, u32> anyf;
                u32 mains = 0;
                for (u32 q = 0; q < 8; ++q) {
                    for (u32 i = 0; i < 256; ++i) {
                        if (G::RtCapture::main_keys[q][i] != 0) {
                            anyf.emplace(G::RtCapture::main_keys[q][i], q);
                            mains += q == fr;
                        }
                    }
                }
                u32 hit_any = 0, sh = 0;
                std::string fs;
                for (u32 k = 0; k < G::RtCapture::kShadowDraws; ++k) {
                    const u64 key = G::RtCapture::shadow_keys[fr][k];
                    sh += key != 0;
                    const auto it = anyf.find(key);
                    if (key != 0 && it != anyf.end()) {
                        ++hit_any;
                        if (fs.size() < 60) fs += std::to_string(it->second) + " ";
                    }
                }
                LOG_ERROR(Render_Vulkan, "RT sun keys: frame {} main keys {} shadow keys {} found in any frame {} (frames {})", fr, mains, sh, hit_any, fs);
                LOG_ERROR(Render_Vulkan, "RT sun fit: error x {:.4f} y {:.4f} z {:.4f}; sun x = {:.4f} {:.4f} {:.4f}, sun y = {:.4f} {:.4f} {:.4f}", G::RtTrace::sun_rms[0], G::RtTrace::sun_rms[1], G::RtTrace::sun_rms[2], G::RtTrace::sun_rows[0], G::RtTrace::sun_rows[1], G::RtTrace::sun_rows[2], G::RtTrace::sun_rows[3], G::RtTrace::sun_rows[4], G::RtTrace::sun_rows[5]);
                LOG_ERROR(Render_Vulkan, "RT sun: {} matched draws ({} sun-view draws), direction {:.3f} {:.3f} {:.3f} (agreement {:.2f})", matched,
                          G::RtCapture::shadow_next.load(std::memory_order_relaxed), G::RtTrace::sun_dir[0], G::RtTrace::sun_dir[1], G::RtTrace::sun_dir[2],
                          matched > 0 ? al / matched : 0.0);
            }
            }
        }
        if (rt_log) {
            const float* r = G::RtCapture::fit_read_ptr + G::RtCapture::last_ring.load(std::memory_order_relaxed) * 16;
            LOG_ERROR(Render_Vulkan, "RT v2: sun triangles {} pairs {} (kept {}, worst error {:.5f}, valid {}) frames built {} empty {}; map x = {:.5f} {:.5f} {:.5f} {:.5f}, y = {:.5f} {:.5f} {:.5f} {:.5f}, z = {:.6f} {:.6f} {:.6f} {:.5f}",
                      G::RtCapture::last_tris.load(std::memory_order_relaxed), G::RtCapture::last_pairs.load(std::memory_order_relaxed), r[12], r[13], r[15],
                      G::RtCapture::c_built.exchange(0), G::RtCapture::c_empty.exchange(0), r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
        }
        std::string all;
        for (const auto& f2 : fits) {
            all += fmt::format(" {:.4f}", f2.second);
        }
        if (rt_log) LOG_ERROR(Render_Vulkan, "RT fit per draw (B):{} -> chosen B {} ({} votes)", all, G::RtTrace::fit_b, best_votes);
        const u32 raw = G::RtTrace::probe_ptr != nullptr ? G::RtTrace::probe_ptr[0] : 0;
        float as_float = 0;
        std::memcpy(&as_float, &raw, 4);
        LOG_ERROR(Render_Vulkan, "RT fit: z = {} * w + {} from {} vertices ({}), traces {}, depth probe raw {:#x} float {} unorm24 {}", G::RtTrace::fit_a, G::RtTrace::fit_b, n,
                  G::RtTrace::fit_ok ? "ok" : "none", G::RtTrace::c_traces.exchange(0), raw, as_float, (raw & 0xffffff) / 16777215.0);
    }
    if (rt_log) LOG_ERROR(Render_Vulkan, "RT capture: frame {} vertices {} ({} triangles), draws {}", f - 4, bytes / 16, bytes / 48, Core::Vkb::Graphics::RtCapture::draws_last.load(std::memory_order_relaxed));
}

/// RT prototype: the shadow pass on the frame's final picture (presenter, after all of the frame's draws were submitted)
void TlgVkbRtTraceDisplay(vk::CommandBuffer cmd, vk::ImageView view, vk::Format format, u32 w, u32 h) {
    namespace G = Core::Vkb::Graphics;
    if (!G::RtCapture::On() || !G::RtCapture::ready || G::RtCapture::slot_next.load(std::memory_order_relaxed) == 0) {
        return;
    }
    G::RtTrace::Shade(cmd, view, format, w, h);
    G::RtTrace::c_traces.fetch_add(1, std::memory_order_relaxed);
}
