#!/usr/bin/env python
"""Median ledger of repeated runs: python -I ledger_median.py <Tag> <scene> <N> -> prints JSON.

Reads user/log/runs/ledger_<Tag>r<i>_<scene>.json for i in 1..N (written by wall_ledger.ps1 -Repeat N). Every number becomes its median; `frame_ms_runs` lists the frame times
and `frame_ms_spread` is max - min, the noise of this configuration in this scene (a change smaller than it is not a result).
"""
import json
import os
import statistics
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402

RUNS = dev_paths.RUNS


def main():
    tag, scene, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    rows = []
    for i in range(1, n + 1):
        p = RUNS / f"ledger_{tag}r{i}_{scene}.json"
        if p.exists():
            rows.append(json.loads(p.read_text(encoding="utf-8-sig")))
    if not rows:
        print(json.dumps({"tag": f"{tag}_{scene}", "error": "no runs"}))
        return
    out = {"tag": f"{tag}_{scene}", "runs": len(rows)}
    for k in rows[0]:
        vals = [r.get(k) for r in rows]
        if all(isinstance(v, (int, float)) for v in vals):
            out[k] = round(statistics.median(vals), 3)
    frames = [r.get("frame_ms_steady") for r in rows if r.get("frame_ms_steady") is not None]
    out["frame_ms_runs"] = frames
    out["frame_ms_spread"] = round(max(frames) - min(frames), 2) if frames else None
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
