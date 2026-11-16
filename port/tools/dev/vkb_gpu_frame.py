"""GPU ms per frame by kind of work, from two /stats snapshots (vkb_run_snapshots.ps1 -Endpoints "status,stats,sleep,status,stats" -Between 5: <tag>_status_1, _stats_2, _status_4, _stats_5).
usage: gpu_frame.py tag [tag ...]"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_paths  # noqa: E402
R = str(dev_paths.RUNS)


def ld(tag, n):
    return json.load(open(f"{R}\\{tag}_{n}.json", encoding="utf-8-sig"))


for tag in sys.argv[1:]:
    s1, t1, s2, t2 = ld(tag, "status_1"), ld(tag, "stats_2"), ld(tag, "status_4"), ld(tag, "stats_5")
    frames = s2["presents"] - s1["presents"]
    w1, w2 = t1["vkb"]["compute"]["stream"]["gpu_ms_by_work"], t2["vkb"]["compute"]["stream"]["gpu_ms_by_work"]
    n1, n2 = w1["timed_submissions"], w2["timed_submissions"]
    row = {}
    for k in ("draws", "dispatches", "copies", "clears", "other", "uploads", "total"):
        row[k] = (w2["whole_run"].get(k, 0) * n2 - w1["whole_run"].get(k, 0) * n1) / frames
    busy = w2["last_window"]["gpu_busy_pct_of_the_window"]
    st1, st2 = t1["vkb"]["compute"]["stream"].get("staging_gpu"), t2["vkb"]["compute"]["stream"].get("staging_gpu")
    staging = (st2["ms_total"] - st1["ms_total"]) / frames if st1 and st2 else float("nan")
    secs = frames / s2.get("fps_5s", 1) if s2.get("fps_5s") else 0
    print("%-6s fps %5.1f  frame %5.2f ms | GPU ms/frame: total %5.2f  draws %5.2f  dispatches %5.2f  copies %5.2f  uploads %4.2f  clears %4.2f | staging %4.2f (not in total) | busy %4.1f %% | submissions/frame %.1f"
          % (tag, s2.get("fps_5s", 0), 1000 / s2.get("fps_5s", 1), row["total"], row["draws"], row["dispatches"], row["copies"], row["uploads"], row["clears"], staging, busy, (n2 - n1) / frames))
