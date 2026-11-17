#!/usr/bin/env python3
"""Difference between two pictures of the same size. No AI inside; needs ImageMagick (convert).

Usage: compare.py A.png B.png [DIFF.png]
Prints: mean absolute difference per channel (0..255), the share of pixels whose largest channel difference
is above 8, 16 and 32, and the largest difference. DIFF.png (optional) is the difference picture, made 4x
brighter so small differences can be seen."""
import subprocess, sys


def rgb(path):
    size = subprocess.run(["identify", "-format", "%w %h", path], capture_output=True, text=True, check=True).stdout
    w, h = map(int, size.split())
    data = subprocess.run(["convert", path, "-depth", "8", "rgb:-"], capture_output=True, check=True).stdout
    return w, h, data


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    wa, ha, a = rgb(sys.argv[1])
    wb, hb, b = rgb(sys.argv[2])
    if (wa, ha) != (wb, hb):
        sys.exit(f"sizes differ: {wa}x{ha} and {wb}x{hb}")
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


main()
