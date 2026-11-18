# New engine (VKB): decision log

Newest on top. One entry per design decision (rule 15 of the loop). Each entry: date, the problem,
the ways considered, the choice and the reason, who chose, links. Plain words, no game data.
Where a source is unclear, the entry says so. Entries are from 2026-11-17 and 2026-11-18 (box times are UTC,
as shown by GitHub). Repo: `tabnul/tlg-vkb-work`.

An entry marked "not yet merged" counts only once the owner merges the pull request that carries it.

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
