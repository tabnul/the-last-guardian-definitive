#!/usr/bin/env python3
"""Log verdict and the "log only" list for the sweep. No AI inside. Linux test box.

Usage: verdict.py RESULTS.jsonl [--reference REF_OUT_FOLDER_OR_JSONL]
  Prints one log verdict line per scene of RESULTS (a results-vkbV.jsonl of sweep.py) and the counts per group.

The log verdict of a scene (every scene gets it, next to the picture verdict where there is one):
  ran      every take reached the photo moment (status ok) and the game did not end by itself
  frames   the game window was captured; a flat (blank) picture fails only where the normal renderer's is not flat
  crash    no crash, and no `Fault: exception` line more than the normal renderer's log of the same scene has
  lines    no `<Error>`/`<Critical>`/`<Warning>` line KIND that the normal renderer's log of the same scene does
           not have (a kind = the line with numbers and addresses blanked out; repeats of one kind count once)
  skips    the new engine's skip/fail counters are all 0 (only for vkb=1; the normal renderer reports none)
Verdict "pass", or "fail" with the reasons. Without a normal-renderer log of that scene the `lines` check cannot
compare and says "no reference log"; the other checks still run (never "not checked").

log-only.txt: scenes that are left out of the PICTURE comparison. One line each: STATUS STAGE SCENE REASON.
STATUS is `proposed` or `confirmed`; only the owner confirms (an issue), the tool honours only `confirmed`.
Scenes without a usable normal-renderer picture (fewer than 2 good takes) fall back to log only automatically."""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, "log-only.txt")
KIND = re.compile(r"<(Error|Critical|Warning)>")


def load_list(path=LIST):
    """{(stage, scene): (status, reason)}"""
    out = {}
    if os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.lstrip().startswith("#"):
                if len(line.split(None, 3)) < 3 or line.split()[0] not in ("proposed", "confirmed"):
                    sys.exit(f"{path}: bad line (STATUS must be proposed or confirmed, then STAGE SCENE): {line.strip()}")
                status, stage, scene, *reason = line.split(None, 3)
                out[(stage, scene)] = (status, reason[0].strip() if reason else "")
    return out


def kinds(path):
    """Set of line kinds (Error/Critical/Warning) in one game log."""
    found = set()
    if path and os.path.exists(path):
        for l in open(path, errors="replace"):
            if KIND.search(l):
                found.add(re.sub(r"0x[0-9a-fA-F]+|[0-9a-fA-F]{8,}|\d+", "N", l.strip())[:200])
    return found


def faults(path):
    return sum(1 for l in open(path, errors="replace") if "Fault: exception" in l) if path and os.path.exists(path) else 0


def logs_of(row):
    """Game logs of the takes of a results row (the picture path names the take; the log sits next to it)."""
    out = []
    for t in row["takes"]:
        p = t.get("picture")
        if p:
            log = re.sub(r"(\.blank)?\.png$", ".log", p)
            if os.path.exists(log):  # a log file that is gone is "no log", never an empty clean one
                out.append(log)
    return out


def log_verdict(row, ref_row=None):
    """{"verdict": "pass"|"fail", "reasons": [...], "new_kinds": [...]} for one results row of this renderer.
    ref_row: the results row of the normal renderer for the same scene (or None)."""
    why = []
    takes = row["takes"]
    if not takes:
        why.append("ran: no take")
    for t in takes:
        if not t["ok"] and not t["status"].startswith("picture failed: blank"):
            why.append(f"ran: take {t['take']}: {t['status']}")
        if t.get("crash"):
            why.append(f"crash: take {t['take']}: game ended by itself")
        if not t.get("picture"):
            why.append(f"frames: take {t['take']}: no picture of the game window")
        elif t.get("blank") and ref_row is not None and any(r.get("picture") and not r.get("blank") for r in ref_row["takes"]):
            why.append(f"frames: take {t['take']}: flat picture, the normal renderer's is not")  # a flat scene stays flat
    mine = logs_of(row)
    ref = logs_of(ref_row) if ref_row else []
    ref_kinds = set().union(*[kinds(p) for p in ref]) if ref else None  # None = no reference log: not compared
    ref_faults = max([faults(p) for p in ref] or [0])
    if any(faults(p) > ref_faults for p in mine):
        why.append(f"crash: Fault: exception lines {max(faults(p) for p in mine)} (normal renderer {ref_faults})")
    new = set()
    if ref_kinds is not None:  # without a reference log the lines are not compared; "reference_log" says so
        new = set().union(*[kinds(p) for p in mine]) - ref_kinds if mine else set()
        if new:
            why.append(f"lines: {len(new)} line kind(s) the normal renderer does not give")
    for t in takes:
        if row["vkb"] == 1 and t["ok"] and not t.get("counters"):
            why.append(f"skips: take {t['take']}: no counters kept (stats file missing), skips unknown")
        c = {k: v for k, v in (t.get("counters") or {}).items() if v}
        if row["vkb"] == 1 and c:
            why.append(f"skips: take {t['take']}: {c}")
    return {"verdict": "fail" if why else "pass", "reasons": why, "new_kinds": sorted(new)[:20],
            "reference_log": ref_kinds is not None}


def group(stage, scene, ref_row, listing=None):
    """('log only', reason) or ('picture', ''): confirmed list entry, or no usable reference picture."""
    status, reason = (listing if listing is not None else load_list()).get((stage, scene), (None, ""))
    if status == "confirmed":
        return "log only", reason
    if not ref_row:
        return "log only", "no normal-renderer result for this scene (is --reference given and right?)"
    if sum(1 for t in ref_row["takes"] if t["ok"]) < 2:
        return "log only", "no usable picture of the normal renderer (fewer than 2 good takes)"
    return "picture", ""


def rows(path):
    if os.path.isdir(path):
        path = os.path.join(path, "results-vkb0.jsonl")
    if not os.path.exists(path):
        return {}
    return {(r["stage"], r["scene"]): r for r in map(json.loads, open(path)) if r}


def main():
    args = sys.argv[1:]
    ref = {}
    if "--reference" in args:
        i = args.index("--reference")
        if i + 1 >= len(args):
            sys.exit(__doc__)
        ref = rows(args[i + 1])
        if not ref:
            sys.exit(f"--reference {args[i + 1]}: no results-vkb0.jsonl there or it is empty")
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    listing = load_list()
    counts = {}
    for key, row in rows(args[0]).items():
        v = log_verdict(row, ref.get(key) if row["vkb"] == 1 else None)
        g, why = group(*key, ref.get(key) or (row if row["vkb"] == 0 else None), listing)
        counts[(g, v["verdict"])] = counts.get((g, v["verdict"]), 0) + 1
        print(f"{key[0]} {key[1]}: group {g}, log {v['verdict']}" + ("" if v["reference_log"] or row["vkb"] == 0 else " (no reference log)") + "".join(f"\n    {r}" for r in v["reasons"]))
    for k, n in sorted(counts.items()):
        print(f"{n} scenes: group {k[0]}, log {k[1]}")


if __name__ == "__main__":
    main()
