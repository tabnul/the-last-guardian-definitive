"""GPU ms and MB a frame per kind of copy and upload (the R5 itemized meter, stats vkb.compute.stream.gpu_ms_by_work), from two /stats snapshots
(vkb_run_snapshots.ps1 -Endpoints "status,stats,sleep,status,stats" -Between 5: <tag>_status_1, _stats_2, _status_4, _stats_5).
usage: vkb_gpu_copies.py tag [tag ...]"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402
R = str(dev_paths.RUNS)
KINDS = ("copies_texture", "copies_buffer", "copies_gds", "fills", "uploads_first_bind", "uploads_initial", "uploads_unmap")


def ld(tag, n):
    return json.load(open(f"{R}\\{tag}_{n}.json", encoding="utf-8-sig"))


for tag in sys.argv[1:]:
    s1, t1, s2, t2 = ld(tag, "status_1"), ld(tag, "stats_2"), ld(tag, "status_4"), ld(tag, "stats_5")
    frames = s2["presents"] - s1["presents"]
    w1, w2 = t1["vkb"]["compute"]["stream"]["gpu_ms_by_work"], t2["vkb"]["compute"]["stream"]["gpu_ms_by_work"]
    n1, n2 = w1["timed_submissions"], w2["timed_submissions"]
    b1, b2 = w1.get("bytes_by_kind", {}), w2.get("bytes_by_kind", {})
    total = (w2["whole_run"]["total"] * n2 - w1["whole_run"]["total"] * n1) / frames
    print(f"{tag}: {frames} frames, GPU total {total:.2f} ms a frame")
    for k in KINDS:
        ms = (w2["whole_run"].get(k, 0) * n2 - w1["whole_run"].get(k, 0) * n1) / frames
        jobs = (b2.get(k, {}).get("jobs", 0) - b1.get(k, {}).get("jobs", 0)) / frames
        mb = (b2.get(k, {}).get("mb", 0) - b1.get(k, {}).get("mb", 0)) / frames
        print(f"   {k:20s} {ms:6.3f} ms  {jobs:7.1f} jobs  {mb:7.2f} MB")
