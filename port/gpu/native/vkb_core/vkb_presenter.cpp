// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project, tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
// The engine-to-Vulkan backend's presenter (merge note 5). From the old port's vk_presenter.cpp: its backend path (PrepareFrame with the display image of
// Core::Vkb::DisplayImageAt, note 42 sRGB view, note 69 own frame scheduler), the blank and last frames, the frame pool and Present; without the old renderer.

#include "native/vkb_core/vkb_presenter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "native/native_capture.h"
#include "native/native_debug_server.h"
#include "native/native_frame_gate.h"
#include "native/native_hooks.h"
#include "native/native_prof.h"
#include "native/native_timeline.h"
#include "native/native_vkb_objects.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_platform.h"
#include "native/vkb_core/vkb_device.h"
#include "sdl_window.h"
#include "tlg_overlay.h"
#include "tlg_settings.h"
#ifdef MemoryBarrier
#undef MemoryBarrier // winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

void TlgVkbRtOnPresent(); // native_vkb_graphics.cpp, RT prototype
void TlgVkbRtTraceDisplay(vk::CommandBuffer cmd, vk::ImageView view, vk::Format format, u32 w, u32 h);

namespace VkbVulkan {

std::unique_ptr<Presenter> g_presenter;

namespace {

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = VK_REMAINING_ARRAY_LAYERS,
};

/// TLG_FRAME_STATS=1: the same "Frame stats:" line as the default renderer (videoout driver.cpp), so the
/// scene check and bench tools read both renderers. TLG_FRAME_STATS_MS sets the window (default 5 s).
/// Also logs the game frame size whenever it changes.
void NoteFrameStats(const Frame* frame) {
    static const bool on = std::getenv("TLG_FRAME_STATS") != nullptr;
    if (!on) {
        return;
    }
    static const double window_s = [] {
        const char* env = std::getenv("TLG_FRAME_STATS_MS");
        const int ms = env ? std::atoi(env) : 0;
        return ms > 0 ? ms / 1000.0 : 5.0;
    }();
    using Clock = std::chrono::steady_clock;
    static Clock::time_point start = Clock::now(), last = start;
    static u32 frames = 0, last_w = 0, last_h = 0;
    static double worst_ms = 0.0;
    if (frame && (frame->width != last_w || frame->height != last_h)) {
        last_w = frame->width;
        last_h = frame->height;
        std::printf("VKB game frame size %ux%u\n", last_w, last_h);
    }
    const auto now = Clock::now();
    worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
    ++frames;
    const double window = std::chrono::duration<double>(now - start).count();
    if (window >= window_s) {
        std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank 0 Hz); 0 shader/pipeline compiles, 0.0 ms\n",
                    frames / window, worst_ms);
        std::fflush(stdout);
        frames = 0;
        worst_ms = 0.0;
        start = now;
    }
}

vk::ImageBlit BlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width, s32 swapchain_height) {
    const float frame_aspect = static_cast<float>(frame_width) / frame_height;
    const float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;
    s32 dst_width = swapchain_width;
    s32 dst_height = swapchain_height;
    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }
    const s32 x = (swapchain_width - dst_width) / 2;
    const s32 y = (swapchain_height - dst_height) / 2;
    const vk::ImageSubresourceLayers layers{.aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    return vk::ImageBlit{
        .srcSubresource = layers,
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{frame_width, frame_height, 1}},
        .dstSubresource = layers,
        .dstOffsets = std::array{vk::Offset3D{x, y, 0}, vk::Offset3D{x + dst_width, y + dst_height, 1}},
    };
}

void WaitFence(vk::Device device, vk::Fence fence) {
    while (true) {
        const vk::Result result = device.waitForFences(fence, false, std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            return;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during waiting for a frame");
    }
}

} // namespace

Presenter::Presenter(Frontend::WindowSDL& window_)
    : window{window_}, instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(), EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      frame_scheduler{instance}, present_scheduler{instance}, swapchain{instance, window} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = static_cast<u8>(i);
        frame.present_done = Check<"create present done fence">(device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        free_queue.push(&frame);
    }
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    // the overlay's own font atlas uploads go to the graphics queue under our queue lock (not their renderer's)
    TlgOverlay::SetQueueMutex(&Scheduler::submit_mutex);
    TlgOverlay::Init(TlgOverlay::Device{.api_version = instance.ApiVersion(),
                                        .instance = instance.GetInstance(),
                                        .physical_device = instance.GetPhysicalDevice(),
                                        .device = device,
                                        .queue_family = instance.GetGraphicsQueueFamilyIndex(),
                                        .queue = instance.GetGraphicsQueue()},
                     swapchain.GetSurfaceFormat().format, num_images);
    // the performance overlay's GPU name and video memory (their renderer hands its Vulkan::Instance instead)
    TlgOverlay::SetStatsDevice(TlgPerfStats::Device{
        .name = std::string(instance.GetModelName()),
        .can_report_memory = [this] { return instance.CanReportMemoryUsage(); },
        .memory_used = [this] { return uint64_t(instance.GetDeviceMemoryUsage()); },
        .memory_budget = [this] { return uint64_t(instance.GetDeviceMemoryBudgetNow()); },
    });
    // The backend's objects, pipelines and streams reach this device from here on (they wait for it: Core::Vkb::Device::Ready)
    Core::Vkb::Device::Init(instance, frame_scheduler);
    LOG_INFO(Render_Vulkan, "Engine-to-Vulkan backend: presenter ready ({} swapchain images)", num_images);
}

Presenter::~Presenter() {
    frame_scheduler.Finish();
    present_scheduler.Finish();
    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        if (frame.image) {
            vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
            device.destroyImageView(frame.image_view);
        }
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::MustKeepDrawing() {
    // only while the menu or a dialog takes input (it must answer while the game sends no frames). Not for the FPS overlay: the present
    // thread then repeated the last frame on every vblank without a new one, and the overlay counted those repeats as frames.
    return TlgOverlay::CapturesInput();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }
    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                 vk::ImageUsageFlagBits::eSampled,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage unsafe_image{};
    const VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);
    const VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info, &unsafe_image, &frame->allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "vkb presenter: frame image allocation failed: {}", vk::to_string(vk::Result{result}));
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "VKB frame image #{}", frame->id);
    frame->image_view = Check<"create frame image view">(device.createImageView({
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    frame->width = width;
    frame->height = height;
}

Frame* Presenter::GetRenderFrame() {
    Frame* frame;
    {
        TLG_PROF("present.wait_free_frame");
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        frame = free_queue.front();
        free_queue.pop();
    }
    {
        TLG_PROF("present.wait_present_done");
        WaitFence(instance.GetDevice(), frame->present_done);
    }
    if (frame->width != u32(expected_frame_width) || frame->height != u32(expected_frame_height)) {
        TLG_PROF("present.recreate_frame");
        // the frame's last use (a present, or a reading of it as the last frame) is done: its fence was waited above, the last frame reads only on the present thread
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }
    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = static_cast<float>(width) / static_cast<float>(height);
    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

Frame* Presenter::PrepareFrame(const Libraries::VkbVideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
    TLG_PROF_LEVEL("present.prepare_frame", 1);
    std::scoped_lock frame_lock{frame_mutex};
    // V4c: the display buffer is our image of the engine's swap chain texture; the guest memory is not read
    Core::Vkb::DisplayImage display{};
    if (!Core::Vkb::DisplayImageAt(cpu_address, display)) {
        // nothing drawn into it yet (start-up): a black frame
        return ClearFrame(frame_scheduler);
    }
    Frame* frame = GetRenderFrame();
    const vk::CommandBuffer cmdbuf = frame_scheduler.CommandBuffer();
    const vk::ImageMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &pre_barrier});

    // note 42: an sRGB video out buffer is read through the sRGB twin of our image's format (the engine's last pass writes gamma-encoded values into its unorm
    // swap chain texture; a unorm view handed them on as linear: too bright, blacks lifted)
    const auto pixel_format = attribute.attrib.pixel_format;
    vk::Format view_format = display.format;
    const bool srgb_out = pixel_format == Libraries::VkbVideoOut::PixelFormat::A8B8G8R8Srgb || pixel_format == Libraries::VkbVideoOut::PixelFormat::A8R8G8B8Srgb;
    if (srgb_out && display.mutable_format) {
        if (view_format == vk::Format::eR8G8B8A8Unorm) {
            view_format = vk::Format::eR8G8B8A8Srgb;
        } else if (view_format == vk::Format::eB8G8R8A8Unorm) {
            view_format = vk::Format::eB8G8R8A8Srgb;
        }
    }
    const vk::Device device = instance.GetDevice();
    const vk::ImageView image_view = Check(device.createImageView({
        .image = display.image,
        .viewType = vk::ImageViewType::e2D,
        .format = view_format,
        .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::Extent2D image_size{display.width, display.height};
    TlgVkbRtTraceDisplay(cmdbuf, image_view, view_format, display.width, display.height);
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = display.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer is decoded in the pass
    pp_settings.srgb_input = pixel_format == Libraries::VkbVideoOut::PixelFormat::A2R10G10B10Srgb;
    // the menu's colour adjustments (SDR), in the post process pass
    {
        TlgSettings::ReloadIfRequested(); // the in-game menu and the launcher change tlg.ini live
        const auto& graphics = TlgSettings::Get();
        pp_settings.color_adjust = graphics.color_adjust ? 1u : 0u;
        pp_settings.brightness = graphics.brightness;
        pp_settings.contrast = graphics.contrast;
        pp_settings.saturation = graphics.saturation;
        pp_settings.user_gamma = graphics.color_gamma;
    }
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);

    // our image goes back to the general layout (the backend keeps every image there)
    const vk::ImageMemoryBarrier2 back{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = display.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back});

    frame->ready_semaphore = frame_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = frame_scheduler.CurrentTick();
    {
        TLG_PROF("present.flush");
        frame_scheduler.Flush();
    }
    // the view lives until the GPU has read it: kept with its tick, destroyed by a later frame
    {
        static std::deque<std::pair<u64, vk::ImageView>> retired;
        retired.emplace_back(frame->ready_tick, image_view);
        while (!retired.empty() && frame_scheduler.IsFree(retired.front().first)) {
            device.destroyImageView(retired.front().second);
            retired.pop_front();
        }
    }

    // TLG_FRAMES_AHEAD / frames in flight (native/native_frame_gate.h): the flipping thread runs at most that many frames ahead of the GPU
    const u32 frames_ahead = Core::FrameGate::PresenterAhead(u32(std::max(0, TlgSettings::Get().frames_queued.load())));
    if (Core::FrameGate::PresenterWaitsOwnFrame()) {
        recent_frame_ticks.clear();
        TLG_PROF("present.wait_own_frame (TLG_FRAMES_IN_FLIGHT=0)");
        frame_scheduler.Wait(frame->ready_tick);
    } else if (!frames_ahead) {
        recent_frame_ticks.clear();
    } else {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                TLG_PROF("present.wait_frames_ahead");
                frame_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::ClearFrame(Scheduler& scheduler) {
    Frame* frame = GetRenderFrame();
    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    const vk::ImageMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = ColorRange,
    };
    const vk::ImageMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = ColorRange,
    };
    const vk::RenderingAttachmentInfo attachment{
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}}},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &pre_barrier});
    cmdbuf.beginRendering({.renderArea = {.extent = {frame->width, frame->height}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment});
    cmdbuf.endRendering();
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &post_barrier});
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    scheduler.Flush();
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    if (present_thread) {
        return ClearFrame(present_scheduler);
    }
    std::scoped_lock frame_lock{frame_mutex};
    return ClearFrame(frame_scheduler);
}

Frame* Presenter::PrepareLastFrame() {
    // present thread only; the last frame is shown again (its picture is unchanged, General layout)
    if (last_submit_frame == nullptr) {
        return nullptr;
    }
    Frame* frame = last_submit_frame;
    WaitFence(instance.GetDevice(), frame->present_done);
    frame->ready_semaphore = present_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = present_scheduler.CurrentTick();
    present_scheduler.Flush();
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    Core::FrameTimeline::Mark(Core::FrameTimeline::PresentStart, is_reusing_frame ? 1 : 0);
    if (!is_reusing_frame) {
        // the frame counter of the debug server (fps, frame time) and the frame tick of the guest hooks
        Core::DebugServer::NotePresent();
        Core::GuestHooks::OnFrame();
        NoteFrameStats(frame);
        TlgVkbRtOnPresent();
    }
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock lock{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };
    // a minimized window has no pixels on Windows (0x0): keep the swapchain, skip presenting until it is restored
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }
    // a VSync change and a swapchain recreate wait for the device to go idle (vkDeviceWaitIdle): every queue must be left alone meanwhile, so the
    // backend's recording and upload threads wait at the queue lock
    {
        std::scoped_lock queue_lock{Scheduler::submit_mutex};
        swapchain.UpdateVsync();
        if (u32(window.GetWidth()) != swapchain.GetWidth() || u32(window.GetHeight()) != swapchain.GetHeight()) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }
    if (!swapchain.AcquireNextImage()) {
        {
            std::scoped_lock queue_lock{Scheduler::submit_mutex};
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
        if (!swapchain.AcquireNextImage()) {
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }
    // reset here, not in GetRenderFrame: a skipped frame would leave the fence unsignalled and the next GetRenderFrame waiting forever
    const vk::Result reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess, "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    const vk::Image swapchain_image = swapchain.Image();
    const vk::CommandBuffer cmdbuf = present_scheduler.CommandBuffer();
    const vk::Extent2D extent = swapchain.GetExtent();
    SetExpectedGameSize(s32(extent.width), s32(extent.height));
    const std::array pre_barriers{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = ColorRange,
        },
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = frame->image,
            .subresourceRange = ColorRange,
        },
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);
    cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}}, ColorRange);
    const vk::MemoryBarrier clear_done{.srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eTransferWrite};
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
    cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image, vk::ImageLayout::eTransferDstOptimal,
                     BlitFit(frame->width, frame->height, extent.width, extent.height), vk::Filter::eLinear);
    // F12 / Shift+F12, the glitch marker and the frame walk: a copy of the game frame, before the overlay (native/native_capture.h)
    if (!is_reusing_frame) {
        // photo mode's Cross and the menu's Print Screen ask the overlay for a picture; under ours it is our capture (user/screenshots)
        if (TlgOverlay::TakeScreenshotRequest()) {
            TlgCapture::RequestScreenshot();
        }
        TlgCapture::Record(instance, cmdbuf, frame->image, frame->width, frame->height, swapchain.GetSurfaceFormat().format, present_scheduler.CurrentTick());
    }
    // the overlay (menu, save dialogs, frame counter) over the frame, at the window's resolution
    const bool overlay = TlgOverlay::Visible();
    const std::array post_barriers{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite : vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal : vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = ColorRange,
        },
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = frame->image,
            .subresourceRange = ColorRange,
        },
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
    if (overlay) {
        TlgOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
        const vk::ImageMemoryBarrier to_present{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eBottomOfPipe, vk::DependencyFlagBits::eByRegion, {}, {},
                               to_present);
    }

    Scheduler::SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.fence = frame->present_done;
    present_scheduler.Flush(info);
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }
    Core::FrameTimeline::Mark(Core::FrameTimeline::PresentEnd, is_reusing_frame ? 1 : 0);
    TlgCapture::Poll(present_scheduler); // copies of earlier frames the GPU finished go to the writer
    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

} // namespace VkbVulkan
