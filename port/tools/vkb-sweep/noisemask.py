#!/usr/bin/env python3
"""The noise mask: which pixels of a scene the picture check leaves out. No AI inside; needs ImageMagick (convert).

Rule (one rule for every cause, no special case per scene): a pixel counts in the picture check only where the
normal renderer agrees with itself. Mask = every pixel where take 1 and take 2 of the NORMAL renderer differ by more
than 16 (largest channel), grown by 4 pixels so that soft edges and a little movement are covered. The boy standing
elsewhere, drifting mist, swaying grass, flickering flames, floating particles all land in the mask the same way.

Usage:
  noisemask.py make A.png B.png MASK.png   build the mask (white = left out) and print its share of the picture
  noisemask.py list                        print noise-ignore.txt with its status per entry

noise-ignore.txt: one entry per scene, `STATUS STAGE SCENE SHARE% CAUSES | reason`. STATUS is `proposed` or
`confirmed`. Only the owner confirms (issue "Ignore list: entries to confirm"); the tools honour only `confirmed`.
`confirmed_mask(stage, scene)` gives the entry only then. Nothing in the game or the engine changes."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, "noise-ignore.txt")
THRESHOLD = "6.3%"  # 16 of 255
GROW = "Disk:4"


def make(a, b, mask):
    """Write MASK (white = left out) from two pictures of the normal renderer; return the share left out (%)."""
    subprocess.run(["convert", a, b, "-compose", "difference", "-composite", "-separate", "-evaluate-sequence", "max",
                    "-threshold", THRESHOLD, "-morphology", "Dilate", GROW, mask], check=True)
    return share(mask)


def share(mask):
    out = subprocess.run(["convert", mask, "-format", "%[fx:mean*100]", "info:"], capture_output=True, text=True, check=True)
    return round(float(out.stdout), 2)


def load_list(path=LIST):
    """{(stage, scene): (status, share, causes, reason)}"""
    out = {}
    if os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.lstrip().startswith("#"):
                head, _, reason = line.partition("|")
                f = head.split()
                if len(f) < 5 or f[0] not in ("proposed", "confirmed") or not f[3].endswith("%"):
                    sys.exit(f"{path}: bad line (STATUS STAGE SCENE SHARE% CAUSES | reason): {line.strip()}")
                out[(f[1], f[2])] = (f[0], float(f[3][:-1]), f[4:], reason.strip())
    return out


def confirmed_mask(stage, scene, listing=None):
    """The entry of this scene, only when the owner has confirmed it; else None (the scene is judged whole)."""
    e = (listing if listing is not None else load_list()).get((stage, scene))
    return e if e and e[0] == "confirmed" else None


def main():
    a = sys.argv[1:]
    if a[:1] == ["make"] and len(a) == 4:
        print(f"ignored {make(a[1], a[2], a[3])}% of the picture")
    elif a == ["list"]:
        for (st, sc), (s, sh, c, r) in load_list().items():
            print(f"{s:9} {st} {sc} {sh}% {','.join(c)}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
