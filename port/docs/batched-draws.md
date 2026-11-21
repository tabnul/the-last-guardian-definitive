# Batched draws (record mode): the per-draw CPU path rebuilt

Status: **default on since 2026-11-21** (owner play OK on the heaviest save). Each part can be turned off: `TLG_VKB_RECORD=0` (all of it), `TLG_VKB_RECORD_ALL=0`
(shadow cascades only), `TLG_VKB_FAST_ROW=0` (rows by the old lookups).

Heaviest save, 4K, headless, AA / FG off, uncapped (`TLG_FPS=unlocked`, `TLG_FPS_LIMIT=0`), one round of 5 readings:

| | fps (median) | readings | render thread busy |
|---|---|---|---|
| base | 83.7 | 79-95 | 98 % |
| batched draws (now the default) | **102.2** | 98-103 | 89 % |

## Why

The engine issues ~2,800 draws a frame from one thread (`ps4render`). Our backend captured each one into an 8.6 KB draw job (fixed state, variant key, targets,
both stages' bindings, vertex, index) and a recording thread wrote it again as Vulkan commands. That per-draw capture was the frame's limit: with it skipped
(`TLG_VKB_TEST_NO_CAPTURE=1`, nothing drawn) the same save runs 135-139 fps uncapped. Every earlier attempt trimmed pieces of the capture; none was big.

## What it does

1. **One global descriptor set** (`native_vkb_indexed.*`): all buffers, image views (per array key) and samplers live in bindless arrays; slots are made at first
   use and freed when our object is destroyed (after the GPU is done with it).
2. **Record-mode pipelines**: the recompiler's indexed mode. A shader reads each resource through the draw's **row** (slot, offset, size per buffer; slot per image /
   sampler) in a host ring; the row's start is a push constant. Built per variant next to the plain pipeline ("record twin"); refused for flat user data, written
   guest buffers, fixed-function vertex input (those draws stay on the plain path).
3. **Batches**: consecutive record-mode draws with the same targets go into one batch job (the pass, the hazards and the global set once; per draw only what changed:
   pipeline, dynamic state, push constants, index buffer, draw). API order is kept: a batch is flushed before any other job, at a submission, at a target change,
   and every 128 draws (`TLG_VKB_BATCH_MAX`).
4. **Lean path**: a draw finds its pipeline and fixed state in a per-thread cache keyed by the context words both depend on (shaders, state objects and their ids,
   targets, viewport, blend factor, stencil reference, topology, the objects in the shader slots); it appends straight into the open batch, without a draw job.
5. **Fast rows** (`TLG_VKB_FAST_ROW=1`): the row is built from per-thread tables keyed by the engine object id (`+0x10`). An engine id is never used again after its
   object is deleted, and our answer for an id changes only at that delete, so an entry (checked against the object pointer and its memory word) holds for the
   object's life. Only device answers are kept; frame-ring data (moves every submission) and constant buffers (renamed by the engine per update) are taken per draw.
   Hazards once per object per batch.

## The latency trap (fixed)

The stream cuts a submission every 512 jobs (V4l). With batches, one job stood for up to a whole pass, so those mid-frame submissions stopped: the GPU got work
only at the engine's ~3 submits a frame and sat at 68-73 % busy (the engine waits for the GPU after every dispatch). **A batch now counts as its number of draws
in the split**, and batches are cut every 128 draws. Before this fix the same switches were slower than base (76.6 fps).

## Checks

- `TLG_VKB_FAST_ROW_VERIFY=1`: both rows built, the old one drawn, differences counted: **0 in 6.1 M draws** (all passes).
- `TLG_VKB_R7_VERIFY=1`: the pipeline / fixed state cache against the slow path: **0 differences in 6.1 M draws**.
- 478 record pipelines compiled in the heaviest save, 93 % of all draws in batches, no device loss.
- Screenshot of the heaviest save looks right (not pixel-compared: the scene moves).

## Bounds measured on the way (test switches, the picture is wrong)

| switch | what it does | heaviest, uncapped |
|---|---|---|
| `TLG_VKB_TEST_NO_CAPTURE=1` | every draw returns before our capture | 135-139 |
| `TLG_VKB_TEST_RAW_ONLY=1` | per draw only the pipeline cache + a raw record, nothing drawn | 118 |
| `TLG_VKB_TEST_LEAN_FREE=1` | reuses the previous row | **device loss: never run again** |

The GPU itself caps the heaviest save at ~110-120 fps at 4K (busy ~88 % with timestamps, ~9 ms a frame; dispatches ~55 %, draws ~36 %). Past that, the GPU's own
work has to shrink.

## Debug censuses (off)

`TLG_VKB_GD_CENSUS` (which draws a GPU-built row could take, data stability until the GPU starts), `TLG_VKB_R7_CENSUS`, `TLG_VKB_DEPTH_CENSUS`, `TLG_VKB_ID_CENSUS`,
`TLG_VKB_LEAN_TIMES` (per-draw timer of the lean path: row 892 -> 402 ns, lean draw 1,346 -> 770 ns with fast rows).

## Code

`port/gpu/native/native_vkb_graphics.cpp` (Compile record twins, `BuildRow`, `FastRow`, `FastIndex`, `LeanWords`, `LeanAppend`, `BatchJob`, `JoinBatch`,
`BatchCutIfFull`, `RecordAll`), `native_vkb_indexed.*` (global set), `native_vkb_stream.*` (`Job::weight`, the submission split, `SetBeforeEnqueue`).
The full research log is in the maintainer's notes (scene-on-gpu plan, backend note 110).
