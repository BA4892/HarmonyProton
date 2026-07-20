[CmdletBinding()]
param(
    [ValidateSet('dxvk_legacy', 'wined3d')]
    [string]$D3DBackend = 'dxvk_legacy',
    [string]$GamePath = '',
    [string]$DeviceId = '5KPBB25818203996'
)

$ErrorActionPreference = 'Stop'
$hdc = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
$bundle = 'app.hackeris.winehua'
if (-not (Test-Path -LiteralPath $hdc)) { throw "Windows HDC not found: $hdc" }

& $hdc -t $DeviceId shell aa force-stop $bundle | Out-Null
if ($GamePath) {
    $output = & $hdc -t $DeviceId shell aa start -a EntryAbility -b $bundle `
        --ps winehua.mode game --ps winehua.d3d_backend $D3DBackend --ps winehua.game_path $GamePath
} else {
    $output = & $hdc -t $DeviceId shell aa start -a EntryAbility -b $bundle `
        --ps winehua.d3d_backend $D3DBackend
}
if ($LASTEXITCODE -ne 0) { throw "WineHua start failed: $($output -join ' ')" }

Write-Host "WineHua desktop requested with D3D backend: $D3DBackend"
if ($GamePath) {
    Write-Host "Launching game through WineHua: $GamePath"
} else {
    Write-Host "Launch the DX11 game from the Wine desktop Explorer; its managed DXVK overlay is inherited by the game."
}
