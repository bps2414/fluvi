# Downloads what is not in git: the FidelityFX SDK 2.4 (source + AMD signed DLLs) into third_party/fidelityfx/sdk.
# The archive is cached in D:\Bryan\Dev\Caches\fluvi-deps. vcpkg packages come from the manifest at configure time.
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$cache = 'D:\Bryan\Dev\Caches\fluvi-deps'
$zip   = Join-Path $cache 'FidelityFX-SDK-v2.4.0.zip'
$dest  = Join-Path $root 'third_party\fidelityfx'
New-Item -ItemType Directory -Force $cache, $dest | Out-Null
if (-not (Test-Path $zip)) {
    Invoke-WebRequest 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip' -OutFile $zip
}
if (-not (Test-Path (Join-Path $dest 'sdk'))) {
    Expand-Archive $zip -DestinationPath $dest
    Rename-Item (Join-Path $dest 'FidelityFX-SDK-2.4.0') 'sdk'
}
Write-Host "FidelityFX SDK: $dest\sdk"
