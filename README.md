<p align="center"><img src="port/packaging/icon/tlg-icon-1024.png" width="160" alt=""></p>

<h1 align="center">The Last Guardian — engine-to-Vulkan renderer</h1>

An experimental fork of [tabnul/the-last-guardian-definitive](https://github.com/tabnul/the-last-guardian-definitive) that adds a **second renderer**: the game's own render API drawn straight with Vulkan. **Windows only**, tested on Windows 11 with an RTX 4090. No game files are included: you need your own copy of The Last Guardian (CUSA03627 / CUSA03745, update 01.03) dumped from your own PS4. For the game files, the launcher, the features and everything the port does, see the upstream project ([README-UPSTREAM.md](README-UPSTREAM.md)).

**This is a work in progress.** `vulkan` holds the latest build that was played and works. **The goal is 120 fps.**

## Progress (2026-11-17)

The game's renderer calls a Direct3D 11 style device layer. With this renderer on (`TLG_VKB=1`, or **Renderer: vulkan** in the launcher), those calls go straight to the port's Vulkan code, the way a PC version of the engine would work. The engine's PS4 graphics code underneath (command packets, PS4 descriptors, guest GPU memory) no longer runs, and neither does any emulation of it. What stays PS4 by necessity: the game's own shaders (recompiled, fed from the engine's resource slots) and a one-time conversion of the PS4 texture layout.

```
Overall                  [█████████████░░░░░░░]  67%
Backend built            [███████████████████░]  97%   every draw, dispatch, copy, clear, fence and flip in Vulkan
Speed                    [█████████████████░░░]  87%   120 fps target at 4K
Picture                  [███████████████░░░░░]  75%   matches the engine's API, checked call by call
Default and clean-up     [██░░░░░░░░░░░░░░░░░░]  10%   renderer on by default, old paths deleted
```

The overall bar is the average of the four below it.

* **Backend built:** nearly done. Engine objects (textures, views, buffers, samplers, shaders, states) get their Vulkan objects when the engine creates them. Draws and dispatches are bound from the engine's slots by object, recorded on the port's own threads, and the game's fences are signalled by the Vulkan work.
* **Speed:** 113 to 120 fps in the default scene, about 92 to 102 in the heaviest scene tested. The game's render thread is the limit in heavy scenes; that work continues.
* **Picture:** every part of the engine's render API has been mapped from the game's code and checked against this renderer, call by call (what each call means, what the renderer does, and whether they match). That pass fixed CPU-written GPU records that never arrived, textures losing their contents when the engine moves them in memory, texture rewrites out of frame order or over the wrong mip, depth copies without stencil, buffer views bound past their range, and streaming reads that refreshed too little. Paths no tested scene reaches yet stay listed as open to test.
* **Default and clean-up:** after more play-testing the renderer becomes the default, and the old emulator paths (still in the code behind switches) are deleted.

## Features

- **Renderer switch:** launcher → Renderer → `vulkan` (sets 4K, 120 FPS and skips the boot logos), or `TLG_VKB=1`. Restart to switch.
- **Anti-aliasing on this renderer:** `TLG_VKB_AA=fsr3` (FSR 3.1) or `TLG_VKB_AA=dlaa` (DLAA), off by default.
- **Frame generation:** `TLG_VKB_FG=dlss` with an AA mode on (DLSS frame generation), off by default.
- **API census** (for testers): `TLG_VKB_CONTRACT_CENSUS=1` counts which paths of the render API a scene takes; the counts are in the debug server's stats.

## Known issues

- The launcher's **Start at** warp to `s02_TricosCave / _050_Tritrigger_Get` shows a broken picture on **both** renderers, so it is not this renderer. It is being compared with the same scene reached in normal play.

## Running it

Use the upstream instructions for the game files and the settings ([Get started](README-UPSTREAM.md#get-started)): start the launcher, pick the game folder, set **Renderer** to `vulkan` and press **Play**. To force it from outside the launcher, set `TLG_VKB=1` before starting it (`TLG_VKB=0` forces the default renderer).

## Credits and license

Built on the work of [tabnul](https://github.com/tabnul/the-last-guardian-definitive) (the definitive port), [HayatoG](https://github.com/HayatoG/tlg_pc) (the original port), [gow3_pc](https://github.com/FelipePrado0/gow3_pc), [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4), all GPL-2.0, as is this project ([LICENSE](port/LICENSE)). Not affiliated with Sony Interactive Entertainment. The Last Guardian is a trademark of Sony Interactive Entertainment. No game code, files or art are distributed here. The icon is original artwork of the original project.
