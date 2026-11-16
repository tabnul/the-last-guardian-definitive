# Copies a named save backup over the live save slot (hash checked); the game must be closed. The game autosaves, so run it before each run of a scene.
#   restore_save.ps1 heaviest        (a key of "saves" in paths.local.json, or a backup folder name)
param([Parameter(Mandatory = $true)][string]$Name)
. "$PSScriptRoot\dev_paths.ps1"
if (Test-GameRunning) { Write-Output "the game is running"; exit 1 }
if (Restore-Save $Name) { Write-Output "save '$Name' restored ($Live)" } else { exit 1 }
