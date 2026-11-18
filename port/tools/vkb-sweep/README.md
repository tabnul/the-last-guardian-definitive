# vkb-sweep: one comparable picture of one scene

Scripts for the Linux test box. No AI inside. They take the same picture of one scene with the normal
renderer (`TLG_VKB=0`) and with the new engine (`TLG_VKB=1`), and measure how far two pictures differ.

```
source /home/runner/loop/build-env
python3 shot.py OUT STAGE SCENE VKB TAKES [--cam "X Y Z PITCH YAW"] [NAME=VALUE ...]
python3 compare.py A.png B.png [DIFF.png]
```

Example:

```
python3 shot.py ~/vkb-shots s13_FirstFlight FF_b01_EnterGarden 0 2
python3 shot.py ~/vkb-shots s13_FirstFlight FF_b01_EnterGarden 1 2
python3 compare.py ~/vkb-shots/s13_FirstFlight__FF_b01_EnterGarden/vkb0_take1.png \
                   ~/vkb-shots/s13_FirstFlight__FF_b01_EnterGarden/vkb1_take1.png
```

## What one take does

1. Fresh start of the game with the save `../scene-check/start.save` (the player's save is put back after).
2. Waits for the title screen, then makes CONTINUE load `STAGE` / `SCENE` instead.
3. Waits until the load overlay has ended, then 8 s.
4. Hides Trico, switches to its own camera at a fixed position (set again every tick), game speed 0.001.
5. Waits 4 s and takes the picture of the game window on the virtual screen.

A take fails (and the script exits with 1) when a step does not happen in time, when the game ends, or
when the renderer that runs is not the one asked for (the new engine names itself in the log), or when
the picture is blank (one flat colour, such as a black screen: at least 99.5% of its pixels within 8 of
the most common colour). A blank picture is kept as `vkbV_takeN.blank.png`, never as a normal take.

The game runs on its own Xvfb (`SHOT_DISPLAY`, default `:93`) with `MESA_VK_WSI_DEBUG=sw`, so the picture
is what the game presented, taken the same way for both renderers. Fixed settings for every take:
`TLG_SKIP_LOGOS=1 TLG_FPS=unlocked TLG_HDR=0 TLG_FG=0 TLG_DLAA=1`; `TLG_VKB` is always set from the
`VKB` argument. Extra `NAME=VALUE` arguments go into the game's environment; no other `TLG_` setting of the
calling shell reaches the game.

## GPU memory guard

Why: on 2026-11-17 a test run of the new engine filled the GPU memory (GTT 31.5 of 33.4 GB). A game thread
then hung inside the AMD kernel driver, the process could not be killed, and the host had to be
power-cycled (issue 11). Killing the game before the memory is full avoids that.

- Before each take the script reads the GPU memory use (`/sys/class/drm/card*/device/mem_info_gtt_used`
  and `mem_info_vram_used`) and prints it. While the game runs, a thread reads it every 0.25 s (a GTT fill faster than that between two reads is not caught).
- When GTT use of any card passes the limit, the whole process group of the game gets SIGKILL at once.
  The take fails with `GPU memory guard: X GB used, limit Y GB (card, game killed)`, the remaining takes
  of the run are not started, and a line starting with `MAJOR:` is written to the journal
  (`SHOT_JOURNAL`, default `/home/runner/loop/journal.md`; printed instead when that file does not exist).
- The limit is on the absolute use, not on the growth during the take. When the use is already above the
  limit before the game starts, the take is not started.
- After each take the peak GTT and VRAM use of that take is printed.
- Switch: `SHOT_GTT_LIMIT_GB` (default `16`; 1 GB = 2^30 bytes). `0` switches the guard off. The normal
  renderer's garden take peaks at about 4.6 GB and the new engine's GPU pool at about 10.6 GB; runs that need more (for example a new engine change
  with a large GPU pool) raise the limit, they do not switch the guard off.
- Without those sysfs files (another GPU maker, Windows) the script says once
  `GPU memory guard: NOT ACTIVE` and runs without the guard.
- It is part of this test tool only: nothing in the game or the engine changes, and it costs nothing when
  the tool is not used.

## Output

`OUT/STAGE__SCENE/` holds `vkbV_takeN.png`, the game log of each take (`vkbV_takeN.log`) and `camera.txt`.
The fixed camera comes from the game's own camera in the first take ever made of that scene; every later
take of either renderer uses the same `camera.txt`, so all pictures of a scene are from the same spot.
An existing picture or log is never overwritten: new takes get the next free number. The save is backed up
to `OUT/save_backup` for the run and put back at the end; a backup left from a broken run stops the next run.

Keep `OUT` outside the repository: pictures of the game go only to the `shots` branch of the work repo, never into a code branch.

## compare.py

Prints the mean absolute difference per channel (0..255), the share of pixels whose largest channel
difference is above 8, 16 and 32, and the largest difference. With a third path it writes a difference
picture (4x brighter). It refuses a blank picture (exit code 2), so a black screen is never compared.
No clean / not clean limits are applied here: those are the owner's call (issue 9). The blank limits (8 and 99.5%) only decide whether a picture can be compared at all.
