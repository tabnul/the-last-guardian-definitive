#!/usr/bin/env python3
"""A list of scenes, one renderer, unattended. No AI inside. Linux test box.

Usage: sweep.py OUT VKB TAKES [--scenes FILE] [--reference DIR] [NAME=VALUE ...]
  OUT     folder for everything (outside the repo); the same layout as shot.py: OUT/STAGE__SCENE/
  VKB     0 = normal renderer, 1 = new engine
  TAKES   takes per scene (2 or more also gives the noise between take 1 and take 2)
  --scenes FILE   "STAGE SCENE" lines instead of the default set: every point of ../scene-check/scenes.json
                  minus the ones the scene check skips (SKIP in scene_check.py), so 219 scenes.
  --reference DIR the folder of the normal renderer's sweep (its results-vkb0.jsonl and logs): the log verdict of
                  a scene compares the error/warning lines with that scene's normal-renderer logs.
  NAME=VALUE      go into the game's environment of every take (passed on to shot.py).

Every take is one run of shot.py: a fresh game start, the GPU memory guard (SHOT_GTT_LIMIT_GB), the fixed
picture recipe. The debug server is switched on for every take (TLG_DEBUG_SERVER_PORT, default 8790 here) so
the game's counters at the moment of the picture are kept next to the picture.

One result line per scene goes to OUT/results-vkbV.jsonl as soon as the scene is done; a scene that has a line
is not run again, so the same command resumes after a stop. Takes of a scene that already ended (they have a
result in OUT/STAGE__SCENE/vkbV_shot.txt) count; a take cut off by a stop is left and a new one is made.

The sweep stops (and can be resumed) when shot.py cannot run at all (the game is still running, a save backup
is left, the display is in use), and when the GPU memory guard fired. Ctrl+C or SIGTERM stops it cleanly."""
import ast, glob, json, os, re, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare  # noqa: E402
import noisemask  # noqa: E402
import verdict  # noqa: E402

CHECK = os.path.join(HERE, "..", "scene-check")
TAKE_LINE = re.compile(r"^vkb(\d)_take(\d+): (.*?)  \((\d+) s\)$")
PEAK_LINE = re.compile(r"GPU memory peak during the take: GTT ([\d.]+) GB")
ENDED_LINE = "game ended by itself before the take was done"
# skip and fail counters of the new engine (/stats); a name that is not in the file is left out, never set to 0
COUNTERS = {
    "gfx_skipped": ("vkb", "graphics", "skipped"), "gfx_failed": ("vkb", "graphics", "failed"),
    "cs_skipped": ("vkb", "compute", "skipped"), "cs_failed": ("vkb", "compute", "failed"),
    "upload_skipped": ("vkb_objects", "upload", "skipped*"), "images_failed": ("vkb_objects", "images_failed"),
    "views_failed": ("vkb_objects", "views_failed"), "pool_dropped": ("vkb_objects", "gpu_pool", "exec_dropped"),
}


def default_scenes():
    """scenes.json in story order, minus the scene check's SKIP (read from scene_check.py, so there is one list)."""
    src = open(os.path.join(CHECK, "scene_check.py")).read()
    node = next(n for n in ast.parse(src).body if isinstance(n, ast.Assign) and getattr(n.targets[0], "id", "") == "SKIP")
    skip = ast.literal_eval(node.value)
    pts = json.load(open(os.path.join(CHECK, "scenes.json")))
    return [(p["stage"], p["scene"]) for p in pts if (p["stage"], p["area"]) not in skip]


def total(value):
    """A counter is a number or a table of reasons (summed)."""
    if isinstance(value, dict):
        return sum(total(v) for v in value.values())
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else 0


def counters(path):
    if not os.path.exists(path):
        return None
    try:
        data = json.load(open(path))
    except ValueError:
        return None
    out = {}
    for name, keys in COUNTERS.items():
        node = data
        for k in keys[:-1]:
            node = node.get(k, {}) if isinstance(node, dict) else {}
        last = keys[-1]
        if last.endswith("*"):
            found = {k: v for k, v in node.items() if k.startswith(last[:-1])} if isinstance(node, dict) else {}
            if found:
                out[name] = total(found)
        elif isinstance(node, dict) and last in node:
            out[name] = total(node[last])
    return out


LATE_RESULT = "anim query order: timeout on thread"


def log_counts(path):
    if not os.path.exists(path):
        return None
    lines = open(path, errors="replace").read().splitlines()
    return {"errors": sum(1 for l in lines if "<Error>" in l or "<Critical>" in l),
            "warnings": sum(1 for l in lines if "<Warning>" in l),
            "faults": sum(1 for l in lines if "Fault: exception" in l),
            # late query results dropped after the 50 ms limit (the author's fix A, one log line per timeout)
            "late_results": sum(1 for l in lines if LATE_RESULT in l)}


def ended_takes(folder, vkb):
    """{take number: (status, seconds, peak GTT GB, crash)} of the takes of this renderer that ended, from
    vkbV_shot.txt. A take stopped by the GPU memory guard does not count: the scene is not done."""
    done, last, ended = {}, None, False
    path = os.path.join(folder, f"vkb{vkb}_shot.txt")
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            line = line.rstrip("\n")
            m = PEAK_LINE.search(line)
            if m:
                last = float(m[1])
            ended = ended or ENDED_LINE in line
            m = TAKE_LINE.match(line)
            if m and m[1] == vkb:
                if not m[3].startswith("GPU memory guard"):
                    done[int(m[2])] = (m[3], int(m[4]), last, ended)
                last, ended = None, False
    return done


def result(folder, stage, scene, vkb, takes):
    ended = ended_takes(folder, vkb)
    row = {"stage": stage, "scene": scene, "vkb": int(vkb), "takes": []}
    for n in sorted(ended)[:takes]:
        status, seconds, peak, crash = ended[n]
        stem = os.path.join(folder, f"vkb{vkb}_take{n}")
        pic = next((stem + e for e in (".png", ".blank.png") if os.path.exists(stem + e)), None)
        row["takes"].append({
            "take": n, "ok": status.startswith("ok "), "status": status, "seconds": seconds,
            "picture": pic, "blank": bool(pic and pic.endswith(".blank.png")),
            # the game ended by itself before the take was done (a crash or an exit), as shot.py saw it
            "crash": crash,
            "log": log_counts(stem + ".log"), "counters": counters(stem + ".stats.json"), "peak_gtt_gb": peak})
    good = [t["picture"] for t in row["takes"] if t["ok"]]
    row["noise"] = None
    if len(good) >= 2:  # take 1 against take 2 of the same renderer: what this scene changes by itself
        a, b = compare.rgb(good[0]), compare.rgb(good[1])
        row["noise"] = compare.measure(a[2], b[2]) if a[:2] == b[:2] else {"error": "sizes differ"}
    return row


def picture_check(row, ref_row, noise_list):
    """New engine take 1 against the normal renderer's take 1 (reference sweep). `all` = the whole picture;
    `masked` = the same without the noise mask, only where the owner has confirmed that scene's entry in
    noise-ignore.txt (a `proposed` entry is not applied; `entry_status` then says so). None = nothing to compare."""
    mine = next((t["picture"] for t in row["takes"] if t["ok"]), None)
    ref = next((t["picture"] for t in (ref_row or {"takes": []})["takes"] if t["ok"]), None)
    if not mine or not ref:
        return None
    a, b = compare.rgb(mine), compare.rgb(ref)
    if a[:2] != b[:2]:
        return {"error": "sizes differ"}
    out = {"all": compare.measure(a[2], b[2]), "masked": None}
    key = (row["stage"], row["scene"])
    entry = noise_list.get(key)
    if entry:
        out["entry_status"] = entry[0]
    need = noisemask.takes_needed(entry[2]) if entry else 2
    second = [t["picture"] for t in ref_row["takes"] if t["ok"]][1:need]
    if entry and entry[0] == "confirmed" and len(second) < need - 1:
        out["masked_error"] = f"mask needs {need} good reference takes, found {len(second) + 1}"
    elif entry and entry[0] == "confirmed" and second:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            m = os.path.join(tmp, "mask.png")
            noisemask.make([ref] + second, m, moving=noisemask.is_moving(entry[2]))
            try:
                out["masked"] = compare.measure(a[2], b[2], compare.mask_bits(m))
            except ValueError as e:
                out["masked_error"] = str(e)
    return out


def summary(row):
    t = row["takes"]
    n = row["noise"]
    noise = "-" if not n else (n.get("error") or f">16 {n['gt16']:.3f}% >32 {n['gt32']:.3f}%")
    errs = "/".join(str(x["log"]["errors"]) if x["log"] else "-" for x in t)
    return (f"{row['stage']} {row['scene']}: ok {sum(x['ok'] for x in t)}/{len(t)}, crash {sum(x['crash'] for x in t)}, "
            f"log errors {errs}, noise {noise}, group {row['group']}, log {row['log_verdict']['verdict']}" + "".join(f"\n    take{x['take']}: {x['status']}" for x in t if not x["ok"]))


STOP = []


def pid_alive(pid):
    """Is process pid running (the lock check; /proc on Linux, tasklist on Windows: issue 89)."""
    if os.name != "nt":
        return os.path.exists(f"/proc/{pid}")
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True).stdout
    return str(pid) in out


def main():
    args = sys.argv[1:]
    scenes_file = None
    if "--scenes" in args:
        i = args.index("--scenes")
        scenes_file = args[i + 1]
        del args[i:i + 2]
    reference = {}
    if "--reference" in args:
        i = args.index("--reference")
        if i + 1 >= len(args):
            sys.exit(__doc__)
        reference = verdict.rows(os.path.abspath(args[i + 1]))
        if not reference:
            sys.exit(f"--reference {args[i + 1]}: no results-vkb0.jsonl there or it is empty")
        del args[i:i + 2]
    if len(args) < 3 or args[1] not in ("0", "1") or not args[2].isdigit() or int(args[2]) < 1:
        sys.exit(__doc__)
    root, vkb, takes, extra = os.path.abspath(args[0]), args[1], int(args[2]), args[3:]
    if not any(e.startswith("TLG_DEBUG_SERVER_PORT=") for e in extra):
        extra.append("TLG_DEBUG_SERVER_PORT=8790")
    scenes = ([tuple(l.split()[:2]) for l in open(scenes_file) if l.strip() and not l.startswith("#")]
              if scenes_file else default_scenes())
    os.makedirs(root, exist_ok=True)
    lock = os.path.join(root, f"sweep-vkb{vkb}.pid")
    if os.path.exists(lock):
        pid = int(open(lock).read().strip() or 0)
        if pid and pid_alive(pid):
            sys.exit(f"a sweep is already running in {root} (pid {pid})")
    with open(lock, "w") as f:
        f.write(f"{os.getpid()}\n")
    results = os.path.join(root, f"results-vkb{vkb}.jsonl")
    done = set()
    if os.path.exists(results):
        done = {(r["stage"], r["scene"]) for r in map(json.loads, open(results)) if r}
    child = []

    def stop(sig, frame):
        STOP.append(sig)
        # Windows: Ctrl+C already reaches shot.py (same console); send_signal(SIGINT) is not supported there
        if child and child[0].poll() is None and os.name != "nt":
            child[0].send_signal(signal.SIGINT)  # shot.py ends its take, puts the save back, closes its screen
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    listing = verdict.load_list()
    noise_list = noisemask.load_list()
    if vkb == "1" and not reference:
        print("WARNING: no --reference: every scene of this sweep has no normal-renderer data, so every scene is "
              "'log only' and the lines are not compared", flush=True)
    todo = [s for s in scenes if s not in done]
    print(f"sweep vkb{vkb}: {len(scenes)} scenes, {len(scenes) - len(todo)} done before, {len(todo)} to run, "
          f"{takes} takes each, out {root}", flush=True)
    code = 0
    try:
        for k, (stage, scene) in enumerate(todo, 1):
            if STOP:
                break
            folder = os.path.join(root, f"{stage}__{scene}")
            os.makedirs(folder, exist_ok=True)
            need = takes - len(ended_takes(folder, vkb))
            t0 = time.monotonic()
            if need > 0:
                with open(os.path.join(folder, f"vkb{vkb}_shot.txt"), "a") as out:
                    out.write(f"== {time.strftime('%F %T')} shot.py {stage} {scene} {vkb} {need} {' '.join(extra)}\n")
                    out.flush()
                    p = subprocess.Popen([sys.executable, os.path.join(HERE, "shot.py"), root, stage, scene, vkb,
                                          str(need)] + extra, stdout=out, stderr=subprocess.STDOUT,
                                         start_new_session=True)
                    child[:] = [p]
                    p.wait()
                    child.clear()
            if STOP:
                break
            text = open(os.path.join(folder, f"vkb{vkb}_shot.txt"), errors="replace").read()
            row = result(folder, stage, scene, vkb, takes)
            last_run = text.split("\n== ")[-1].splitlines()
            takes_run = [m[3] for m in map(TAKE_LINE.match, last_run) if m]
            guard = next((t for t in takes_run if t.startswith("GPU memory guard")), "")
            if guard:
                # no result line: the scene is not done, a resume runs it again (with a higher SHOT_GTT_LIMIT_GB)
                print(f"STOPPED at {stage} {scene}: {guard}; the scene has no result line; resume with the same "
                      "command", flush=True)
                code = 3
                break
            if len(row["takes"]) < takes:
                # shot.py could not run its takes (not a failed take): a problem of the box, not of the scene
                print(f"STOPPED at {stage} {scene}: shot.py did not run its takes: {' | '.join(last_run[-3:])}",
                      flush=True)
                code = 2
                break
            # the log verdict is given for every scene; scenes of the log-only group (confirmed entries of log-only.txt,
            # or no usable normal-renderer picture) get no picture verdict, the log verdict is all they get
            ref_row = reference.get((stage, scene))
            row["group"], row["group_reason"] = verdict.group(stage, scene, ref_row if vkb == "1" else row, listing)
            row["log_verdict"] = verdict.log_verdict(row, ref_row if vkb == "1" else None)
            row["picture"] = picture_check(row, ref_row, noise_list) if vkb == "1" else None
            row["time"] = time.strftime("%F %T")
            with open(results, "a") as f:
                f.write(json.dumps(row) + "\n")
            print(f"[{k}/{len(todo)}] {summary(row)}  ({time.monotonic() - t0:.0f} s)", flush=True)
    finally:
        os.remove(lock)
    if STOP:
        print("stopped by a signal; resume with the same command", flush=True)
        code = code or 130
    elif code == 0:
        print(f"sweep vkb{vkb}: all {len(scenes)} scenes have a result line in {results}", flush=True)
    sys.exit(code)


main()
