# Author's code: findings of the second reads

Rule 19 of the loop: the author's commits are taken as they come. The second read (`vkb-reader`) of
every author sync is advice. Its findings are kept here, one list, updated at every sync. A finding
becomes a fix job only when a sweep shows a wrong picture or a crash that comes from it, or when the
owner asks. Before the official release the owner goes through this list and applies the principles
to the author's code where he finds it necessary (tracked in the owner's own issue 21, outside the
loop).

Format: commit (or area, where the reader saw only the merge diff) | principle | what.
Status of every line below: open, advice only.

## Sync 2026-11-18, first read (merge diff of 34 commits, up to 465579d). Verdict FAIL

Source: pull request 18, first second-read comment.

| Commit / area | Principle | What |
|---|---|---|
| 820a869 W5b (`native_draw.cpp`, `TagAscCsCopy` handler) | 9 | A return address (`0x1f7eec8`) is tested in code, outside the hook table, with no expected bytes. |
| sampler rewrite | 7 | Filter tables, address modes, mip bias forced to 0; old code deleted; no switch. |
| view type 6 fix (b9abef1 V7) | 7 | Field fix with no switch. |
| 6a7291c W12 | 7 | Depth clear clamp and stencil mask with no switch. |
| f758810 | 7 | Depth copy includes stencil, file-read overlap fix: no switch. |
| c8f44b5 A6 | 7 | Buffer view ranges (`ViewRange`): no switch. |
| 51d104a A3 | 7 | Single-subresource texture rewrites: no switch. |
| a4548ab B2 | 7 | Failed file reads no longer uploaded: no switch. |
| be84ec7 W16 | 7 | Probe cubemap layer copy: no switch. |
| F1/F2 presenter | 7 | `PresentImage` split and new acquire-slot waits run on every present, not only with frame generation; no switch. |
| sampler rewrite | 5 | The invalid-filter counter and warning were removed (any filter value is taken `% 9`). |
| a4548ab B2 | 5 | A failed file read returns with no counter. |
| F2 DLSS | 5 | A needed DLSS device extension is silently not added when the list is full. |
| F2 DLSS | 5 | `TLG_VKB_FG=dlss` without the DLLs does nothing and says nothing. |
| e6e1116 | 5 | The in-stream texture upload falls back to the batch path, uncounted. |
| ca33784 K1, 465579d K2 | 5 | The K2 skip count is only in the log; K1/K2 counters are not in the check report. |
| be84ec7, 465579d, 8eb6725 | 5 | Early `return 0` paths in `ProbeFaceCopy`, `AabbReadCheck` and `TextureMove` are not counted. |
| F1a-c FSR | 6 | Sets sync2, subgroup-size control and storage-write-without-format to true without asking the driver; storage use of RG16F / RGBA16F not checked against format support. |
| 9d3bfc7 launcher | 6 | The Video page offers DLSS frame generation with no vendor or driver check; default `choice = 'dlss'` sends non-Nvidia machines to "AA off" with only a log line. |
| debugger mode (1eb78b1 and others) | 13 | `ShaderDump::Keep` runs on every shader compile and keeps SPIR-V and GCN copies in memory with debugger mode off. |
| 465579d K2 | 13 | `RecordingWrites` calls `ActiveGroups` (up to 4096 groups) on every recording; `AabbReadCheck` loops over records on every read. |
| ca33784 K1 | 13 | `TypedUnitRelease` and the fence path take `label_mutex`. |
| be84ec7, 8eb6725, 820a869 | 2 | New side paths (probe cube copy, texture move, AsyncMap placement) have no entry on the principles page list. Owner decision: the three go on the list (pull request from the box). |
| BufferCpuAddressable (f758810) | 3 | A buffer's device copy is retired after the buffer was made, so the owner changes later than "when it is made". |
| 820a869 W5b `MoveBufferNow` | 3, 4 | Overwrites game memory on the CPU at once; relies on a "not GPU-written" test; the diff does not show that earlier GPU work on the source cannot still be pending. |
| 465579d K2 | 3 | `AabbReadCheck` writes 0 into game memory (the count word). |
| `ViewRange`, `MoveBufferNow` | 1 | Decodes a PS4 buffer descriptor at fixed offsets and does a raw guest-memory `memcpy`: leans toward guessing what an address holds. |
| 465579d K2 | 10 | The AABB guard skips reads that name a group not in the table: it treats the symptom of an engine race. |
| whole merge | 7, 11, 12 | Frame generation busy-waits with `yield`; no measured cost stated. Several comments ("owner play") name runs that are not shown. |
| whole merge | one cause per change | 34 commits mixing AA, frame generation, debug capture, sampler fixes, K1/K2, launcher and profile changes. Accepted by the owner (issue 17, rule 19). |

## Sync 2026-11-18, round 2 (1068fdf, c423d17). Verdict FAIL

Source: pull request 18, second-read comment of round 2 (the reader saw the two commits, their messages
and the principles page).

| Commit | Principle | What |
|---|---|---|
| 1068fdf | 1 | Reads engine-internal fields by raw offset (`+0x27a8`, `+0x2870`, `+0x2890`, `+0x28b0`, `+0x2710`) and decodes the game's AABB records: close to guessing a data layout. The reader asks for an owner ruling. |
| 1068fdf | 2 | No entry on the side-path list for the CounterReadback and recording paths it acts on. |
| 1068fdf | 3 | The CPU writes 0 into the count buffer that the GPU's readback fills. |
| c423d17 (default path) | 5 | The AABB skip counter is only in a log line (every 1800 reads), never in the check report; it also counts when the guard is off, so it means "seen", not "skipped". |
| 1068fdf (opt-in) | 5 | `c_aabb_pass_skipped` and `c_third_pass_skipped` are logged only. |
| 1068fdf | 6 | Offsets and hook addresses are tied to one game build; no AMD or Linux run stated in the commit. |
| 1068fdf | 7 | Message names `TLG_LEGACY_READBACK_PASS_FLAGS=1`, which no longer exists; both switches are read once into a `static const`, so they cannot change while the game runs (remark only). |
| c423d17 (default path) | 10 | The default guard is a workaround that catches one case. Older-pass records that still name existing groups get through. The cause fix is off because Trico's feathers disappear (cause not understood; the author's api-contract-plan C21). |
| 1068fdf, c423d17 | 11 | No measured cost stated; the opt-in path visibly loses a picture element. |
| 1068fdf | 13 | With the flag off, every `TagAscCounterReadback` still does up to four reads and atomic stores, `RecordingWrites` does extra atomic stores, `AabbReadStart` increments a counter. Small, not zero, no switch. |
| 1068fdf, c423d17 | proof | "Owner play: pass skipped twice, 0 bad reads" has no check-loop run behind it; two commits mixed, the second reverses the first's default. |

Passed in round 2: principles 4, 8, 9.

## Found while fixing log cause D (issue 36), 2026-11-19

Source: diagnostic takes and the second read of the pull request for branch `fix/vkb-sync-untyped`. No wrong
picture, crash or log line comes from it after that fix, so it stays advice (rule 19).

| Commit / area | Principle | What |
|---|---|---|
| `native_draw.cpp`, `TagMap` (texture Map, the `MapNative` path) and other old tag paths (`compute_exec` counts 711 in one take of s02_01) | 1, 2 | With the new engine on, these still write old-style tags and records into the game's command buffers, which the new engine never runs (V1b). They are read by nothing. Whether the work behind them reaches the new engine another way was not checked. |

## Sync 2026-11-20, author 1bbd48a (22 commits since the last sync). Second read: PASS WITH REMARKS

Source: sync pull request of branch `sync/author-2026-11-19`, second-read comment. Advice only (rule 19).
Correction by the box: the reader said the new hook entries have no duplicate address; they do (see the first line).

| Commit / area | Principle | What |
|---|---|---|
| b2eff48 T2 count hook on the wet simulation function | 2, 24 | Same address as our `WetSim` hook; the second hook is refused, one error line in every scene. Found by script, not by the reader. |
| K2 fix A (AnimQueryOrder, now default) | 7, 12, 10 | Always on, no switch, no way back; waits up to 50 ms in a game worker; tied to one object type; a mid hook runs on every scheduler job (cost not measured). |
| AABB guard deleted | 7, 5 | Old path unreachable; the "0 hits" proof is from the owner's plays, not this box; read counters stop without the census and the report does not say so. |
| TakePendingDynamicMap | 2, 7 | Draw-time flush of in-place Map writes (the engine's Unmap is empty); no switch; fills, clears and other copy hooks not covered (cannot tell). |
| native_vkb_objects.cpp formats | 6, 7 | E5B9G9R9 and cube-array view used without a device check; sRGB mapping of formats 90-93 changed, no switch. |
| sampler mirror fallback | 5 | Fallback to mirrored repeat uses the same counter as the normal case. |
| TagCopyResource different-format copy | 5, 7 | Skipped like the engine; logged 8 times, not counted, no switch. |
| W7 GDS init, K4 camera-cut reset | 7 | Default behaviour, no switch (K4 has a counter, W7 none). K4 logs one warning per camera cut. |
| hard-coded game addresses in handlers (about 8) | 9 | Caller checks fit this one game build only. |
| always-on checks on the draw path (R3d size, pending-map check) | 13 | Cost not measured. |

## Found by the dark-scenes halving (issue 137), 2026-11-23

Source: issue 137 (dark scenes, part 3): halving of the author's commits between 8dc4dcd and c9b65dc on this box (AMD
Linux), one photo per commit of SD_a01_Open_Door_01, SD_a01_B1F_01 and ST_000_Start, scored as the share of pixels off
by more than 16 against the normal renderer (one photo each, AMD Linux only). Same faults as issue 130 (handed to the author). The author's newest
commit 5c21c57 turns off by default the two switches named below and a third one (`TLG_VKB_QUERY_CARRY`, not part of these faults); with it the three scenes are right again (0.6, 0.1 and 5.2 percent; ST_000_Start was 5.7 at the last good commit 8dc4dcd).
The other eight dark scenes are right with 5c21c57 too (0.5 to 7.5 percent, all within or near the normal renderer's own take-to-take spread). All numbers and pictures: issue 137, box comment "dark scenes part 3" of 2026-11-23. What stays open is the cause behind each switch, for when they are turned on again.

| Commit / area | Principle | What |
|---|---|---|
| a1d8cca, G-buffer passes on the workers (`TLG_VKB_PAR_GBUFFER`, default on from a1d8cca to 5c21c57) | 11, 4 | With it on, the still world (walls, floors) is missing in SD_a01_Open_Door_01 (96.9 percent off, 17.4 with it off) and ST_000_Start (93.1, 6.6 with it off). The draws are made (34472 worker draws in one photo, no decline, no log error), so the data the worker reads at its draws is not what the game's own pass would have read. The author's lead (5c21c57): material constant data copied at the worker's draw while the engine rewrites it after the hand-over. |
| e13c3cd, batched draws for every pass (`TLG_VKB_RECORD_ALL`, default on from e13c3cd to 5c21c57) | 11, 5 | With it on, SD_a01_B1F_01 loses its still world (99.6 percent off; 0.3 at the commit before) and SD_a01_Open_Door_01 loses part of it (17.4; 0.6 before). Earlier switch test on c9b65dc: `TLG_VKB_RECORD=0` makes these scenes right; `record.row_failures` 3745 only with it on. Which of the commit's three switches (`TLG_VKB_RECORD`, `TLG_VKB_RECORD_ALL`, `TLG_VKB_FAST_ROW`) carries it was not split by takes here; with 5c21c57, which turns `TLG_VKB_RECORD_ALL` off (and two other switches, see its message), they are right. |
