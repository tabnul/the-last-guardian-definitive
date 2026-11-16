# Live view of the debug server of the tlg_pc port (our native renderer inside the port): frame time, the stages of the render pipeline, the cores and the native renderer, 4 times per second.
# Copy of threads.ps1 for the port: shadPS4 keeps using threads.ps1. Run with threads-pc-port.cmd (double-click) or: powershell -ExecutionPolicy Bypass -File threads-pc-port.ps1 [-Port 7777] [-TargetFps 120]
# Differences from threads.ps1: the port has no /pipeline measurement (frame time comes from /frames, the stage load from the CPU use of each thread), the bars are scaled to the target
# frame rate (auto: 120 when the port runs above 75 FPS, else 60), the settings line shows the port's switches, and the files are threads-pc-port.log and threads-pc-port.csv.
# Frame profile (F9 in the game, 10 s): when a capture ends, the older captures are deleted (only the newest timeline_f9_* stays in user/log), the graphs of the
# whole capture are written to profile-latest.html next to this script (documents/tlg/tools/profile_view.py, replacing the previous one) and opened in the browser.
# P in this window starts the same 10 s profile (for a headless game, which has no window to press F9 in).
param([int]$Port = 7777, [int]$TargetFps = 0)
$base = "http://127.0.0.1:$Port"
# A log line; another program holding the file for a moment (an editor, a second copy of this window, a virus scan) must not stop the view:
# a few tries, then the line is dropped (before: the failed write threw, the view showed 'waiting for the port' and printed a red error)
function Write-LogLine([string]$Path, [string]$Value, [string]$Encoding = 'utf8') {
    for ($i = 0; $i -lt 5; $i++) {
        try { Add-Content -Path $Path -Value $Value -Encoding $Encoding -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 20 }
    }
}
$log = Join-Path $PSScriptRoot 'threads.log'
$csv = Join-Path $PSScriptRoot 'threads.csv'
Set-Content -Path $log -Value ("{0} started" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding utf8
Set-Content -Path $csv -Value 'time,fps,ms_mean,ms_p95,ms_worst,render_pct,cp_pct,raster_pct,encoder_pct,gpu_pct,cores_busy,front_blocked_5s,raster_idle_ms_5s,draws_native_s,dispatch_native_s,async_native_s,pkt_skipped_buffers,pkt_skipped_packets,write_faults_s,spikes_over_20ms_60s,recorder_pct,completion_pct,gpu_ms_frame,gpu_draws_ms,gpu_dispatches_ms,gpu_copies_ms,vkb_draws_frame,vkb_dispatches_frame,flip_drain_ms' -Encoding utf8
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}

$full = [string][char]0x2588
$empty = [string][char]0x2591
$refreshMs = 250
$logicalProcessors = [Environment]::ProcessorCount
$stageNames = @('ps4render', 'GpuCommandProcessor', 'GpuRaster', 'GpuEncoder', 'PresentThread')
$script:lastLogSecond = ''
$script:lastCount = -1
$script:spikes = New-Object System.Collections.ArrayList   # {time, ms} of frames over 20 ms in the last 60 s
$script:nativePrev = $null; $script:nativePrevTime = $null; $script:nativeRates = $null
$script:readbackPrev = $null; $script:readbackPrevTime = $null; $script:readbackRates = $null
$script:faults = $null; $script:faultsTime = [DateTime]::MinValue
$script:readbackTime = [DateTime]::MinValue
$script:slowTime = [DateTime]::MinValue
$script:stats = $null; $script:policy = $null; $script:native = $null; $script:all = $null
$script:capture = $null; $script:profileDone = ''; $script:profileJob = $null; $script:profileState = ''
# the engine-to-Vulkan backend (TLG_VKB=1): its counters from /stats, as rates between two polls (1 s apart)
$script:vkbPrev = $null; $script:vkbPrevTime = $null; $script:vkb = $null
$profileTool = Join-Path $PSScriptRoot 'profile_view.py'
$profilePage = Join-Path $PSScriptRoot 'profile-latest.html'

function K([double]$v) { if ($v -ge 10000) { '{0,5:N1}k' -f ($v / 1000) } elseif ($v -ge 1000) { '{0,5:N2}k' -f ($v / 1000) } else { '{0,6:N0}' -f $v } }
function Bar([double]$pct, [int]$w) {
    $n = [int][math]::Max(0, [math]::Min($w, [math]::Round($pct / 100 * $w)))
    ($full * $n) + ($empty * ($w - $n))
}
function LoadColor([double]$pct) { if ($pct -ge 85) { 'Red' } elseif ($pct -ge 60) { 'Yellow' } else { 'Green' } }
function Line([string]$text, [string]$color = 'Gray') { [pscustomobject]@{ Text = $text; Color = $color } }
function Rate($now, $prev, [double]$dt) { if ($null -eq $prev -or $dt -le 0) { 0 } else { [math]::Max(0, ([double]$now - [double]$prev) / $dt) } }

# Terminal control with ANSI escape sequences (cursor home, erase to end of line / screen).
$esc = [string][char]27
try {
    Add-Type -Namespace Native -Name Vt -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int n);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool GetConsoleMode(System.IntPtr h, out int m);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool SetConsoleMode(System.IntPtr h, int m);
'@
    $hOut = [Native.Vt]::GetStdHandle(-11)
    $mode = 0
    if ([Native.Vt]::GetConsoleMode($hOut, [ref]$mode)) { [void][Native.Vt]::SetConsoleMode($hOut, $mode -bor 4) }
} catch {}
$script:colors = @{ Cyan = 36; DarkCyan = 36; Green = 32; Yellow = 33; Red = 31; Gray = 37; DarkGray = 90 }
$script:first = $true

function Show($lines) {
    $h = 0
    try { $h = [Console]::WindowHeight } catch {}
    if ($h -ge 10 -and $lines.Count -gt $h) { $lines = @($lines | Select-Object -First $h) }
    $sb = New-Object Text.StringBuilder
    if ($script:first) { [void]$sb.Append("$esc[?1049h$esc[?7l$esc[?25l$esc[2J"); $script:first = $false }
    $row = 1
    foreach ($l in $lines) {
        $code = $script:colors[$l.Color]
        if (-not $code) { $code = 37 }
        [void]$sb.Append("$esc[$row;1H$esc[${code}m").Append($l.Text).Append("$esc[0m$esc[K")
        $row++
    }
    [void]$sb.Append("$esc[$row;1H$esc[J")
    [Console]::Out.Write($sb.ToString())
}
function ThreadRows($threads) {
    foreach ($x in $threads) {
        $name = $x.name -replace '^shadPS4:', ''
        Line (" {0,-24} {1} {2,4:N0}%" -f $name, (Bar $x.percent 24), $x.percent) (LoadColor $x.percent)
    }
}
function Mean($values) { if ($values.Count -eq 0) { 0 } else { ($values | Measure-Object -Average).Average } }
function Quantile($values, [double]$q) {
    if ($values.Count -eq 0) { return 0 }
    $sorted = @($values | Sort-Object)
    $sorted[[int][math]::Floor(($sorted.Count - 1) * $q)]
}
function Poll([string]$path) { Invoke-RestMethod "$base$path" -TimeoutSec 2 }
function OnOff($v) { if ($v) { 'on' } else { 'off' } }
# One sample of the Vulkan backend's counters (cumulative), or $null when the backend is off (no draws recorded)
function VkbSample($stats, [double]$presents) {
    if (-not $stats -or -not $stats.vkb -or -not $stats.vkb.graphics) { return $null }
    $v = $stats.vkb
    if ([double]$v.graphics.draws -le 0 -and [double]$v.dispatches -le 0) { return $null }
    $stream = $v.compute.stream
    $w = $stream.gpu_ms_by_work
    $n = [double]$w.timed_submissions
    [pscustomobject]@{
        Presents = $presents; Draws = [double]$v.draws; Dispatches = [double]$v.dispatches; Async = [double]$v.async_dispatches
        Submissions = $n
        GpuDraws = [double]$w.whole_run.draws * $n; GpuDispatches = [double]$w.whole_run.dispatches * $n
        GpuCopies = [double]$w.whole_run.copies * $n; GpuClears = [double]$w.whole_run.clears * $n; GpuTotal = [double]$w.whole_run.total * $n
        Busy = [double]$w.last_window.gpu_busy_pct_of_the_window
        Drain = [double]$stream.recorder.drain_wait_ms_mean
        Splits = [double]$stream.recorder.split_submits
        Staging = $(if ($stream.staging_gpu) { [double]$stream.staging_gpu.ms_total } else { 0.0 })
    }
}


try {
    while ($true) {
        $nowTime = Get-Date
        $now = $nowTime.ToString('HH:mm:ss')
        try {
            while ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)
                if ($key.Key -eq 'P') {
                    $tag = 'f9_' + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
                    try { $script:capture = (Poll "/timeline_capture?seconds=10&tag=$tag").status; $script:profileState = '' } catch {}
                }
            }
        } catch {}
        try {
            $s = Poll '/status'
            $pipe = $null
            # the port has no /pipeline: frames come from /frames below

            # slower sources: once a second (counters, queue pressure, settings), every 2 s (readbacks, write faults)
            if (($nowTime - $script:slowTime).TotalSeconds -ge 1) {
                $script:slowTime = $nowTime
                try { $script:all = Poll '/threads' } catch {}
                try { $script:stats = Poll '/stats' } catch {}
                try {
                    $vs = VkbSample $script:stats ([double]$s.presents)
                    $t = [DateTime]::UtcNow
                    if (-not $vs) {
                        $script:vkb = $null; $script:vkbPrev = $null
                    } else {
                        $p = $script:vkbPrev
                        if ($p -and $vs.Presents -gt $p.Presents) {
                            $dt = ($t - $script:vkbPrevTime).TotalSeconds
                            $frames = $vs.Presents - $p.Presents
                            $script:vkb = [pscustomobject]@{
                                DrawsS = (Rate $vs.Draws $p.Draws $dt); DispatchesS = (Rate $vs.Dispatches $p.Dispatches $dt); AsyncS = (Rate $vs.Async $p.Async $dt)
                                DrawsF = ($vs.Draws - $p.Draws) / $frames; DispatchesF = (($vs.Dispatches - $p.Dispatches) + ($vs.Async - $p.Async)) / $frames
                                GpuF = ($vs.GpuTotal - $p.GpuTotal) / $frames; GpuDrawsF = ($vs.GpuDraws - $p.GpuDraws) / $frames
                                GpuDispatchesF = ($vs.GpuDispatches - $p.GpuDispatches) / $frames; GpuCopiesF = ($vs.GpuCopies - $p.GpuCopies) / $frames
                                GpuClearsF = ($vs.GpuClears - $p.GpuClears) / $frames
                                StagingF = ($vs.Staging - $p.Staging) / $frames
                                SubmissionsF = ($vs.Submissions - $p.Submissions) / $frames
                                Busy = $vs.Busy; Drain = $vs.Drain
                            }
                        }
                        $script:vkbPrev = $vs; $script:vkbPrevTime = $t
                    }
                } catch {}
                try { $script:policy = Poll '/policy' } catch {}
                try { $script:capture = (Poll '/timeline_capture').status } catch { $script:capture = $null }
                # a frame profile that just ended: keep only it, draw its graphs (in the background), open the page when done
                $cap = $script:capture
                if ($cap -and -not $cap.running -and $cap.last_file -and $cap.last_file -ne $script:profileDone -and -not $script:profileJob -and (Test-Path ($cap.last_file + '.bin'))) {
                    $script:profileDone = $cap.last_file
                    $logDir = Split-Path $cap.last_file -Parent
                    $keep = Split-Path $cap.last_file -Leaf
                    Get-ChildItem $logDir -Filter 'timeline_f9_*' -ErrorAction SilentlyContinue | Where-Object { $_.BaseName -ne $keep } | Remove-Item -Force -ErrorAction SilentlyContinue
                    $script:profileState = 'drawing the graphs of ' + $keep + ' ...'
                    $script:profileJob = Start-Job -ScriptBlock { param($tool, $base, $out) & python -I $tool $base $out 2>&1 } -ArgumentList $profileTool, $cap.last_file, $profilePage
                }
                if ($script:profileJob -and $script:profileJob.State -ne 'Running') {
                    $result = Receive-Job $script:profileJob -ErrorAction SilentlyContinue | Out-String
                    $ok = $script:profileJob.State -eq 'Completed' -and $result -match 'wrote'
                    Remove-Job $script:profileJob -Force -ErrorAction SilentlyContinue
                    $script:profileJob = $null
                    if ($ok) {
                        $script:profileState = 'profile ' + (Split-Path $script:profileDone -Leaf) + ': graphs in profile-latest.html (opened)'
                        try { Start-Process $profilePage } catch {}
                    } else {
                        $script:profileState = 'profile graphs failed: ' + ($result.Trim() -split "`n" | Select-Object -Last 1)
                    }
                }
                try {
                    $n = Poll '/native'
                    $t = [DateTime]::UtcNow
                    if ($script:nativePrev) {
                        $dt = ($t - $script:nativePrevTime).TotalSeconds
                        $p = $script:nativePrev
                        $script:nativeRates = [pscustomobject]@{
                            Gfx = (Rate $n.gfx_exec $p.gfx_exec $dt); Tags = (Rate $n.gfx_tags $p.gfx_tags $dt)
                            Comp = (Rate $n.compute_exec $p.compute_exec $dt); Asc = (Rate $n.asc_exec $p.asc_exec $dt)
                            Tracking = (Rate $n.track_native $p.track_native $dt)
                        }
                    }
                    $script:nativePrev = $n; $script:nativePrevTime = $t; $script:native = $n
                } catch {}
            }
            if (($nowTime - $script:readbackTime).TotalSeconds -ge 2) {
                $script:readbackTime = $nowTime
                try {
                    $r = Poll '/readbacks'
                    $t = [DateTime]::UtcNow
                    if ($script:readbackPrev) {
                        $dt = ($t - $script:readbackPrevTime).TotalSeconds
                        $script:readbackRates = [pscustomobject]@{ Copies = (Rate $r.explicit.copies $script:readbackPrev.explicit.copies $dt) }
                    }
                    $script:readbackPrev = $r; $script:readbackPrevTime = $t
                } catch {}
                try { $script:faults = Poll '/faults?top=1' } catch {}
            }

            # frame time and the stages from /pipeline (frames only exist while measure_pipeline is on)
            $stageOnlyThreads = $false
            if ($pipe) {
                $frames = @($pipe.frames)
            } else {
                # a build without /pipeline: the frame lengths come from /frames, the stage load from the CPU use of the stage threads
                $stageOnlyThreads = $true
                $frames = @()
                try { $frames = @((Poll '/frames?n=300').frames | ForEach-Object { [pscustomobject]@{ ms = [double]$_[1] } }) } catch {}
            }
            $haveFrames = $frames.Count -ge 5 -and ($pipe -or $script:all)
            if ($haveFrames) {
                $newCount = if ($pipe) { [int]$pipe.count } else { [int]$s.presents }
                if ($script:lastCount -ge 0 -and $newCount -gt $script:lastCount) {
                    $fresh = [math]::Min($newCount - $script:lastCount, $frames.Count)
                    foreach ($f in @($frames | Select-Object -Last $fresh)) {
                        if ($f.ms -gt 20) { [void]$script:spikes.Add([pscustomobject]@{ Time = $nowTime; Ms = $f.ms }) }
                    }
                }
                $script:lastCount = $newCount
                while ($script:spikes.Count -gt 0 -and ($nowTime - $script:spikes[0].Time).TotalSeconds -gt 60) { $script:spikes.RemoveAt(0) }
                $recent = @($frames | Select-Object -Last 60)
                $ms = @($frames | ForEach-Object { $_.ms })
                $msMean = Mean ($recent | ForEach-Object { $_.ms })
                $msP95 = Quantile $ms 0.95
                $msWorst = ($ms | Measure-Object -Maximum).Maximum
                if ($pipe) {
                    $busy = @(0..4 | ForEach-Object { $i = $_; Mean ($recent | ForEach-Object { $_.busy[$i] }) })
                    $gpu = Mean ($recent | ForEach-Object { $_.gpu })
                } else {
                    $stageNamesCpu = @('ps4render', 'shadPS4:GpuCommandProcessor', 'shadPS4:GpuRaster', 'shadPS4:GpuEncoder', 'shadPS4:PresentThread')
                    $busy = @($stageNamesCpu | ForEach-Object { $n = $_; $row = $script:all.threads | Where-Object { $_.name -eq $n } | Select-Object -First 1; if ($row) { [double]$row.percent } else { 0.0 } })
                    $gpu = -1
                }
            }

            $fps = [double]$s.fps_1s
            $target = if ($TargetFps -gt 0) { $TargetFps } elseif ($fps -gt 75) { 120 } else { 60 }
            $fpsColor = if ($fps -ge 0.92 * $target) { 'Green' } elseif ($fps -ge 0.66 * $target) { 'Yellow' } else { 'Red' }
            $up = [timespan]::FromSeconds([double]$s.uptime_s)
            $lines = @()
            $lines += Line (" PC port live view   {0}   target {3} FPS   uptime {1:hh\:mm\:ss}   frames {2}" -f $s.game_serial, $up, $s.presents, $target) 'Cyan'
            if ($haveFrames) {
                $lines += Line (" FPS {0,5:N1} {1}  frame {2:N1} ms   p95 {3:N1}   worst {4:N1}   (last 5 s)" -f $fps, (Bar ($fps / $target * 100) 20), $msMean, $msP95, $msWorst) $fpsColor
                $over20 = $script:spikes.Count
                $over33 = @($script:spikes | Where-Object { $_.Ms -gt 33 }).Count
                $lines += Line (" stutter, last 60 s:  {0} frames over 20 ms,  {1} over 33 ms" -f $over20, $over33) $(if ($over33 -gt 0) { 'Red' } elseif ($over20 -gt 6) { 'Yellow' } else { 'Gray' })
            } else {
                $lines += Line (" FPS {0,5:N1} {1}   5 s average {2:N1}" -f $fps, (Bar ($fps / $target * 100) 20), $s.fps_5s) $fpsColor
                $lines += Line ' (waiting for frames ...)' 'DarkGray'
            }
            if ($script:policy) {
                $pol = $script:policy
                $lines += Line (" settings: native mode {0} | packet path {1} | audit every {2} | strip {3} | readbacks override {4} | native map {5} | skip shadows {6}" -f
                        $pol.native_draw_mode, $(if ($pol.packet_path -eq 0) { 'off' } else { 'on' }), $pol.native_audit_every, $pol.native_sdk_strip,
                        $pol.readbacks_mode_override, $pol.map_native, $(if ($pol.skip_shadows -eq 0) { 'no' } else { $pol.skip_shadows })) 'DarkCyan'
            } else {
                $lines += Line ''
            }
            $lines += Line ''

            # 1 and 2: the stages of the pipeline and the GPU
            $vk = $script:vkb
            $vkbBusy = @{}
            if ($vk -and $script:all) {
                # the Vulkan backend: the engine's render thread, our recording and completion threads, the GPU
                $lines += Line ' PIPELINE (Vulkan backend): the game render thread calls our backend, the recording thread writes the command buffers, the GPU runs them' 'Cyan'
                $rows = @(@('game render thread', 'ps4render'), @('recording thread', 'vkb:Recorder'), @('completion thread', 'vkb:Completion'), @('present thread', 'tlg:Present'))
                foreach ($r in $rows) {
                    $row = $script:all.threads | Where-Object { $_.name -eq $r[1] } | Select-Object -First 1
                    $vkbBusy[$r[1]] = if ($row) { [double]$row.percent } else { 0.0 }
                }
                $lim = @{ 'game render thread' = $vkbBusy['ps4render']; 'recording thread' = $vkbBusy['vkb:Recorder']; 'GPU' = $vk.Busy }
                $limName = ($lim.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
                foreach ($r in $rows) {
                    $mark = if ($r[0] -eq $limName) { '  <- busiest' } else { '' }
                    $lines += Line (" {0,-20} {1} {2,4:N0}%{3}" -f $r[0], (Bar $vkbBusy[$r[1]] 24), $vkbBusy[$r[1]], $mark) (LoadColor $vkbBusy[$r[1]])
                }
                $mark = if ($limName -eq 'GPU') { '  <- busiest' } else { '' }
                $lines += Line (" {0,-20} {1} {2,4:N0}%{3}   (GPU time of our command buffers)" -f 'GPU busy', (Bar $vk.Busy 24), $vk.Busy, $mark) (LoadColor $vk.Busy)
                $lines += Line (" GPU ms a frame: {0:N2} = draws {1:N2} + dispatches {2:N2} + engine copies {3:N2} + clears {4:N2} + staging {5:N2}   ({6:N1} submissions a frame)" -f
                        ($vk.GpuF + $vk.StagingF), $vk.GpuDrawsF, $vk.GpuDispatchesF, $vk.GpuCopiesF, $vk.GpuClearsF, $vk.StagingF, $vk.SubmissionsF) 'Gray'
                $lines += Line (" the flip waits {0:N2} ms for the recording thread (mean over the run)" -f $vk.Drain) 'Gray'
            } elseif ($vk) {
                $lines += Line ' PIPELINE (Vulkan backend): (waiting for thread figures ...)' 'DarkGray'
            }
            if (-not $vk) { $lines += Line ' PIPELINE: the stages a draw goes through (busy, last second; 100% = one core)' 'Cyan' }
            if ($haveFrames -and -not $vk) {
                $limitIdx = 0; $limitVal = -1; $second = 0
                foreach ($i in 0..2) { if ($busy[$i] -gt $limitVal) { $limitVal = $busy[$i]; $limitIdx = $i } }
                foreach ($i in 0..2) { if ($i -ne $limitIdx -and $busy[$i] -gt $second) { $second = $busy[$i] } }
                $labels = @('game render thread', 'command processor', 'raster thread', 'encoder thread', 'present thread')
                foreach ($i in 0..3) {
                    $mark = if ($i -eq $limitIdx) { '  <- limit' } else { '' }
                    $lines += Line (" {0,-20} {1} {2,4:N0}%{3}" -f $labels[$i], (Bar $busy[$i] 24), $busy[$i], $mark) (LoadColor $busy[$i])
                }
                if ($gpu -ge 0) {
                    $lines += Line (" {0,-20} {1} {2,4:N0}%   (GPU time of the command buffers)" -f 'GPU busy', (Bar $gpu 24), $gpu) 'Cyan'
                } else {
                    $lines += Line (" {0,-20} (this build has no GPU timing; stage load above is the CPU use of each thread)" -f 'GPU busy') 'DarkGray'
                }
                $lines += Line (" limit: {0} {1:N0}%   (next stage {2:N0}%)" -f $labels[$limitIdx], $limitVal, $second) 'Gray'
            } elseif (-not $vk) {
                $lines += Line ' (no frames yet)' 'DarkGray'
            }
            $cp = $null
            if ($script:stats -and $script:stats.cp_split) { $cp = $script:stats.cp_split }
            if ($cp -and $null -ne $cp.front_blocked_on_full_queue) {
                $lines += Line (" queue pressure per 5 s: command processor blocked on a full raster queue {0} times, raster thread idle {1} ms" -f $cp.front_blocked_on_full_queue, $cp.raster_idle_ms) $(if ($cp.front_blocked_on_full_queue -gt 100) { 'Yellow' } else { 'Gray' })
            }
            $coresBusy = if ($script:all) { [double]$script:all.total_percent / 100 } else { 0 }
            $lines += Line (" cores: {0:N1} busy of {1} logical processors  {2}" -f $coresBusy, $logicalProcessors, (Bar (100 * $coresBusy / $logicalProcessors) 24)) (LoadColor (100 * $coresBusy / $logicalProcessors))
            $lines += Line ''

            # other threads
            $stage = @('shadPS4:GpuCommandProcessor', 'shadPS4:GpuRaster', 'shadPS4:GpuEncoder', 'shadPS4:PresentThread', 'ps4render')
            $emu = @($script:all.threads | Where-Object { $_.name -like 'shadPS4:*' -and $stage -notcontains $_.name } | Sort-Object percent -Descending | Select-Object -First 2)
            $game = @($script:all.threads | Where-Object { $_.name -like 'workerThread*' -or $_.name -like 'Havok*' -or $_.name -like 'sndx*' } | Sort-Object percent -Descending | Select-Object -First 4)
            $lines += Line ' OTHER THREADS (game workers, audio, emulator helpers)' 'Cyan'
            $lines += ThreadRows ($game + $emu)
            $lines += Line ''

            # native renderer, refreshed for the current design (records executed, packet path off by default)
            $n = $script:native; $nr = $script:nativeRates
            $skipB = 0; $skipP = 0
            if ($vk) {
                $lines += Line ' VULKAN BACKEND (the engine API straight to our Vulkan code)' 'Cyan'
                $lines += Line (" per second: draws {0}   dispatches {1}   async dispatches {2}" -f (K $vk.DrawsS), (K $vk.DispatchesS), (K $vk.AsyncS)) 'Green'
                $lines += Line (" per frame: {0:N0} draws, {1:N0} dispatches" -f $vk.DrawsF, $vk.DispatchesF) 'Gray'
            } elseif ($n) {
                $skipB = [int]$n.packet_path_skipped.buffers; $skipP = [int]$n.packet_path_skipped.packets
                $lines += Line ' NATIVE RENDERER' 'Cyan'
                if ($nr) {
                    $share = if ($nr.Tags -gt 0) { 100 * $nr.Gfx / $nr.Tags } else { 100 }
                    $lines += Line (" executed from records per second: draws {0}   dispatches {1}   async {2}   tracking {3}" -f (K $nr.Gfx), (K $nr.Comp), (K $nr.Asc), (K $nr.Tracking)) 'Green'
                    $lines += Line (" draws on the game's packets: {0}/s ({1:N1}% of tagged draws)" -f (K ([math]::Max(0, $nr.Tags - $nr.Gfx))), (100 - $share)) $(if ($share -lt 99) { 'Yellow' } else { 'Gray' })
                }
                $lines += Line (" packet path: {0} buffers, {1} packets not executed since start   mixed buffers {2}   entry mismatches {3}   built {4}" -f $skipB, $skipP, $n.direct_mixed, $n.direct_entry_mismatch, $n.direct_built) $(if ($n.direct_entry_mismatch -gt 0) { 'Red' } else { 'Gray' })
            }
            $rb = $script:readbackPrev
            if ($rb) {
                $cps = if ($script:readbackRates) { $script:readbackRates.Copies } else { 0 }
                $lines += Line (" explicit readbacks: {0}/s copies   gate waits {1}   read faults {2}   refused {3}" -f (K $cps), $rb.gate.waits, $rb.read_faults_total, ($rb.explicit.refused_too_large + $rb.explicit.refused_other_path)) $(if ($rb.read_faults_total -gt 0) { 'Yellow' } else { 'Gray' })
            }
            $wfs = 0; $wcores = 0.0
            if ($script:faults) {
                $wfs = [double]$script:faults.faults_per_second; $wcores = [double]$script:faults.handler_cores
                $lines += Line (" write faults: {0}/s   handler {1:N3} cores ({2:N1} us each)" -f (K $wfs), $wcores, $script:faults.handler_us_per_fault) 'Gray'
            }
            $lines += Line ''
            if ($script:capture -and $script:capture.running) {
                $lines += Line (" PROFILE: recording (F9 again stops it) ... {0:N0}% of the buffer" -f (100.0 * $script:capture.events / [math]::Max(1, $script:capture.capacity))) 'Yellow'
            } elseif ($script:profileState) {
                $lines += Line (" PROFILE: " + $script:profileState) 'Cyan'
            } else {
                $lines += Line ' PROFILE: press P here (or F9 in the game) for a 10 s frame profile; its graphs open when it ends' 'DarkGray'
            }
            $lines += Line ' the stage near 100% is the limit; the GPU waits for it    Ctrl+C to quit' 'DarkGray'
            Show $lines

            if ($now -ne $script:lastLogSecond -and $haveFrames) {
                $script:lastLogSecond = $now
                $spike = if (@($frames | Select-Object -Last 15 | Where-Object { $_.ms -gt 25 }).Count -gt 0) { ' SPIKE' } else { '' }
                $sp = $script:spikes.Count
                if ($vk) { $gpu = $vk.Busy }
                if ($gpu -lt 0) { $gpu = 0 }
                $rec = 0.0; $cmp = 0.0
                if ($vk -and $vkbBusy.Count -gt 0) { $rec = $vkbBusy['vkb:Recorder']; $cmp = $vkbBusy['vkb:Completion'] }
                $vkLog = if ($vk) { " | vkb: rec={0:F0} gpu_ms={1:F2} (draws {2:F2} disp {3:F2} copies {4:F2} staging {8:F2}) draws/f={5:F0} disp/f={6:F0} drain={7:F2}" -f $rec, $vk.GpuF, $vk.GpuDrawsF, $vk.GpuDispatchesF, $vk.GpuCopiesF, $vk.DrawsF, $vk.DispatchesF, $vk.Drain, $vk.StagingF } else { '' }
                Write-LogLine -Path $log -Value ("{0} fps={1:F1} ms={2:F1}/p95 {3:F1}/worst {4:F1} | render={5:F0} cp={6:F0} raster={7:F0} enc={8:F0} gpu={9:F0} cores={10:F1} | pktskip={11}/{12} wfault/s={13:F0} over20ms/60s={14}{15}{16}" -f
                        $now, $s.fps_1s, $msMean, $msP95, $msWorst, $busy[0], $busy[1], $busy[2], $busy[3], $gpu, $coresBusy, $skipB, $skipP, $wfs, $sp, $spike, $vkLog) -Encoding utf8
                $fb = if ($cp) { $cp.front_blocked_on_full_queue } else { 0 }; $ri = if ($cp) { $cp.raster_idle_ms } else { 0 }
                $g = if ($nr) { $nr.Gfx } else { 0 }; $c = if ($nr) { $nr.Comp } else { 0 }; $a = if ($nr) { $nr.Asc } else { 0 }
                Write-LogLine -Path $csv -Value ("{0},{1:F1},{2:F2},{3:F2},{4:F2},{5:F1},{6:F1},{7:F1},{8:F1},{9:F1},{10:F2},{11},{12},{13:F0},{14:F0},{15:F0},{16},{17},{18:F0},{19},{20:F1},{21:F1},{22:F2},{23:F2},{24:F2},{25:F2},{26:F0},{27:F0},{28:F2}" -f
                        $now, $s.fps_1s, $msMean, $msP95, $msWorst, $busy[0], $busy[1], $busy[2], $busy[3], $gpu, $coresBusy, $fb, $ri, $g, $c, $a, $skipB, $skipP, $wfs, $sp, $rec, $cmp, $(if ($vk) { $vk.GpuF } else { 0 }), $(if ($vk) { $vk.GpuDrawsF } else { 0 }), $(if ($vk) { $vk.GpuDispatchesF } else { 0 }), $(if ($vk) { $vk.GpuCopiesF } else { 0 }), $(if ($vk) { $vk.DrawsF } else { 0 }), $(if ($vk) { $vk.DispatchesF } else { 0 }), $(if ($vk) { $vk.Drain } else { 0 })) -Encoding utf8
            }
        } catch {
            Show @(
                (Line " PC port live view" 'Cyan'),
                (Line " waiting for the port on $base ... (start the game with the native overlay: debug_server_port 7777; Ctrl+C to quit)" 'Yellow')
            )
            Write-LogLine -Path $log -Value ("{0} no answer: {1}" -f $now, $_.Exception.Message) -Encoding utf8
            $script:lastCount = -1
        }
        Start-Sleep -Milliseconds $refreshMs
    }
} catch {
    Write-LogLine -Path $log -Value ("{0} script error: {1}" -f (Get-Date -Format 'HH:mm:ss'), $_) -Encoding utf8
    throw
} finally {
    try { [Console]::Out.Write("$esc[?25h$esc[?7h$esc[?1049l") } catch {}
}
