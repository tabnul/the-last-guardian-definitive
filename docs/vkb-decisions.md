# New engine (VKB): decision log

Newest on top. One entry per design decision (rule 15 of the loop). Each entry: date, the problem,
the ways considered, the choice and the reason, who chose, links. Plain words, no game data.
Where a source is unclear, the entry says so. Entries are from 2026-11-17 and 2026-11-18 (box times are UTC,
as shown by GitHub). Repo: `tabnul/tlg-vkb-work`.

An entry marked "not yet merged" counts only once the owner merges the pull request that carries it.

---

## 2026-11-20 - Draw targets: reuse the last draw's answer while the targets stay the same (not yet merged)

- **Problem:** every draw looked up its colour and depth targets twice in the engine's object table (once to check the
  view is known, once for the attachment), each time under the shared objects lock, although 98% of draws use the same
  targets as the draw before. Measured on the condor scene: about 1.5 ms per frame on the engine's render thread (issue 82).
- **Considered:** (1) each thread keeps the targets it found for its last draw and reuses them while the draw's target
  set (view ids, formats, slots in use) is the same and the existing view change counter (moves when a view or an image
  goes) has not moved; one place, in the draw recorder; (2) a small per-view answer table inside the attachment lookup,
  as the existing lookup memo for shader textures does: also helps when two target sets alternate, but needs a second
  table for the first-use view check and still does one table probe per target per draw; (3) split the shared lock:
  that is point 1 of the issue, larger and riskier, a design note first (owner).
- **Choice:** 1. It matches the measured pattern (same targets as the previous draw), keeps the exact answers the
  lookups give (the same change counter the shader-texture memo trusts), and adds one switch and no new path for any
  machine. Owner chose to fix point 2 ("yes", issue 82); the box chose the way.
- **Links:** issue 82; pull request of branch `fix/vkb-target-reuse-issue82`.

## 2026-11-20 - A second picture measure for faint detail, as a separate tool (not yet merged)

- **Owner review 2026-11-20 (pull request 97), added to this entry:** owner: "some should be ignored (log only), some it is way too harsh". Done: (1) the tool reads the confirmed log-only list (no per-scene rule in the tool); the fight scene and the two ending videos the owner named are on that list already. (2) a flagged tile must also change its detail by at least 0.6 grey levels in absolute terms, so faint dark differences nobody can see are not flagged. This clears the owner's two "no difference by eye" scenes and the mild floor-grain scene without a mask; the Tritrigger wall without the fix and the blurred rock scene stay flagged. The two masks the owner asked for were not needed, since neither scene is flagged any more. Cost: a tile whose detail falls from about 1.0 to 0.3 grey levels is no longer flagged, so very faint losses in dark areas are not seen by this tool. Proof numbers (new verdicts, from stored pictures): FA_f02_End OK, FB2_End OK, FF_c01_TunnelEntrance OK, SF_c02_Before_bridge OK, Care_Step2 LOST (404 tiles), Tritrigger without PR 51 LOST (168 tiles), with PR 51 UNJUDGED. The fight scene is SD_a01_Start (confirmed in log-only.txt). The list is read only with `--scene-dir`. Chosen by: owner asked, box built.
- **Problem:** the picture check counts pixels that differ by more than 8, 16 or 32 levels. A faint detail that goes
  missing on a still surface (carved lines on a wall, under 10 levels) never reaches those limits, so the check could
  not see it (issue 54).
- **Considered:** (1) a new tool `detail.py` that compares how much fine detail each 64x64 tile holds in the new
  picture against the normal renderer, with an allowance taken from the normal renderer's own take-to-take spread in
  that tile; (2) lower the existing limits: not allowed, the limits are the owner's (issue 9) and it would flag every
  moving thing; (3) a whole-picture sharpness number: too coarse, one lost wall is a few percent of the picture.
- **Choice:** 1, as a proposal. It changes no limit and nothing calls it (off costs nothing). Tiles whose coarse
  brightness also differs are treated as moved things and left out, and it only flags 10 touching tiles or more, so
  a boy's pose or a door that moves does not count. Box choice; the owner decides by merging. The verdict it adds
  (LOST / ADDED / LOST+ADDED) is not part of the limits. The numbers were set on stored pictures of this box only
  (AMD Linux); a blank picture, a size difference or too many moved tiles give no verdict instead of a false OK.
- **Links:** issue 54; pull request of branch `fix/detail-measure-issue54`.

## 2026-11-20 - Fix A's wait keeps the game's job scheduler awake (not yet merged)

- **Problem:** The author's fix A makes the game's query recording wait (up to 50 ms) for the soldiers' animation work
  of the next game step. In the soldier scenes the limit ran out 6 to 27 times per take at 120 FPS on the owner's PC, each
  time one frame of 53-56 ms (issue 78). Measured on this box: the waiting worker is counted as busy by the game's job
  scheduler, which relies on every busy worker coming back soon and signalling. In the timeouts it then sleeps once more
  after its cycle is finished, or leaves a queued task unrun, so the next step (the one waited for) cannot start.
- **Considered:** (1) while waiting, give the scheduler's "work available" signal every 0.5 ms, as the game's own enqueue
  does: an idle worker wakes, runs what is queued and wakes the scheduler; no game memory written, covers both forms;
  (2) mark the waiting worker as idle in the scheduler's busy count and signal once: no delay, but writes the game's
  scheduler state, covers only the first form, and is wrong when the recording runs on a thread that is not counted;
  (3) let the waiting worker run the game's queued jobs itself: covers both, but copies the scheduler's queue logic
  (three queues, locks, the exclusion check) into the backend; (4) no wait at all, keep late soldier queries for the next
  recording instead: changes what the game's data means and could bring back what fix A guards against.
- **Choice:** 1. It removes the cause for every scene that waits (principle 10), uses only the game's own wake path and
  keeps its order (4), and is CPU side only, the same on every machine (6). Switch `TLG_VKB_SCHEDULER_WAKE` (default on;
  `=0` the old wait). The 50 ms limit stays as a safety net. Box proof in the pull request; the 120 FPS check on the
  owner's PC is still to be run.
- **Who:** box (rule 14).
- **Links:** issue 78 (https://github.com/tabnul/tlg-vkb-work/issues/78); pull request for branch `fix/vkb-scheduler-wake`.

## 2026-11-20 - Author sync: how the four merge conflicts were resolved (not yet merged)

- **Problem:** Merging the author's branch into ours conflicted in four files (`native_draw.cpp`, `native_legacy.h`,
  `native_vkb_aa.cpp`, `native_vkb_objects.cpp`). The author deleted the guard that stopped the old AABB reader,
  while our table lock (pull request 52, switch `TLG_AABB_TABLE_LOCK`) is still used by the K2 census check.
- **Considered:** (1) take the author's side everywhere and drop our lock; (2) keep both: the author's code as it is,
  and our lock kept only where the census check and the start of recording still use it; (3) put the author's commit
  aside (not allowed, rule 13).
- **Choice:** 2. No author commit is left out or changed; our lock stays a switch, used only with `TLG_K2_CENSUS=1`
  and at the start of recording. Our water fix (mesh key, pull request 72) is untouched by the author and stays on.
  One restore commit puts back the RCAS declarations next to the author's new K4 counter.
- **Who chose:** box (rule 13), the owner can overrule at the merge.
- **Links:** sync pull request of branch `sync/author-2026-11-19`.

---

## 2026-11-20 - One hand-over route when an anti-aliasing cannot run; DLAA hands over to FSR 4 or FSR 3.1 (not yet merged)

- **Problem:** in the new engine a DLAA that could not run (DLSS bridge from another version on the owner's PC; a DLSS
  feature that cannot be made; a frame that fails) left the rest of the session without anti-aliasing (issue 99). The
  owner: "fall back to FSR 4, or FSR 3.1 where FSR 4 is not available, like the old engine does". Three other cases (a
  choice without an AA, FSR 4, TAA) each had their own hand-written fallback, so this is the fourth fix of that kind
  (rule 14f).
- **Considered:** (A) one more fallback routine for DLAA; (B) one hand-over route for every AA that cannot run: one rule
  for what comes next, one line written when the AA that takes over is really made (a chain is one line), errors and
  an on-screen message for faults, info for the two quiet cases the owner named (DLSS not installed, FSR 4 model files
  not installed); (C) a list of AAs to try per choice: rebuilds how the AA is made, no extra case covered.
- **Choice:** B. It removes the cause for every AA at once (principle 10) and replaces the three earlier routines; plain
  code, so every machine (6). What comes next: DLAA -> FSR 4 on a card that runs it (the normal renderer's own FSR 4
  test), else FSR 3.1; FSR 4, TAA and a choice without an AA -> FSR 3.1; when FSR 3.1 or the parts every AA shares
  cannot be made, the line says AA is off. A DLSS that is installed but did not come up with the device is caught at
  start, before the first frame. Switch `TLG_VKB_DLAA_FALLBACK` (`=0` the old DLAA way: FSR 3.1 when not installed,
  AA off when it fails).
- **Who:** box (rule 14); the fallback order is the owner's (issue 99).
- **Links:** issue 99 (https://github.com/tabnul/tlg-vkb-work/issues/99), design note
  https://github.com/tabnul/tlg-vkb-work/issues/99#issuecomment-7046024649 ; pull request for branch
  `fix/vkb-dlaa-fallback`.

---

## 2026-11-20 - 14 scenes where the photo setup crashes the normal renderer: log only (not yet merged)

- **Problem:** in 14 scenes the normal renderer crashes when the photo is set up (7 scenes in both takes, 7 in one take;
  one of them also gives a blank picture), so there is no usable picture to compare with (issue 20).
- **Considered:** keep judging them by picture (impossible without a normal picture); fix the normal renderer (not
  allowed); judge them through the log only, as other confirmed entries.
- **Choice:** the 14 scenes the owner ticked go into `log-only.txt` as `confirmed`. They still run in every sweep and
  errors, warnings and crashes in them are fixed as usual. Owner words: "all of the other checked images look good,
  differences are moving objects or antialiasing"; the cutscene `CH_e01_EndCare_001` he accepts as log only. The scene
  `FA_a04_OpenGate_002` is not added: he cannot judge it (no normal picture), it stays as it is.
- **Chosen by:** owner. Issue 20; pull request: this one.

---

## 2026-11-20 - Texture reads out of per-pixel branches in the new engine's translator (not yet merged)

- **Problem:** with forced 16x AF the new engine drew horizontal stripes in the sky of one scene (issue 75). The cloud
  shader reads the cloud picture inside a per-pixel "if" (made from a console region where pixels are switched off); a
  read with automatic level choice looks at its 2x2 neighbours, and neighbours that skipped the "if" bring leftover
  values, so the level and direction are undefined at the edge of the "if". That holds for every pixel shader with
  such a read, not only this one.
- **Considered:** (1) in the translator, move each such read, with the plain arithmetic its coordinates need, to the
  block before the outermost per-pixel "if"; reads have no side effects, so the values inside stay the same; reads
  that need anything else stay and are counted with a reason; (2) let the whole 2x2 group enter the "if" and keep the
  per-pixel condition for results and writes: covers every derivative, but needs quad operations (an optional driver
  feature, a second path) and a bigger change to how branches are built; (3) forced AF off for samplers read inside
  such an "if": hides the fault, the level stays undefined, the player loses sharpness.
- **Choice:** 1, chosen in the design note on issue 75 and built as planned. Plain SPIR-V, so every machine (principle
  6); one rule for all pixel shaders (10); the right picture first, the cost is stated in the pull request (11).
  Widened on the way: a constant buffer read at an address that is the same for every pixel may move with the read (on
  the console it is a scalar load that runs for the whole wave anyway; out of range it reads zero). Narrowed after the
  second read: a constant read by raw address does not move, and a shader that writes memory is left as it is (a read
  must not pass a write); both are counted. The normal renderer has the same fault in its own
  translation and keeps it (principle 8). Way 2 stays the fallback if a sweep shows a wrong picture from a read that
  way 1 keeps. Switch `TLG_VKB_READS_OUT_OF_BRANCH` (`=0` the old translation).
- **Who:** box (rule 14).
- **Links:** issue 75 (https://github.com/tabnul/tlg-vkb-work/issues/75); pull request for branch
  `fix/vkb-reads-out-of-branch`.

## 2026-11-19 - The AA's shift: a second pass of a mesh is known by its index data, as in the normal renderer (not yet merged)

- **Problem:** with an anti-aliasing on (FSR 3.1 by default since pull request 51), the stained-glass window of the first room
  lost most of its lit glass, so the metal frames behind it showed as extra dark lines (issue 54). The cause is the small
  per-frame picture shift (jitter) the AA needs: a surface that is drawn a second time over its own depth must get the same
  shift as its first draw, or its depth test fails on part of it. The new engine had that rule, but it knew a mesh by its
  draw counts and its vertex shader, so a second pass with another vertex shader never matched (0 matches in every take).
- **Considered:** (A) know a mesh by its index data (address of the first index and the index count), as the normal
  renderer does; (B) shift every draw that tests a shifted depth (the normal renderer's debug mode "follow"): also shifts
  lights and fog volumes, which the normal renderer does not do; (C) no shift after the scene geometry, or a mesh table
  kept across frames: other behaviour than the normal renderer, more special paths. Two quick tries before the note did
  not help and were dropped: FSR 3.1 reading the cleaned motion vectors, and keeping the frame's shift after the AA's step.
- **Choice:** A. The same rule as the normal renderer, no card feature, it replaces the old key. Switch
  `TLG_VKB_AA_MESH_KEY` (`=0` the old key). Proof: window 0.0% / 3.1% off (old key 20.2% / 19.2%), garden unchanged.
- **Who:** box (rule 14).
- **Links:** issue 54 (https://github.com/tabnul/tlg-vkb-work/issues/54), design note
  https://github.com/tabnul/tlg-vkb-work/issues/54#issuecomment-7024036618 ; pull request for branch `fix/vkb-aa-mesh-key`.

## 2026-11-19 - TAA in the new engine: the normal renderer's own TAA shader (not yet merged)

- **Problem:** a player who chose TAA (in-game menu or launcher) got, in the new engine, an error line "the new engine has no
  TAA yet" and FSR 3.1 instead (pull request 58). That line is a fault until TAA runs (rule 24).
- **Considered:** (A) run the normal renderer's TAA shader itself: it is already compiled into the same library, so the
  new engine loads that compiled shader as it is, with the inputs its anti-aliasing step already collects (scene colour,
  depth, motion in pixels, the jitter) and its own two history images; the sharpening is the passes the new engine already
  has (as for DLAA). (B) a copy of the TAA shader in the new engine's shader folder: two copies that can drift apart.
  (C) keep FSR 3.1 for a TAA choice: the fault stays.
- **Choice:** A. One TAA in the code base instead of two; the same picture rule as the normal renderer by construction; a
  plain compute shader, so no card feature is involved. With this, every anti-aliasing the game's settings can load has
  its own path in the new engine (off, FSR 3.1, FSR 4, DLAA, TAA). The TAA's depth test wants the game's camera; the new
  engine reads none, and the normal renderer finds none in this game either, so both give it zeros (the same test).
  Giving TAA a camera would change both renderers: the owner's call. Switch `TLG_VKB_TAA` (`=0` the old way: fault line
  and FSR 3.1).
- **Who:** box (rule 14).
- **Links:** issue 57 (https://github.com/tabnul/tlg-vkb-work/issues/57); pull request for branch `fix/vkb-taa`.

## 2026-11-19 - The frame ring is mapped below 40 bits with a placed mapping (not yet merged)

- **Problem:** On this box (AMD, Linux) the frame ring never moves into graphics memory (R3a): the driver picks an
  address above 40 bits for the CPU's view of it, and the game's buffer descriptions hold only 40 bit addresses. One
  error line in every take (issue 36, log cause B), and ring data takes the slower guest / staging / memo routes.
- **Considered:** (1) ask the driver to map the same memory at an address we reserve below 40 bits
  (`VK_EXT_map_memory_placed`, asked once at start; the range comes from the runtime's own low memory); (2) import
  private memory we place below 40 bits (`VK_EXT_external_memory_host`): host memory, not device memory, so a card
  with its own memory reads the ring over the bus; (3) count the ring's window in the GPU pool as device memory: only
  renames the in-place route, AMD Linux only, a special path.
- **Choice:** 1. It removes the cause for every driver with the feature (principle 10), keeps today's way as the second
  path with a message that says why (principle 6), keeps the same kind of memory (11), and adds no special path. Linux
  only for now: on Windows the reservation would need a placeholder, not built; Windows keeps today's way unchanged.
  Switch `TLG_VKB_RING_PLACED` (default on; `=0` today's way). The report's `frame_ring_in_device_memory` names the route.
  Proof: garden 2 takes per arm on this box, and the medium check in the pull request.
- **Who:** box (rule 14).
- **Links:** issue 36 (https://github.com/tabnul/tlg-vkb-work/issues/36), design note
  https://github.com/tabnul/tlg-vkb-work/issues/36#issuecomment-6999062612 ; pull request for branch
  `fix/vkb-ring-placed-map`.

## 2026-11-19 - Every anti-aliasing choice in tlg.ini reaches the new engine, read as the game reads it (not yet merged)

- **Problem:** with the new engine, some tlg.ini choices gave no anti-aliasing at all, without a line: FSR 4.1.1
  (`upscaler=fsr411`), TAA, an unknown name. The launcher had its own small table of choices; the third fix of that
  kind (after pull requests 51 and 56), so treated as recurring (rule 14f).
- **Considered:** (A) run FSR 4.1.1 in the new engine: cannot run on the test box, a second FSR 4 path, and the game's own
  settings code reads `fsr411` in tlg.ini as DLSS anyway (no menu offers 4.1.1; only the test override
  `TLG_UPSCALER=fsr411` runs it). (B) map `fsr411` to the new engine's FSR 4: one more table entry, differs from what the
  game loads, TAA still without AA. (C) the launcher reads the choice with the game's own loading rule for every value
  and passes it on; the engine names every value; a choice it has no AA for (TAA, an unknown name) is a fault (error
  line, on-screen message, counter) and FSR 3.1 runs.
- **Choice:** C. One rule instead of a table, checked by a test against the game's real loader; the new engine gets
  the AA the normal renderer runs for the same file; nothing depends on the card. TAA in the new engine is a separate
  job. Making `fsr411` mean FSR 4 would change settings code the normal renderer shares (principle 8): owner's call.
  Switch `TLG_VKB_AA_CHOICE` (`=0` the old table).
- **Who:** box (rule 14).
- **Links:** issue 57 (https://github.com/tabnul/tlg-vkb-work/issues/57); pull request for branch `fix/vkb-aa-choice`.

## 2026-11-19 - The new engine runs FSR 4 itself; the owner's FSR rules for a choice, a fresh launch and a failure (not yet merged)

- **Problem:** in the new engine a player's FSR 4 choice ran as FSR 3.1 (the launcher mapped it, the engine had no FSR 4).
  The owner's rules (pull request 51, 2026-11-19): "Default on a fresh launch (player chose nothing): DLSS if available;
  else FSR 4 on an RDNA 4 card (RX 9000); else FSR 3.1." "A player who CHOOSES FSR 4 gets FSR 4 on any card that can run
  it." "No card-based fallback. If a chosen FSR 4 fails to start, that is a fault: error line, visible to the player, no
  quiet switch to FSR 3.1." "If the FSR 4 library (DLL) is missing, fall back to FSR 3.1 quietly with an info line in the
  log." "The game is never left without AA."
- **Considered:** (A) make the normal renderer's FSR 4 wrapper usable by both engines: edits the normal renderer
  (principle 8). (B) a new-engine copy of the wrapper's FSR 4 path, same library, model files and inputs, frames on the
  new engine's own timeline. (C) build the normal renderer's wrapper a second time under other names: fragile. For the
  inputs: copy the scene colour and depth into images the AA owns each frame, or keep one view per game image (an earlier
  cache like that lost the device, and the FSR 4 library keeps at most 8 views and never forgets one).
- **Choice:** B, with the copies. The fresh-launch default is decided by the engine (it sees the GPU): RDNA 4 = an AMD
  chip with a PCI device ID 0x75xx (from public ID lists; not proven on a card). A failure of a chosen FSR 4 is reported
  (error line, on-screen message, counter) and FSR 3.1 then runs, so the game keeps its anti-aliasing: the box's reading
  of "no quiet switch" together with "never left without AA"; the owner can overrule in issue 55. Missing FSR 4 files: an
  info line and FSR 3.1. Switch `TLG_VKB_FSR4` (`=0` the old way).
- **Who:** owner set the rules; box chose the way (rule 14).
- **Links:** issue 55 (https://github.com/tabnul/tlg-vkb-work/issues/55); pull request 51 (the owner's rules); pull request
  for branch `fix/vkb-fsr4`.

## 2026-11-19 - The game's "CreateBlob: Error" line for never-shipped shader variants becomes one info line per name (not yet merged)

- **Problem:** with both renderers, on this box and on Windows, every run logs about 70-90 lines
  `CreateBlob: Error. Cannot open file data_trico/ps4_shader_cache/material/<name>.sb.` The text is the game's
  own message, not the port's. The game builds these names itself from a material's variant keys and, when the
  file is missing, uses its own fallback shader. None of the names is in any of the game's archives, under any
  case or folder, while the same lookup finds the other material shaders. So the line reports a state of the
  shipped game, not a fault of the port, but it reads as an error in every log check.
- **Considered:** (1) a short table in the port's shared file code: whole game lines of a listed form are written at
  info level once per name, but only after the port has checked the name against the index of every archive the
  game can open; (2) the same without the check, with a cap on the number of names as a guard (rejected after the
  second read: a partly broken install would also read as "not shipped"); (3) patch the game's call to its print
  function in the blob loader (a game hook in shared code, tied to one game build, changes game code for a log
  line); (4) leave the text and only teach the log checks that the line is known (the owner still sees "Error" in
  every log, on every machine).
- **Choice:** (1). The level follows a fact checked on the player's own install: a name in none of the archives
  was never shipped (info line, once per name); a name that IS in an archive means the game failed to load a file
  that exists, and the game's error line stays; an archive index that cannot be read keeps the error line too.
  It works the same on Linux and Windows and for a packed game, touches no game code and neither renderer, and
  keeps every other game line as it is. Cost: the archive indexes are read once, at the first such line (0.02 s and
  about 2 MB on this box). Switch `TLG_GAME_NOTES=0` gives the old output. Limit: an archive FILE that is missing
  from the game folder altogether is not seen by the check (the port has no list of what a full install holds); all
  material shaders sit in the boot archive and its update, without which the game does not start (assumed).
- **Who:** box (rule 14c), on the owner's job of 2026-11-19 ("log once per name at info level" if the files do
  not exist and nothing is lost).
- **Links:** issue 49, issue 47 (owner comment), pull request: see issue 49.

## 2026-11-19 - The new engine applies the player's picture settings the way the normal renderer does (not yet merged)

- **Problem:** the new engine's picture is about half as sharp as the normal renderer's in 195 of 197 scenes (issue 42).
  The logs of the pot scene show why: the normal renderer reads the player's settings and, on a machine without DLSS,
  runs FSR 3.1 as its anti-aliasing, sharpens at the Sharpening setting (175%), draws the scene's materials with a
  texture detail bias of -1 while that anti-aliasing runs, and filters textures at 16x anisotropy. The new engine did
  none of the four: without DLSS it switched its anti-aliasing off (one warning line), it sharpened at its own value of
  50% only when its anti-aliasing ran, and it used the game's own texture bias and filtering.
- **Considered:** (A) the new engine applies the same four settings with the normal renderer's rules, in its own code:
  the DLSS fallback to FSR 3.1, the sharpening scale (0 to 100% the anti-aliasing's own sharpening, every 100% above one
  more pass of the same sharpening shader), the texture bias for draws with five or more colour outputs while the
  anti-aliasing runs, and the anisotropy rule. One switch for all four. (B) only the fallback, and leave sharpening,
  bias and filtering for later: the picture would still differ wherever the normal renderer's settings act, so it
  fixes the case, not the cause. (C) a sharpening filter on the final picture tuned until the number matches: hides the
  cause (rule 24), and texture detail that the bias brings cannot be made back by sharpening.
- **Choice:** A. Cause and not case (10): the settings are applied, not one scene tuned. Every machine (6): the fallback
  removes the "AA off" state on any machine without DLSS; the sharpening is a plain compute shader; bias and anisotropy
  are plain sampler fields that every Vulkan driver has (anisotropy only where the driver reports it, as before). Right
  picture first (11): the cost is one to three extra passes of a small shader per frame and the anti-aliasing step
  itself on machines without DLSS. Switch `TLG_VKB_PICTURE_SETTINGS` (default on; `=0` the old way; letters `f`, `s`,
  `b`, `a` choose parts, for measurement only).
  After the second read: the anisotropy rule lives in one function that all three ways a sampler is made ask (the
  game's sampler objects, the samplers written in the shader, the sampler words), instead of three copies that had
  drifted apart; and the texture bias is left off depth comparisons, as the normal renderer does.
  Not covered: with the anti-aliasing switched off by the player, the normal renderer sharpens the final picture in its
  presenter instead (CAS); the new engine's presenter does not yet. Separate job.
- **Who:** box (rule 14).
- **Links:** issue 42 (https://github.com/tabnul/tlg-vkb-work/issues/42); pull request for branch `fix/vkb-picture-settings`.

## 2026-11-19 - AABB read check: read the game's group table under the game's own lock (not yet merged)

- **Problem:** In AL_a01_firstRoom_01 the new engine sometimes wrote the error `K1 AABB readback: N of N records name
  a group not in the table`, and the author's crash guard (K2) then skipped that read (issue 36, log cause E). The
  records were not from another pass: they were this frame's, made from the same table. The game adds groups to that
  table from other threads while the reader is on its way. Adding one raises the count before it writes the new
  entry, and can move the whole table to new memory; the game does this under the table's own small lock, and the
  game's reader takes the same lock before it uses the records. Our check read the table without that lock, so it
  could see the old, moved-away table or an entry not written yet, and then wrongly called good records foreign.
- **Considered:** (A) our check takes the game's own lock (the same lock word, the same way) for the moment it reads
  the table, and so do our other two reads of that table (both only for log lines). (B) move the check to a new hook
  point inside the game's locked part and skip the read there by changing the loop count: needs a new hook with new
  expected bytes and a register change, more risk for the same result. (C) read the table twice and retry until it
  stops changing: cannot see an entry that is counted but not yet written, and can read freed memory.
- **Choice:** A. The cause and not the case (10): every read of that table in our code goes through the lock, not just
  this scene. Every machine (6): plain CPU code, no driver feature. Right picture first (11): the cost is one
  compare-and-swap per read (rate: see "Cost" below); it can only wait while another thread is adding a group.
  The author's K2 guard stays as it is and still catches a real mismatch. Switch `TLG_AABB_TABLE_LOCK` (default on;
  `=0` the old unlocked read).
- **Checked in the game code (second read, remark 2):** the lock word is a 32 bit int (the game's lock function
  compares and swaps a dword; its release stores 0). The reader has one path to our hook point: it takes the lock for
  its "any active group" loop, releases it, maps the count and the records (our hook point), and takes the lock again
  before the record loop. BeginRecording takes the lock inside itself, after our hook at its entry. So neither thread
  holds the lock at our hook points. The wait still has a time limit (100 ms, a push holds the lock for microseconds):
  past it the check reads without the lock as before, counted and logged as an error (it should never happen).
- **Cost (remark 3):** the check runs fewer than 1800 times in every take of the full run (438 takes of under a minute each; no take reached
  the 1800 reads after which the summary line prints; AL_a01_firstRoom_01 has 3 reads per take). One wait for the lock
  was seen in 8 takes. Waits and time-outs are in every take's engine report (`native.json`, `aabb_table_lock`).
  The time-out path was never reached in any run, so it is not tested.
- **Who:** box (rule 14).
- **Links:** issue 36 (https://github.com/tabnul/tlg-vkb-work/issues/36); pull request for branch
  `fix/vkb-aabb-table-lock`.

## 2026-11-19 - Sync packets in the game's command buffers: the new engine does not walk them (not yet merged)

- **Problem:** 58 scenes wrote 1 to 5 error lines "sync_untyped_eop / _wait / _dma": a sync packet without its typed
  record. With the new engine the game's command buffers are never run. Every hook that makes a fence, a wait or a
  copy sends it straight to the new engine and makes no record, on purpose. Yet an older tag path (the texture Map)
  still writes into those buffers, and every such write walked the game's packets before it (the old command
  processor's stream sweep). The walk found sync packets that nothing reads and reported each one as missing its record.
  A diagnostic take showed that every flagged fence and wait had already reached the new engine through its hook. The
  rest were a barrier on a label inside the buffer (only the GPU's own work order, which the new engine keeps) and a
  cache warm-up copy into a register, which the normal renderer skips too.
- **Considered:** (A) with the new engine the stream sweep does not run. It is the only way into the converter in
  that mode. This removes reading the game's command list (principle 1) and needs no new hook. (B) let every
  producer leave its record with the new engine too, so the packet "has its record". The walk would then rewrite
  game memory into tags that nothing reads. The barrier and the warm-up copy have no producer, so the count could
  never reach 0. (C) stop the texture Map path writing old tags. That removes this trigger only, not the rule, and
  changes how a texture Map behaves (a separate question).
- **Choice:** A. Cause and not case (10): no sync packet of any kind is walked or converted with the new engine. Every
  machine (6): only our own CPU code changes, no driver feature. Fewest special paths: it takes a path out. Switch
  `TLG_VKB_STREAM_SWEEP=1` brings the old walk back. The question why the texture Map path still writes old tags with
  the new engine is a separate job.
- **Who:** box (rule 14).
- **Links:** issue 36 (https://github.com/tabnul/tlg-vkb-work/issues/36); pull request for branch `fix/vkb-sync-untyped`.

---

## 2026-11-19 - Moving-things mask for all 69 masked scenes; no scene is log only for noise (not yet merged)

- **Problem:** the solid-blob mask still hid still surfaces (pots, mirror frame, buildings) in several scenes, where a real
  new-engine fault could sit (issue 27, issue 34, rule 20).
- **Considered:** (A) the solid-blob mask (open the difference before growing); (B) count only differences above 48, then
  open, then grow: the shine of a still surface differs by little, the boy, grass and loose objects by a lot; (C) detect
  surfaces by kind (per-scene special cases, rejected before).
- **Choice:** B, one rule for every scene, and the scenes judged with the scene's own noise limit (issue 9) because the
  normal renderer still differs from itself a little on still surfaces. Mask share 0.9% on average. The 15 scenes the rule
  A had sent to log only are back in the picture check. Owner, 2026-11-19: "take the still surfaces out of the mask
  (pots, pillars, walls, buildings, the mirror frame). Grass, the boy and things that really move stay masked ... judge
  it with its own noise limit (issue 9) ... instead of the fixed limit."
- **Who:** owner decided the aim; box chose the threshold rule.
- **Links:** issue 27, issue 9, issue 34; pull request 33.

---

## 2026-11-19 - The new engine's developer dumps become one test tool, off by default (not yet merged)

- **Problem:** Under the new engine every take writes about 120 to 210 lines at warning or error level that only print
  state for the developer: a one-time note on where the three scene slot groups' memory sits (sent through the
  rare-event channel, so it shows as 3 error lines), the bindings of each new compute shader, values of staging reads,
  copies into staging textures, BeginRecording's device fields and the first AABB read check (issue 36, log cause C).
  None of them reports a skipped, failed or missing step, so they hide the lines that do. The owner asked for a clean log.
- **Considered:** (1) one switch for all of them (`TLG_VKB_DEV_LOG=1` writes them again, at info level, rule 24), off by
  default, and one counter per kind in the stats that counts what each dump is about whether the switch is on or off;
  (2) lower each line to the debug level: no switch (principle 7), the work to build each line still runs (13), and
  whether it shows depends on the log filter, not on one named switch; (3) delete the dumps: the developer loses tools
  that are still used for open questions, and the stats would lose what they show.
- **Choice:** 1. Principle 13 (a test tool costs nothing when off: with the switch off the dumps do not build their
  text, read their extra memory or keep their own maps), principle 5 kept (none of the lines counts a skip; what
  they are about is counted in `vkb.dev_log`, including whether the AABB guard is on), principle 7 (the switch is the
  old behaviour). With the new engine off the dumps are written as before. The `staging read` lines with `-nan` and
  `inf` were checked first: in the full run of 2026-11-18 (20,162 such lines) they are only seen on 32x32 to 256x256
  textures, always on a copy that had finished, never on the 16x16 float textures. The dump reads every 4-byte texel as
  one float; a run with the switch on, which now also prints the format, shows these textures are two 16-bit signed
  numbers per texel (`R16G16Snorm`), and such a texel read as one float gives exactly these huge, `-nan` and `inf`
  values. So they are a dump that misreads the format, not wrong data, and were not split off.
- **Who:** box (rule 14).
- **Links:** issue 36 (https://github.com/tabnul/tlg-vkb-work/issues/36), list
  https://github.com/tabnul/tlg-vkb-work/issues/36#issuecomment-6997220598 ; pull request for branch `fix/vkb-dev-log`.

---

## 2026-11-19 - Cube images are bound as the 2D arrays the recompiled shaders declare

- **Problem:** The pots in s10_03_Escape_Pitfall_001 lost their teal reflection (issue 34). The recompiled shaders
  declare every cube image as a 2D array and pick the layer themselves (probe slot x 6 + face). The new engine bound
  a Vulkan cube view instead, and its bind code cut that view to the first cube (6 layers). The deferred lighting pass
  reads a cube array of 64 probe slots, so every probe read slot 0's cube. Found by comparing RenderDoc captures of
  both renderers (the normal renderer binds all 384 layers as a 2D array).
- **Considered:** (A) ask for a 2D array view of the whole view range for every cube binding, one shared mapping for
  draws and dispatches (the two identical copies become one); nothing recompiled. (B) let the shader translator declare
  real cube and cube-array images: every shader with a cube is compiled again, and the shaders compute the layer
  themselves, so the cube type does not fit them. (C) only for cube arrays (more than 6 layers): a special case, and
  single cubes would keep a view type that does not match the declaration (undefined in Vulkan, works on AMD by luck).
- **Choice:** A. It fixes the cause for every cube in every shader (principle 10), the view type now matches the
  declaration on every driver (6), no measurable cost (11), fewer paths (one mapping instead of two). Switch
  `TLG_VKB_CUBE_AS_ARRAY` (default on; `=0` the old way). Counter `cube_bindings_as_array` in the objects report.
  Left for a separate change: the view's level count (the probe array's own descriptor says 7 levels, the Direct3D
  description 8).
- **Who:** box (rule 14).
- **Links:** issue 34 (https://github.com/tabnul/tlg-vkb-work/issues/34); pull request for branch
  `fix/vkb-cube-as-array`.

---

## 2026-11-18 - First vertex of a direct draw: Vulkan adds it only when the shader does not (not yet merged)

- **Problem:** s02_05_Tritrigger_Get_001 gave "photo setup failed" in both takes (scoreboard issue 36). The scene was
  not stalled: after the load the new engine drew about one frame every 0.65 s, with the GPU fully busy. Two cave
  wall draws turned into huge spikes (250 million pixels shaded from 33,000 triangles in one pass). Cause: the game
  hands a draw's first vertex to the vertex shader, and the shader adds it itself. These cave shaders read their
  vertices from buffers and add it in their own code, and the new engine also gave it to Vulkan, so every vertex
  number was moved twice. The normal renderer gives it to Vulkan only when the game's fetch shader uses it (that part
  of the shader becomes Vulkan's vertex input).
- **Considered:** (A) at the draw: Vulkan gets the first vertex only when the shader names no register for it, or
  when its fetch shader reads that register; otherwise 0. This is the normal renderer's rule, one place, nothing
  recompiled. (B) handle direct draws like indirect ones already are: always give it to Vulkan and let the
  recompiled shader take it back off where its own code adds it. Also one rule, but every such shader is compiled
  again, with new pipeline keys. Higher risk for the same result. (C) a fix for the cave shaders only: against
  principle 10.
- **Choice:** A. Cause and not case (10): every direct draw, indexed or not. Same on every machine (6): only the
  numbers handed to Vulkan change, so no driver feature is involved. Right picture first (11): no measurable cost
  (one read and two compares per draw). Fewest special paths: no new compile path. Switch `TLG_VKB_BASE_VERTEX`
  (default on; `=0` the old way). Counter `first_vertex_by_vulkan_by_shader` in the graphics report.
- **Who:** box (rule 14).
- **Links:** issue 36 (https://github.com/tabnul/tlg-vkb-work/issues/36); pull request for branch `fix/vkb-base-vertex`.

---

## 2026-11-18 - Buffer lookup memo: an entry answers only for the memory it was resolved for (not yet merged)

- **Problem:** Some takes of the same scene draw the boy wrong (issue 12). In every bad take the boy's skinning
  output equals the output of two frames before. Code read: the per-thread memo of `BufferOfObjectFields` reuses an
  entry when object, id and buffer epoch match, but not the memory pointer it resolved. Several paths move a
  buffer's memory without a new object (the skin ring rename every skin pass, the recording ring rename, the
  engine's buffer move), so a memo hit can hand out the window of the old memory. On this box the frame ring stays
  in place (R3a refused), so ring memory reaches this memo; where R3a works it does not.
- **Considered:** (A) the memo entry also holds memory pointer and byte width; a hit needs them equal, else it
  resolves again: one rule for every reader and every mover. (B) no memo: every bind resolves under the lock; the
  memo was added for speed, unmeasured loss. (C) every path that moves the memory bumps the epoch: covers only the
  movers someone remembers, and each bump flushes every entry. (D) make R3a work on AMD (frame ring in device
  memory below 40 bits): covers only ring memory, high cost and risk, per OS.
- **Choice:** A. It fixes the key once for every reader and every mover (principle 10), is CPU side and the same on
  every machine (6), costs two compares per bind and a few extra lookups per frame (11), and adds no special path.
  B is the fallback if A still leaves bad takes (the second structural way, rule 14e). Switch
  `TLG_VKB_MEMO_BY_MEMORY` (default on, `=0` the old key); counter `memo_memory_moved` in the bind-by-object report.
  Proof (script job 20261118-1600a, s10 Escape_Pitfall, 8 takes per arm): switch on 0 bad of 8, switch off (old key) 6 bad of 8; garden with the switch on 0 bad of 4; no device lost and the same error-line count in every arm. `memo_memory_moved` and skipped draws were not recorded (no end.stats.json), so they are not part of the proof.
- **Who:** box (rule 14).
- **Links:** issue 12 (https://github.com/tabnul/tlg-vkb-work/issues/12), design note
  https://github.com/tabnul/tlg-vkb-work/issues/12#issuecomment-6978580374 ; pull request for branch
  `fix/vkb-object-memo-key`.

---

## 2026-11-18 - Noise mask from three photos (not yet merged)

- **Problem:** for 38 scenes the 2-photo mask did not hold on a fresh retake (issue 27). The owner's words
  (12:46): "The 14 'above' scenes ... yes, do it" (mask from 3 photos) and "The 19 'cannot judge' scenes proposed as
  log only: not log only yet. Try the wider mask (3 photos) there too first. Only what still fails after that is
  proposed for log only again." At 12:49: `AT_BarrelBox_001` and `ST_040_FallingFromSentinelTower` log only;
  `AT_Outdoor_001` wider mask. The camera-shift check (comment 13:38) found no shift, so no recipe change.
- **Considered:** (A) mask = every pixel where any two of the three photos differ by more than 16, same grow rule.
  (B) per cause detectors (rejected before, see the entry below). (C) a looser limit for these scenes (changes the
  yardstick: the owner's call, not done).
- **Choice:** A, one rule, no special case. Measured on a fourth photo (retake 2) that was not used for the mask. 13 of
  36 scenes come within the limit; 23 still fail or need more than 25% left out and go to the log-only list as
  `proposed`. `sweep.py` now builds the mask from every good reference take, so the reference run of these scenes
  needs 3 takes.
- **Who:** owner (the 3-photo mask, the two log-only scenes), box (the measurement and the split).
- **Links:** issue 27; pull request of branch `fix/vkb-mask3`.

---

## 2026-11-18 - Ignore list: what the owner confirmed (not yet merged)

- **Problem:** the log-only list and the noise mask held only `proposed` entries (rule 20). The owner decided on
  issue 27 (comment 6974437546, 2026-11-18). The owner's words: "All part-of-picture confirmations below hold on one
  condition: the fresh two-photo run (noise-126 retake) must show the same moving parts. Where it does not, come back
  with that scene."
- **Considered:** (A) confirm every scene on the owner's list. (B) confirm the 35 log-only scenes and only those mask
  scenes where the retake agrees; list the rest for the owner. (C) move the scenes that disagree to log-only by
  ourselves.
- **Choice:** B, because it is the owner's condition and rule 20 (the loop proposes, never confirms beyond what the
  owner said). 35 log-only scenes confirmed (29 from the old list plus the 6 "need a look"). 56 of the owner's 94
  mask scenes confirmed: those whose retake picture, outside the mask, stays within the limit. The other 38 stay
  `proposed` (full picture check) and are listed for the owner. `s02_01_Care_Step1_001` stays `proposed`
  (full picture check), as the owner said.
- **Who:** owner (the confirmation), box (applying the retake condition).
- **Links:** issue 27; pull request of branch `fix/vkb-ignore-confirmed`.

---

## 2026-11-18 - Noisy scenes: one noise mask from the normal renderer's own two takes (not yet merged)

- **Problem:** 126 scenes are not judgeable by picture because two takes of the NORMAL renderer differ by more than
  the limits (issue 9). The owner looked at examples: the pictures are not broken, the photo is not the same moment
  (boy elsewhere, drifting mist, swaying grass, flames, particles). Forcing all of that still was ruled out.
- **Considered:** (A) a mask per scene made from the normal renderer's two takes: a pixel counts only where the
  normal renderer agrees with itself; one rule for every cause. (B) one detector per cause (find the boy, find
  mist, ...): needs knowledge of the picture, many special paths. (C) a wider tolerance per cause: hides a small
  broken area in every scene. (D) force the moment still (freeze the boy, particles): the owner said no.
- **Choice:** A. The ignored share is small where only the boy or a flame moves and is printed per scene; when more
  than 25% would have to be left out the scene goes to the log-only list instead. Scenes alive with action go there
  too. Causes that are not explainable (whole picture or tone, parts missing) are not masked: listed for a look.
  Every entry is `proposed`; the tools honour only `confirmed` (owner, 2026-11-18).
- **Who:** the form: box (rule 14); the confirmation: owner.
- **Links:** issues "Answer key noise: cause per scene" and "Ignore list: entries to confirm"; pull request for
  branch `fix/vkb-noise-mask` (stacked on `fix/vkb-log-only`).

---

## 2026-11-18 - Log only group: left out of the picture comparison only after the owner confirms (not yet merged)

- **Problem:** Cutscenes (and scenes without a usable normal-renderer picture) cannot be judged with one photo, but
  must not drop out of checking.
- **Considered:** (A) a list file with a status per entry, the sweep honours only `confirmed`; every scene gets a
  log verdict, log only scenes get nothing else. (B) a limit on the picture difference that drops noisy scenes
  by itself. (C) mark scenes in the scene list.
- **Choice:** A. B would hide a renderer fault behind noise (rule 20); C mixes two lists. The log verdict compares
  the kinds of error and warning lines with the normal renderer's log of the same scene.
- **Who:** the owner (2026-11-18: "Nothing is ignored in the picture comparison until the owner has confirmed it");
  the form of the list and the verdict: box.
- **Links:** pull request for branch `fix/vkb-log-only`, issue "Ignore list: entries to confirm".

---

## 2026-11-18 - The author's code is taken as it comes (not yet merged)

- **Problem:** The second read of the author's sync (pull request 18) said FAIL on principles 1-3, 5-7, 9, 10 and 13
  for author commits. Rule 13 does not let the box change author commits, so the box asked the owner what to do
  with the findings.
- **Considered:** (A) merge the sync as it is; keep the findings as a list; fix only what a sweep shows as a wrong
  picture or a crash. (B) hold the sync until the author's side changes. (C) the box adds switches, counters and
  gates to author changes in separate `fix/` pull requests.
- **Choice:** A. The author's commits are not judged as box changes (rule 19). The findings go on one list,
  `docs/vkb-author-findings.md`, kept up to date at every sync. No switches are added to author changes unless a
  sweep points at one. The three new side paths (probe cube copy, texture move, AsyncMap placement) go on the
  principles page list, by a pull request from the box that the owner merges. Before the official release the owner
  goes through the list and applies the principles to the author's code where he finds it necessary; that work is
  the owner's own issue 21 and not part of the loop.
- **Who:** the owner, in issue 17 (2026-11-18): "A and add a note that when we are done for official release, we
  will apply our rules to the code if we deem neccesary."
- **Links:** issue #17, pull request #18, issue #21 (before release).

---


## 2026-11-18 - Default of the GPU memory guard is 16 GB (not yet merged)

- **Problem:** the guard (pull request 15) had a default limit of 8 GB. The new engine's GPU pool
  peaks at about 10.6 GB, so every run of it would need the limit raised by hand.
- **Considered:** keep 8 and raise per run; raise the default.
- **Choice:** default 16 GB (`SHOT_GTT_LIMIT_GB`, `0` = off stays). The hang was at 31.5 GB, so 16 GB
  still stops a runaway well before it.
- **Who:** owner, pull request 15, 2026-11-18: "fine to merge, with the default limit at 16 GB instead
  of 8 (the GPU pool of pull request 13 peaks at about 10.6 GB; the hang was at 31.5 GB)."
- **Links:** pull request 15 (merged before this change reached it); the pull request of this change.

---

## 2026-11-17 - The import refusal is an AMD Linux driver rule, not the container (not yet merged)

- **In short:** the import that failed in #8 is refused by a rule in the AMD Linux driver itself, not
  by the container. Shown by the driver's source and by a test in this container; a run on a plain
  install (no container) is still to come, see the owner test in the comment below. This entry does
  not settle the open owner question of the principles page about two paths (principle 6).
- **Problem:** Owner (asked 2026-11-18 owner time): "I don't want to be fixing things that are related
  to LXC." The test box is an LXC container with the GPU passed in. Was the refusal to import the
  game's memory (#8), which the GPU pool works around, caused by the container?
- **Considered:** (1) a container effect: then the GPU pool would only be needed on this box; (2) a
  driver rule: then any AMD Linux machine with this driver needs it.
- **Finding:** (2), by reading the source and one run on this box. The kernel driver (Linux 7.0,
  `amdgpu_ttm.c`, function `amdgpu_ttm_tt_get_user_pages`) refuses memory that has a file behind it
  when the caller asks for "anonymous memory only", and libdrm (2.4.125, `amdgpu_create_bo_from_user_mem`,
  used by RADV in Mesa 25.2.8) always asks for that. On this path there is no check of namespaces,
  cgroups, limits or permissions. A small test in this container that calls the driver directly gets
  the same shared memory accepted as soon as "anonymous only" is not asked for. The same check is in
  the kernel sources of 6.8 and 6.14 (read, not run). Not run on a plain install; the test for that is
  in the comment linked below. Sources, the test and its output are in that comment.
- **Choice:** the GPU pool stays the way for AMD Linux. Nothing in the engine is changed for the
  container. Container differences that can only change timing (9 of 12 CPUs, possibly a shared GPU,
  software present on Xvfb) and the hidden kernel log are listed in the comment. Whether #12 (takes
  differ) also happens without the container is not proven; one test on the host without the
  container is given there for the owner.
- **Switch:** none, docs only.
- **Who:** the box (loop rule 14), **not yet merged**. The owner can overrule in the issue or at the merge.
- **Links:** https://github.com/tabnul/tlg-vkb-work/issues/8#issuecomment-6964582869 , pull request
  #13, issue #12.


## 2026-11-17 - The guest pool becomes GPU memory (not yet merged)

- **Problem:** On AMD Linux the new engine drew a black picture. The GPU reads the game's memory in
  place, and the driver (RADV) refuses to import it: the game's memory is one shared memfd, and only
  private, writable, anonymous memory can be imported (measured on this box only).
- **Considered:** (A) make the GPU-read memory private anonymous memory: rejected, high cost and
  risk, rewrites the Linux memory layer, AMD Linux only. (B) one general copy layer with change
  tracking: rejected as the main way, it needs a hook for every write and many writes have none (the
  copy job ran 15 runs without gain). (C) let the GPU own the guest pool: allocate it as Vulkan
  memory at start, export it as a dma-buf, and give it to the runtime in place of the memfd.
- **Choice:** C, switch `TLG_VKB_GPU_POOL`. Reasons given in the note: principle 10 (the cause for
  every route at once), principle 6 (one memory design on every machine), principle 11 (no copies),
  fewest special paths.
- **Who:** the box (rule 14), **not yet merged**. The owner can overrule in the issue or at the merge.
  This entry only records the box's choice; it is not a judgment of it.
- **Links:** design note https://github.com/tabnul/tlg-vkb-work/issues/8#issuecomment-6963854420 ,
  pull request https://github.com/tabnul/tlg-vkb-work/pull/13 , follow-up issue #12 (boy and overall
  tone vary from take to take).

## 2026-11-17 - This log itself

- **Problem:** design choices were spread over issues and comments.
- **Considered:** nothing recorded in the sources beyond the rule.
- **Choice:** one page, `docs/vkb-decisions.md`, newest on top. A choice made by the box under rule 14
  gets its entry in the same pull request as the fix. An owner decision gets its entry in the next
  pull request that touches the matter, with the owner's words. The page is read before a design
  note and before the recurring check (rule 15).
- **Who:** owner (rule 15; the journal says "owner approved", 23:10-23:12). The owner's own words are
  not in the sources I could read.
- **Links:** rules.md rule 15; journal line "rules.md rule 15 added".

## 2026-11-17 - Step back and decide yourself; recurring causes (rules 14 and 14f)

- **Problem:** the black-picture job stopped after 15 runs without gain. Owner words, as recorded in
  rule 14: "I do not want to be a blocker with every decision"; "something structural is needed,
  instead it keeps trying".
- **Considered:** more tuning runs; or a design note first.
- **Choice:** two runs in a row without improvement, or a third fix of the same kind, stops the tuning.
  The box then writes a design note (full list of routes, at least two structural ways, its choice)
  and builds the choice itself. It asks the owner only when a principle would change, the normal
  renderer would be touched, something outside the box is needed, or every structural way failed
  (a job with three failed structural ways stops with `MAJOR:`). Rule 14f: before a fix, look for an
  earlier fix of the same kind; a second one is treated as recurring and gets a structural
  replacement, not a new patch.
- **Who:** owner (journal: "owner: structural solution wanted, no more trying"; 14f "owner approved").
- **Links:** issue #8 (the stopped job, and the design note that followed), rules.md rule 14 and 14f.

## 2026-11-17 - The black picture is fixed before any comparison; the answer key may run meanwhile

- **Problem:** a black picture of the new engine cannot be compared with anything; a full sweep
  would mark all 219 scenes not clean for one reason and cost about 8 hours.
- **Considered (issue #10, options 1-3):** fix #8 first; run the answer key only; run the full round anyway.
- **Choice:** the owner answered, on #10: "1. yes fix 2. first 1. keep working in sequence per scene
  3. no. fix first". Later: "2. in hindsight, just continue with this while the black fix is being
  worked on", so the answer key (normal renderer only) runs meanwhile. On #8 the owner wrote: "fix
  the black screen. a black screen cannot be used for comparison." On pull request #7: "fix the black
  image. Fix it in a structured way so it does not occur anymore." The picture script therefore
  refuses a blank picture and the take fails (it is never compared).
- **Who:** owner.
- **Links:** https://github.com/tabnul/tlg-vkb-work/issues/10#issuecomment-6961894498 ,
  https://github.com/tabnul/tlg-vkb-work/issues/10#issuecomment-6962626676 ,
  https://github.com/tabnul/tlg-vkb-work/issues/8#issuecomment-6961836438 ,
  https://github.com/tabnul/tlg-vkb-work/pull/7#issuecomment-6961769854 , pull request #7.

## 2026-11-17 - GPU memory guard for test runs

- **Problem:** a test run of the black-picture fix used up all GPU memory (GTT 31.5 of 33.4 GB); the
  game process hung in the AMD kernel driver, could not be killed, and the box had to be rebooted by
  the owner.
- **Considered:** the sources show only the chosen way.
- **Choice:** every game start on the box runs under a watcher that kills the game when GTT memory
  passes a limit (default 8 GB, `SHOT_GTT_LIMIT_GB`, `0` = off), fails the take with a named reason,
  and writes a `MAJOR:` journal line. First as a job-local script, then in the photo tool. Test
  tooling only; no engine change.
- **Who:** owner approved the job (journal: "queued gpu-memory-guard (owner approved)"). The owner's
  own words are not in the sources I could read.
- **Links:** issue #11 (box blocked, closed), issue #8 comments of 22:09 and 22:37. The job's pull
  request is not open yet.

## 2026-11-17 - The author's fork is synced every round

- **Problem:** the new engine's author keeps working in a public fork; our branch must not drift away.
- **Considered:** the sources show only the chosen way.
- **Choice (rule 13, dated 2026-11-17 by the owner):** at the start of every round, fetch
  `thearturord/the-last-guardian-definitive`, branch `vulkan`, read only. New commits are merged for
  real (author's commits and name kept) into `sync/author-<date>`, with our README kept. Build, garden
  and medium set must show no crash, hang or fault and no more log errors or skipped draws; pictures
  and the scoreboard are compared as soon as that is possible. Anything worse: find the commit by
  halving, name it, put REGRESSION in the title, open a `needs owner` issue. Author commits are never
  left out or changed by the box. One pull request, credits @thearturord, and says which checks could
  not be run.
- **Who:** owner.
- **Links:** rules.md rule 13.

## 2026-11-17 - Pictures go to the `shots` branch

- **Problem:** pull requests must show their pictures without putting game pictures into code branches.
- **Choice (rule 12, owner 2026-11-17):** screenshots go only to the orphan branch `shots` of the
  work repo, folder `pr-<number>/`, JPEG quality 90 at full size; PNG originals stay on the box. The
  pull request shows a table per scene: normal renderer | new engine | difference; at most 12 scenes,
  the rest by number. Screenshots only, no other game data. Pull request #7's README was updated to
  say so.
- **Considered:** the sources show only the chosen way.
- **Who:** owner.
- **Links:** rules.md rule 12; pull request #7.

## 2026-11-17 - Picture limits: 1.0% / 0.5%, per-scene noise, and the boy

- **Problem:** how much picture difference counts as clean. The box measured the normal renderer
  against itself on the garden (4 takes, 6 pairs): the share of pixels differing by more than 16
  was 0.185% to 0.372%, by more than 32 about 0.07% and up.
- **Considered:** the box's proposal on issue #9 (limits plus a per-scene noise limit). The owner
  asked about the boy as well.
- **Choice:** on #9 the owner wrote: "the measure limits are fine then", "yes its fine" (the
  proposal, as recorded by the box: 1.0% of pixels above 16, 0.5% above 32, plus a per-scene noise
  limit from two normal-renderer takes per scene). First wish: "lets ignore the boy as he will almost
  always be different. if possible remove him from the statistics." Then, corrected: "well we should
  do something with the boy , but only if extremely different. not the sub 1% difference". So the boy
  is not removed: small differences on him do not count, an extreme one (missing, black, badly
  distorted) makes the scene not clean. The box plans to measure the boy's area separately with a
  looser limit; the exact method and numbers come in the thresholds pull request and are not decided yet.
- **Who:** owner (limits and rule), box (method, still open).
- **Links:** https://github.com/tabnul/tlg-vkb-work/issues/9#issuecomment-6961952698 ,
  https://github.com/tabnul/tlg-vkb-work/issues/9#issuecomment-6963538565 ,
  https://github.com/tabnul/tlg-vkb-work/issues/9#issuecomment-6963672792 .

## 2026-11-17 - `/dev/udmabuf` is not a general path

- **Problem:** the game memory (a memfd) could be turned into a dma-buf with `udmabuf` for import.
- **Choice:** ruled out by the box: the kernel has it, but `/dev/udmabuf` is not passed into this
  container, and normal desktops usually do not give it to users either. It is not a general path
  for players (principle 6). The statement about desktops is the box's assumption, not tested.
- **Who:** box.
- **Links:** https://github.com/tabnul/tlg-vkb-work/issues/8#issuecomment-6963042896 .

## 2026-11-17 - Compatibility over speed; a second, slower path is accepted

- **Problem:** the principles page asked whether two memory paths are acceptable (principle 6 against
  in-place reading) and whether slower is acceptable (principle 11).
- **Choice:** from "Decided by the owner (2026-11-17)" in `docs/vkb-principles.md`: where the two
  conflict, the path that works on every supported machine wins. A second, slower path for machines
  that lack a driver feature is accepted. A fix may cost speed; the measured cost is stated in the
  pull request. Principle 12 adds that the test box has no speed target. (Note: the "Open, for the
  owner to decide" list on that page still lists these two questions; the page does not say which
  open item the decision closes, so this entry takes the decided section as the answer.)
- **Who:** owner.
- **Links:** `docs/vkb-principles.md`; issue #8 ("the fix will follow principle 6 as you decided").

## 2026-11-17 - Principle 7 exception: no switch for a change that cannot be switched

- **Problem:** a Linux build fix has no behaviour that could be switched (reader's question on pull
  request #1; issue #2).
- **Choice:** the owner answered on #2: "yes, if it can't be switched , dont make a switch". Principle 7
  now says such a change (only makes the code build or start, changes no behaviour where it already
  worked) says so in place of the switch name. This log's own pull request uses this exception.
- **Who:** owner.
- **Links:** https://github.com/tabnul/tlg-vkb-work/issues/2 , pull request #6, pull request #1.

## 2026-11-17 - Working rules from the principles page

- **Decided by the owner** (section "Decided by the owner" of `docs/vkb-principles.md`): the loop may
  push work branches and open pull requests by itself and never merges; the pull requests live in a
  private repo, not the public fork; the role table (script, Haiku, Opus, Sonnet reader, owner for
  design questions and principle exceptions).
- **Who:** owner. **Links:** `docs/vkb-principles.md`.
