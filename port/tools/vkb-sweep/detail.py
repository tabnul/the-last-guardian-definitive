#!/usr/bin/env python3
"""Faint-detail check (issue 54). A second, separate measure next to compare.py; it changes no limit of compare.py.
No AI inside. Needs numpy and Pillow (on the test box: /home/runner/diag/venv/bin/python).

compare.py counts pixels that differ by more than 8/16/32. A faint detail that goes missing on a still surface
(carved lines on a wall, differing by under 10 levels) never reaches those limits. This tool compares how much
fine detail each small tile of the picture holds:

  detail of a tile = standard deviation of (grey picture minus its blurred copy), in grey levels
  ratio of a tile  = detail of the new picture / detail of the normal renderer's picture
  self spread      = the same ratio between the normal renderer's own two takes (mist, water, things that move
                     change from take to take, so they show up here and widen the allowance of that tile)

A tile whose coarse brightness (blur about 12 pixels) differs by more than 4 levels or 3 x its self spread is a moved thing and is
left out. When no group is found and more than half of the tiles with detail are left out like that (or no tile has detail), nothing can be judged and the verdict is UNJUDGED (never OK).
A blank picture (one flat colour) or pictures of different sizes are refused with exit code 2. A tile is flagged when its detail ratio is off by more than max(MARGIN, SPREAD_K x self spread) AND it holds real
detail (at least MIN_DETAIL levels in the normal picture) AND every new-engine take agrees (same tile, same
direction). LOST = the new engine shows less detail, ADDED = more. A scene is flagged (LOST or ADDED) when at
least FLAG_TILES flagged tiles touch each other (a group of touching tiles, 8 neighbours, not a lone tile such as the
boy's pose). When both a LOST and an ADDED group exist the verdict is LOST+ADDED: nothing is
hidden. The strip of the picture that does not fill a whole tile (at the right and bottom edge, 28 rows of a 988-line
picture) is not judged.

Scenes on the confirmed log-only list (log-only.txt; with --scene-dir the folder name STAGE__SCENE is used) get `detail LOGONLY` and no picture verdict.
A flagged tile also needs an absolute change of its detail of at least DETAIL_ABS_MIN (0.6) grey levels, so faint dark differences that nobody sees are not flagged.

Usage:
  detail.py NORMAL1.png NORMAL2.png NEW1.png [NEW2.png ...] [--map OUT.png] [--tile N] [--sigma S]
  detail.py --scene-dir FOLDER [--map OUT.png]       (FOLDER holds vkb0_take1/2.png and vkb1_take1/2.png)
Prints one line:  detail <OK|LOST|ADDED|LOST+ADDED|UNJUDGED> tiles flagged F of D with detail (worst x,y ratio R, self spread S)
Exit code 0 for every verdict; 2 for a blank picture or a size difference (like compare.py); 1 for a missing argument or
picture. Nothing in the sweep reads this tool yet: it is a proposal.
Switch: nothing runs unless this script is called (off costs nothing). Environment DETAIL_MARGIN, DETAIL_SPREAD_K,
DETAIL_MIN, DETAIL_ABS_MIN, DETAIL_FLAG_TILES override the numbers below."""
import os, sys
import numpy as np
from PIL import Image

TILE = 64
SIGMA = 4.0
MARGIN = float(os.environ.get("DETAIL_MARGIN", 0.20))        # allowed detail change (20%) even where nothing moves
SPREAD_K = float(os.environ.get("DETAIL_SPREAD_K", 3.0))     # times the normal renderer's own take-to-take spread
MIN_DETAIL = float(os.environ.get("DETAIL_MIN", 0.8))        # grey levels; below this a tile is flat, not judged
LOW_MAX = float(os.environ.get("DETAIL_LOW_MAX", 4.0))      # grey levels; coarse brightness of a still tile may differ this much
ABS_MIN = float(os.environ.get("DETAIL_ABS_MIN", 0.6))       # grey levels; a tile also needs this much absolute change of detail (the eye sees no less, owner PR 97)
FLAG_TILES = int(os.environ.get("DETAIL_FLAG_TILES", 10))     # touching flagged tiles needed to flag the scene


def log_only_reason(name):
    """Reason when the scene (folder name STAGE__SCENE) has a confirmed line in log-only.txt, else None."""
    try:
        stage, scene = name.split("__", 1)
        for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "log-only.txt")):
            f = line.split(None, 3)
            if len(f) >= 3 and f[0] == "confirmed" and f[1] == stage and f[2] == scene:
                return f[3].strip() if len(f) > 3 else "no reason"
    except (OSError, ValueError):
        pass
    return None


def grey(path):
    return np.asarray(Image.open(path).convert("L"), dtype=np.float32)


def blur(a, sigma):
    r = int(3 * sigma)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    for axis in (0, 1):
        p = np.pad(a, [(r, r) if i == axis else (0, 0) for i in range(2)], mode="edge")
        out = np.zeros_like(a)
        n = a.shape[axis]
        for i, w in enumerate(k):
            out += w * (p[i:i + n] if axis == 0 else p[:, i:i + n])
        a = out
    return a


def tiles(x, tile):
    h, w = (x.shape[0] // tile) * tile, (x.shape[1] // tile) * tile
    return x[:h, :w].reshape(h // tile, tile, w // tile, tile)


def tile_detail(a, tile, sigma):
    """(detail, coarse): per tile the standard deviation of the fine detail, and the coarse picture (blurred
    with 3 x sigma on top of sigma, about 12 pixels) as a small map of the tile means."""
    low = blur(a, sigma)
    return tiles(a - low, tile).std(axis=(1, 3)), tiles(blur(low, 3 * sigma), tile).mean(axis=(1, 3))


def groups(mask, need):
    """True where the flagged tile belongs to a group of at least `need` touching flagged tiles (8 neighbours)."""
    seen = np.zeros_like(mask)
    keep = np.zeros_like(mask)
    H, W = mask.shape
    for y in range(H):
        for x in range(W):
            if mask[y, x] and not seen[y, x]:
                stack, comp = [(y, x)], []
                seen[y, x] = True
                while stack:
                    cy, cx = stack.pop()
                    comp.append((cy, cx))
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            ny, nx = cy + dy, cx + dx
                            if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and not seen[ny, nx]:
                                seen[ny, nx] = True
                                stack.append((ny, nx))
                if len(comp) >= need:
                    for cy, cx in comp:
                        keep[cy, cx] = True
    return keep


def check_pictures(paths):
    """Refuse pictures that cannot be compared: different sizes, or a blank picture (as compare.py does)."""
    size = None
    for p in paths:
        g = grey(p)
        if size not in (None, g.shape):
            print(f"sizes differ: {p} is {g.shape[::-1]}, the first picture {size[::-1]}", file=sys.stderr)
            sys.exit(2)
        size = g.shape
        v, c = np.unique(g.astype(np.uint8), return_counts=True)
        top = v[np.argmax(c)]
        if (np.abs(g - float(top)) <= 8).mean() >= 0.995:
            print(f"not judged, blank picture: {p}", file=sys.stderr)
            sys.exit(2)


def judge(normal1, normal2, news, tile=TILE, sigma=SIGMA):
    """Returns dict: verdict, flagged tiles (lost/added masks), worst tile, self spread, tiles with detail."""
    check_pictures([normal1, normal2, *news])
    (d1, c1), (d2, c2) = tile_detail(grey(normal1), tile, sigma), tile_detail(grey(normal2), tile, sigma)
    has = np.minimum(d1, d2) >= MIN_DETAIL
    eps = 1e-3
    spread = np.abs(np.log((d2 + eps) / (d1 + eps)))
    allow = np.maximum(np.log(1 + MARGIN), SPREAD_K * spread)
    mean_ref = (d1 + d2) / 2
    # a tile whose coarse brightness differs more than a still surface should is a moved object, shadow, door,
    # water ring ...: it is judged by compare.py, not here
    low_allow = np.maximum(LOW_MAX, 3 * np.abs(c1 - c2))
    still = np.ones_like(has)
    lost = has.copy()
    added = has.copy()
    ratios = []
    for p in news:
        dn, cn = tile_detail(grey(p), tile, sigma)
        still &= np.abs(cn - (c1 + c2) / 2) <= low_allow
        lr = np.log((dn + eps) / (mean_ref + eps))
        ratios.append(lr)
        big = np.abs(dn - mean_ref) >= ABS_MIN
        lost &= (lr < -allow) & big
        added &= (lr > allow) & big
    lost, added = groups(lost & still, FLAG_TILES), groups(added & still, FLAG_TILES)
    worst = np.mean(ratios, axis=0)
    flagged = lost | added
    verdict = "OK" if not flagged.any() else ("LOST" if lost.any() and not added.any() else "ADDED" if added.any() and not lost.any() else "LOST+ADDED")
    if verdict == "OK" and (int(has.sum()) == 0 or int((has & ~still).sum()) * 2 > int(has.sum())):
        verdict = "UNJUDGED"  # nothing to judge (flat picture) or most tiles moved: never a false OK
    if flagged.any():
        wy, wx = np.unravel_index(np.argmax(np.where(flagged, np.abs(worst), -1)), worst.shape)
    else:
        wy, wx = np.unravel_index(np.argmax(np.where(has, np.abs(worst), -1)), worst.shape) if has.any() else (0, 0)
    return {"verdict": verdict, "lost": lost, "added": added, "flagged": int(flagged.sum()), "with_detail": int(has.sum()),
            "worst_xy": (int(wx * tile), int(wy * tile)), "worst_ratio": float(np.exp(worst[wy, wx])),
            "moved_tiles": int((has & ~still).sum()), "self_spread": float(np.exp(spread[wy, wx]) - 1), "tile": tile}


def draw_map(res, base_png, out):
    """The new picture (or the first normal one) with flagged tiles tinted: red = LOST, blue = ADDED."""
    im = np.asarray(Image.open(base_png).convert("RGB")).astype(np.float32)
    t = res["tile"]
    for mask, col in ((res["lost"], (255, 0, 0)), (res["added"], (0, 120, 255))):
        for y, x in zip(*np.nonzero(mask)):
            im[y * t:(y + 1) * t, x * t:(x + 1) * t] = 0.5 * im[y * t:(y + 1) * t, x * t:(x + 1) * t] + 0.5 * np.array(col)
    Image.fromarray(im.clip(0, 255).astype(np.uint8)).save(out)


def main():
    a = sys.argv[1:]
    out = tile = sigma = None
    for flag in ("--map", "--tile", "--sigma"):
        if flag in a:
            i = a.index(flag)
            v = a[i + 1]
            del a[i:i + 2]
            if flag == "--map":
                out = v
            elif flag == "--tile":
                tile = int(v)
            else:
                sigma = float(v)
    if a[:1] == ["--scene-dir"]:
        d = a[1]
        reason = log_only_reason(os.path.basename(os.path.normpath(d)))
        if reason:
            print(f"detail LOGONLY  scene is on the log-only list ({reason}); picture not judged")
            return
        n1, n2 = os.path.join(d, "vkb0_take1.png"), os.path.join(d, "vkb0_take2.png")
        news = [os.path.join(d, f"vkb1_take{i}.png") for i in (1, 2) if os.path.exists(os.path.join(d, f"vkb1_take{i}.png"))]
    elif len(a) >= 3:
        n1, n2, news = a[0], a[1], a[2:]
    else:
        sys.exit(__doc__)
    if not news:
        sys.exit("no new-engine picture")
    r = judge(n1, n2, news, tile or TILE, sigma or SIGMA)
    print(f"detail {r['verdict']}  tiles flagged {r['flagged']} of {r['with_detail']} with detail  "
          f"(worst tile x,y {r['worst_xy'][0]},{r['worst_xy'][1]} ratio {r['worst_ratio']:.2f}, normal self spread {r['self_spread']:.2f}; {r['moved_tiles']} tiles left out as moved)")
    if out:
        draw_map(r, news[0], out)


if __name__ == "__main__":
    main()
