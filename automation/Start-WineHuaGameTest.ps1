[CmdletBinding()]
param(
    [ValidateSet('dxvk_legacy', 'wined3d')]
    [string]$D3DBackend = 'dxvk_legacy',
    [ValidateSet('baseline', 'direct-fence-wait', 'no-remote-sync', 'no-dynamic-flush', 'fence-feedback', 'shadow-none', 'shadow-trace', 'shadow-to-host-explicit', 'shadow-precise', 'shadow-precise-single-ring', 'shadow-precise-sync-submit', 'shadow-precise-strong-ring', 'shadow-precise-direct-fence', 'shadow-precise-retain-shmem')]
    [string]$PerfProfile = 'shadow-precise-strong-ring',
    [string]$GamePath = '',
    [string]$DeviceId = '5KPBB25818203996'
)

$ErrorActionPreference = 'Stop'
$hdc = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
$bundle = 'app.hackeris.winehua'
if (-not (Test-Path -LiteralPath $hdc)) { throw "Windows HDC not found: $hdc" }

& $hdc -t $DeviceId shell aa force-stop $bundle | Out-Null
if ($GamePath) {
    # hdc shell treats backslashes as escape characters. Win32 accepts forward
    # slashes in drive paths, and this preserves the Want parameter verbatim.
    $wantGamePath = $GamePath.Replace('\', '/')
    $output = & $hdc -t $DeviceId shell aa start -a EntryAbility -b $bundle `
        --ps winehua.mode game --ps winehua.d3d_backend $D3DBackend `
        --ps winehua.perf_profile $PerfProfile --ps winehua.game_path $wantGamePath
} else {
    $output = & $hdc -t $DeviceId shell aa start -a EntryAbility -b $bundle `
        --ps winehua.d3d_backend $D3DBackend `
        --ps winehua.perf_profile $PerfProfile
}
if ($LASTEXITCODE -ne 0) { throw "WineHua start failed: $($output -join ' ')" }

Write-Host "WineHua desktop requested with D3D backend: $D3DBackend"
Write-Host "Performance profile: $PerfProfile"
if ($GamePath) {
    Write-Host "Launching game through WineHua: $GamePath"
} else {
    Write-Host "Launch the DX11 game from the Wine desktop Explorer; its managed DXVK overlay is inherited by the game."
}
