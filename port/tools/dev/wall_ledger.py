#!/usr/bin/env python
"""Prints the wall ledger of the 120 fps master plan (2026-11-08) section 2 for one measured run.

    python -I wall_ledger.py <tag> <fps_samples (space separated numbers in one string)> <presents_per_s> [<nvidia-smi csv line>]

Reads user/log/timeline_<tag>.bin/.json (through timeline_report.py), user/log/runs/<tag>_stats.json. One line per quantity, in ms per frame unless noted.
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402

LOG = dev_paths.LOG
TOOLS = dev_paths.DEV


def main():
    tag = sys.argv[1]
    samples = [float(x) for x in sys.argv[2].split()] if len(sys.argv) > 2 and sys.argv[2] else []
    pps = float(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else 0.0
    smi = sys.argv[4] if len(sys.argv) > 4 else ""
    out = subprocess.run([sys.executable, "-I", str(TOOLS / "timeline_report.py"), str(LOG / f"timeline_{tag}")], capture_output=True, text=True).stdout
    row = {"tag": tag}
    if pps:
        row["fps_steady"] = round(pps, 2)
        row["frame_ms_steady"] = round(1000.0 / pps, 2)
    if samples:
        row["fps_min"] = min(samples)
        row["fps_max"] = max(samples)
    m = re.search(r"frames: (\d+) presented, ([\d.]+) fps\s+frame ms: mean ([\d.]+)", out)
    if m:
        row["capture_fps"] = float(m.group(2))
        row["capture_frame_ms"] = float(m.group(3))
    # Thread CPU, two sources (design review 2026-11-09, M3):
    #   *_ms_per_frame          from the operating system's thread times over the uncaptured 20 s sample (<tag>_threads.json, written by wall_ledger.ps1), divided by
    #                           the frames of that same sample: the number to compare with frame_ms_steady.
    #   *_ms_per_frame_capture  the timeline capture's busy share times the capture's own frame time (the capture perturbs the run): use it for composition only.
    # Before 2026-11-09 the ledger had only the capture form, under the name *_ms_per_frame.
    frame_ms = row.get("capture_frame_ms", 0.0)
    names = (("shadPS4:GpuRaster", "raster_cpu"), ("ps4render", "ps4render_cpu"), ("shadPS4:GpuCommandProcessor", "front_cpu"), ("shadPS4:GpuEncoder", "encoder_cpu"))
    for name, key in names:
        m = re.search(r"^\s+" + re.escape(name) + r"\s+([\d.]+)%", out, re.M)
        if m:
            row[key + "_pct_capture"] = float(m.group(1))
            if frame_ms:
                row[key + "_ms_per_frame_capture"] = round(float(m.group(1)) / 100.0 * frame_ms, 2)
    tp = LOG / "runs" / f"{tag}_threads.json"
    if tp.exists() and row.get("frame_ms_steady"):
        th = json.loads(tp.read_text(encoding="utf-8-sig"))
        for name, key in names:
            pct = sum(float(t.get("percent", 0.0)) for t in th.get("threads", []) if t.get("name") == name)
            if pct:
                row[key + "_pct"] = round(pct, 1)
                row[key + "_ms_per_frame"] = round(pct / 100.0 * row["frame_ms_steady"], 2)
        if th.get("total_percent") is not None:
            row["process_cpu_cores"] = round(float(th["total_percent"]) / 100.0, 2)
    for label, key in (("dispatch ", "gpu_dispatch_ms"), ("between passes \\(before a dispatch\\)", "gpu_gap_before_dispatch_ms"), ("pass color", "gpu_color_pass_ms"), ("pass gbuffer \\(4\\+ targets\\)", "gpu_gbuffer_pass_ms"),
                       ("pass depth only", "gpu_depth_pass_ms"), ("between passes \\(before a pass\\)", "gpu_gap_before_pass_ms")):
        m = re.search(r"^\s+" + label + r"\s+\d+ x\s+([\d.]+) ms/frame", out, re.M)
        if m:
            row[key] = float(m.group(1))
    m = re.search(r"^\s+dispatch\s+(\d+) x", out, re.M)
    frames = re.search(r"frames: (\d+) presented", out)
    if m and frames:
        row["dispatches_per_frame"] = round(int(m.group(1)) / int(frames.group(1)), 1)
    m = re.search(r"raster op\s+shadPS4:GpuRaster\s+([\d.]+) ms/frame", out)
    if m:
        row["raster_op_ms"] = float(m.group(1))
    m = re.search(r"front: queue full\s+shadPS4:GpuCommandProcessor\s+([\d.]+) ms/frame", out)
    if m:
        row["front_queue_full_ms"] = float(m.group(1))
    gpu = 0.0
    for k in ("gpu_dispatch_ms", "gpu_color_pass_ms", "gpu_gbuffer_pass_ms", "gpu_depth_pass_ms"):
        gpu += row.get(k, 0.0)
    row["gpu_timed_work_ms"] = round(gpu, 2)
    row["gpu_timed_plus_gaps_ms"] = round(gpu + row.get("gpu_gap_before_dispatch_ms", 0.0) + row.get("gpu_gap_before_pass_ms", 0.0), 2)
    sp = LOG / "runs" / f"{tag}_stats.json"
    if sp.exists():
        st = json.loads(sp.read_text(encoding="utf-8-sig"))
        g = st.get("gpu_time", {}).get("stream0", {})
        fps_ref = row.get("fps_steady") or row.get("capture_fps") or 1.0
        if g.get("busy_ms_per_s"):
            row["gpu_active_ms_per_frame"] = round(g["busy_ms_per_s"] / fps_ref, 2)
        cp = st.get("cp_split", {})
        if cp.get("draws_and_dispatches"):
            row["ops_per_frame"] = round(cp["draws_and_dispatches"] / (fps_ref * 5.0), 0)
            row["draws_per_frame"] = round(cp.get("draws", 0) / (fps_ref * 5.0), 0)
    if smi:
        row["nvidia_smi"] = smi.strip()
    m = re.search(r"critical path.*?chain: .*?\n", out, re.S)
    print(json.dumps(row, indent=1))


if __name__ == "__main__":
    main()
