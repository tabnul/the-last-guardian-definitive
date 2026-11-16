param([string]$Tag = "ep", [string]$Endpoints = "status", [string]$Extra = "", [int]$Presents = 700, [int]$Settle = 8, [int]$Between = 0, [string]$Save = "spears")
# As vkb_run.ps1, but each endpoint file gets its position: <Tag>_<endpoint>_<n>.json (two snapshots of the same endpoint). For vkb_gpu_frame.py:
#   vkb_run_snapshots.ps1 -Tag g0 -Endpoints "status,stats,sleep,status,stats" -Between 5 -Presents 2300 -Settle 6 -Extra "TLG_VKB=1,..."
. "$PSScriptRoot\dev_paths.ps1"
Stop-Game
Start-Sleep 2
if (-not (Restore-Save $Save)) { exit 1 }
New-Item -ItemType Directory -Force $Runs | Out-Null
& "$DevDir\play_start.ps1" -Headless -Measure -Tag $Tag -Extra $Extra | Out-Null
$deadline = (Get-Date).AddSeconds(300); $ok = $false
while ((Get-Date) -lt $deadline) {
    try { $s = Invoke-RestMethod "$Base/status" -TimeoutSec 2; if ($s.presents -ge $Presents) { $ok = $true; break } } catch { }
    Start-Sleep 1
}
if (-not $ok) { "FAIL: $Presents presents not reached"; Stop-Game; exit 1 }
Start-Sleep $Settle
$i = 0
foreach ($e in $Endpoints.Split(',')) {
    $i++
    if ($e -eq "sleep") { Start-Sleep $Between; continue }
    Invoke-WebRequest "$Base/$e" -UseBasicParsing -TimeoutSec 60 | Select-Object -ExpandProperty Content | Out-File "$Runs\${Tag}_$($e.Split('?')[0])_$i.json" -Encoding utf8
}
$fps = (Invoke-RestMethod "$Base/status").fps_5s
Stop-Game
"fps $fps"
