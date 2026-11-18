#!/usr/bin/env python3
"""The noise mask: which pixels of a scene the picture check leaves out. No AI inside; needs ImageMagick (convert).

Rule (one rule for every cause, no special case per scene): a pixel counts in the picture check only where the
normal renderer agrees with itself. Mask = every pixel where two of the takes of the NORMAL renderer differ by more
than 16 (largest channel; with 2 takes: take 1 and take 2, with 3 or more: any two of them, issue 27), grown by 4
pixels so that soft edges and a little movement are covered. More takes catch moving parts that two takes miss. The boy standing
elsewhere, drifting mist, swaying grass, flickering flames, floating particles all land in the mask the same way.

Usage:
  noisemask.py make A.png B.png [C.png ...] MASK.png   build the mask (white = left out) from 2 or more takes and
                                           print its share of the picture
  noisemask.py list                        print noise-ignore.txt with its status per entry

noise-ignore.txt: one entry per scene, `STATUS STAGE SCENE SHARE% CAUSES | reason`. STATUS is `proposed` or
`confirmed`. Only the owner confirms (issue "Ignore list: entries to confirm"); the tools honour only `confirmed`.
`confirmed_mask(stage, scene)` gives the entry only then. Nothing in the game or the engine changes."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, "noise-ignore.txt")
THRESHOLD = "6.3%"  # 16 of 255
GROW = "Disk:4"
OPEN = "Disk:2"  # solid-blob rule (issue 27): before the growing, what is thinner than 5 pixels is dropped


def make(pics, mask, solid=False):
    """Write MASK (white = left out) from two or more pictures of the normal renderer; return the share left out (%).
    Per channel, largest minus smallest value over the pictures is the largest difference of any two of them."""
    if len(pics) < 2:
        raise ValueError("a noise mask needs at least 2 pictures")
    lo, hi = mask + ".lo.png", mask + ".hi.png"
    try:
        subprocess.run(["convert", *pics, "-evaluate-sequence", "min", lo], check=True)
        subprocess.run(["convert", *pics, "-evaluate-sequence", "max", hi], check=True)
        subprocess.run(["convert", hi, lo, "-compose", "difference", "-composite", "-separate", "-evaluate-sequence", "max",
                        "-threshold", THRESHOLD, *solid_steps(solid), "-morphology", "Dilate", GROW, mask], check=True)
    finally:
        for f in (lo, hi):
            if os.path.exists(f):
                os.remove(f)
    return share(mask)


def solid_steps(solid):
    """Solid-blob rule (owner 2026-11-18, issue 27): open the raw difference (erode, then dilate) BEFORE the growing, so
    that sparse speckle and thin rims on still walls, floors and ceilings drop out; only solid blobs (boy, mist, foliage,
    flames, hanging or loose objects) stay and are then grown as always. (Opening after the growing was tried first: the
    growing joins the speckle into blobs, 58 of 69 masks stayed as they were.)"""
    return ["-morphology", "Open", OPEN] if solid else []


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


def is_solid(causes):
    """True when the CAUSES field holds `solid` (the solid-blob rule applies to this entry's mask)."""
    return "solid" in causes


def takes_needed(causes):
    """How many good reference takes build this entry's mask: 3 when the CAUSES field holds `takes=3`
    (issue 27), else 2 (the first two takes, as the confirmed entries were measured)."""
    return 3 if "takes=3" in causes else 2


def confirmed_mask(stage, scene, listing=None):
    """The entry of this scene, only when the owner has confirmed it; else None (the scene is judged whole)."""
    e = (listing if listing is not None else load_list()).get((stage, scene))
    return e if e and e[0] == "confirmed" else None


def main():
    a = sys.argv[1:]
    if a[:1] == ["make"] and len(a) >= 4:
        print(f"ignored {make(a[1:-1], a[-1])}% of the picture")
    elif a == ["list"]:
        for (st, sc), (s, sh, c, r) in load_list().items():
            print(f"{s:9} {st} {sc} {sh}% {','.join(c)}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
