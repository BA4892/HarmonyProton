[CmdletBinding()]
param(
    [ValidateSet('dxvk_legacy', 'wined3d')]
    [string]$D3DBackend = 'dxvk_legacy',
    [ValidateSet('baseline', 'direct-fence-wait', 'no-remote-sync', 'no-dynamic-flush', 'fence-feedback', 'shadow-none', 'shadow-trace', 'shadow-to-host-explicit', 'shadow-precise', 'shadow-precise-single-ring', 'shadow-precise-sync-submit', 'shadow-precise-strong-ring', 'shadow-precise-strong-ring-async-present', 'shadow-precise-strong-ring-fence-poll', 'shadow-precise-strong-ring-mailbox', 'shadow-precise-direct-fence', 'shadow-precise-retain-shmem')]
    [string]$PerfProfile = 'shadow-precise-strong-ring',
    [ValidateSet('', 'heaven-dx11')]
    [string]$GamePreset = '',
    [string]$GamePath = '',
    [string[]]$GameArguments = @(),
    [hashtable]$D3DEnvironment = @{},
    [string]$ClickTitlePrefix = '',
    [string]$ClickButtonText = '',
    [ValidateRange(0, 30000)]
    [int]$ClickDelayMs = 1500,
    [ValidateRange(-1, 1000)]
    [int]$ClickClientXPermille = -1,
    [ValidateRange(-1, 1000)]
    [int]$ClickClientYPermille = -1,
    [string]$DeviceId = '5KPBB25818203996'
)

$ErrorActionPreference = 'Stop'
$hdc = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
$bundle = 'app.hackeris.winehua'
if (-not (Test-Path -LiteralPath $hdc)) { throw "Windows HDC not found: $hdc" }

if ($GamePreset -eq 'heaven-dx11') {
    if ($GameArguments.Count -gt 0 -or $ClickTitlePrefix -or $ClickButtonText) {
        throw '-GamePreset heaven-dx11 supplies its own arguments and bypasses the launcher'
    }
    if (-not $GamePath) {
        $GamePath = 'Z:\home\Heaven Benchmark 4.0\Heaven Benchmark 4.0\bin\heaven.exe'
    }
    $GameArguments = @(
        '-project_name', 'Heaven',
        '-data_path', '../',
        '-engine_config', '../data/heaven_4.0.cfg',
        '-system_script', 'heaven/unigine.cpp',
        '-sound_app', 'openal',
        '-video_app', 'direct3d11',
        '-video_multisample', '0',
        '-video_fullscreen', '0',
        '-video_mode', '0',
        '-extern_define', ',RELEASE,LANGUAGE_EN,QUALITY_LOW,TESSELLATION_DISABLED',
        '-extern_plugin', ',GPUMonitor'
    )
}

$environmentPairs = @($D3DEnvironment.GetEnumerator() | Sort-Object { [string]$_.Key })
if ($environmentPairs.Count -gt 32) {
    throw "At most 32 D3D environment overrides are supported"
}
if ($environmentPairs.Count -gt 0 -and -not $GamePath) {
    throw "-D3DEnvironment requires -GamePath so the overrides apply to the intended process"
}
if (($ClickTitlePrefix -or $ClickButtonText) -and
    (-not $GamePath -or -not $ClickTitlePrefix -or -not $ClickButtonText)) {
    throw "-ClickTitlePrefix and -ClickButtonText must be used together with -GamePath"
}
if (($ClickClientXPermille -ge 0) -xor ($ClickClientYPermille -ge 0)) {
    throw "-ClickClientXPermille and -ClickClientYPermille must be used together"
}
if ($ClickClientXPermille -ge 0 -and -not $ClickTitlePrefix) {
    throw "Relative client click requires -ClickTitlePrefix"
}
foreach ($pair in $environmentPairs) {
    $key = [string]$pair.Key
    $value = [string]$pair.Value
    if ($key -notmatch '^(WINEDEBUG|DXVK_|VN_|VKR_|WINEHUA_DXVK_)[A-Za-z0-9_]*$') {
        throw "Unsupported D3D environment key: $key"
    }
    if ($value.Length -gt 1024) {
        throw "D3D environment value is too long: $key"
    }
}

& $hdc -t $DeviceId shell aa force-stop $bundle | Out-Null
$stopped = $false
for ($attempt = 0; $attempt -lt 25; ++$attempt) {
    $pids = @(& $hdc -t $DeviceId shell pidof $bundle 2>$null)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($pids -join '').Trim())) {
        $stopped = $true
        break
    }
    Start-Sleep -Milliseconds 200
}
if (-not $stopped) {
    throw "WineHua process did not stop before relaunch: $($pids -join ' ')"
}
# Let the old Ability session finish publishing its termination before aa start.
# Without this short settling window, the system can redeliver the previous Want
# and silently replace an A/B profile with the stable desktop profile.
Start-Sleep -Milliseconds 300
if ($GamePath) {
    # hdc shell treats backslashes as escape characters. Win32 accepts forward
    # slashes in drive paths, and this preserves the Want parameter verbatim.
    $wantGamePath = $GamePath.Replace('\', '/')
    $gameArgumentsJson = if ($GameArguments.Count -gt 0) {
        $GameArguments | ConvertTo-Json -Compress
    } else {
        '[]'
    }
    $startArguments = @('-t', $DeviceId, 'shell', 'aa', 'start',
        '-a', 'EntryAbility', '-b', $bundle,
        '--ps', 'winehua.mode', 'game',
        '--ps', 'winehua.d3d_backend', $D3DBackend,
        '--ps', 'winehua.perf_profile', $PerfProfile,
        '--ps', 'winehua.game_path', $wantGamePath,
        '--pi', 'winehua.game_argc', [string]$GameArguments.Count,
        '--ps', 'winehua.game_args_json',
        [Uri]::EscapeDataString($gameArgumentsJson),
        '--pi', 'winehua.d3d_env_count', [string]$environmentPairs.Count,
        '--ps', 'winehua.click_title_prefix', $ClickTitlePrefix,
        '--ps', 'winehua.click_button_text', $ClickButtonText,
        '--pi', 'winehua.click_delay_ms', [string]$ClickDelayMs,
        '--pi', 'winehua.click_client_x_permille', [string]$ClickClientXPermille,
        '--pi', 'winehua.click_client_y_permille', [string]$ClickClientYPermille)
    for ($index = 0; $index -lt $environmentPairs.Count; ++$index) {
        $key = [string]$environmentPairs[$index].Key
        $escapedValue = ([string]$environmentPairs[$index].Value).Replace('\', '\\')
        $startArguments += @('--ps', "winehua.d3d_env_key$index", $key,
            '--ps', "winehua.d3d_env_value$index", $escapedValue)
    }
    $output = & $hdc @startArguments
} else {
    $output = & $hdc -t $DeviceId shell aa start -a EntryAbility -b $bundle `
        --ps winehua.d3d_backend $D3DBackend `
        --ps winehua.perf_profile $PerfProfile
}
if ($LASTEXITCODE -ne 0) { throw "WineHua start failed: $($output -join ' ')" }

Write-Host "WineHua desktop requested with D3D backend: $D3DBackend"
Write-Host "Performance profile: $PerfProfile"
if ($GamePath) {
    if ($GamePreset) {
        Write-Host "Game preset: $GamePreset"
    }
    Write-Host "Launching game through WineHua: $GamePath"
    Write-Host "Game argument count: $($GameArguments.Count)"
    if ($environmentPairs.Count -gt 0) {
        Write-Host "D3D environment overrides: $((@($environmentPairs | ForEach-Object Key)) -join ', ')"
    }
    if ($ClickTitlePrefix) {
        Write-Host "Automatic click: '$ClickButtonText' in '$ClickTitlePrefix*' after ${ClickDelayMs}ms"
        if ($ClickClientXPermille -ge 0) {
            Write-Host "Relative click fallback: ${ClickClientXPermille}/1000, ${ClickClientYPermille}/1000"
        }
    }
} else {
    Write-Host "Launch the DX11 game from the Wine desktop Explorer; its managed DXVK overlay is inherited by the game."
}
