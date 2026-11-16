"""GPU ms a frame by render pass kind and size (the R5 pass meter, stats vkb.compute.stream.gpu_by_pass), from two /stats snapshots
(vkb_run_snapshots.ps1 -Endpoints "status,stats,sleep,status,stats" -Between 5: <tag>_status_1, _stats_2, _status_4, _stats_5).
usage: vkb_gpu_passes.py tag [tag ...]"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402
R = str(dev_paths.RUNS)


def ld(tag, n):
    return json.load(open(f"{R}\\{tag}_{n}.json", encoding="utf-8-sig"))


for tag in sys.argv[1:]:
    s1, t1, s2, t2 = ld(tag, "status_1"), ld(tag, "stats_2"), ld(tag, "status_4"), ld(tag, "stats_5")
    frames = s2["presents"] - s1["presents"]
    before = {(e["kind"], e["width"], e["height"]): e for e in t1["vkb"]["compute"]["stream"].get("gpu_by_pass", [])}
    rows = []
    for e in t2["vkb"]["compute"]["stream"].get("gpu_by_pass", []):
        b = before.get((e["kind"], e["width"], e["height"]), {"ms_total": 0, "passes": 0, "draws": 0})
        rows.append(((e["ms_total"] - b["ms_total"]) / frames, (e["passes"] - b["passes"]) / frames, (e["draws"] - b["draws"]) / frames, e["kind"], e["width"], e["height"]))
    rows.sort(reverse=True)
    print(f"{tag}: {frames} frames, passes {sum(r[0] for r in rows):.2f} ms in {sum(r[1] for r in rows):.1f} passes a frame")
    for ms, n, d, k, w, h in rows[:10]:
        print(f"   {k:8s} {w:5d}x{h:<5d} {ms:6.3f} ms  {n:5.1f} passes  {d:7.1f} draws")
