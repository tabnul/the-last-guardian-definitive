// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <nlohmann/json_fwd.hpp>
#include "common/types.h"
#include "native/native_flip.h"
#include "native/native_sync.h"

/// The engine-to-Vulkan backend (documents/tlg/native plan/engine-vulkan-backend.md), `TLG_VKB=1`.
///
/// V1b: the game's command buffers are not parsed or run (our command processor gets no work). What the engine's sync sites mean is taken from their hooks, which already
/// build the typed record from the engine's own arguments (native_draw.cpp: FenceEop, the inline fences, the flip fences, the compute units): the record goes to the list
/// of its queue and runs when the game submits that queue, in order. With no GPU work yet a fence is done at its submit; V2 and later sign it at the completion of the
/// Vulkan work recorded before it.
namespace Core::Vkb {

/// `TLG_VKB=1`, read once
[[nodiscard]] bool On();

/// Note 76: the engine's own PS4 work below the API that still ran under TLG_VKB is skipped: ClearRenderTargetView and ClearDepthStencilView after our Vulkan clear,
/// CopyResource of a texture after our Vulkan copy, the target eliminate, the clear quad, the HTILE clear, and our old-path fast clear eliminate in the colour transition.
/// They wrote command packets, the engine's descriptor cache of its own internal shaders and resource compression states, none of which we read (side census).
/// The work and side censuses keep them (they measure them).
[[nodiscard]] bool SkipEnginePs4();
enum class EngineCall : u32 { ClearRtv, ClearDsv, CopyTexture, TargetEliminate, ClearQuad, HtileClear, FastClearEliminate, Count };
void NoteEngineCallSkipped(EngineCall call);
[[nodiscard]] nlohmann::json EngineCallsSkipped();

/// Queue numbers: 0 the graphics queue, otherwise the Gnm virtual queue id of a compute queue (1..64, the value sceGnmMapComputeQueue returned)
constexpr u32 GraphicsQueue = 0;

/// The engine wrote a sync packet whose meaning is `fence` / `write` / `dma` into a command buffer of `queue`
void Fence(const NativeSync::Fence& fence, u32 queue, u64 unit = 0);
void Write(u64 address, const u32* data, u32 bytes, u32 queue, u64 unit = 0);
/// api-contract.md K1: JsComputeQueue_Submit (02472ed0) on a shared label (the engine set it to 0x11 just now): this unit becomes the label's newest; the records of an
/// older unit on it (still on our completion thread) no longer write it. Returns the unit's number for its Fence / Write.
u64 BeginLabelUnit(u64 address);
void Dma(const NativeSync::DmaCopy& dma, u32 queue);

/// R6 census (note 36, `TLG_VKB_ASYNC_CENSUS=1`): the async compute queue against graphics, in recording order. Work: 0 a graphics draw, 1 a graphics dispatch, 2 an async
/// dispatch. Waits: a graphics wait on a label (`InlineWait*`) and an async wait on a label (`AscWaitFence`). Labels come from `Fence` of each queue.
/// note 44: the backend's counters per graphics submit while the frame walk records (Shift+F5); written into the freeze folder
[[nodiscard]] nlohmann::json FrameSnapshots();
/// note 44: a line for the current submit's entry (only while recording)
void FrameNote(const std::string& text);

namespace Census {
[[nodiscard]] bool On();
void Work(u32 kind, u64 hash = 0, u32 x = 0, u32 y = 0, u32 z = 0);
void GraphicsWait(u64 label);
void AsyncWait(u64 label);
[[nodiscard]] nlohmann::json Report();
} // namespace Census

/// The game submitted the graphics queue (sceGnmSubmitCommandBuffers, sceGnmSubmitAndFlipCommandBuffers): its list runs
void SubmitGraphics();
/// sceGnmDingDong of `queue` (compute pipe `pipe_id`): its list runs
void SubmitCompute(u32 queue, u32 pipe_id);
/// The frame's flip (after SubmitGraphics of the same call): what the command processor did at the typed flip tail, in the same order
void Flip(const NativeFlip::Request& request);

/// Graphics submits so far (the clock of deferred destruction)
[[nodiscard]] u64 GraphicsSubmits();
/// R3 census: the engine's frame allocator window (`JsDevice_AllocFrameGarlic`'s ring), noted by the allocator hook
void NoteFrameRing(u64 base, u64 size);
/// R3 census: the frame ring's window in the in-place import (false when unknown)
bool FrameRingInPlace(u64& buffer_handle, u64& begin, u64& end);
/// R3a (TLG_VKB_RING_VRAM, default on; =0 off): the engine's frame allocator ring (`JsDevice_AllocFrameGarlic`, VolatileGarlic: CPU write only, as Garlic on the console) is moved into
/// device-local, CPU-visible memory of our own (resizable BAR): the allocator's base (`allocator + 0x18`) points at our mapping, so every frame block is GPU memory the
/// engine writes directly; nothing is staged or read over the bus. Once, at the first allocation (blocks handed out before stay where they are). False when off or not possible.
bool PlaceFrameRing(u64 allocator);
/// R3a: once the frame ring is in device memory, its old window in guest memory (host memory the CPU reads at full speed) for our own ring redirects (slot, skin,
/// light): the game reads what they copy back on the CPU, so it stays there. False before or without the move.
bool HostSideRing(u64& base, u64& size);
/// R3b census (TLG_VKB_MAP_CENSUS=1): a buffer Map the engine made (its usage, the Map type, whether the pointer it handed out is the buffer's own memory, the bytes)
void NoteBufferMap(u32 usage, u32 type, bool in_place, u64 bytes);
/// R3b (TLG_VKB_DYNAMIC_DEVICE, default on; =0 off): dynamic engine buffers (D3D11 usage DYNAMIC) are our own device buffers, as the GPU-written ones (V2f). The engine's Map either hands out
/// a frame ring block and copies it into the buffer on the GPU (that copy now stays in device memory), or the buffer's own memory: that memory is copied into our buffer at
/// the Unmap (`DynamicUnmapped`), in the frame stream. Draws bind our buffer: nothing is staged.
bool DynamicDeviceOn();
void DynamicUnmapped(u64 memory, u64 bytes);
/// R3b check (TLG_VKB_DYN_CHECK=1): the memory of an in-place Map hashed at its Unmap and again at the buffer's next Map: a difference is a CPU write without a Map
void DynamicCheck(u64 memory, u64 bytes, bool at_unmap);
/// V5b hooks (`vkb_occlusion_begin` 0x2481330, `vkb_occlusion_end` 0x24814a0: the device context's occlusion begin / end, (ctx, result block)): our occlusion query around the
/// draws in between. The engine's own function still runs (its packets go unread). 0 when TLG_VKB is off.
u64 PS4_SYSV_ABI OcclusionBeginHook(u64 ctx, u64 address, u64 a2, u64 a3, u64 a4, u64 a5, u64 ret);
u64 PS4_SYSV_ABI OcclusionEndHook(u64 ctx, u64 address, u64 a2, u64 a3, u64 a4, u64 a5, u64 ret);
/// V4g: the GPU has done graphics submit `index` (counted from 1; false while it is not submitted yet)
[[nodiscard]] bool GraphicsSubmitDone(u64 index);
/// Note 103 (the free hold's GPU rule): the graphics submits handed to the GPU so far, and how many of them the GPU has finished (every submit up to that number is done).
/// The work the engine thread recorded since the last submit goes with submit GraphicsSubmits() + 1.
[[nodiscard]] u64 GraphicsSubmitted();
[[nodiscard]] u64 GraphicsSubmitsDone();

/// /stats `vkb_sync`
void Publish();

} // namespace Core::Vkb
