# New engine (VKB): principles and way of working

DRAFT 2026-11-17, not yet approved by the owner.

Applies to every change to the new Vulkan engine (`TLG_VKB=1`, code in `port/gpu/native/`).
A change that breaks a principle is rejected, however good the picture looks.
Only the owner changes this page.

## Goal

Every one of the 219 scenes of our current set draws the same picture as the normal renderer, on every
machine we support. Speed comes after that. Crashes are tracked separately.

## Principles

**1. No PS4 emulation comes back in.**
The new engine takes what the game hands over at its own drawing calls. It does not read or
build PS4 command lists, does not guess what an address holds, and does not watch memory for
writes.

**2. Every piece of data has a known way in.**
Data reaches the graphics card through a hooked drawing call, or through a side path that is
on the list in this page. Data arriving any other way means a hook is missing: add the hook
and the list entry in the same change. Do not patch around it at the draw.

**3. One owner per resource.**
Each buffer and texture is either filled by the game or written by the graphics card, decided
when it is made. Game memory is never copied over something the graphics card writes.

**4. The game's order is kept.**
Work may be prepared in parallel, but it is handed to the graphics card in the order the game
asked for it.

**5. Nothing is dropped or swapped silently.**
Every skipped draw, missing feature, unknown format or failed step is counted, has a named
reason, and shows up in the check report. No quiet switch to another path.

**6. It works on every machine we support.**
Nvidia and AMD, Windows and Linux. What the driver can do is asked once at start. For a
missing feature there is either a second path or a clear message and a stop. Never an
assumption.

**7. Every change has a switch.**
The old behaviour stays reachable until the new one is proven, preferably switchable while
the game runs. Switches for proven changes are removed later, in a separate cleanup.
Exception: a change that cannot be switched, because it only makes the code build or start
and changes no behaviour where it already worked, gets no switch. The change says so in place
of the switch name.

**8. The normal renderer is not touched.**
`port/gpu/shadps4` stays as it is. With the new engine off, the game behaves exactly as before.

**9. Hooks are data.**
Hooks live only in the hook table, each with the bytes it expects to find. No address is
patched from code elsewhere.

**10. Fix the cause, not the case.**
No fix for one shader, one scene or one object. If one case is wrong, the general rule behind
it is wrong.

**11. Compatibility and a right picture first, then speed.**
A fix that makes the game slower is allowed; its measured cost is stated in the pull request.

**12. The speed target is a straight 120 fps.**
Not an average: a flat line at 120 on the owner's Nvidia PC. It does not apply on the test
box, whose graphics chip cannot reach it: the test box checks pictures only and carries no
speed target. Every hitch on the Nvidia PC is a finding that gets
investigated and written up as an issue, never waved away as noise. This does not overrule
principle 11: a compatible, right picture comes first, and the road back to a straight 120
comes after.

**13. Test tools cost nothing when off.**

## Side paths (principle 2)

Ways data comes in besides the plain drawing calls. "Confirmed" means someone checked in the
game code what the hook really covers.

| Path | Confirmed |
|---|---|
| File reads straight into graphics memory | yes |
| Dynamic texture fill | no |
| Memory copies (DMA) | no |
| Map / async map / staging worker | no |
| Texture and buffer updates | no |
| Skinning writes | no |
| Light writes | no |
| Scene slot writes | no |
| Constant setters | no |

## The check loop

One command builds, runs scenes with the fixed picture recipe, compares with the answer-key
pictures from the normal renderer, and prints a sorted list plus the dropped-draw counters.

| Size | When | Scenes |
|---|---|---|
| Small | while writing a fix | the scenes of the group being fixed |
| Medium | before the pull request is opened | those plus about 10 fixed scenes |
| Full | start of every round | all 219 |

- **The scoreboard only goes up.** A scene that was clean stays clean. A change that breaks a
  clean scene is fixed or taken out; nothing is stacked on top of it.
- **One cause per change.** No mixed changes.
- **A scene right on one graphics card and wrong on another is a bug**, not something to hide
  by loosening the threshold.
- One still picture misses one-frame flickers and anything about feel. Those are judged by
  the owner while playing.
- Final confirmation of a change is on the Nvidia Windows PC, not only on the test box.

## What every change must state

In the commit message, in plain words:

1. Which principle it serves, and that it breaks none.
2. What it removes or replaces (a change that only adds is suspect).
3. The proof: check size run, scenes clean before and after, counters before and after.
4. The switch name.
5. Tested / not tested on: Nvidia Windows, AMD Linux.
6. What is verified and what is only assumed.

## Who checks

- The build checks what a machine can check: the normal renderer's folder is unchanged, every
  hook has its expected bytes, new skip reasons have a counter.
- Every change gets a second read by a reader that has not seen the writer's reasoning, with
  this page as the only yardstick.
- An exception to a principle is never decided by the writer or the reader. It goes to the
  owner, and stops there until answered.

## How work flows: GitHub only

All communication is through pull requests and issues on GitHub. No other channel.

- **One cause = one pull request** against the `vkb` branch, with the list from "What every
  change must state", the before/after pictures next to the answer key, and the scoreboard
  and counter changes.
- **The second reader's verdict is a review comment** on that pull request.
- **The owner answers there:** merge = keep, close = take out, comment = change it. Comments
  are picked up on the next round and the same pull request is updated.
- **A question for the owner is an issue** labelled `needs owner` (principle exception, parked
  item, anything stuck after three tries). Work continues with the next item meanwhile.
- **Only the owner merges.** A scene counts as clean once its fix is merged; the nightly full
  run on the merged branch is the scoreboard.
- **Sweep first, fixes after.** No fix is proposed while a sweep is still running. A round is:
  1. Run the full sweep until it is done or entirely stuck.
  2. Collect every finding of that sweep and group the ones that look alike.
  3. Look for the shared cause behind each group.
  4. Propose one general fix per cause, as one pull request that lists every scene it
     should clean.
  5. After the owner's merges, sweep again.
- A fix that cleans only one scene of a group has not found the cause (principle 10).
- Done = a full sweep with nothing left on the list.

## Who does what

| Job | Who |
|---|---|
| Build, run scenes, compare pictures | script on the test box, no AI |
| Sort results, read logs, pick the next item | Haiku |
| Find the cause, write the fix | Opus |
| Second read against this page | Sonnet (never the model that wrote the change) |
| Design questions, principle exceptions | the owner |

Each role is an agent file with its model fixed in it. Fable takes no part in the loop.

## Decided by the owner (2026-11-17)

- **Compatibility over speed.** Where the two conflict, the path that works on every
  supported machine wins. A second, slower path for machines that lack a driver feature is
  accepted. A fix may cost speed; the measured cost is stated in the pull request.
- The loop may push work branches and open pull requests by itself. It never merges.
- The pull requests live in a private repo, not the public fork.
- The role table above.

## Open, for the owner to decide

- **Principle 6 against the current memory design.** Today the graphics card reads the game's
  memory in place. That depends on a driver feature that is likely missing or unreliable on
  AMD Linux (not yet tested). Principle 6 then demands a second, slower path that copies.
  Accept two paths, or support that machine only when the feature is there?
- **Principle 11.** Is "may be slower with the owner's OK" right, or should it be a hard
  "never slower"?
- **Thresholds.** How much picture difference counts as clean (the garden gave under 0.1%
  between the two renderers), and where the numbers are kept.
