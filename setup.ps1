#
# Copyright (c) 2026, Daily
#
# Copies the content of Unreal Engine's Third Person template (the player's
# character, the mannequins and their animations, and the input setup) into
# Content\, and creates the game's materials with Scripts\create_content.py.
# The project doesn't include them, since every engine install has the
# template. It creates the materials in the editor, so build the game first.
#
# Usage:
#   $env:UE_ROOT = "C:\Program Files\Epic Games\UE_5.8"
#   .\setup.ps1
#

$ErrorActionPreference = "Stop"

if (-not $env:UE_ROOT) { throw "Set UE_ROOT to your Unreal Engine directory" }

$Here = $PSScriptRoot
$Templates = Join-Path $env:UE_ROOT "Templates"

if (-not (Test-Path "$Here\Binaries\Win64\UnrealEditor-PipecatRoom.dll")) {
    Write-Host "Build the game first, e.g.:"
    Write-Host "  & `"$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat`" PipecatRoomEditor Win64 Development -Project=`"$Here\PipecatRoom.uproject`""
    exit 1
}

New-Item -ItemType Directory -Force "$Here\Content" | Out-Null
Copy-Item -Recurse -Force "$Templates\TP_ThirdPersonBP\Content\*" "$Here\Content"
# The template's shared packs, each mounted in its own folder, e.g. /Game/Input.
foreach ($Pack in "LevelPrototyping", "Characters", "Input") {
    New-Item -ItemType Directory -Force "$Here\Content\$Pack" | Out-Null
    Copy-Item -Recurse -Force "$Templates\TemplateResources\High\$Pack\Content\*" "$Here\Content\$Pack"
}
# The engine's templates can be read-only, and the editor needs to save them.
Get-ChildItem "$Here\Content" -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }

Write-Host "Copied the Third Person template content into $Here\Content"

& "$env:UE_ROOT\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$Here\PipecatRoom.uproject" `
    -run=pythonscript "-script=$Here\Scripts\create_content.py" `
    -unattended -nosplash -nullrhi | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Unable to create the game's materials, see $Here\Saved\Logs\PipecatRoom.log"
    exit 1
}
Write-Host "Created the game's materials in $Here\Content\Room"
