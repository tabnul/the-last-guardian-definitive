# Builds the port in the machine's MSYS2 CLANG64 (msys2_root in paths.local.json), as build.sh does on Windows, with two fixes for a Windows checkout:
# the FSR patches are applied with this shell's git (MSYS2's git fails on CRLF files), and the exes are copied to port\out (where the launcher looks).
#   build.ps1 [-Pgo use|off] [ninja targets...]     PGO: use when pgo\tlggpu.profdata exists (the release setting), else off
# Logs: port\out\gpu-config.log, port\out\gpu-build.log. Prints "ninja exit=<n>".
param([string]$Pgo = "", [Parameter(ValueFromRemainingArguments = $true)][string[]]$Targets)
. "$PSScriptRoot\dev_paths.ps1"
foreach ($patch in Get-ChildItem "$Port\gpu\patches\fsr-vulkan\*.patch") {
    git -C "$Port\gpu\third_party\fsr-vulkan" apply --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -ne 0) {
        git -C "$Port\gpu\third_party\fsr-vulkan" apply $patch.FullName
        if ($LASTEXITCODE -ne 0) { "FAIL: FSR patch $($patch.Name)"; exit 1 }
    }
}
if (-not $Pgo) { $Pgo = if (Test-Path "$Port\pgo\tlggpu.profdata") { 'use' } else { 'off' } }
if (-not $Targets) { $Targets = @('tlg-probe', 'tlg-gpu-capabilities', 'tlg-play', 'tlg-pad-tool', 'tlg-music-tool') }
$bash = Join-Path $Cfg.msys2_root 'usr\bin\bash.exe'
$portUnix = '/' + $Port.Substring(0, 1).ToLower() + ($Port.Substring(2) -replace '\\', '/')
$env:MSYSTEM = 'CLANG64'; $env:CHERE_INVOKING = '1'
$cmd = "export PATH=/clang64/bin:`$PATH; cd '$portUnix' && mkdir -p out && cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTLG_PGO=$Pgo '-DTLG_PGO_DIR=$portUnix/pgo' -DTLG_LTO=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > out/gpu-config.log 2>&1 && ninja -C out/gpu $($Targets -join ' ') > out/gpu-build.log 2>&1; echo ninja exit=`$?"
$result = & $bash -lc $cmd
Remove-Item Env:MSYSTEM, Env:CHERE_INVOKING -ErrorAction SilentlyContinue
$result
if ($result -match 'ninja exit=0') {
    foreach ($t in $Targets) { if (Test-Path "$Out\gpu\$t.exe") { Copy-Item "$Out\gpu\$t.exe" "$Out\$t.exe" -Force } }
}
