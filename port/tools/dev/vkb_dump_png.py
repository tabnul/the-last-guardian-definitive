# Converts the backend's raw image dumps (log/vkb_image_dump/<name>_<w>x<h>_<format>.bin, note 41) into PNGs next to them.
# Usage: python vkb_dump_png.py [folder]   (default: the app's log/vkb_image_dump)
# Float images are shown with x / (1 + x) per channel; depth is stretched over the range of its texels that are neither 0 nor 1 (printed).
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402

import numpy as np
from PIL import Image

folder = sys.argv[1] if len(sys.argv) > 1 else str(dev_paths.LOG / "vkb_image_dump")


def tonemap(x):
    x = np.nan_to_num(np.maximum(x, 0.0))
    return (x / (1.0 + x) * 255.0).astype(np.uint8)


def unorm(x, bits):
    return (x.astype(np.float32) / ((1 << bits) - 1) * 255.0).astype(np.uint8)


def uf11(v):  # 6 bit mantissa, 5 bit exponent
    m, e = v & 0x3F, (v >> 6) & 0x1F
    return np.where(e == 0, m / 64.0 * 2.0 ** -14, (1.0 + m / 64.0) * 2.0 ** (e.astype(np.float32) - 15.0))


def uf10(v):  # 5 bit mantissa, 5 bit exponent
    m, e = v & 0x1F, (v >> 5) & 0x1F
    return np.where(e == 0, m / 32.0 * 2.0 ** -14, (1.0 + m / 32.0) * 2.0 ** (e.astype(np.float32) - 15.0))


BC = {"Bc1RgbaUnormBlock": 71, "Bc1RgbaSrgbBlock": 72, "Bc1RgbUnormBlock": 71, "Bc1RgbSrgbBlock": 72, "Bc2UnormBlock": 74, "Bc2SrgbBlock": 75,
      "Bc3UnormBlock": 77, "Bc3SrgbBlock": 78, "Bc4UnormBlock": 80, "Bc4SnormBlock": 81, "Bc5UnormBlock": 83, "Bc5SnormBlock": 84,
      "Bc7UnormBlock": 98, "Bc7SrgbBlock": 99}

for path in sorted(glob.glob(os.path.join(folder, "*.bin"))):
    m = re.match(r"(.+)_(\d+)x(\d+)_(\w+)\.bin$", os.path.basename(path))
    if not m:
        continue
    name, w, h, fmt = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
    raw = open(path, "rb").read()
    note = ""
    if fmt == "R8Unorm":
        img = np.frombuffer(raw, np.uint8).reshape(h, w)
    elif fmt in ("R8G8B8A8Unorm", "R8G8B8A8Srgb", "B8G8R8A8Unorm", "B8G8R8A8Srgb"):
        a = np.frombuffer(raw, np.uint8).reshape(h, w, 4)
        img = a[:, :, [2, 1, 0]] if fmt.startswith("B8") else a[:, :, :3]
        Image.fromarray(a[:, :, 3]).save(path[:-4] + "_alpha.png")
    elif fmt == "A2B10G10R10UnormPack32":
        v = np.frombuffer(raw, np.uint32).reshape(h, w)
        img = np.stack([unorm(v & 0x3FF, 10), unorm((v >> 10) & 0x3FF, 10), unorm((v >> 20) & 0x3FF, 10)], -1)
    elif fmt == "B10G11R11UfloatPack32":
        v = np.frombuffer(raw, np.uint32).reshape(h, w)
        img = tonemap(np.stack([uf11(v & 0x7FF), uf11((v >> 11) & 0x7FF), uf10((v >> 22) & 0x3FF)], -1))
    elif fmt in ("R16G16B16A16Sfloat",):
        a = np.frombuffer(raw, np.float16).reshape(h, w, 4).astype(np.float32)
        img = tonemap(a[:, :, :3])
        note = f"rgb max {a[:, :, :3].max():.3f} mean {a[:, :, :3].mean():.3f} alpha mean {a[:, :, 3].mean():.3f}"
    elif fmt in ("R16Sfloat",):
        img = tonemap(np.frombuffer(raw, np.float16).reshape(h, w).astype(np.float32))
    elif fmt in ("R32Sfloat", "D32Sfloat", "D32SfloatS8Uint"):
        d = np.frombuffer(raw, np.float32).reshape(h, w)
        inner = d[(d > 0.0) & (d < 1.0)]
        lo, hi = (float(inner.min()), float(inner.max())) if inner.size else (0.0, 1.0)
        img = (np.clip((d - lo) / max(hi - lo, 1e-9), 0, 1) * 255).astype(np.uint8)
        note = f"depth: {np.mean(d == 0):.3f} at 0, {np.mean(d == 1):.3f} at 1, inner {lo:.6f}..{hi:.6f}"
    elif fmt == "D16Unorm":
        img = (np.frombuffer(raw, np.uint16).reshape(h, w) >> 8).astype(np.uint8)
    elif fmt in BC:  # raw 4x4 blocks: wrapped in a DDS (DX10 header) for Pillow's decoder
        import io
        import struct
        hdr = struct.pack("<4s7I44x2I4s5I5I", b"DDS ", 124, 0x1007, h, w, 0, 0, 1, 32, 4, b"DX10", 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
        dx10 = struct.pack("<5I", BC[fmt], 3, 0, 1, 0)
        try:
            pil = Image.open(io.BytesIO(hdr + dx10 + raw))
            pil.load()
        except Exception as e:  # noqa: BLE001
            print(f"{name}: {fmt} not decoded ({e})")
            continue
        pil.convert("RGBA").save(path[:-4] + ".png")
        print(f"{name} {w}x{h} {fmt}")
        continue
    else:
        print(f"{name}: format {fmt} not handled")
        continue
    Image.fromarray(img).save(path[:-4] + ".png")
    print(f"{name} {w}x{h} {fmt} {note}")
