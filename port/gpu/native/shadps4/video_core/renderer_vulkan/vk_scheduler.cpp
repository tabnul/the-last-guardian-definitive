// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/shadps4/video_core/renderer_vulkan/vk_scheduler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_instance.h"

namespace VkbVulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance_)
    : instance{instance_}, master_semaphore{instance_}, command_pool{instance_, &master_semaphore} {
    BeginNext();
}

Scheduler::~Scheduler() {
    // the command buffer being recorded is never submitted; wait for everything that was
    master_semaphore.Wait(master_semaphore.CurrentTick() - 1);
}

void Scheduler::BeginNext() {
    current_cmdbuf = command_pool.Commit();
    const vk::CommandBufferBeginInfo begin_info{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    const vk::Result result = current_cmdbuf.begin(begin_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "vkb scheduler: begin command buffer failed: {}", vk::to_string(result));
}

void Scheduler::Flush(SubmitInfo& info) {
    const u64 signal_tick = master_semaphore.NextTick();
    const vk::Result end_result = current_cmdbuf.end();
    ASSERT_MSG(end_result == vk::Result::eSuccess, "vkb scheduler: end command buffer failed: {}", vk::to_string(end_result));

    info.AddSignal(master_semaphore.Handle(), signal_tick);
    const vk::TimelineSemaphoreSubmitInfo timeline{
        .waitSemaphoreValueCount = static_cast<u32>(info.wait_values.size()),
        .pWaitSemaphoreValues = info.wait_values.data(),
        .signalSemaphoreValueCount = static_cast<u32>(info.signal_values.size()),
        .pSignalSemaphoreValues = info.signal_values.data(),
    };
    const vk::SubmitInfo submit{
        .pNext = &timeline,
        .waitSemaphoreCount = static_cast<u32>(info.wait_semaphores.size()),
        .pWaitSemaphores = info.wait_semaphores.data(),
        .pWaitDstStageMask = info.wait_stages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = static_cast<u32>(info.signal_semaphores.size()),
        .pSignalSemaphores = info.signal_semaphores.data(),
    };
    {
        std::scoped_lock lock{submit_mutex};
        const vk::Result result = instance.GetGraphicsQueue().submit(submit, info.fence);
        if (result == vk::Result::eErrorDeviceLost) {
            LOG_CRITICAL(Render_Vulkan, "vkb scheduler: device lost at submit");
        }
        ASSERT_MSG(result == vk::Result::eSuccess, "vkb scheduler: queue submit failed: {}", vk::to_string(result));
    }
    BeginNext();
}

void Scheduler::Finish() {
    const u64 tick = CurrentTick();
    Flush();
    master_semaphore.Wait(tick);
}

} // namespace VkbVulkan
