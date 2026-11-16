# Dot-sourced by every dev tool here (. "$PSScriptRoot\dev_paths.ps1"): the folders of this repo, found from this file's place,
# and the machine's own paths from paths.local.json next to it (git-ignored; copy paths.example.json and fill it in).
#   $DevDir   port\tools\dev           $Port  port\          $Repo  the repository root
#   $User     the game's data folder (TLG_USER_DIR if set, else port\user, as the launcher's default)
#   $Out      port\out (tlg-probe.exe and the other built exes)   $Runs  $User\log\runs (tool output)
#   $Base     the debug server (http://127.0.0.1:<TLG_DEBUG_SERVER_PORT, default 7777>)
#   $Live     the save slot the game loads: $User\savedata\1\CUSA03627.memory\memory.dat
#   $Cfg      paths.local.json as an object; Get-SaveBackup <name>: memory.dat of a named backup (a key of saves, or a folder name)
$DevDir = $PSScriptRoot
$Port = (Resolve-Path (Join-Path $DevDir '..\..')).Path
$Repo = (Resolve-Path (Join-Path $Port '..')).Path
$User = if ($env:TLG_USER_DIR) { $env:TLG_USER_DIR } else { Join-Path $Port 'user' }
$Out = Join-Path $Port 'out'
$Runs = Join-Path $User 'log\runs'
$DebugPort = if ($env:TLG_DEBUG_SERVER_PORT) { $env:TLG_DEBUG_SERVER_PORT } else { '7777' }
$Base = "http://127.0.0.1:$DebugPort"
$Live = Join-Path $User 'savedata\1\CUSA03627.memory\memory.dat'
$cfgFile = Join-Path $DevDir 'paths.local.json'
if (-not (Test-Path $cfgFile)) { throw "dev tools: $cfgFile is missing (copy paths.example.json and fill it in)" }
$Cfg = Get-Content $cfgFile -Raw | ConvertFrom-Json

function Get-SaveBackup([string]$Name) {
    $folder = if ($Cfg.saves.PSObject.Properties.Name -contains $Name) { $Cfg.saves.$Name } else { $Name }
    Join-Path $Cfg.save_backups "$folder\home_1000_CUSA03627\sce_sdmemory\memory.dat"
}

# Copies a named save backup into the live slot and checks the hash (the game must be closed). False on a mismatch or a missing backup.
function Restore-Save([string]$Name) {
    $src = Get-SaveBackup $Name
    if (-not (Test-Path $src)) { Write-Output "FAIL: no save backup $src"; return $false }
    New-Item -ItemType Directory -Force (Split-Path $Live) | Out-Null
    Copy-Item $src $Live -Force
    if ((Get-FileHash $src).Hash -ne (Get-FileHash $Live).Hash) { Write-Output "FAIL: save hash"; return $false }
    return $true
}

function Stop-Game { Get-Process -Name 'tlg-probe' -ErrorAction SilentlyContinue | Stop-Process -Force }
function Test-GameRunning { [bool](Get-Process -Name 'tlg-probe' -ErrorAction SilentlyContinue) }
