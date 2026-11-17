# The Last Guardian for PC: the engine-to-Vulkan renderer

This fork of [tabnul/the-last-guardian-definitive](https://github.com/tabnul/the-last-guardian-definitive)
builds a second renderer for the port: **the game's own render API drawn straight with Vulkan**.
The port itself (features, setup, building, credits) is described in [README-UPSTREAM.md](README-UPSTREAM.md).

> **Status: experimental.** Pick it in the launcher (**Renderer: vulkan**) or start with `TLG_VKB=1`.
> The default renderer stays the port's own until the new one has passed long play.

## What it does

The game talks to its engine's render layer, a Direct3D-11-shaped API (shader, view, constant buffer
and sampler slots, state objects, `Map` / `UpdateSubresource`, draws and dispatches). On PS4 that layer
turns every call into PS4 command packets, descriptor words and GPU memory, and an emulator then has to
translate all of that back.

This renderer removes that middle: the engine's API calls go straight to our own Vulkan code, as a PC
version of the engine would.

```
before:  game -> engine render API -> PS4 backend (packets, descriptors, memory) -> emulation -> Vulkan
now:     game -> engine render API -> our Vulkan backend
```

What stays PS4 by necessity: the game's GCN shaders (recompiled to SPIR-V, fed from the engine's slots)
and the one-time conversion of PS4-tiled texture data.

## Progress

| Area | State |
|---|---|
| Draws, dispatches, copies, clears from the engine's own calls | done |
| Buffers and textures by engine object (no address lookups), frame ring, delayed deletes | done |
| Samplers, blend, depth-stencil, rasterizer, input layout, viewports from the engine's state objects | done |
| Readbacks the CPU uses (auto exposure, staging textures) | done |
| Async compute folded into the frame, the engine's fences and waits honoured | done |
| Parallel recording (two recording threads), dynamic buffers in VRAM | done |
| The engine's PS4 backend calls (`PrepareDraw`, packet writers, HTILE / CMASK fills) | deleted under this renderer |
| **API contract map**: every part of the render API mapped and checked against this renderer | done (steps C0-C20, see below) |
| FSR 3.1 / DLAA anti-aliasing and DLSS frame generation on this renderer | built, off by default |
| Long play by the owner, then make it the default | pending |

### API contract map (2026-11-17)

Every entry point of the engine's render API (both context vtables, device creators, the work, data and
sync calls, the swap chain, the game-side texture loader, writers that bypass `Map`) was read in the
game's code and written down as a contract: what the call **means**, what this renderer does, and a
verdict. A census mode (`TLG_VKB_CONTRACT_CENSUS=1`) counts which paths each scene really takes.

Fixed in this pass:

- CPU records written through `AsyncMap` reach the GPU (the buffer is read where the CPU writes it).
- Textures the engine moves to new memory (defragmentation, mip drop) keep their contents.
- Texture rewrites happen at the engine's call, in frame order, and only touch the subresource it names.
- Depth copies carry the stencil plane; buffer views bind their own range, not the whole buffer.
- Streaming reads that span several buffers refresh all of them; failed reads upload nothing.
- Occlusion answers can no longer be overwritten by older ones; depth clears are clamped like the engine's.

Checked in four test scenes (and two more), with 0 skipped draws and no device loss.
Paths no scene has reached yet (graphics storage views, 3D render-target slices, PS4 Pro modes, some
sampler modes) stay listed as open to test rather than declared done.

## Performance

Measured on the developer's machine (fps capped at 120):

| Scene | This renderer |
|---|---|
| Game start (Trico's cave) | up to the 120 cap |
| Heaviest saved scene | about 92-102 |

## Known issues

- The launcher's **Start at** warp to `s02_TricosCave / _050_Tritrigger_Get` shows a broken picture on
  **both** renderers, so it is not caused by this one. It is being checked against the same scene reached
  in normal play.
- Experimental: if something looks wrong, switch the launcher back to the default renderer and report it.

## Branches

- `vulkan`: this renderer (default branch of this fork).
- `master`: follows the upstream port.
