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
when the renderer that runs is not the one asked for (the new engine names itself in the log).

The game runs on its own Xvfb (`SHOT_DISPLAY`, default `:93`) with `MESA_VK_WSI_DEBUG=sw`, so the picture
is what the game presented, taken the same way for both renderers. Fixed settings for every take:
`TLG_SKIP_LOGOS=1 TLG_FPS=unlocked TLG_HDR=0 TLG_FG=0 TLG_DLAA=1`; `TLG_VKB` is always set from the
`VKB` argument. Extra `NAME=VALUE` arguments go into the game's environment; no other `TLG_` setting of the
calling shell reaches the game.

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
picture (4x brighter). No thresholds are applied here: what counts as clean is the owner's call.
