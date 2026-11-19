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
