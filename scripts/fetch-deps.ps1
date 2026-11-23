# Downloads what is not in git into third_party/:
#   fidelityfx/sdk   AMD FidelityFX SDK 2.4 (source + AMD signed DLLs)
#   ffmpeg           FFmpeg 9.0 shared build with d3d12va (BtbN), skipped when FLUVI_FFMPEG_DIR is set
# Archives are cached in FLUVI_DEPS_CACHE, or third_party/.cache by default.
# vcpkg packages come from the manifest at configure time.
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$cache = if ($env:FLUVI_DEPS_CACHE) { $env:FLUVI_DEPS_CACHE } else { Join-Path $root 'third_party\.cache' }
New-Item -ItemType Directory -Force $cache | Out-Null

function Get-Archive([string]$url, [string]$name) {
    $zip = Join-Path $cache $name
    if (-not (Test-Path $zip)) { Invoke-WebRequest $url -OutFile $zip }
    return $zip
}

$ffx = Join-Path $root 'third_party\fidelityfx'
if (-not (Test-Path (Join-Path $ffx 'sdk'))) {
    $zip = Get-Archive 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip' 'FidelityFX-SDK-v2.4.0.zip'
    New-Item -ItemType Directory -Force $ffx | Out-Null
    Expand-Archive $zip -DestinationPath $ffx
    Rename-Item (Join-Path $ffx 'FidelityFX-SDK-2.4.0') 'sdk'
}
Write-Host "FidelityFX SDK: $ffx\sdk"

if ($env:FLUVI_FFMPEG_DIR) {
    Write-Host "FFmpeg: using FLUVI_FFMPEG_DIR=$env:FLUVI_FFMPEG_DIR"
} else {
    $ffmpeg = Join-Path $root 'third_party\ffmpeg'
    if (-not (Test-Path $ffmpeg)) {
        $name = 'ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip'
        $zip = Get-Archive "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$name" $name
        Expand-Archive $zip -DestinationPath $ffmpeg
    }
    Write-Host "FFmpeg: $ffmpeg"
}
