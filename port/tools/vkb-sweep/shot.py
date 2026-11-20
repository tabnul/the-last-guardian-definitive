#!/usr/bin/env python3
"""One comparable picture of one scene, for one renderer. Linux, virtual screen (Xvfb). No AI inside.

Recipe per take: fresh start (clean save from scene-check/start.save) -> title -> retarget CONTINUE to
STAGE/SCENE -> wait until the load overlay ended -> 8 s -> hide Trico -> own camera at a fixed position,
set every tick -> game speed 0.001 -> 4 s -> picture of the game window on the virtual screen.
SHOT_TRICO=show (environment) leaves out the "hide Trico" step only; the default is SHOT_TRICO=hide.
SHOT_LEAVEOUT=camera,pose,speed,tick leaves out those photo steps (active camera, camera pose once, game speed, camera tick).
SHOT_PLAY_S=N lets the game play N seconds after the load, before the photo steps (measurement runs; default 0).

Usage: shot.py OUT STAGE SCENE VKB TAKES [--cam "X Y Z PITCH YAW"] [NAME=VALUE ...]
  OUT   folder for the pictures and logs (outside the repo). Pictures: OUT/STAGE__SCENE/vkbV_takeN.png
  VKB   0 = normal renderer, 1 = new engine (always set explicitly)
The fixed camera is taken from the game's own camera in the first take ever made of that scene and kept in
OUT/STAGE__SCENE/camera.txt; every later take, of either renderer, uses the same one. --cam overrides it.
Run with the build environment loaded (source /home/runner/loop/build-env on the test box)."""
import glob, http.client, os, re, shutil, signal, subprocess, sys, threading, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402  (blank picture check)

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(os.path.dirname(HERE))
START = os.path.join(PORT, "tools", "scene-check", "start.save")
USER = os.environ.get("TLG_USER_DIR", os.path.expanduser("~/tlgrun/user"))
DISPLAY = os.environ.get("SHOT_DISPLAY", ":93")
TITLE_S, LOAD_S, WAIT_S, SETTLE_S = 400, 240, 8, 4
PLAY_S = float(os.environ.get("SHOT_PLAY_S", "0"))
GB = 1 << 30
JOURNAL = os.environ.get("SHOT_JOURNAL", "/home/runner/loop/journal.md")

HEAD = ("local ls = loadstring or load\n"
        "local function L(s) local g = ls('--TLGLOG ' .. tostring(s)) if g then g() end end\n")
WARP = HEAD + """L('shot warp @STAGE@ @SCENE@')
if ControllerMenu and ControllerMenu.Tick then
  pcall(function()
    UnRegisterTick(ControllerMenu.Tick)
    DelayExec(function() GUI_DrawText(0, 0, 1, 1, "0x0", " ") end)
  end)
end
local orig = LoadCheckPoint
LoadCheckPoint = function(...)
  LoadCheckPoint = orig
  local r = orig(...)
  GameFlag.Stage, GameFlag.Scene = '@STAGE@', '@SCENE@'
  L('shot retarget ' .. GameFlag.Stage .. ' ' .. GameFlag.Scene)
  return r
end
local ok, e = pcall(GameMenu_AutoAsing_SelectTitleContinue)
if not ok then L('shot warp error: ' .. tostring(e)) end
"""
CAM = HEAD + """local ok, e = pcall(function()
  local src = FindObject('_world').activeCamera
  local p = Vector(src.worldPosition) local r = Vector(src.worldRotation:ToEuler())
  L(string.format('shot cam %.4f %.4f %.4f %.4f %.4f from=%s', p.X, p.Y, p.Z, r.X, r.Y, tostring(src:GetName())))
end)
if not ok then L('shot cam error: ' .. tostring(e)) end
"""
PHOTO = HEAD + """local ok, e = pcall(function()
@TRICO@
@CAMERA@
@POSE@
@SPEED@
@TICK@
  L('shot photo on cam=' .. tostring(FindObject('_world').activeCamera:GetName()) .. ' speed=' .. tostring(MS.GetGameSpeed()))
end)
if not ok then L('shot photo error: ' .. tostring(e)) end
"""
# the photo setup as separate steps; SHOT_LEAVEOUT=camera,pose,speed,tick leaves those out (measurement, issue 20)
STEPS = {
    "camera": "  CameraManager:SetActiveCamera('default')",
    "pose": ("  local cam = FindObject('_default_camera')\n"
             "  cam.position = Vector(@PX@, @PY@, @PZ@) cam.rotation = Vector(@PITCH@, @YAW@, 0)"),
    "speed": "  MS.SetGameSpeed(0.001, 0)",
    "tick": ("  local pos, rot = Vector(@PX@, @PY@, @PZ@), Vector(@PITCH@, @YAW@, 0)\n"
             "  RegisterTick(function() local c = FindObject('_default_camera') if c then c.position = pos c.rotation = rot end end)"),
}
LEAVEOUT = [x for x in os.environ.get("SHOT_LEAVEOUT", "").split(",") if x]
HIDE = """  local t = FindObject('Condor')
  if t then
    pcall(MS.CondorStopAll)
    local ok, e = pcall(MS.InvisibleEntity, t)
    L('shot trico hide ' .. tostring(ok) .. ' ' .. tostring(e))
  else
    L('shot trico hide none')
  end"""
SHOW = "  L('shot trico hide off (SHOT_TRICO=show): Trico stays visible and keeps moving')"
# SHOT_TRICO=show leaves out the "hide Trico" step (no CondorStopAll, no InvisibleEntity); every other step stays
TRICO = os.environ.get("SHOT_TRICO", "hide")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


class Screen:
    """Own Xvfb, so the game window is the only thing on it and the display number is known."""
    def __init__(self):
        if os.path.exists(f"/tmp/.X11-unix/X{DISPLAY[1:]}"):
            sys.exit(f"display {DISPLAY} is in use (set SHOT_DISPLAY)")
        self.proc = subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            if run(["xwininfo", "-root", "-display", DISPLAY]).returncode == 0:
                return
            time.sleep(0.1)
        sys.exit("Xvfb did not start")

    def close(self):
        self.proc.terminate()
        self.proc.wait(10)


def game_window():
    """The largest mapped top level window on the virtual screen (the game's)."""
    best = None
    out = run(["xwininfo", "-root", "-children", "-display", DISPLAY]).stdout
    for wid in re.findall(r"^\s+(0x[0-9a-f]+)", out, re.M):
        info = run(["xwininfo", "-id", wid, "-display", DISPLAY]).stdout
        if "IsViewable" not in info:
            continue
        w = int(re.search(r"Width: (\d+)", info)[1])
        h = int(re.search(r"Height: (\d+)", info)[1])
        if not best or w * h > best[1] * best[2]:
            best = (wid, w, h)
    return best


def picture(path):
    win = game_window()
    if not win:
        return "no window"
    r = run(["import", "-display", DISPLAY, "-window", win[0], path])
    return f"{win[1]}x{win[2]}" if r.returncode == 0 and os.path.exists(path) else "import failed: " + r.stderr.strip()


class MemoryGuard:
    """GPU memory guard. A run that filled the GPU memory once hung the game inside the kernel driver
    (unkillable, the host had to be power-cycled: issue 11). While the game runs, a thread reads the GTT use
    of every AMD card from sysfs every 0.25 s; above the limit it kills the whole process group of the game at
    once, before the driver can block. SHOT_GTT_LIMIT_GB (default 16, 0 = off) is the switch."""
    FILES = "/sys/class/drm/card[0-9]*/device/mem_info_gtt_used"

    def __init__(self):
        text = os.environ.get("SHOT_GTT_LIMIT_GB", "16")
        try:
            self.limit = float(text) * GB
        except ValueError:
            sys.exit(f"SHOT_GTT_LIMIT_GB={text!r} is not a number")
        self.files, self.unreadable = sorted(glob.glob(self.FILES)), set()
        if self.limit <= 0:
            print("GPU memory guard: OFF (SHOT_GTT_LIMIT_GB=0)", flush=True)
            self.files = []
        elif not self.files:
            print("GPU memory guard: NOT ACTIVE, no mem_info_gtt_used in sysfs (not an AMD card on Linux?)", flush=True)
        else:
            print(f"GPU memory guard: limit {self.limit / GB:.2f} GB GTT, cards: "
                  + " ".join(f.split("/")[4] for f in self.files), flush=True)

    @staticmethod
    def read(path):
        try:
            with open(path) as f:
                return int(f.read())
        except (OSError, ValueError):
            return None

    def use(self):
        """(highest GTT use, its VRAM use, card) over the cards, in bytes."""
        best = (0, 0, "")
        for f in self.files:
            gtt = self.read(f)
            if gtt is None:  # an unreadable file is said once, never counted as 0 silently
                if f not in self.unreadable:
                    self.unreadable.add(f)
                    print(f"  GPU memory guard: cannot read {f}; that card is not guarded", flush=True)
                continue
            if gtt >= best[0]:
                best = (gtt, self.read(f.replace("gtt_used", "vram_used")) or 0, f.split("/")[4])
        return best

    def before(self):
        """Before the game starts: the start value. Returns a failure reason when the limit is already passed."""
        self.fired, self.peak, self.stop, self.thread = "", (0, 0, ""), threading.Event(), None
        if not self.files:
            return ""
        gtt, vram, card = self.use()
        print(f"  GPU memory at start: GTT {gtt / GB:.2f} GB, VRAM {vram / GB:.2f} GB ({card})", flush=True)
        if gtt > self.limit:
            return self.trip(gtt, card, "before the game started; not started")
        return ""

    def watch(self, proc):
        self.proc = proc
        if self.files:
            self.thread = threading.Thread(target=self.loop, daemon=True)
            self.thread.start()

    def loop(self):
        while not self.stop.is_set():
            gtt, vram, card = self.use()
            if gtt > self.peak[0]:
                self.peak = (gtt, vram, card)
            if gtt > self.limit:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.trip(gtt, card, "game killed")
                return
            self.stop.wait(0.25)

    def trip(self, gtt, card, what):
        self.fired = f"GPU memory guard: {gtt / GB:.2f} GB used, limit {self.limit / GB:.2f} GB ({card}, {what})"
        print("  " + self.fired, flush=True)
        line = f"{time.strftime('%F %T')} MAJOR: shot.py stopped a take because the GPU memory passed the guard ({self.fired})."
        if os.path.isfile(JOURNAL):
            with open(JOURNAL, "a") as f:
                f.write(line + "\n")
        else:
            print(f"  (no journal at {JOURNAL}: {line})", flush=True)
        return self.fired

    def end(self):
        if not self.files:
            return
        self.stop.set()
        if self.thread:
            self.thread.join(5)
        gtt, vram, card = self.peak
        print(f"  GPU memory peak during the take: GTT {gtt / GB:.2f} GB, VRAM {vram / GB:.2f} GB ({card})", flush=True)


class Game:
    def __init__(self, env, log_path, exec_path):
        self.log_path, self.seen = log_path, []
        # no TLG_ setting of the calling shell reaches the game: only the fixed ones below and NAME=VALUE
        base = {k: v for k, v in os.environ.items() if not k.startswith("TLG_")}
        full = dict(base, DISPLAY=DISPLAY, MESA_VK_WSI_DEBUG="sw", TLG_USER_DIR=USER, TLG_SKIP_LOGOS="1",
                    TLG_FPS="unlocked", TLG_HDR="0", TLG_FG="0", TLG_DLAA="1", TLG_LUA_EXEC=exec_path)
        full.update(env)
        self.log = open(log_path, "w")
        self.proc = subprocess.Popen([sys.executable, os.path.join(PORT, "launcher", "tlg_run.py")], cwd=PORT,
                                     env=full, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)

    def lines(self):
        with open(self.log_path, errors="replace") as f:
            self.seen = f.read().splitlines()
        return self.seen

    def wait(self, seconds, until=lambda ls: False):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if until(self.lines()):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def find(self, text, start=0):
        return next((l for l in self.lines()[start:] if text in l), "")

    def close(self):
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(self.proc.pid, sig)
            except ProcessLookupError:
                break
            try:
                self.proc.wait(10)
                break
            except subprocess.TimeoutExpired:
                pass
        self.log.close()
        if not gone():
            raise SystemExit("the game did not exit after the take; stopped")


def gone(seconds=20):
    """True once no game process is left (a killed game takes a moment to go)."""
    end = time.monotonic() + seconds
    while run(["pgrep", "-x", "tlg-probe"]).returncode == 0:
        if time.monotonic() > end:
            return False
        time.sleep(0.5)
    return True


def take(guard, out, tag, stage, scene, env, cam_file, cam_arg):
    exec_path = os.path.join(out, "exec.lua")

    def send(text):
        with open(exec_path + ".new", "w", newline="\n") as f:
            f.write(text + f"-- {time.time()}\n")
        os.replace(exec_path + ".new", exec_path)  # the game never reads a half written file
    send("-- idle\n")
    why = guard.before()
    if why:
        return why
    game = Game(env, os.path.join(out, tag + ".log"), exec_path)
    guard.watch(game.proc)
    try:
        status = steps(game, send, out, tag, stage, scene, env, cam_file, cam_arg)
        code = game.proc.poll()
        if code is not None and not guard.fired:  # it ended by itself, before this script closed it
            print(f"  game ended by itself before the take was done (exit code {code})", flush=True)
    finally:
        try:
            game.close()
        finally:
            guard.end()
    return guard.fired or status


def steps(game, send, out, tag, stage, scene, env, cam_file, cam_arg):
    if not game.wait(TITLE_S, lambda ls: any("opA_010 titleScreen" in l for l in ls)):
        return "no title" if game.proc.poll() is None else "ended before title"
    # the renderer that really runs: the new engine says so in its log, the normal renderer does not
    new_engine = any(l.startswith("Renderer: engine-to-Vulkan backend") for l in game.seen)
    if new_engine != (env["TLG_VKB"] == "1"):
        return f"wrong renderer: TLG_VKB={env['TLG_VKB']} but the new engine {'ran' if new_engine else 'did not run'}"
    game.wait(1)
    send(WARP.replace("@STAGE@", stage).replace("@SCENE@", scene))
    if not game.wait(20, lambda ls: any("Lua: shot retarget" in l or "Lua: shot warp error" in l for l in ls)):
        return "no continue"
    seen = next(i for i, l in enumerate(game.seen) if "Lua: shot retarget" in l or "shot warp error" in l)
    if "error" in game.seen[seen]:
        return game.seen[seen]

    def loaded(ls):
        new = ls[seen:]
        i = next((j for j, l in enumerate(new) if "blocking overlay started" in l), None)
        return i is not None and any("blocking overlay ended" in l for l in new[i:])
    if not game.wait(LOAD_S, loaded):
        return "no load" if game.proc.poll() is None else "ended during load"
    game.wait(WAIT_S)
    if PLAY_S > 0:
        print(f"  playing {PLAY_S:.0f} s before the photo (SHOT_PLAY_S)", flush=True)
        game.wait(PLAY_S)
        if game.proc.poll() is not None:
            return "ended while playing"
    cam = cam_arg or (open(cam_file).read().split() if os.path.exists(cam_file) else None)
    if not cam:
        mark = len(game.lines())
        send(CAM)
        game.wait(5, lambda ls: any("Lua: shot cam" in l for l in ls[mark:]))
        m = re.search(r"shot cam (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)", game.find("Lua: shot cam", mark))
        if not m:
            return "no camera position: " + game.find("Lua: shot cam", mark)
        cam = list(m.groups())
        with open(cam_file, "w") as f:
            f.write(" ".join(cam) + "\n")
        print(f"  camera taken from the game in this take: {' '.join(cam)}", flush=True)
    lua = PHOTO.replace("@TRICO@", SHOW if TRICO == "show" else HIDE)
    for k, v in STEPS.items():
        lua = lua.replace("@" + k.upper() + "@", f"  L('shot step {k} left out (SHOT_LEAVEOUT)')" if k in LEAVEOUT else v)
    for k, v in zip(("PX", "PY", "PZ", "PITCH", "YAW"), cam):
        lua = lua.replace(f"@{k}@", v)
    mark = len(game.lines())
    send(lua)
    game.wait(5, lambda ls: any("Lua: shot photo" in l for l in ls[mark:]))
    for text in ("Lua: shot trico hide", "Lua: shot photo"):
        print("  " + (game.find(text, mark)[5:] or f"no '{text}' line"), flush=True)
    if "photo on" not in game.find("Lua: shot photo", mark):
        return "photo setup failed"
    game.wait(SETTLE_S)
    png = os.path.join(out, tag + ".png")
    size = picture(png)
    counters(env, out, tag)
    if size.startswith(("no", "import")):
        return "picture failed: " + size
    # a blank picture (black screen) can never be compared: the take fails and the picture is set aside
    why = compare.blank(compare.rgb(png)[2])
    if why:
        os.replace(png, os.path.join(out, tag + ".blank.png"))
        return f"picture failed: {why} (kept as {tag}.blank.png)"
    faults = sum(1 for l in game.lines() if "Fault: exception" in l)
    return f"ok {size}, faults {faults}"


def counters(env, out, tag):
    """With TLG_DEBUG_SERVER_PORT set (NAME=VALUE), the game's counters at the moment of the picture are kept as
    TAG.stats.json and TAG.native.json (the debug server's /stats and /native). A failed read is said, not hidden."""
    port = env.get("TLG_DEBUG_SERVER_PORT", "0")
    if not port.isdigit() or int(port) <= 0:  # the game starts its server only for a port above 0
        return
    for name in ("stats", "native"):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/{name}", timeout=5) as r:
                data = r.read()
            with open(os.path.join(out, f"{tag}.{name}.json"), "wb") as f:
                f.write(data)
        except (OSError, ValueError, http.client.HTTPException) as e:
            print(f"  counters: /{name} not read: {e}", flush=True)


def main():
    args = [a for a in sys.argv[1:]]
    cam_arg = None
    if "--cam" in args:
        i = args.index("--cam")
        cam_arg = args[i + 1].split()
        del args[i:i + 2]
    if len(args) < 5 or args[3] not in ("0", "1"):
        sys.exit(__doc__)
    root, stage, scene, vkb, takes = args[0], args[1], args[2], args[3], int(args[4])
    if any(x not in STEPS for x in LEAVEOUT):
        sys.exit(f"SHOT_LEAVEOUT={LEAVEOUT}: use a list of {', '.join(STEPS)}")
    if TRICO not in ("hide", "show"):
        sys.exit(f"SHOT_TRICO={TRICO}: use hide (the default) or show")
    env = dict(e.split("=", 1) for e in args[5:])
    env["TLG_VKB"] = vkb
    if "TLG_USER_DIR" in env:
        sys.exit("set TLG_USER_DIR in the shell, not as NAME=VALUE (the script backs up the save in it)")
    if not gone():
        sys.exit("the game is running")
    out = os.path.join(root, f"{stage}__{scene}")
    os.makedirs(out, exist_ok=True)
    save = os.path.join(USER, "savedata", "1", "CUSA03745.memory")
    backup = os.path.join(root, "save_backup")
    if os.path.exists(backup):
        sys.exit(f"{backup} is left from a run that did not finish: put it back by hand, then delete it")
    guard = MemoryGuard()
    screen = Screen()
    if os.path.exists(save):
        shutil.copytree(save, backup)
    failed = 0
    try:
        n = 0
        for _ in range(takes):
            n += 1
            while any(os.path.exists(os.path.join(out, f"vkb{vkb}_take{n}.{e}")) for e in ("png", "blank.png", "log")):  # never overwrite
                n += 1
            tag = f"vkb{vkb}_take{n}"
            shutil.rmtree(save, ignore_errors=True)
            shutil.copytree(START, save)
            t = time.monotonic()
            status = take(guard, out, tag, stage, scene, env, os.path.join(out, "camera.txt"), cam_arg)
            print(f"{tag}: {status}  ({time.monotonic() - t:.0f} s)", flush=True)
            failed += not status.startswith("ok ")
            if guard.fired:
                print(f"remaining takes not started: {guard.fired}", flush=True)
                break
    finally:
        screen.close()
        shutil.rmtree(save, ignore_errors=True)
        if os.path.exists(backup):
            shutil.copytree(backup, save)
            shutil.rmtree(backup)
            print("save restored", flush=True)
        else:
            print("no save before the run: start save removed", flush=True)
    sys.exit(1 if failed else 0)


main()
