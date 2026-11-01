<#
.SYNOPSIS
    Builds and packages the game for one or more platforms.

.DESCRIPTION
    For each target: configures the "package-<target>" CMake preset, builds it
    in Release and runs the kes_package target. Packages land in out/package/.

      windows  -> out/package/<name>-windows-<version>.zip
      gdk      -> out/package/<name>-gdk-msixvc/*.msixvc   (needs the Microsoft GDK)

    The external binaries are downloaded first if slangc is missing.

.PARAMETER Targets
    Platforms to package: windows, gdk, all (default: windows).

.PARAMETER Generator
    Override the preset's CMake generator, e.g. "Visual Studio 17 2022".

.EXAMPLE
    ./package.ps1
    ./package.ps1 windows gdk
    ./package.ps1 all -Generator "Visual Studio 17 2022"
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Targets = @('windows'),
    [string]$Generator
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Known = @('windows', 'gdk')

$list = @()
foreach ($t in $Targets) {
    foreach ($part in ($t -split ',')) {
        $name = $part.Trim().ToLowerInvariant()
        if (-not $name) { continue }
        if ($name -eq 'all') { $list += $Known; continue }
        if ($Known -notcontains $name) { throw "Unknown target '$name'. Known: $($Known -join ', '), all" }
        $list += $name
    }
}
$list = $list | Select-Object -Unique

if (-not (Test-Path (Join-Path $Root 'external/slang/win64/bin/slangc.exe'))) {
    Write-Host "== Downloading external binaries"
    & (Join-Path $Root 'external/download_external.ps1')
}

foreach ($target in $list) {
    $preset = "package-$target"
    Write-Host ""
    Write-Host "== Packaging $target (preset $preset)"

    $configureArgs = @('--preset', $preset)
    if ($Generator) { $configureArgs += @('-G', $Generator) }
    & cmake -S $Root @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "Configure failed for $target" }

    Push-Location $Root
    try {
        & cmake --build --preset $preset --parallel
        if ($LASTEXITCODE -ne 0) { throw "Build/package failed for $target" }
    }
    finally {
        Pop-Location
    }
}

Write-Host ""
Write-Host "== Done. Packages are in $(Join-Path $Root 'out/package')"
