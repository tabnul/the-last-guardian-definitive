# Starts the game from this repo through the launcher's runner (port\launcher\tlg_run.py; TLG_* variables set here win over the launcher's settings),
# with the debug server on 7777. Stops a running game first.
#
#   play_start.ps1                                   owner play, window, fps counter
#   play_start.ps1 -Extra "TLG_VKB=1"                the same with switches (comma separated NAME=VALUE list)
#   play_start.ps1 -Headless -Measure                no window, starts from the live save by itself; the measuring set (the code defaults, no play-set diagnostics)
#
# PLAY = the measuring set plus TLG_FRAME_STORE=1. The environment is set for the child only and removed again here, so the next call starts clean.
# Output of the game: <user>\log\game_<Tag>.txt and .err.
param([string]$Extra = "", [switch]$Headless, [string]$Tag = "play", [switch]$Measure)
. "$PSScriptRoot\dev_paths.ps1"
Stop-Game
Start-Sleep 3
$e = [ordered]@{
    TLG_DEBUG_SERVER_PORT = $DebugPort; TLG_GAME_DIR = $Cfg.game_dir
    TLG_FPS = "120"; TLG_VBLANK_HZ = "120"; TLG_ENGINE_FPS = "120"
    TLG_GARLIC_POOL_PRO = "1"; TLG_DMEM_MB = "8192"; TLG_GARLIC_POOL_MB = "4096"
    TLG_FRAMES_IN_FLIGHT = "1"   # depth 1
}
if (-not $Measure) { $e["TLG_FRAME_STORE"] = "1" }   # play set only
if ($Headless) { $e["TLG_HEADLESS"] = "1"; $e["TLG_AUTO_START"] = "1" } else { $e["TLG_SHOW_FPS"] = "1" }
foreach ($kv in $Extra.Split(",")) { if ($kv.Contains("=")) { $p = $kv.Split("=", 2); $e[$p[0].Trim()] = $p[1].Trim() } }
foreach ($k in @($e.Keys)) { Set-Item "Env:$k" $e[$k] }
$log = Join-Path $User 'log'
New-Item -ItemType Directory -Force $log | Out-Null
$proc = Start-Process -FilePath python -ArgumentList "`"$Port\launcher\tlg_run.py`"" -WorkingDirectory $Port -RedirectStandardOutput "$log\game_$Tag.txt" -RedirectStandardError "$log\game_$Tag.err" -PassThru
Start-Sleep 5
foreach ($k in @($e.Keys)) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
"started pid $($proc.Id), switches: $($Extra)"
