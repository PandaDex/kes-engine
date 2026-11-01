<#
.SYNOPSIS
    Downloads and unpacks the prebuilt external binaries (slang, FBX2glTF).

.DESCRIPTION
    The version comes from the "external_version" file next to this script
    (shared with download_external.sh). The archive is fetched from:
        https://github.com/PandaDex/kes-engine/releases/download/external-v<version>/external.zip
    Each top-level folder in the archive replaces the folder of the same name in
    external/. Folders that are not in the archive (SDL3, imgui, cgltf) are left alone.

.PARAMETER Force
    Reinstall even if the installed version already matches.

.PARAMETER Zip
    Install from a local external.zip instead of downloading (e.g. to test a
    new archive before uploading the release).

.EXAMPLE
    ./external/download_external.ps1
    ./external/download_external.ps1 -Force
    ./external/download_external.ps1 -Zip C:\path\to\external.zip
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$Zip
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar

$Repo = 'PandaDex/kes-engine'
$AssetName = 'external.zip'
$ExternalDir = $PSScriptRoot
$VersionFile = Join-Path $ExternalDir 'external_version'
$StampFile = Join-Path $ExternalDir '.external_installed'

if (-not (Test-Path $VersionFile)) {
    throw "Version file not found: $VersionFile"
}
$Version = (Get-Content -Raw $VersionFile).Trim()
if ([string]::IsNullOrWhiteSpace($Version)) {
    throw "Version file is empty: $VersionFile"
}

$Url = "https://github.com/$Repo/releases/download/external-v$Version/$AssetName"

if (-not $Force -and -not $Zip -and (Test-Path $StampFile)) {
    $installed = (Get-Content -Raw $StampFile).Trim()
    if ($installed -eq $Version) {
        Write-Host "External binaries v$Version already installed. Use -Force to reinstall."
        exit 0
    }
}

$TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("kes-external-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $TempDir | Out-Null

try {
    if ($Zip) {
        $ArchivePath = (Resolve-Path $Zip).Path
        Write-Host "Using local archive $ArchivePath"
    }
    else {
        $ArchivePath = Join-Path $TempDir $AssetName
        Write-Host "Downloading external binaries v$Version"
        # Windows PowerShell 5.1 defaults to old TLS versions that GitHub rejects.
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Write-Host "  $Url"
        try {
            Invoke-WebRequest -Uri $Url -OutFile $ArchivePath -UseBasicParsing
        }
        catch {
            throw "Download failed ($Url): $($_.Exception.Message)`nCheck that release external-v$Version exists and has $AssetName."
        }
    }

    $ExtractDir = Join-Path $TempDir 'extract'
    Write-Host "Unpacking..."
    Expand-Archive -Path $ArchivePath -DestinationPath $ExtractDir -Force

    $entries = Get-ChildItem -Path $ExtractDir
    if (-not $entries) {
        throw "Archive is empty: $ArchivePath"
    }

    foreach ($entry in $entries) {
        $target = Join-Path $ExternalDir $entry.Name
        if (Test-Path $target) {
            Remove-Item -Recurse -Force $target
        }
        Move-Item -Path $entry.FullName -Destination $target
        Write-Host "  installed external/$($entry.Name)"
    }

    if ($Zip) {
        Write-Host "Installed from local archive (version stamp not written)."
        if (Test-Path $StampFile) { Remove-Item -Force $StampFile }
    }
    else {
        Set-Content -Path $StampFile -Value $Version -NoNewline
        Write-Host "External binaries v$Version installed."
    }
}
finally {
    if (Test-Path $TempDir) {
        Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
    }
}
