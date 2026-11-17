#!/usr/bin/env python3
"""Difference between two pictures of the same size. No AI inside; needs ImageMagick (convert).

Usage: compare.py A.png B.png [DIFF.png]
Prints: mean absolute difference per channel (0..255), the share of pixels whose largest channel difference
is above 8, 16 and 32, and the largest difference. DIFF.png (optional) is the difference picture, made 4x
brighter so small differences can be seen.
A blank picture (one flat colour, such as a black screen) is refused with exit code 2: it cannot be
compared with anything."""
import subprocess, sys

BLANK_SPREAD, BLANK_SHARE = 8, 99.5  # blank: this share (%) of pixels within this distance of the most common colour


def rgb(path):
    size = subprocess.run(["identify", "-format", "%w %h", path], capture_output=True, text=True, check=True).stdout
    w, h = map(int, size.split())
    data = subprocess.run(["convert", path, "-depth", "8", "rgb:-"], capture_output=True, check=True).stdout
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


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    wa, ha, a = rgb(sys.argv[1])
    wb, hb, b = rgb(sys.argv[2])
    if (wa, ha) != (wb, hb):
        sys.exit(f"sizes differ: {wa}x{ha} and {wb}x{hb}")
    bad = [f"{p}: {r}" for p, r in ((sys.argv[1], blank(a)), (sys.argv[2], blank(b))) if r]
    if bad:
        print("not compared, blank picture:\n  " + "\n  ".join(bad), file=sys.stderr)
        sys.exit(2)
    # per byte absolute difference, then the largest of the three channels per pixel
    d = bytes(abs(x - y) for x, y in zip(a, b))
    px = [max(d[i], d[i + 1], d[i + 2]) for i in range(0, len(d), 3)]
    n = len(px)
    hist = [0] * 256
    for v in px:
        hist[v] += 1
    above = lambda t: 100.0 * sum(hist[t + 1:]) / n
    print(f"pixels {wa}x{ha}  mean {sum(d) / len(d):.3f}  >8 {above(8):.3f}%  >16 {above(16):.3f}%  "
          f">32 {above(32):.3f}%  max {max(px)}")
    if len(sys.argv) > 3:
        subprocess.run(["convert", sys.argv[1], sys.argv[2], "-compose", "difference", "-composite",
                        "-evaluate", "multiply", "4", sys.argv[3]], check=True)


if __name__ == "__main__":
    main()
