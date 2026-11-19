# RenderDoc frame capture and export on Linux (test tool, off unless TLG_RENDERDOC=1)

1. Portable RenderDoc (no sudo): unpack the official Linux tarball from renderdoc.org into a folder (here
   /home/runner/tools/renderdoc). In `etc/vulkan/implicit_layer.d/renderdoc_capture.json` change `library_path`
   to `../../../lib/librenderdoc.so`. `qrenderdoc` needs libproxy1v5 and libduktape207 (apt-get download, dpkg-deb -x
   into a private folder, put it on LD_LIBRARY_PATH; see run.sh).
2. Capture: start the game with `TLG_RENDERDOC=1 TLG_RENDERDOC_DIR=<folder> TLG_DEBUG_SERVER_PORT=<p>` (through the
   photo tool with the GPU memory guard; a capture needs about 21 GB of GTT, so `SHOT_GTT_LIMIT_GB=28`; a capture file
   is 1-3 GB), then `curl localhost:<p>/renderdoc?capture=1` when the scene stands. Works with TLG_VKB=0 and 1.
3. Read a capture: `run.sh script.py` runs a script inside qrenderdoc on a virtual screen (Xvfb; the first start asks
   the analytics question once: `click.py :97 332 517 836 556` answers "do not gather"). Scripts: `list.py` (all
   dispatches), `desc.py` (resources bound at the dispatches of one size), `export.py` (every bound image and buffer of one
   dispatch, as raw files with an index.json). Variables: RDOC_CAP, RDOC_OUT / RDOC_EID, RDOC_OUTDIR, DISPLAY.

## Limits (read before you trust a replay)

- **New engine: imported game memory replays as zeros.** The new engine imports game memory into Vulkan
  (VK_EXT_external_memory_host). RenderDoc does not see what the game writes there. On the AMD Linux test box the frame
  ring stays in place (the start log says the ring mapping is above 40 bits), so it is one such buffer of several hundred
  MiB, and in a capture it is all zeros. Every copy or shader that reads it replays with zeros. Example: small definition
  buffers copied from the ring at the frame start were zero in the replay, so per-tile lists computed from them came out
  empty, while the live lists (the capture's initial contents) had the right counts.
- **What stays trustworthy in a new-engine capture:** the initial contents of resources the GPU writes (read from the live
  GPU at capture start), textures, and anything not fed from imported memory.
- **The normal renderer is not affected** (it copies game memory into its own buffers).
- **How to spot it:** look at `ctrl.GetUsage()` of the buffer that feeds a suspicious input; a `vkCmdCopyBuffer` whose source
  has a large offset into a several-hundred-MiB buffer that reads as zeros is this case. Do not conclude anything from
  a replayed result that depends on such a buffer.
- There is no switch yet that makes the ring take a copied route for capture runs. `TLG_VKB_RING_VRAM` (default on, `=0`
  keeps the ring in place) goes the other way, and on this box the placement is refused anyway.
