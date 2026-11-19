// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_fsr4.h"

#include <array>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "ffx_vk_fsr4_v07.h"
#include "ffx_vk_fsr4_v07_assets.h"
#include "native/native_vkb_stream.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"

namespace Core::Vkb::Fsr4 {
namespace {

std::vector<u8> scratch;
FfxInterface backend{};
bool backend_ok = false;
ffxContext context{};
bool context_ok = false;
u64 next_frame_id = 1;
std::deque<std::pair<u64, u64>> in_flight; ///< provider frame id, the stream timeline value its commands go out with
std::string problem;

/// Their AssetDir: TLG_FSR4_DIR (the launcher sets it to the packaged folder), else fsr4_shaders in the working folder
std::string AssetDir() {
    const char* dir = std::getenv("TLG_FSR4_DIR");
    return dir != nullptr && dir[0] != 0 ? dir : "fsr4_shaders";
}

bool ReadFile(const std::string& path, std::vector<u8>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    data.resize(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())));
}

void Retire() {
    const u64 done = Core::Vkb::Stream::CompletedValue();
    while (!in_flight.empty() && in_flight.front().second <= done) {
        ffxFsr4VkRetireFrame(&backend, in_flight.front().first);
        in_flight.pop_front();
    }
}

VkResult Register(const Image& image, VkAccessFlags access) {
    const FfxFsr4VkExternalImageState state{
        .structSize = sizeof(FfxFsr4VkExternalImageState),
        .image = image.image,
        .view = image.view,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        .accessMask = access,
        .restoreLayout = VK_IMAGE_LAYOUT_GENERAL,
        .restoreStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        .restoreAccessMask = access,
    };
    return ffxFsr4VkSetExternalImageState(&backend, &state);
}

} // namespace

const std::string& Problem() {
    return problem;
}

void Destroy() {
    if (context_ok) {
        ffxFsr4V07DestroyContext(&context, nullptr);
        context_ok = false;
    }
    if (backend_ok) {
        ffxFsr4VkDestroyContext(reinterpret_cast<FfxFsr4VkContext*>(scratch.data()));
        backend_ok = false;
    }
    backend = {};
    in_flight.clear();
}

Start Create(const VkbVulkan::Instance& instance, u32 w, u32 h) {
    Destroy();
    problem.clear();
    if (!instance.IsFsr4Int8Supported()) {
        problem = "the GPU lacks the INT8 dot product / compute derivative support FSR 4 needs";
        return Start::Fault;
    }
    // their CreateBackend, the native model only (FFX_FSR4_MODEL_PRESET_NATIVE_AA: the packages ship it, the only one native AA loads)
    FfxFsr4V07AssetSet assets{};
    if (!ffxFsr4V07BuildAssetSet(FFX_FSR4_MODEL_PRESET_NATIVE_AA, w, h, &assets)) {
        problem = fmt::format("the output size {}x{} is not supported by the v07 model", w, h);
        return Start::Fault;
    }
    const std::string dir = AssetDir() + "/";
    std::array<std::vector<u8>, FFX_FSR4_VK_PASS_COUNT> code;
    std::vector<u8> initializer, weights;
    // their rule: tools/fsr4_optimize.sh puts fixed or faster passes into opt/; TLG_FSR4_OPT=0 keeps the originals
    const char* opt_env = std::getenv("TLG_FSR4_OPT");
    const bool use_opt = !(opt_env != nullptr && opt_env[0] == '0');
    u32 optimized = 0;
    bool missing = false;
    const auto load = [&](const char* name, std::vector<u8>& data) {
        if (use_opt && ReadFile(dir + "opt/" + name, data)) {
            ++optimized;
            return true;
        }
        if (ReadFile(dir + name, data)) {
            return true;
        }
        problem = "missing " + dir + name;
        missing = true;
        return false;
    };
    bool loaded = load(assets.pre, code[0]);
    for (u32 pass = 0; loaded && pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
        loaded = load(assets.model[pass], code[1 + pass]);
    }
    loaded = loaded && load(assets.post, code[13]) && load(assets.rcas, code[14]) && load(assets.spdAutoExposure, code[15]) &&
             load(assets.initializer, initializer) && load(assets.prePassWeights, weights);
    if (!loaded) {
        return missing ? Start::NoModel : Start::Fault;
    }
    if (initializer.size() != FFX_FSR4_V07_INITIALIZER_BYTES || weights.size() != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES) {
        problem = "the model weights have an unexpected size";
        return Start::Fault;
    }
    static const std::array<std::string, FFX_FSR4_VK_PASS_COUNT> entries = [] {
        std::array<std::string, FFX_FSR4_VK_PASS_COUNT> names;
        names.fill("main");
        for (u32 pass = 1; pass <= FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
            names[pass] = "fsr4_model_v07_i8_pass" + std::to_string(pass);
        }
        return names;
    }();
    FfxFsr4VkCreateInfo ci{};
    ci.device = instance.GetDevice();
    ci.physicalDevice = instance.GetPhysicalDevice();
    for (u32 i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
        if (code[i].size() % 4 != 0) {
            problem = "an FSR 4 SPIR-V file is invalid";
            return Start::Fault;
        }
        ci.shaders[i] = {reinterpret_cast<const uint32_t*>(code[i].data()), code[i].size(), entries[i].c_str()};
    }
    ci.modelInitializer = initializer.data();
    ci.modelInitializerSize = initializer.size();
    ci.prePassWeights = weights.data();
    ci.prePassWeightsSize = weights.size();
    scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
    ci.scratchBuffer = scratch.data();
    ci.scratchBufferSize = scratch.size();
    if (const VkResult result = ffxFsr4VkCreateContext(&ci, &backend); result != VK_SUCCESS) {
        problem = fmt::format("the FSR 4 Vulkan backend could not be made ({})", static_cast<int>(result));
        backend = {};
        return Start::Fault;
    }
    backend_ok = true;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE; // native AA runs on the HDR scene colour
    desc.maxRenderSize = {(w + 7) & ~7u, (h + 7) & ~7u};
    desc.maxUpscaleSize = {(w + 7) & ~7u, (h + 7) & ~7u};
    ffxFsr4V07SetBackendInterface(&backend);
    const auto created = ffxFsr4V07CreateContext(&context, &desc.header, nullptr);
    ffxFsr4V07SetBackendInterface(nullptr);
    if (created != FFX_API_RETURN_OK) {
        problem = fmt::format("the FSR 4 provider context could not be made ({})", static_cast<int>(created));
        Destroy();
        return Start::Fault;
    }
    context_ok = true;
    LOG_INFO(Render_Vulkan, "vkb aa: FSR 4 v07 INT8 native model, {} tier, {}x{}{}", assets.tier, w, h,
             optimized != 0 ? fmt::format(", {} passes from {}opt/", optimized, dir) : std::string{});
    return Start::Ok;
}

bool Record(const Frame& f) {
    if (!context_ok) {
        problem = "FSR 4 was not made";
        return false;
    }
    Retire();
    const u64 frame_id = next_frame_id++;
    VkResult begin = ffxFsr4VkBeginFrame(&backend, frame_id);
    // their rule: all provider frames busy, wait for the oldest (only one already submitted: the one being recorded cannot be waited for here)
    while (begin == VK_NOT_READY && !in_flight.empty() && in_flight.front().second <= Core::Vkb::Stream::LastValue()) {
        Core::Vkb::Stream::Wait(in_flight.front().second);
        Retire();
        begin = ffxFsr4VkBeginFrame(&backend, frame_id);
    }
    if (begin != VK_SUCCESS) {
        problem = fmt::format("no free FSR 4 provider frame ({})", static_cast<int>(begin));
        return false;
    }
    const u64 value = Core::Vkb::Stream::RecordingValue();
    // their note: a frame begun must reach RetireFrame even when it records nothing, or every later BeginFrame fails
    const auto abandon = [&] { in_flight.emplace_back(frame_id, value); };
    constexpr VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
    const std::array<std::pair<const Image*, VkAccessFlags>, 4> images{{
        {&f.color, read},
        {&f.depth, read},
        {&f.motion, read},
        {&f.output, read | VK_ACCESS_SHADER_WRITE_BIT},
    }};
    static constexpr std::array<const char*, 4> names{"colour", "depth", "motion", "output"};
    for (u32 i = 0; i < images.size(); ++i) {
        if (const VkResult result = Register(*images[i].first, images[i].second); result != VK_SUCCESS) {
            problem = fmt::format("FSR 4 image registration failed ({}, {})", names[i], static_cast<int>(result));
            abandon();
            return false;
        }
    }
    const auto resource = [](const Image& image, u32 format, u32 state) {
        FfxApiResource r{};
        r.resource = reinterpret_cast<void*>(static_cast<VkImageView>(image.view));
        r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
        r.description.format = format;
        r.description.width = image.width;
        r.description.height = image.height;
        r.description.depth = 1;
        r.description.mipCount = 1;
        r.state = state;
        return r;
    };
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = reinterpret_cast<void*>(static_cast<VkCommandBuffer>(f.cmd));
    d.color = resource(f.color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(f.depth, FFX_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(f.motion, FFX_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(f.output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f.jitter[0], f.jitter[1]};
    d.motionVectorScale = {1.0f, 1.0f}; // render pixels; the provider divides by the render size
    d.renderSize = {f.color.width, f.color.height};
    d.upscaleSize = {f.output.width, f.output.height};
    d.enableSharpening = f.sharpness > 0.0f;
    d.sharpness = f.sharpness;
    d.enableAutoExposure = f.auto_exposure;
    d.frameTimeDelta = f.frame_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = f.near_plane;
    d.cameraFar = f.far_plane;
    d.cameraFovAngleVertical = f.vertical_fov;
    d.viewSpaceToMetersFactor = 1.0f;
    if (const auto result = ffxFsr4V07Dispatch(&context, &d.header); result != FFX_API_RETURN_OK) {
        problem = fmt::format("the FSR 4 dispatch failed ({})", static_cast<int>(result));
        abandon();
        return false;
    }
    in_flight.emplace_back(frame_id, value);
    problem.clear();
    return true;
}

} // namespace Core::Vkb::Fsr4
