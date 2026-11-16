// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <boost/container/small_vector.hpp>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_resource_pool.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_semaphore.h"

namespace VkbVulkan {

class Instance;

/// The backend's lean scheduler (merge note 5): one command buffer at a time on its own timeline semaphore, for the presenter's frames. The backend's draws and
/// copies have their own recording threads, pools and timelines (native_vkb_stream, native_vkb_upload); they share only `submit_mutex`, the lock around the
/// graphics queue. The old renderer's scheduler (sessions, encoder thread, census and old-path work) does not come over.
class Scheduler {
public:
    /// Every submission to the graphics queue by the backend holds this (vkQueueSubmit needs the queue externally synchronized)
    static std::mutex submit_mutex;

    struct SubmitInfo {
        boost::container::small_vector<vk::Semaphore, 4> wait_semaphores;
        boost::container::small_vector<u64, 4> wait_values;
        boost::container::small_vector<vk::PipelineStageFlags, 4> wait_stages;
        boost::container::small_vector<vk::Semaphore, 4> signal_semaphores;
        boost::container::small_vector<u64, 4> signal_values;
        vk::Fence fence{};

        /// A binary semaphore takes value 0
        void AddWait(vk::Semaphore semaphore, u64 value = 0, vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
            wait_semaphores.push_back(semaphore);
            wait_values.push_back(value);
            wait_stages.push_back(stage);
        }
        void AddSignal(vk::Semaphore semaphore, u64 value = 0) {
            signal_semaphores.push_back(semaphore);
            signal_values.push_back(value);
        }
    };

    explicit Scheduler(const Instance& instance);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// The command buffer being recorded (begun)
    [[nodiscard]] vk::CommandBuffer CommandBuffer() const noexcept {
        return current_cmdbuf;
    }
    /// The tick the current command buffer signals when it is submitted
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore.CurrentTick();
    }
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (master_semaphore.IsFree(tick)) {
            return true;
        }
        master_semaphore.Refresh();
        return master_semaphore.IsFree(tick);
    }
    void Wait(u64 tick) {
        master_semaphore.Wait(tick);
    }
    [[nodiscard]] Semaphore* GetMasterSemaphore() noexcept {
        return &master_semaphore;
    }

    /// Ends and submits the current command buffer (signalling the current tick besides `info`'s semaphores), then begins the next one
    void Flush(SubmitInfo& info);
    void Flush() {
        SubmitInfo info{};
        Flush(info);
    }
    /// Flush, then wait until the GPU has done it
    void Finish();

private:
    void BeginNext();

    const Instance& instance;
    Semaphore master_semaphore;
    CommandPool command_pool;
    vk::CommandBuffer current_cmdbuf{};
};

} // namespace VkbVulkan
