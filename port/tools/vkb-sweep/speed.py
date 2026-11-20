#!/usr/bin/env python3
"""Speed column for a before/after (or on/off) comparison. No game run, no AI.

Usage: speed.py BEFORE AFTER
  BEFORE, AFTER: a results-vkbV.jsonl, or the folder that holds it (or the folder of a sweep: the 'Frame stats' lines
  of the take logs are read when a result line has no fps field, so old results work too).
Prints per scene: average FPS of the takes before and after, the change in %, and the worst frame (largest of the
takes) before and after. A scene is flagged 'slower' only when FPS drops by 15% or more on BOTH takes (compared take
by take); fewer takes than two on a side: all its takes must drop. The box runs at about 15 FPS, so smaller changes
are noise. Speed is never a reason by itself to call a change unsafe: it is a note for the owner. Box numbers are
relative only; real speed numbers come from the owner's Nvidia PC."""
import json, os, re, sys

LIMIT = 15.0  # percent FPS drop that counts as slower
STATS = re.compile(r"Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms")


def last_stats(log):
    last = (None, None)
    try:
        for line in open(log, errors="replace"):
            m = STATS.search(line)
            if m:
                last = (float(m[1]), float(m[2]))
    except OSError:
        pass
    return last


def load(path):
    """{(stage, scene): [(fps, worst_ms) per take]}"""
    if os.path.isdir(path):
        base = path
        path = next((os.path.join(path, f) for f in sorted(os.listdir(path)) if f.startswith("results-vkb")), None)
        if not path:
            sys.exit(f"{base}: no results-vkb*.jsonl")
    out = {}
    for line in open(path):
        if not line.strip():
            continue
        r = json.loads(line)
        takes = []
        for t in r["takes"]:
            if "fps" in t:
                takes.append((t["fps"], t.get("worst_frame_ms")))
            elif t.get("picture"):  # old result: read the take log next to the picture
                takes.append(last_stats(re.sub(r"\.(blank\.)?png$", ".log", t["picture"])))
            else:
                takes.append((None, None))
        out[(r["stage"], r["scene"])] = takes
    return out


def avg(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = load(sys.argv[1]), load(sys.argv[2])
    print(f"speed: 'slower' = FPS down {LIMIT:.0f}% or more on both takes. The box runs at ~15 FPS, smaller changes are "
          "noise. Speed alone never makes a change unsafe; box numbers are relative only (real numbers: owner's Nvidia PC).")
    print(f"{'scene':58} {'fps before':>10} {'fps after':>9} {'change':>7} {'worst ms before':>15} {'after':>7}  note")
    slower = n = 0
    for key in sorted(set(a) & set(b)):
        ta, tb = a[key], b[key]
        fa, fb = avg([t[0] for t in ta]), avg([t[0] for t in tb])
        wa, wb = [t[1] for t in ta if t[1] is not None], [t[1] for t in tb if t[1] is not None]
        pairs = [(x[0], y[0]) for x, y in zip(ta, tb)]
        note = "no data"
        if fa is not None and fb is not None:
            n += 1
            ok = [p for p in pairs if p[0] and p[1] is not None]
            drops = [100.0 * (p[0] - p[1]) / p[0] >= LIMIT for p in ok]
            if ok and all(drops):
                note, slower = "slower", slower + 1
            else:
                note = ""
        ch = f"{100.0 * (fb - fa) / fa:+.0f}%" if fa and fb is not None else "-"
        f = lambda v, w=1: "-" if v is None else f"{v:.{w}f}"
        print(f"{key[1][:58]:58} {f(fa):>10} {f(fb):>9} {ch:>7} {f(max(wa) if wa else None):>15} "
              f"{f(max(wb) if wb else None):>7}  {note}")
    print(f"{n} scenes with data, {slower} flagged slower")


if __name__ == "__main__":
    main()
