#!/usr/bin/env python3
"""mask3-measure.py STAGE__SCENE : 3-photo mask (answer key 1+2, retake 1) vs old 2-photo mask (answer key 1+2); held-out
photo = retake 2, compared with each of the 3 mask photos, outside the mask. Prints one JSON line."""
import json, os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare, noisemask
AK = os.environ.get("MASK3_AK", "answer-key"); RT = os.environ.get("MASK3_RT", "retake"); OUT = os.environ.get("MASK3_OUT", "mask3")  # folders of STAGE__SCENE/vkb0_takeN.png
key = sys.argv[1]
ph = {"ak1": f"{AK}/{key}/vkb0_take1.png", "ak2": f"{AK}/{key}/vkb0_take2.png",
      "rt1": f"{RT}/{key}/vkb0_take1.png", "rt2": f"{RT}/{key}/vkb0_take2.png"}
r = {"key": key}
if not all(os.path.exists(p) for p in ph.values()):
    r["error"] = "missing photo"; print(json.dumps(r)); sys.exit()
img = {k: compare.rgb(p) for k, p in ph.items()}
if len({v[:2] for v in img.values()}) != 1:
    r["error"] = "sizes differ"; print(json.dumps(r)); sys.exit()
def cls(m):
    if m["gt16"] > 2.0: return "cannot"
    if m["gt16"] > 1.0 or m["gt32"] > 0.5: return "above"
    return "within"
with tempfile.TemporaryDirectory() as t:
    for name, pics in (("m2", ["ak1", "ak2"]), ("m3", ["ak1", "ak2", "rt1"])):
        m = f"{t}/{name}.png"; r[name + "_ignored"] = noisemask.make([ph[p] for p in pics], m)
        bits = compare.mask_bits(m); ms = {}
        for ref in pics:
            ms[ref] = compare.measure(img[ref][2], img["rt2"][2], bits)
        w = max(ms.values(), key=lambda x: (x["gt16"], x["gt32"]))
        r[name] = {"worst_gt16": w["gt16"], "worst_gt32": w["gt32"], "class": cls(w)}
        if name == "m3":
            os.makedirs(OUT, exist_ok=True); os.replace(m, f"{OUT}/{key}.png")
print(json.dumps(r))
