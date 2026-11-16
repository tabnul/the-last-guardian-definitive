param([string]$Tag = "ep", [string]$Endpoints = "status", [string]$Extra = "", [int]$Presents = 700, [int]$Settle = 8, [int]$Between = 0, [switch]$Windowed, [switch]$Keep, [string]$Save = "spears", [int]$Wait = 300)
# Headless run with the debug server: stops the game, copies the named save into the live slot (hash checked), starts play_start.ps1 -Headless -Measure,
# waits for -Presents, settles, reads each comma-separated debug-server endpoint into <user>\log\runs\<Tag>_<endpoint>.json ('sleep' waits -Between seconds),
# prints fps_5s, stops the game.
#   -Windowed: a real window (the presenter runs; /marker?mode=1 saves frame_presented.png in the marker's folder); -Keep: the game keeps running at the end
#   -Save: a key of "saves" in paths.local.json (spears, heaviest, ...) or a backup folder name
# Heaviest save: read the scene at 3,200 presents or later (its load stalls; engine-vulkan-backend note 78), e.g. -Presents 2300 -Settle 6 -Endpoints "sleep,status,..." -Between 6.
#   vkb_run.ps1 -Tag t1 -Save heaviest -Presents 2300 -Settle 6 -Endpoints "sleep,status,stats,marker?mode=1,sleep" -Between 6 -Extra "TLG_VKB=1"
. "$PSScriptRoot\dev_paths.ps1"
Stop-Game
Start-Sleep 2
if (-not (Restore-Save $Save)) { exit 1 }
New-Item -ItemType Directory -Force $Runs | Out-Null
if ($Windowed) {
    $x = if ($Extra) { "$Extra,TLG_AUTO_START=1" } else { "TLG_AUTO_START=1" }
    & "$DevDir\play_start.ps1" -Measure -Tag $Tag -Extra $x | Out-Null
} else {
    & "$DevDir\play_start.ps1" -Headless -Measure -Tag $Tag -Extra $Extra | Out-Null
}
$deadline = (Get-Date).AddSeconds($Wait); $ok = $false
while ((Get-Date) -lt $deadline) {
    try { $s = Invoke-RestMethod "$Base/status" -TimeoutSec 2; if ($s.presents -ge $Presents) { $ok = $true; break } } catch { }
    # the game ended (crash or exit): stop now instead of waiting out -Wait
    if (((Get-Date) - $deadline.AddSeconds(-$Wait)).TotalSeconds -gt 30 -and -not (Test-GameRunning)) { "FAIL: the game ended"; exit 1 }
    Start-Sleep 1
}
if (-not $ok) { "FAIL: $Presents presents not reached"; Stop-Game; exit 1 }
Start-Sleep $Settle
$seen = @()
foreach ($e in $Endpoints.Split(',')) {
    if ($e -eq "sleep") { Start-Sleep $Between; continue }
    # an endpoint read twice (e.g. "stats,sleep,stats") gets _2 on its second file
    $f = "$Runs\${Tag}_$($e.Split('?')[0]).json"
    if ($seen -contains $f) { $f = $f -replace '\.json$', '_2.json' }
    $seen += $f
    Invoke-WebRequest "$Base/$e" -UseBasicParsing -TimeoutSec 60 | Select-Object -ExpandProperty Content | Out-File $f -Encoding utf8
}
$fps = (Invoke-RestMethod "$Base/status").fps_5s
if (-not $Keep) { Stop-Game }
"fps $fps"
