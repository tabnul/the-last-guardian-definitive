// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h" // before anything that includes vulkan.h (VK_ENABLE_BETA_EXTENSIONS)
#include <vk_mem_alloc.h>
#include "native/shadps4/core/libraries/videoout/buffer.h"
#include "native/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_scheduler.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_swapchain.h"

namespace Frontend {
class WindowSDL;
}

namespace VkbVulkan {

/// One presentation frame: the game's picture after the post process pass, at the window's size
struct Frame {
    u32 width{};
    u32 height{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageView image_view{};
    vk::Fence present_done{};
    vk::Semaphore ready_semaphore{};
    u64 ready_tick{};
    u8 id{};
    // F2 frame generation: the image made between the previous frame and this one (presented first), ready when made from fresh inputs
    vk::Image fg_image{};
    VmaAllocation fg_allocation{};
    vk::ImageView fg_view{};
    bool fg_ready{};
};

/// The engine-to-Vulkan backend's presenter (merge note 5), written for the backend: its own instance, device, swapchain and schedulers. It shows the backend's
/// image of the engine's swap chain texture (Core::Vkb::DisplayImageAt) through the post process pass, letterboxed into the swapchain, with the shim's overlay
/// (menu, save dialogs, frame counter) over it. Their renderer's presenter features (FSR, SMAA, CAS, HDR, screenshots, the upscalers) are not here: M7.
/// Threads: PrepareFrame / PrepareBlankFrame(false) on the game's flipping threads (under frame_mutex, on frame_scheduler); Present, PrepareLastFrame and
/// PrepareBlankFrame(true) on the VideoOut present thread (present_scheduler).
class Presenter {
public:
    explicit Presenter(Frontend::WindowSDL& window);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }
    /// HDR output is their renderer's (greyed out under ours, merge plan M3 (7))
    [[nodiscard]] bool IsHDRSupported() const {
        return false;
    }
    void SetHDR(bool) {}
    /// The backend reads no guest display memory: its display image is the engine's swap chain texture
    void RegisterVideoOutSurface(const Libraries::VkbVideoOut::BufferAttributeGroup&, VAddr) {}

    Frame* PrepareFrame(const Libraries::VkbVideoOut::BufferAttributeGroup& attribute, VAddr cpu_address);
    Frame* PrepareBlankFrame(bool present_thread);
    Frame* PrepareLastFrame();
    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);

    /// The present thread redraws the last frame while nothing new comes, as long as the menu or a dialog takes input
    [[nodiscard]] static bool MustKeepDrawing();

private:
    Frame* GetRenderFrame();
    void RecreateFrame(Frame* frame, u32 width, u32 height);
    void SetExpectedGameSize(s32 width, s32 height);
    Frame* ClearFrame(Scheduler& scheduler);
    /// F2 (TLG_VKB_FG=dlss): DLSS frame generation into frame->fg_image from the finished frame and the AA's depth and motion (native_vkb_aa.h)
    void RecordFrameGeneration(Frame* frame, vk::CommandBuffer cmdbuf);
    /// One image (the frame's or its generated one) into the swapchain, with the overlay; `real`: the frame's own (its fence, the captures)
    bool PresentImage(Frame* frame, vk::Image source, bool real, bool is_reusing_frame);

    Frontend::WindowSDL& window;
    Instance instance;
    Scheduler frame_scheduler;   ///< game frames (the flipping threads, one at a time)
    Scheduler present_scheduler; ///< the present thread
    Swapchain swapchain;
    HostPasses::PostProcessingPass pp_pass;
    HostPasses::PostProcessingPass::Settings pp_settings{};
    std::mutex frame_mutex;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    Frame* last_submit_frame{};
    std::deque<u64> recent_frame_ticks;
    float expected_ratio{16.0f / 9.0f};
    s32 expected_frame_width{1920};
    s32 expected_frame_height{1080};
    // F2
    int fg_state{-1}; ///< -1 not asked yet, 0 not available, 1 available
    bool fg_failed{};
    u32 fg_width{}, fg_height{};
    vk::Format fg_format{};
    float fg_interval_ms{16.6f};
    std::chrono::steady_clock::time_point fg_last_flip{};
    u64 fg_presented{};
};

extern std::unique_ptr<Presenter> g_presenter;

} // namespace VkbVulkan
