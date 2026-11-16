# The wall ledger (phase 0.1 of the 120 fps master plan, 2026-11-08): per scene, restore the named save (hash checked), start the game headless, wait for gameplay, sample the frame rate for 20 s,
# read the GPU clock and utilisation, capture an 8 s timeline, save /stats, stop the game, print the ledger.
#
#   wall_ledger.ps1 -Scenes spears,furthest,bloom -Tag base [-Extra "TLG_X=1,TLG_Y=2"]
#   wall_ledger.ps1 -Scenes live -Tag now          (no save restore: the live save as it is)
#   -Repeat 3: every scene is run three times (tags <Tag>r1_<scene> ...) and ledger_<Tag>_<scene>.json holds the median of each number plus the three frame times (ledger_median.py)
#
# BEFORE running with a restore: back the live save up in its own command and compare the hashes (its own command, then compare the hashes). The script restores nothing at the end;
# pass -RestoreLive <path to the verified backup memory.dat> to copy that file back (hash checked) after the last scene.
# Output: user\log\runs\ledger_<Tag>_<scene>.json (one ledger per scene) and the same text on screen. A run takes about 2.5 minutes per scene.
#   -PlayEnv: start with the owner's play set (adds TLG_FRAME_STORE=1). Default since 2026-11-09 is the measuring set (play_start.ps1 -Measure: the code defaults);
#   ledgers before 2026-11-09 ran with the play set (frame store on) and with thread CPU taken from the capture, so they are not directly comparable.
param([string[]]$Scenes = @("live"), [string]$Tag = "ledger", [string]$Extra = "", [string]$RestoreLive = "", [int]$Repeat = 1, [switch]$PlayEnv)
. "$PSScriptRoot\dev_paths.ps1"
$live = $Live; $out = $Runs; $base = $Base
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($scene in $Scenes) {
    for ($rep = 1; $rep -le $Repeat; $rep++) {
        $t = if ($Repeat -gt 1) { "${Tag}r${rep}_$scene" } else { "${Tag}_$scene" }
        if ($scene -ne "live") {
            Stop-Game
            Start-Sleep 2
            if (-not (Restore-Save $scene)) { "FAIL: save restore for $scene"; continue }
        }
        & "$DevDir\play_start.ps1" -Headless -Measure:(-not $PlayEnv) -Tag $t -Extra $Extra | Out-Null
        $deadline = (Get-Date).AddSeconds(200); $ok = $false
        while ((Get-Date) -lt $deadline) {
            try { $s = Invoke-RestMethod "$base/status" -TimeoutSec 2; if ($s.presents -ge 2200) { $ok = $true; break } } catch { }
            Start-Sleep 1
        }
        if (-not $ok) { "FAIL ${t}: 2200 presents not reached"; Stop-Game; continue }
        Start-Sleep 3
        $samples = @()
        # Thread CPU over exactly the 20 s sample (review M3), read by Windows from outside the game: cumulative thread times at the start and the end of the
        # sample, names from /timeline (thread id -> name). The game's own /threads endpoint gave percents in steps that fit a window under one second
        # (2026-11-09, m3base), so it is kept only as threads_endpoint for comparison.
        $names = @{}; try { (Invoke-RestMethod "$base/timeline?n=1").threads.PSObject.Properties | ForEach-Object { $names[[int]$_.Name] = $_.Value } } catch { }
        $gp = Get-Process -Name "tlg-probe" -ErrorAction SilentlyContinue | Select-Object -First 1
        $c0 = @{}; if ($gp) { $gp.Refresh(); foreach ($th in $gp.Threads) { try { $c0[$th.Id] = $th.TotalProcessorTime.TotalMilliseconds } catch { } } }
        Invoke-RestMethod "$base/threads" | Out-Null
        $p0 = (Invoke-RestMethod "$base/status").presents; $t0 = Get-Date
        for ($i = 0; $i -lt 20; $i++) { Start-Sleep 1; $samples += (Invoke-RestMethod "$base/status").fps_1s }
        $p1 = (Invoke-RestMethod "$base/status").presents; $t1 = Get-Date
        $endpoint = Invoke-RestMethod "$base/threads"
        $rows = @(); $tot = 0.0; $wall = ($t1 - $t0).TotalMilliseconds
        if ($gp) { $gp.Refresh(); foreach ($th in $gp.Threads) { try { if ($c0.ContainsKey($th.Id)) { $pct = 100.0 * ($th.TotalProcessorTime.TotalMilliseconds - $c0[$th.Id]) / $wall; $tot += $pct
            $nm = if ($names.ContainsKey($th.Id)) { $names[$th.Id] } else { "tid$($th.Id)" }; if ($pct -ge 1.0) { $rows += [ordered]@{ name = $nm; tid = $th.Id; percent = [math]::Round($pct, 2) } } } } catch { } } }
        [ordered]@{ note = "percent of one core over the 20 s sample, from Windows thread times (wall_ledger.ps1)"; wall_ms = [math]::Round($wall, 0); total_percent = [math]::Round($tot, 1)
            threads = @($rows | Sort-Object { $_.percent } -Descending); threads_endpoint = $endpoint } | ConvertTo-Json -Depth 6 | Out-File "$out\${t}_threads.json" -Encoding utf8
        $smi = (nvidia-smi --query-gpu=clocks.gr,power.draw,utilization.gpu --format=csv,noheader) -join "; "
        Invoke-WebRequest "$base/timeline_capture?seconds=8&tag=$t" -UseBasicParsing -TimeoutSec 30 | Out-Null
        Start-Sleep 12
        Invoke-WebRequest "$base/stats" -UseBasicParsing -TimeoutSec 30 | Select-Object -ExpandProperty Content | Out-File "$out\${t}_stats.json" -Encoding utf8
        Stop-Game
        $pps = ($p1 - $p0) / ($t1 - $t0).TotalSeconds
        $text = & python -I "$DevDir\wall_ledger.py" $t (($samples | ForEach-Object { [math]::Round($_, 1) }) -join " ") ([string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:N3}", $pps)) $smi
        $text | Out-File "$out\ledger_$t.json" -Encoding utf8
        "=== $t"; $text
        Start-Sleep 3
    }
    if ($Repeat -gt 1) {
        $med = & python -I "$DevDir\ledger_median.py" $Tag $scene $Repeat
        $med | Out-File "$out\ledger_${Tag}_$scene.json" -Encoding utf8
        "=== ${Tag}_$scene (median of $Repeat)"; $med
    }
}
if ($RestoreLive -ne "") {
    Stop-Game
    Start-Sleep 2
    Copy-Item $RestoreLive $live -Force
    if ((Get-FileHash $RestoreLive).Hash -eq (Get-FileHash $live).Hash) { "live save restored from $RestoreLive (hash verified)" } else { "FAIL: restore hash mismatch" }
}
