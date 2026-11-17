#!/usr/bin/env python3
"""One comparable picture of one scene, for one renderer. Linux, virtual screen (Xvfb). No AI inside.

Recipe per take: fresh start (clean save from scene-check/start.save) -> title -> retarget CONTINUE to
STAGE/SCENE -> wait until the load overlay ended -> 8 s -> hide Trico -> own camera at a fixed position,
set every tick -> game speed 0.001 -> 4 s -> picture of the game window on the virtual screen.

Usage: shot.py OUT STAGE SCENE VKB TAKES [--cam "X Y Z PITCH YAW"] [NAME=VALUE ...]
  OUT   folder for the pictures and logs (outside the repo). Pictures: OUT/STAGE__SCENE/vkbV_takeN.png
  VKB   0 = normal renderer, 1 = new engine (always set explicitly)
The fixed camera is taken from the game's own camera in the first take ever made of that scene and kept in
OUT/STAGE__SCENE/camera.txt; every later take, of either renderer, uses the same one. --cam overrides it.
Run with the build environment loaded (source /home/runner/loop/build-env on the test box)."""
import os, re, shutil, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(os.path.dirname(HERE))
START = os.path.join(PORT, "tools", "scene-check", "start.save")
USER = os.environ.get("TLG_USER_DIR", os.path.expanduser("~/tlgrun/user"))
DISPLAY = os.environ.get("SHOT_DISPLAY", ":93")
TITLE_S, LOAD_S, WAIT_S, SETTLE_S = 400, 240, 8, 4

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
  local t = FindObject('Condor')
  if t then
    pcall(MS.CondorStopAll)
    local ok, e = pcall(MS.InvisibleEntity, t)
    L('shot trico hide ' .. tostring(ok) .. ' ' .. tostring(e))
  else
    L('shot trico hide none')
  end
  CameraManager:SetActiveCamera('default')
  local cam = FindObject('_default_camera')
  local pos, rot = Vector(@PX@, @PY@, @PZ@), Vector(@PITCH@, @YAW@, 0)
  cam.position = pos cam.rotation = rot
  MS.SetGameSpeed(0.001, 0)
  RegisterTick(function() local c = FindObject('_default_camera') if c then c.position = pos c.rotation = rot end end)
  L('shot photo on cam=' .. tostring(FindObject('_world').activeCamera:GetName()) .. ' speed=' .. tostring(MS.GetGameSpeed()))
end)
if not ok then L('shot photo error: ' .. tostring(e)) end
"""


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


def take(out, tag, stage, scene, env, cam_file, cam_arg):
    exec_path = os.path.join(out, "exec.lua")

    def send(text):
        with open(exec_path + ".new", "w", newline="\n") as f:
            f.write(text + f"-- {time.time()}\n")
        os.replace(exec_path + ".new", exec_path)  # the game never reads a half written file
    send("-- idle\n")
    game = Game(env, os.path.join(out, tag + ".log"), exec_path)
    try:
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
        lua = PHOTO
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
        size = picture(os.path.join(out, tag + ".png"))
        faults = sum(1 for l in game.lines() if "Fault: exception" in l)
        return f"ok {size}, faults {faults}" if not size.startswith(("no", "import")) else "picture failed: " + size
    finally:
        game.close()


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
    screen = Screen()
    if os.path.exists(save):
        shutil.copytree(save, backup)
    failed = 0
    try:
        n = 0
        for _ in range(takes):
            n += 1
            while any(os.path.exists(os.path.join(out, f"vkb{vkb}_take{n}.{e}")) for e in ("png", "log")):  # never overwrite
                n += 1
            tag = f"vkb{vkb}_take{n}"
            shutil.rmtree(save, ignore_errors=True)
            shutil.copytree(START, save)
            t = time.monotonic()
            status = take(out, tag, stage, scene, env, os.path.join(out, "camera.txt"), cam_arg)
            print(f"{tag}: {status}  ({time.monotonic() - t:.0f} s)", flush=True)
            failed += not status.startswith("ok ")
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
