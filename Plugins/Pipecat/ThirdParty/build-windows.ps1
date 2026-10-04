#
# Copyright (c) 2026, Daily
#
# Builds the Pipecat C++ client and its Daily and WebSocket transports with
# Visual Studio's compiler, which Unreal Engine uses on Windows, and installs
# them in ThirdParty\Win64 for the Pipecat plugin, with nlohmann/json's
# headers and Daily's Core SDK (its DLL, in bin). They use Unreal's libcurl and
# OpenSSL, and download and build what else they need. The Daily Core SDK is
# downloaded from Daily's releases on GitHub into ThirdParty\daily, once.
#
# Usage:
#   $env:UE_ROOT = "C:\path\to\UnrealEngine"
#   $env:PIPECAT_CLIENT_CXX = "C:\path\to\pipecat-client-cxx"
#   .\build-windows.ps1
#

$ErrorActionPreference = "Stop"

if (-not $env:UE_ROOT) { throw "Set UE_ROOT to your Unreal Engine directory" }
if (-not $env:PIPECAT_CLIENT_CXX) { throw "Set PIPECAT_CLIENT_CXX to the pipecat-client-cxx source" }

$Here = $PSScriptRoot
$Build = Join-Path $Here "build\Win64"
$Prefix = Join-Path $Here "Win64"

# The Daily Core SDK the Daily transport is built with.
$DailyCoreVersion = "0.23.0"
$DailyCoreName = "daily-core-sdk-$DailyCoreVersion-windows-x86_64"
$DailyCore = Join-Path $Here "daily\$DailyCoreName"

# Runs a program, and stops if it fails.
function Invoke-Program {
    $Program, $Arguments = $args
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed" }
}

# CMake wants forward slashes.
function ConvertTo-CMakePath($Path) { $Path.Replace("\", "/") }

# The compiler, CMake and Ninja come from Visual Studio, unless this is already
# one of its developer prompts.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) { throw "Install Visual Studio, with its C++ tools" }
    $VisualStudio = & $VsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VisualStudio) { throw "Install Visual Studio's C++ tools" }
    $VcVars = Join-Path $VisualStudio "VC\Auxiliary\Build\vcvars64.bat"
    cmd /c "`"$VcVars`" > nul 2>&1 && set" | ForEach-Object {
        if ($_ -match "^([^=]+)=(.*)$") { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
}
foreach ($Program in "cl", "cmake", "ninja", "git") {
    if (-not (Get-Command "$Program.exe" -ErrorAction SilentlyContinue)) { throw "Unable to find $Program" }
}

# Unreal's libcurl and OpenSSL, which the Pipecat plugin links through Unreal.
# Finds a library in the newest version of one of them that has it for Win64.
function Find-UnrealLibrary($Package, $Library) {
    $Versions = Join-Path $env:UE_ROOT "Engine\Source\ThirdParty\$Package"
    if (-not (Test-Path $Versions)) { throw "Unable to find $Versions" }
    $Newest = Get-ChildItem $Versions -Directory | Sort-Object -Descending {
        [regex]::Replace($_.Name, "\d+", { $args[0].Value.PadLeft(8, "0") })
    }
    foreach ($Version in $Newest) {
        $Found = Get-ChildItem (Join-Path $Version.FullName "lib\Win64") -Recurse -Filter $Library `
            -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch "\\(Debug|arm64)\\" } | Select-Object -First 1
        if ($Found) { return @{ Version = $Version.FullName; Library = $Found.FullName } }
    }
    throw "Unable to find $Library in $Versions"
}

$Curl = Find-UnrealLibrary "libcurl" "libcurl.lib"
$OpenSsl = Find-UnrealLibrary "OpenSSL" "libssl.lib"
$OpenSslCrypto = Join-Path (Split-Path $OpenSsl.Library) "libcrypto.lib"
$OpenSslHeader = Get-ChildItem (Join-Path $OpenSsl.Version "include\Win64") -Recurse -Filter "opensslv.h" |
    Select-Object -First 1
if (-not $OpenSslHeader) { throw "Unable to find OpenSSL's headers in $($OpenSsl.Version)" }
$OpenSslInclude = $OpenSslHeader.Directory.Parent.FullName

if (-not (Test-Path (Join-Path $DailyCore "include\daily_core.h"))) {
    Write-Host "*** Downloading the Daily Core SDK $DailyCoreVersion ***"
    $Zip = Join-Path $Here "daily\$DailyCoreName.zip"
    New-Item -ItemType Directory -Force (Split-Path $Zip) | Out-Null
    Invoke-WebRequest -UseBasicParsing -OutFile $Zip `
        "https://github.com/daily-co/daily-core-sdk/releases/download/v$DailyCoreVersion/$DailyCoreName.zip"
    Expand-Archive -Force $Zip (Split-Path $Zip)
}

if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }

Write-Host "*** Building the Pipecat C++ client and its Daily and WebSocket transports ***"
# Unreal always links the release C runtime, as a DLL, which is CMake's default
# for a release build. Its libcurl is a static library.
Invoke-Program cmake -S $env:PIPECAT_CLIENT_CXX -B "$Build\pipecat" -G Ninja `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DPIPECAT_BUILD_WEBSOCKET=ON" `
    "-DPIPECAT_BUILD_DAILY=ON" `
    "-DDailyCore_ROOT=$(ConvertTo-CMakePath $DailyCore)" `
    "-DPIPECAT_BUILD_TESTS=OFF" `
    "-DCURL_NO_CURL_CMAKE=ON" `
    "-DCURL_USE_STATIC_LIBS=ON" `
    "-DCURL_INCLUDE_DIR=$(ConvertTo-CMakePath (Join-Path $Curl.Version 'include'))" `
    "-DCURL_LIBRARY=$(ConvertTo-CMakePath $Curl.Library)" `
    "-DOPENSSL_INCLUDE_DIR=$(ConvertTo-CMakePath $OpenSslInclude)" `
    "-DSSL_EAY_RELEASE=$(ConvertTo-CMakePath $OpenSsl.Library)" `
    "-DLIB_EAY_RELEASE=$(ConvertTo-CMakePath $OpenSslCrypto)"
Invoke-Program cmake --build "$Build\pipecat"
Invoke-Program cmake --install "$Build\pipecat" --prefix $Prefix

# The Pipecat headers include nlohmann/json. Unreal doesn't have it, so the
# client's build downloads it.
if (-not (Test-Path "$Prefix\include\nlohmann")) {
    Copy-Item -Recurse "$Build\pipecat\_deps\nlohmann_json-src\include\nlohmann" "$Prefix\include"
}

# Daily's Core SDK: its import library, linked with the plugin, and its DLL,
# which goes next to the plugin's.
New-Item -ItemType Directory -Force "$Prefix\bin" | Out-Null
Copy-Item (Join-Path $DailyCore "lib\daily_core.dll.lib") "$Prefix\lib"
Copy-Item (Join-Path $DailyCore "bin\daily_core.dll") "$Prefix\bin"

Write-Host "*** Installed in $Prefix ***"
Get-ChildItem "$Prefix\lib" -Filter "*.lib" | ForEach-Object { $_.Name }
