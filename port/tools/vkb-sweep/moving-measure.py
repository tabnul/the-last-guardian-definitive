#!/usr/bin/env python3
"""moving-measure.py LIST.tsv OUT.tsv : old mask against moving-things mask (noisemask.make(..., moving=True)) on the existing
normal-renderer photos, no game run (issue 27). LIST.tsv lines: `SOURCE STAGE__SCENE` with SOURCE 56 (mask from answer-key
takes 1+2) or 13 (mask from answer-key 1+2 and retake 1, held out: retake 2). Held-out photos are compared with each
mask photo outside the mask; worst of them is kept. Class: cannot >2% above 16, above >1% above 16 or >0.5% above 32,
else within (nothing is "cannot" any more: a scene above the limit gets its own noise limit; only a mask over 25% sends it to log only). Folders: MASK3_AK (answer-key), MASK3_RT (retake), MASK3_OUT (moving masks written there as PNG)."""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare, noisemask
AK = os.environ.get("MASK3_AK", "answer-key"); RT = os.environ.get("MASK3_RT", "retake"); OUT = os.environ.get("MASK3_OUT", "moving")
def cls(m):
    if m["gt16"] > 1.0 or m["gt32"] > 0.5: return "above"
    return "within"
os.makedirs(OUT, exist_ok=True)
out = open(sys.argv[2], "w"); out.write("source\tscene\told_ignored\told_gt16\told_class\tmoving_ignored\tmoving_gt16\tmoving_gt32\tmoving_class\n")
for line in open(sys.argv[1]):
    if not line.strip(): continue
    src, key = line.split()
    ak = [f"{AK}/{key}/vkb0_take{i}.png" for i in (1, 2)]; rt = [f"{RT}/{key}/vkb0_take{i}.png" for i in (1, 2)]
    if src == "13": refs, held = ak + [rt[0]], [rt[1]]
    else: refs, held = ak, rt
    held = [p for p in held if os.path.exists(p)]
    if not held or not all(os.path.exists(p) for p in refs):
        out.write(f"{src}\t{key}\t-\t-\t-\t-\t-\t-\tno photos\n"); continue
    img = {p: compare.rgb(p) for p in refs + held}
    if len({v[:2] for v in img.values()}) != 1:
        out.write(f"{src}\t{key}\t-\t-\t-\t-\t-\t-\tsizes differ\n"); continue
    row = [src, key]
    with tempfile.TemporaryDirectory() as t:
        for name, mv in (("old", False), ("moving", True)):
            m = f"{t}/{name}.png"; ig = noisemask.make(refs, m, moving=mv); bits = compare.mask_bits(m)
            ms = [compare.measure(img[r][2], img[h][2], bits) for r in refs for h in held]
            w = max(ms, key=lambda x: (x["gt16"], x["gt32"]))
            row += [ig, w["gt16"]] + ([w["gt32"]] if mv else []) + [cls(w)]
            if mv: os.replace(m, f"{OUT}/{key}.png")
    out.write("\t".join(map(str, row)) + "\n"); out.flush()
