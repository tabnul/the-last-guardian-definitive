#!/usr/bin/env python3
"""Difference between two pictures of the same size. No AI inside; needs ImageMagick (convert).

Usage: compare.py A.png B.png [DIFF.png] [--mask MASK.png]
Prints: mean absolute difference per channel (0..255), the share of pixels whose largest channel difference
is above 8, 16 and 32, and the largest difference. DIFF.png (optional) is the difference picture, made 4x
brighter so small differences can be seen.
A blank picture (one flat colour, such as a black screen) is refused with exit code 2: it cannot be
compared with anything."""
import os, subprocess, sys

# Windows (owner's PC sweep, issue 89): ImageMagick 7 is called as "magick"; convert.exe there is a Windows disk tool
WIN = os.name == "nt"


def im(*args):
    """An ImageMagick convert command line: "convert ..." on Linux, "magick ..." on Windows."""
    return ["magick", *args] if WIN else ["convert", *args]


IDENTIFY = ["magick", "identify"] if WIN else ["identify"]
BLANK_SPREAD, BLANK_SHARE = 8, 99.5  # blank: this share (%) of pixels within this distance of the most common colour


def rgb(path):
    size = subprocess.run([*IDENTIFY, "-format", "%w %h", path], capture_output=True, text=True, check=True).stdout
    w, h = map(int, size.split())
    data = subprocess.run(im(path, "-depth", "8", "rgb:-"), capture_output=True, check=True).stdout
    return w, h, data


def blank(data):
    """'' for a real picture, else a short reason: the picture is (almost) one flat colour."""
    n = len(data) // 3
    if n == 0:
        return "no pixels"
    counts = {}
    for i in range(0, len(data), 3 * 7):  # every 7th pixel is plenty to find the most common colour
        c = data[i:i + 3]
        counts[c] = counts.get(c, 0) + 1
    top = max(counts, key=counts.get)
    near = sum(1 for i in range(0, len(data), 3)
               if max(abs(data[i] - top[0]), abs(data[i + 1] - top[1]), abs(data[i + 2] - top[2])) <= BLANK_SPREAD)
    share = 100.0 * near / n
    if share < BLANK_SHARE:
        return ""
    return f"blank: {share:.2f}% of pixels within {BLANK_SPREAD} of colour {tuple(top)}"


def mask_bits(path):
    """Noise mask picture (white = left out) as one byte per pixel, 1 = left out."""
    return [1 if v > 127 else 0 for v in subprocess.run(im(path, "-depth", "8", "-colorspace", "gray", "gray:-"),
                                                        capture_output=True, check=True).stdout]


def measure(a, b, mask=None):
    """Difference of two equal sized RGB byte strings: mean per channel, share (%) of pixels whose largest
    channel difference is above 8, 16 and 32, and the largest difference. With a mask (list from mask_bits) the
    pixels it marks are left out: all shares are of the pixels that remain, and `ignored` is the share left out."""
    d = bytes(abs(x - y) for x, y in zip(a, b))
    px = [max(d[i], d[i + 1], d[i + 2]) for i in range(0, len(d), 3)]
    if mask is not None:
        total = len(px)
        px = [v for v, m in zip(px, mask) if not m]
        d = bytes(x for i, x in enumerate(d) if not mask[i // 3])
        n = len(px)
        if n == 0:
            raise ValueError("the mask leaves out every pixel")
        r = {"ignored": round(100.0 * (total - n) / total, 2)}
    else:
        r = {}
    n = len(px)
    hist = [0] * 256
    for v in px:
        hist[v] += 1
    above = lambda t: round(100.0 * sum(hist[t + 1:]) / n, 3)
    r.update({"mean": round(sum(d) / len(d), 3), "gt8": above(8), "gt16": above(16), "gt32": above(32), "max": max(px)})
    return r


def main():
    mask = None
    if "--mask" in sys.argv:
        i = sys.argv.index("--mask")
        mask = mask_bits(sys.argv[i + 1])  # white pixels of the mask are left out (see noisemask.py)
        del sys.argv[i:i + 2]
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    # a confirmed log-only scene (log-only.txt) is never counted by picture, for or against a fix
    import importlib.util
    spec = importlib.util.spec_from_file_location("verdict", os.path.join(os.path.dirname(os.path.abspath(__file__)), "verdict.py"))
    verdict = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verdict)
    listed = {} if os.environ.get("COMPARE_LOG_ONLY") == "0" else verdict.load_list()  # switch: COMPARE_LOG_ONLY=0 compares them anyway
    for p in sys.argv[1:3]:
        for part in os.path.abspath(p).split(os.sep):
            if "__" in part and tuple(part.split("__", 1)) in listed and listed[tuple(part.split("__", 1))][0] == "confirmed":
                print(f"log only: {part} is on the log-only list, picture not counted")
                return
    wa, ha, a = rgb(sys.argv[1])
    wb, hb, b = rgb(sys.argv[2])
    if (wa, ha) != (wb, hb):
        sys.exit(f"sizes differ: {wa}x{ha} and {wb}x{hb}")
    bad = [f"{p}: {r}" for p, r in ((sys.argv[1], blank(a)), (sys.argv[2], blank(b))) if r]
    if bad:
        print("not compared, blank picture:\n  " + "\n  ".join(bad), file=sys.stderr)
        sys.exit(2)
    if mask is not None and len(mask) != wa * ha:
        sys.exit("mask size differs from the pictures")
    m = measure(a, b, mask)
    print((f"ignored {m['ignored']}%  " if mask is not None else "") + f"pixels {wa}x{ha}  mean {m['mean']:.3f}  >8 {m['gt8']:.3f}%  >16 {m['gt16']:.3f}%  "
          f">32 {m['gt32']:.3f}%  max {m['max']}")
    if len(sys.argv) > 3:
        subprocess.run(im(sys.argv[1], sys.argv[2], "-compose", "difference", "-composite",
                        "-evaluate", "multiply", "4", sys.argv[3]), check=True)


if __name__ == "__main__":
    main()
