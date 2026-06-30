# Fetches the vendored dependencies into third_party/.
# Run this once after cloning the repo, and any time a dependency version changes.
#
# Versions are pinned by tag. Bumping a version means: change the tag here,
# delete the matching third_party/<name> directory, and run this script again.

[CmdletBinding()]
param(
    [switch] $Force,
    [switch] $Quiet
)

$ErrorActionPreference = "Stop"

# Git writes its progress to stderr; PowerShell, when it sees stderr output from
# a native command, treats it as a non-terminating error and (with strict modes)
# can flip $? to false even on a clean exit. Suppress that behaviour for the
# duration of this script.
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-Git {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $Args)
    & git @Args
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Args -join ' ') failed with exit $LASTEXITCODE"
    }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ThirdParty = Join-Path $RepoRoot "third_party"

if (-not (Test-Path $ThirdParty)) {
    New-Item -ItemType Directory -Path $ThirdParty | Out-Null
}

function Write-Step([string] $msg) {
    if (-not $Quiet) {
        Write-Host "[third_party] $msg" -ForegroundColor Cyan
    }
}

function Ensure-GitClone {
    param(
        [string] $Name,
        [string] $Url,
        [string] $Tag,
        [switch] $Shallow
    )

    $target = Join-Path $ThirdParty $Name

    if ((Test-Path $target) -and -not $Force) {
        Write-Step "$Name already present, skipping."
        return
    }

    if ($Force -and (Test-Path $target)) {
        Write-Step "removing existing $Name"
        Remove-Item -Recurse -Force $target
    }

    Write-Step "cloning $Name@$Tag"
    if ($Shallow) {
        Invoke-Git clone --depth 1 --branch $Tag $Url $target
    } else {
        Invoke-Git clone $Url $target
        Push-Location $target
        try {
            Invoke-Git checkout $Tag
        } finally {
            Pop-Location
        }
    }
}

function Ensure-SingleFile {
    param(
        [string] $Name,
        [string] $Url,
        [string] $RelPath
    )

    $target = Join-Path $ThirdParty (Join-Path $Name $RelPath)
    $dir = Split-Path -Parent $target

    if ((Test-Path $target) -and -not $Force) {
        Write-Step "$Name/$RelPath already present, skipping."
        return
    }

    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }

    Write-Step "downloading $Name/$RelPath"
    Invoke-WebRequest -Uri $Url -OutFile $target -UseBasicParsing
}

Ensure-GitClone -Name "imgui" `
    -Url "https://github.com/ocornut/imgui.git" `
    -Tag "docking" `
    -Shallow

Ensure-GitClone -Name "mbedtls" `
    -Url "https://github.com/Mbed-TLS/mbedtls.git" `
    -Tag "v3.6.2" `
    -Shallow

Ensure-GitClone -Name "spdlog" `
    -Url "https://github.com/gabime/spdlog.git" `
    -Tag "v1.14.1" `
    -Shallow

Ensure-GitClone -Name "doctest" `
    -Url "https://github.com/doctest/doctest.git" `
    -Tag "v2.4.11" `
    -Shallow

Ensure-GitClone -Name "utfcpp" `
    -Url "https://github.com/nemtrif/utfcpp.git" `
    -Tag "v4.0.5" `
    -Shallow

Ensure-SingleFile -Name "nlohmann_json" `
    -Url "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" `
    -RelPath "single_include/nlohmann/json.hpp"

Ensure-SingleFile -Name "stb" `
    -Url "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" `
    -RelPath "stb_image.h"

Ensure-SingleFile -Name "stb" `
    -Url "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h" `
    -RelPath "stb_image_write.h"

# puff: Mark Adler's tiny DEFLATE decompressor (zlib license), used to inflate
# gzip-compressed CMsgMulti payloads from the Steam CM.
Ensure-SingleFile -Name "puff" `
    -Url "https://raw.githubusercontent.com/madler/zlib/v1.3.1/contrib/puff/puff.c" `
    -RelPath "puff.c"

Ensure-SingleFile -Name "puff" `
    -Url "https://raw.githubusercontent.com/madler/zlib/v1.3.1/contrib/puff/puff.h" `
    -RelPath "puff.h"

Ensure-GitClone -Name "minhook" `
    -Url "https://github.com/TsudaKageyu/minhook.git" `
    -Tag "v1.3.3" `
    -Shallow

Ensure-SingleFile -Name "xorstr" `
    -Url "https://raw.githubusercontent.com/JustasMasiulis/xorstr/master/include/xorstr.hpp" `
    -RelPath "xorstr.hpp"

Write-Step "done"
