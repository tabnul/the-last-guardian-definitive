// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <atomic>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "common/types.h"
#include "native/shadps4/video_core/renderer_vulkan/vk_common.h"

namespace VkbVulkan {
class Instance;
}

/// V3b of the engine-to-Vulkan backend: the frame's command stream. The engine's work calls record into one command buffer (in the order the game makes them); a graphics submit
/// submits it to the graphics queue with the next value of our timeline. What must wait for the GPU (the queue's fences and interrupts, the flip) runs on the submitting thread
/// once the timeline has passed the frame's value.
namespace Core::Vkb::Stream {

void Init(const VkbVulkan::Instance& instance);
[[nodiscard]] bool Ready();

/// What a recording does, for the GPU time per kind of work (diagnostic: a timestamp where the kind changes)
enum class Work : u8 { Draw, Dispatch, Copy, Clear, Other, Upload, CopyBuffer, CopyGds, Fill, UploadInitial, UploadCpuWritten, Count };
/// R5 itemized copies: Copy = the engine's texture copies, CopyBuffer / CopyGds / Fill = its buffer copies, GDS reads and pattern fills; Upload = our device copy at a static
/// buffer's first bind, UploadInitial = a new buffer's initial data, UploadCpuWritten = the Unmap / UpdateSubresource copies.
/// The bytes each kind moves (CPU side, counted where the job is made).
void NoteBytes(Work work, u64 bytes); ///< Upload (R5 meter): our own copies of engine data into our buffers (first bind, initial data, Unmap / UpdateSubresource)

/// V4g: an image's move to the general layout, decided where the engine's call is resolved and recorded in front of the next job of that thread
struct Transition {
    vk::Image image{};
    vk::ImageLayout old_layout{};
    vk::ImageAspectFlags aspect{};
};
void PendTransition(vk::Image image, vk::ImageLayout old_layout, vk::ImageAspectFlags aspect);

/// V4g: the recording thread. An engine call is resolved where it is made (its engine state read, our objects found) into a job; the job's Vulkan commands are recorded on
/// our recording thread, in the order the jobs were made (the stream lock held, the command buffer begun; a job that is not a draw ends an open pass). An unlocked job (a submit)
/// runs without the lock and gets no command buffer. `TLG_VKB_RECORDER=0` (or `TLG_VKB_OVERLAP=0`) runs every job at once on the calling thread.
struct Job {
    virtual ~Job() = default;
    virtual void Run(vk::CommandBuffer cmd) = 0;
    /// R4a (note 65): run in stream order on the resolver before the job goes to a recording thread (a draw's late buffers)
    virtual void Prepare() {}
    Work work{Work::Other};
    bool locked{true};
    s8 query{}; ///< R4a: +1 opens an occlusion query, -1 ends it (no submission is cut inside one)
    std::vector<Transition> transitions;
    std::atomic<Job*> queue_next{nullptr}; ///< note 60: the lock-free hand-over queue's link
};
/// A2 (note 46; TLG_VKB_JOB_POOL, default on, =0 off): the memory of jobs of one size is reused (a job is made on the engine's thread and freed on the recording thread,
/// thousands a frame; each was a heap block of several KB)
bool JobPoolOn();
/// note 60: changes whenever the recorder begins a command buffer (the draw state cache is per recording)
u64 RecordingGeneration();
template <size_t Size>
struct JobBlocks {
    static inline std::atomic_flag lock = ATOMIC_FLAG_INIT;
    static inline std::vector<void*> free_blocks;
    static void* Get() {
        if (JobPoolOn()) {
            void* p = nullptr;
            while (lock.test_and_set(std::memory_order_acquire)) {
            }
            if (!free_blocks.empty()) {
                p = free_blocks.back();
                free_blocks.pop_back();
            }
            lock.clear(std::memory_order_release);
            if (p != nullptr) {
                return p;
            }
        }
        return ::operator new(Size);
    }
    static void Put(void* p) {
        if (JobPoolOn()) {
            while (lock.test_and_set(std::memory_order_acquire)) {
            }
            const bool kept = free_blocks.size() < 16384;
            if (kept) {
                free_blocks.push_back(p);
            }
            lock.clear(std::memory_order_release);
            if (kept) {
                return;
            }
        }
        ::operator delete(p);
    }
};
template <class F>
struct FnJob final : Job {
    explicit FnJob(F&& f_) : f(std::move(f_)) {}
    static void* operator new(size_t) {
        return JobBlocks<sizeof(FnJob)>::Get();
    }
    static void operator delete(void* p) {
        JobBlocks<sizeof(FnJob)>::Put(p);
    }
    void Run(vk::CommandBuffer cmd) override {
        f(cmd);
    }
    F f;
};
void Enqueue(std::unique_ptr<Job> job);
template <class F>
void Defer(Work work, F&& f) {
    auto job = std::make_unique<FnJob<std::decay_t<F>>>(std::decay_t<F>(std::forward<F>(f)));
    job->work = work;
    Enqueue(std::move(job));
}
/// R4a: a job that begins (+1) or ends (-1) an occlusion query
template <class F>
void DeferQuery(s8 query, F&& f) {
    auto job = std::make_unique<FnJob<std::decay_t<F>>>(std::decay_t<F>(std::forward<F>(f)));
    job->work = Work::Other;
    job->query = query;
    Enqueue(std::move(job));
}
template <class F>
void DeferUnlocked(F&& f) {
    auto job = std::make_unique<FnJob<std::decay_t<F>>>(std::decay_t<F>(std::forward<F>(f)));
    job->locked = false;
    Enqueue(std::move(job));
}
/// V4h (TLG_VKB_STAGE=1): a range of engine memory a command reads in place (guest memory over the bus) is copied, once per submission, into our device ring by a command
/// buffer that runs in front of the submission's; `buffer` / `offset` then name the copy. Not for a range our GPU work wrote earlier in the same submission. Lock held (a job).
/// False: left in place.
bool StageRead(vk::Buffer& buffer, u64& offset, u64 size);
/// V4i: our GPU work that writes engine memory in place says so: those ranges stay in place for the rest of the frame (a copy in front of a later submission would
/// miss the write). Lock held (a job). NewFrame: at the engine's graphics submit.
void NoteInPlaceWrite(vk::Buffer buffer, u64 offset, u64 size);
void NewFrame();

/// V5b: occlusion queries. The engine begins and ends counting for a result block (`address`: per counter pair a begin word and an end word, 16 bytes apart, bit 63
/// valid). Begin / End run in jobs (lock held, outside a pass): a Vulkan occlusion query counts the draws in between, in one command buffer (no split while it is open;
/// a submit in between ends it there). The answer (seen or not) is written into the block once the GPU has finished; a block never answered reads "visible".
void FirstAnswer(u64 address, u32 pairs);
void QueryBegin(vk::CommandBuffer cmd, u64 address);
void QueryEnd(vk::CommandBuffer cmd, u64 address);
/// Writes the answers of the queries the GPU has finished (the recording thread, at a submit)
void PollQueries();

/// Waits until every job made so far has run
void Drain();
[[nodiscard]] bool RecorderOn();
/// True on the recording thread (or wherever jobs run at once)
[[nodiscard]] bool OnRecorder();
/// The value the GPU has passed on our timeline
[[nodiscard]] u64 CompletedValue();

/// The stream's command buffer, locked for the caller while it records
struct Recording {
    std::unique_lock<std::mutex> lock;
    vk::CommandBuffer cmd;
};
[[nodiscard]] Recording Record(Work work = Work::Other);
/// V4: the same for a draw: an open render pass stays open (Record ends it: every other command runs outside a pass)
[[nodiscard]] Recording RecordDraw();

/// V4d: what a command reads or writes, for the barriers: a range of a buffer, or a whole image (by handle)
struct Use {
    u64 handle{};
    u64 begin{}, end{};
    bool write{};
};
[[nodiscard]] inline Use BufferUse(vk::Buffer buffer, u64 offset, u64 size, bool write) {
    return {reinterpret_cast<u64>(static_cast<VkBuffer>(buffer)), offset, size == VK_WHOLE_SIZE ? ~0ull : offset + size, write};
}
[[nodiscard]] inline Use ImageUse(vk::Image image, bool write) {
    return {reinterpret_cast<u64>(static_cast<VkImage>(image)), 0, ~0ull, write};
}
/// V4d: before a command is recorded (lock held: the recording's): one full memory barrier when the command touches a range an earlier command wrote since the last barrier,
/// or writes a range one read since then (an open pass ends first); then the command's uses are noted. True: a barrier was recorded.
/// `TLG_LEGACY_VKB_BARRIERS=1` brings back the old rule (a barrier before every command but a draw, and at every pass change).
/// V4h: the barrier waits only for the kinds of GPU work (graphics, compute, transfer) that wrote what the command touches (or read what it writes); `agent` is the command's
/// kind (Current: the job's). `TLG_VKB_FULL_BARRIERS=1`: every barrier waits for all earlier work (V4d).
enum class Agent : u8 { Current = 0, Graphics = 1, Compute = 2, Transfer = 4 };
bool Hazard(vk::CommandBuffer cmd, std::span<const Use> uses, Agent agent = Agent::Current);
/// R5 census TLG_VKB_HAZARD_LOG=1: the shader hash the next Hazard calls belong to (recording thread), for log/vkb_hazards.txt
void SetHazardTag(u64 tag);
[[nodiscard]] bool LegacyBarriersOn();
inline bool Hazard(vk::CommandBuffer cmd, std::initializer_list<Use> uses, Agent agent = Agent::Current) {
    return Hazard(cmd, std::span<const Use>{uses.begin(), uses.size()}, agent);
}

/// V4: a render pass (dynamic rendering) of our attachments, in the general layout
struct Pass {
    std::array<vk::ImageView, 8> colors{};
    u32 color_count{};
    vk::ImageView depth{};
    bool depth_has_stencil{};
    vk::Extent2D extent{};
    u32 layers{1};
    bool operator==(const Pass&) const = default;
};
/// Makes `pass` the open pass of the recording (lock held): the same pass stays open; another ends the open one and begins, after a barrier when its attachments (`attachments`,
/// written) need one (V4d). Returns true when a new pass began.
bool UsePass(vk::CommandBuffer cmd, const Pass& pass, std::span<const Use> attachments);
/// R5 meter: what a draw in the open pass tests (bits: 1 depth attachment, 2 stencil attachment, 4 depth test, 8 stencil test, 16 depth bounds, 32 depth write), its pixel
/// shader and first color format (lock held, after UsePass). TLG_VKB_PASS_DUMP=1 writes one frame's passes with these to log/vkb_pass_dump.txt.
void NotePassDraw(u32 flags, u64 ps_hash, u32 format, const vk::Viewport& viewport, const vk::Rect2D& scissor);
/// Ends the open pass, if any (lock held): before a barrier or any command that is not a draw
void BreakPass(vk::CommandBuffer cmd);

/// Submits what was recorded since the last submit; returns the timeline value that is reached when it is done (the last value when nothing was recorded)
u64 Submit();
/// The last value handed out by Submit
[[nodiscard]] u64 LastValue();
/// R4a: the timeline value the command buffer being recorded on this thread goes out with (inside a job; LastValue() + 1 on the single recorder)
u64 RecordingValue();
/// Waits until the GPU has passed `value`
void Wait(u64 value);

/// A slice of the host ring (mapped, coherent; reused after the frames that use it are done)
struct Slice {
    vk::Buffer buffer{};
    u64 offset{};
    void* data{};
};
[[nodiscard]] Slice HostRing(u64 size, u64 alignment);
/// The GDS stand-in (64 KiB, device local), a 64 KiB zero-initialised buffer for empty buffer slots, and a 1x1 image view in the general layout for empty image slots
[[nodiscard]] vk::Buffer GdsBuffer();
/// Kill-experiment TLG_VKB_HOST_SCRATCH=1 (wrong picture, fps bound only): a 256 MiB zeroed device-local buffer that stands in for every engine buffer read in place
/// (guest memory over the bus); null when the switch is off
[[nodiscard]] vk::Buffer HostScratch();
constexpr u64 HostScratchBytes = 256ull << 20;
/// Kill-experiment TLG_VKB_HOST_CACHE=1 (fps bound; the data is a snapshot of the first use, stale after): a 2 GiB device-local arena that holds a copy of every engine buffer
/// range read in place; null when the switch is off
[[nodiscard]] vk::Buffer HostCacheArena();
[[nodiscard]] bool HostCacheOn();
constexpr u64 HostCacheBytes = 2048ull << 20;
/// Kill-experiment TLG_VKB_NO_PASS_BARRIER=1: no full barrier when a render pass changes
[[nodiscard]] vk::Buffer NullBuffer();
[[nodiscard]] vk::ImageView NullImageView(bool storage);

/// GPU timestamps (diagnostic): a timestamp written into `cmd` (the recording's, lock held), its index in the current command buffer, or ~0u when there is no room
u32 Stamp(vk::CommandBuffer cmd);
/// R5: one timestamp at each submission's start and end only (the default); TLG_VKB_STAMPS=1 adds the per-kind, per-pass and per-dispatch ones
bool StampsLean();
/// The current command buffer's generation (lock held: the recording's)
[[nodiscard]] u64 Generation();
/// The timestamps (nanoseconds) of every submission the GPU has finished since the last call, with its generation
struct Stamps {
    u64 generation{};
    std::vector<u64> ns;
};
[[nodiscard]] std::vector<Stamps> FinishedStamps();

[[nodiscard]] nlohmann::json Report();
/// note 87: debugger mode's GPU submissions with their slowest passes (the slowest since debugger mode started, the newest); the frame walk freeze writes it (gpu_passes.json).
[[nodiscard]] nlohmann::json GpuPassReport();

} // namespace Core::Vkb::Stream
