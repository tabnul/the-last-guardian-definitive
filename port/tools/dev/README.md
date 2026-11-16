# Dev tools

Test and measuring tools for the engine-to-Vulkan backend work (`TLG_VKB`). Everything is found from this folder's place in the repo; the machine's own paths
(game folder, save backups, MSYS2) come from `paths.local.json` here, which is git-ignored: copy `paths.example.json` to it and fill it in. The game's data folder is
`port/user` (or `TLG_USER_DIR`), the same as the launcher's default.

The debug server (`TLG_DEBUG_SERVER_PORT`, default 7777), headless mode (`TLG_HEADLESS=1`) and auto start (`TLG_AUTO_START=1`) come with the backend; the tools
that read the debug server need them.

| Tool | What it does |
|---|---|
| `build.cmd` / `build.ps1` | builds the port in MSYS2 CLANG64 as `build.sh` does, applies the FSR patches with Windows git (MSYS2's git fails on a CRLF checkout), copies the exes to `port/out`. `-Pgo use\|off` (default: use when `pgo/tlggpu.profdata` exists) |
| `play_start.ps1` | starts the game through `launcher/tlg_run.py` with the debug server; `-Headless`, `-Measure` (the measuring set), `-Extra "NAME=VALUE,..."`, `-Tag` (log `user/log/game_<tag>.txt`) |
| `restore_save.ps1 <name>` | copies a named save backup (`saves` in `paths.local.json`) over the live slot, hash checked |
| `vkb_run.ps1` | one headless run: restore a save, start, wait for N presents, read debug-server endpoints into `user/log/runs/<tag>_<endpoint>.json`, print fps. Heaviest save: read at 3,200 presents or later |
| `vkb_run_snapshots.ps1` | the same, every endpoint file numbered (two snapshots), for the `vkb_gpu_*` reports |
| `vkb_gpu_frame.py`, `vkb_gpu_passes.py`, `vkb_gpu_copies.py` | GPU ms a frame by kind of work, by pass, by copy kind, from two `/stats` snapshots |
| `vkb_dump_png.py` | the backend's raw image dumps (`user/log/vkb_image_dump`) to PNG |
| `wall_ledger.ps1` (+ `wall_ledger.py`, `ledger_median.py`, `timeline_report.py`) | per scene: fps, thread CPU from Windows, GPU clock, an 8 s timeline, `/stats`; `-Repeat N` gives medians |
| `profile_leaf.py` | top functions of a leaf profile (`/profile?thread=ps4render&seconds=8&hz=1000&top=200&leaf=1`), names from `llvm-symbolizer` |
| `threads.cmd` / `threads.ps1` (+ `profile_view.py`) | live view of the game's threads, frame rate and switches from the debug server |
| `shadps4_mcp.py` | MCP server (stdio) over the debug server: status, threads, stats, profile, logs, memory, hooks. Register: `claude mcp add shadps4 -s local -- python "<repo>\port\tools\dev\shadps4_mcp.py"` |

In-game dev keys (with the backend): F5 / Shift+F5 frame walk (freeze, record), F6 / F7 glitch markers, F8 stall profiler, F9 / Shift+F9 frame and probe
profilers, F12 / Shift+F12 screenshot and 5 s video.
