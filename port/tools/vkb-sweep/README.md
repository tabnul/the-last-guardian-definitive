# vkb-sweep: comparable pictures of the scenes

Scripts for the Linux test box. No AI inside. They take the same picture of one scene with the normal
renderer (`TLG_VKB=0`) and with the new engine (`TLG_VKB=1`), run that over a list of scenes, and measure how
far two pictures differ.

```
source /home/runner/loop/build-env
python3 shot.py OUT STAGE SCENE VKB TAKES [--cam "X Y Z PITCH YAW"] [NAME=VALUE ...]
python3 sweep.py OUT VKB TAKES [--scenes FILE] [NAME=VALUE ...]
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

## Leaving Trico visible (experiment, issue 20)

- Switch: `SHOT_TRICO` in the environment of `shot.py` (and so of `sweep.py`, which passes its environment on).
  `hide` (the default) keeps the recipe as it is. `show` leaves out the "hide Trico" step only: no
  `MS.CondorStopAll`, no `MS.InvisibleEntity`. The own camera, the game speed 0.001 and every wait stay.
  The take's log then says `shot trico hide off (SHOT_TRICO=show)`.
- Pictures with `show` hold a Trico that keeps moving, so they are not the same picture as a `hide` take:
  keep them in their own OUT folder, never mixed with an answer key made with `hide`.

## Output

`OUT/STAGE__SCENE/` holds `vkbV_takeN.png`, the game log of each take (`vkbV_takeN.log`) and `camera.txt`.
The fixed camera comes from the game's own camera in the first take ever made of that scene; every later
take of either renderer uses the same `camera.txt`, so all pictures of a scene are from the same spot.
An existing picture or log is never overwritten: new takes get the next free number. The save is backed up
to `OUT/save_backup` for the run and put back at the end; a backup left from a broken run stops the next run.

Keep `OUT` outside the repository: pictures of the game go only to the `shots` branch of the work repo, never into a code branch.

## sweep.py: a list of scenes

One command for a list of scenes and one renderer, unattended. Every take is one run of `shot.py`, so every
take is a fresh game start with the GPU memory guard and the fixed recipe above.

- The list: by default every point of `../scene-check/scenes.json` minus the three the scene check skips
  (`SKIP` in `scene_check.py`, read from there so there is one list): 219 scenes, in story order.
  `--scenes FILE` takes `STAGE SCENE` lines instead.
- Counters: the debug server is on for every take (`TLG_DEBUG_SERVER_PORT=8790` unless given), and `shot.py`
  keeps `/stats` and `/native` at the moment of the picture as `vkbV_takeN.stats.json` and `.native.json`.
  (`shot.py` does this whenever `TLG_DEBUG_SERVER_PORT` is above 0; without it, nothing changes.
  `TLG_DEBUG_SERVER_PORT=0` given to the sweep switches the server and the counters off.)
  The server is on for both renderers, so the answer key and the new engine's takes are made the same way.
  It answers only when asked (twice per take, after the picture). Checked on one scene only: the garden's
  take-to-take noise with it on (0.348% above 16, 0.155% above 32) is inside the range measured without it
  in issue 9 (0.185-0.372% and 0.070-0.172%).
- One result line per scene (JSON) in `OUT/results-vkbV.jsonl`, written as soon as the scene is done, and a
  short line on the screen. Per take: status, picture path, blank yes/no, crash yes/no (the game process
  ended by itself before `shot.py` closed it, at any step; `shot.py` then prints `game ended by itself before
  the take was done (exit code N)`), log counts (`<Error>`/`<Critical>` lines, `<Warning>` lines,
  `Fault: exception` lines), the new engine's skip and fail counters (left out when the game does not report
  them; the normal renderer reports none), peak GTT (empty where the guard is not active). With 2 or more good takes: `noise`, take 1 against
  take 2 of the same renderer (mean, share of pixels above 8, 16, 32, largest; the same numbers as `compare.py`).
- Resumable: a scene with a result line is not run again. Takes that ended (their line is in
  `OUT/STAGE__SCENE/vkbV_shot.txt`, the output of `shot.py`) count; a take cut off by a stop is left and a new
  one is made. Run the same command again to go on.
- It stops (resume later with the same command) on Ctrl+C or SIGTERM (the running take is ended cleanly and
  the save is put back), when `shot.py` cannot run at all (the game is still running, a save backup is left,
  the display is in use), and when the GPU memory guard fired. A failed take of a scene does not stop it and is
  not taken again on a resume: it is a result (its status is in the line).
  A guard stop leaves the scene without a result line, and its guard take does not count, so a resume runs
  the scene again (raise `SHOT_GTT_LIMIT_GB` first if it really needs more; never `0`).
- No limits are applied: it records, it does not judge clean or not clean (issue 9).

Example, the answer key (normal renderer, 2 takes per scene):

```
nohup python3 sweep.py ~/sweep/answer-key 0 2 > ~/sweep/answer-key/sweep.out 2>&1 &
```

## compare.py

Prints the mean absolute difference per channel (0..255), the share of pixels whose largest channel
difference is above 8, 16 and 32, and the largest difference. With a third path it writes a difference
picture (4x brighter). It refuses a blank picture (exit code 2), so a black screen is never compared.
No clean / not clean limits are applied here: those are the owner's call (issue 9). The blank limits (8 and 99.5%) only decide whether a picture can be compared at all.

## Log only group and the log verdict (verdict.py)

`log-only.txt` lists scenes that are left out of the PICTURE comparison (cutscenes and the like): `STATUS STAGE
SCENE REASON`. `STATUS` is `proposed` or `confirmed`. Only the owner confirms (an issue); the tools honour only
`confirmed` entries, a `proposed` scene is judged by picture as before. A scene without a usable normal-renderer
picture (fewer than 2 good takes) is log only by itself.

Every scene gets a log verdict, next to the picture verdict (log only scenes get nothing else): `ran` (the photo
moment was reached, the game did not end by itself), `frames` (the game window was captured; a flat picture fails
only where the normal renderer's is not flat), `crash` (no more `Fault: exception` lines than the normal
renderer's log of that scene), `lines` (no `<Error>`/`<Critical>`/`<Warning>` line kind that the normal renderer's
log of that scene does not have; numbers and addresses are blanked, so one kind counts once), `skips` (new engine:
all skip/fail counters 0). No reference log: `lines` is not compared and says so.

`sweep.py ... --reference DIR` writes `group`, `group_reason` and `log_verdict` into every result line (DIR = the
normal renderer's sweep). `python3 verdict.py RESULTS.jsonl [--reference DIR]` prints them for a finished sweep
with the count per group. Nothing in the game or the engine changes.

## The noise mask (noisemask.py, noise-ignore.txt)

Some scenes differ between two takes of the NORMAL renderer (boy in another place or frame, drifting mist, grass in
the wind, flames, particles): the photo is not the same moment. `noise-ignore.txt` lists, per scene, the part of
the picture that the picture check leaves out. One rule for every cause: a pixel counts only where the takes of
the normal renderer agree with each other (no two differ by more than 16), the left-out area grown by 4 pixels
(`noisemask.py make A.png B.png [C.png ...] MASK.png`; the mask is made again from the good takes of the reference
run, it is not stored). Two takes catch only what moves between those two. An entry whose CAUSES field holds `takes=3` (issue 27) is
made from the first three good reference takes (the reference run needs `TAKES=3` for that scene; fewer takes give
`masked_error` in the result, not a silent skip). All other entries keep the first two takes, as measured.
`python3 compare.py A.png B.png --mask MASK.png` measures without the masked pixels and prints the share left out.

Same status rule as the log-only list: `proposed` or `confirmed`, only the owner confirms, the tools honour only
`confirmed`. A scene where more than 25% would have to be left out is on the log-only list instead.
`sweep.py` (new engine, with `--reference`) writes `picture`: take 1 against the normal renderer's take 1, the
whole picture (`all`) and, for a confirmed entry, without the mask (`masked`). Nothing in the game or the engine
changes.

### Confirmed entries (2026-11-18, owner, issue 27)

`log-only.txt`: 35 scenes `confirmed` on 2026-11-18 (later additions are listed in the file itself; 8 cutscenes, 6 endings, 15 too much moves or alive with action, 6 "need a look").
`noise-ignore.txt`: 56 scenes `confirmed`: the owner's list, and only where the retake (noise-126, two new normal takes
against the answer-key take, outside the mask) stays within the limit. 39 entries stay `proposed` (full picture check),
(not `s02_01_Care_Step1_001`: the owner put it on the log-only list, event-driven scene, picture not comparable between takes; issue 27). Scenes still judged by the whole picture: everything not on a confirmed line.

### 3-photo masks (issue 27, owner decision 12:46)

For the 38 scenes whose retake did not agree with the 2-photo mask: mask from answer key 1+2 and retake 1,
measured on retake 2 against each of the three, outside the mask (no game run: the four existing normal photos).
13 scenes come within the limit (`noise-ignore.txt`, still `proposed`, new share in the line). 23 still fail or
would need more than 25% left out: `log-only.txt`, `proposed`. `AT_BarrelBox_001` and
`ST_040_FallingFromSentinelTower` are `confirmed` log only (owner, comment 6979040549).
A confirmed entry with fewer good reference takes than it needs now gives `masked_error` (before: for one take the mask was skipped silently).

## Solid-blob mask (issue 27, owner decision 2026-11-18)
`noisemask.make(pics, mask, solid=True)` opens the raw difference (erode then dilate, 5 pixels) before the usual growing, so sparse
speckle and thin rims on still surfaces drop out and only solid blobs stay. An entry would opt in with `solid` in its CAUSES field
(`noisemask.is_solid`); no entry has it yet, nothing is confirmed or un-confirmed. `solid-measure.py LIST OUT.tsv` measures old and
solid mask on the existing normal-renderer photos (no game run). Result on 69 scenes: 31 within the limit, 23 above, 15 cannot.

### Applied (owner decision 2026-11-19, PR 33)
All 69 entries now use the solid-blob mask: `confirmed` in `noise-ignore.txt` with `solid` in CAUSES (31 scenes within the limit).
23 scenes a bit above the limit also carry `own-limit`: the picture limit for them is twice the scene's own noise (issue 9)
instead of the fixed 1% / 0.5%. `sweep.py` now builds the mask with `solid` when the entry has it. Note: no code in the sweep
judges a picture against a limit yet, so `own-limit` is data for the report step. The 15 scenes that stay far above the limit moved to
`log-only.txt` (`confirmed`); they still run every sweep and are judged by the log. Switch: remove `solid` from an entry's CAUSES
to go back to the 2-photo mask, or remove the line from `log-only.txt`.
