[CmdletBinding()]
param(
    [ValidateSet('dxvk_legacy', 'wined3d')]
    [string]$D3DBackend = 'dxvk_legacy',
    [ValidateSet('baseline', 'direct-fence-wait', 'no-remote-sync', 'no-dynamic-flush', 'fence-feedback', 'shadow-none', 'shadow-trace', 'shadow-to-host-explicit', 'shadow-precise', 'shadow-precise-single-ring', 'shadow-precise-sync-submit', 'shadow-precise-strong-ring', 'shadow-precise-legacy-host-sync', 'shadow-precise-strong-ring-trace', 'shadow-precise-strong-ring-perf', 'shadow-precise-strong-ring-async-present', 'shadow-precise-strong-ring-fence-poll', 'shadow-precise-strong-ring-mailbox', 'shadow-precise-direct-fence', 'shadow-precise-retain-shmem', 'shadow-precise-cpu-upload', 'shadow-precise-dirty-ring', 'shadow-precise-dirty-ring-perf', 'shadow-precise-dirty-ring-no-merge', 'shadow-precise-dirty-ring-no-upload', 'shadow-precise-dirty-ring-no-upload-fast', 'shadow-precise-dirty-ring-inline-upload', 'shadow-precise-dirty-ring-inline-upload-serialized','shadow-precise-dirty-ring-inline-upload-descriptor-serialized', 'shadow-precise-dirty-ring-frame-assoc-trace', 'shadow-precise-dirty-ring-present-image-trace')]
    [string]$PerfProfile = 'shadow-precise-dirty-ring-inline-upload',
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
    if ($GameArguments.Count -gt 0) {
        throw '-GamePreset heaven-dx11 supplies its own arguments'
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
    # The direct executable opens Heaven's Win32 launcher first. Use the same
    # managed driver as other game automation so the benchmark scene, rather
    # than the black pre-run client area, is the workload under test.
    if (-not $ClickTitlePrefix) {
        $ClickTitlePrefix = 'Unigine Heaven Benchmark 4.0 Basic (Direct3D11)'
    }
    if (-not $ClickButtonText) {
        $ClickButtonText = 'Benchmark'
    }
    # Heaven draws its launcher controls itself, so there is no Win32 BUTTON
    # for BM_CLICK. Keep a deterministic client-coordinate fallback.
    if ($ClickClientXPermille -lt 0) {
        $ClickClientXPermille = 80
    }
    if ($ClickClientYPermille -lt 0) {
        $ClickClientYPermille = 65
    }
}

# Carry the DXVK half of the frame-association trace explicitly in the game
# Want. The Host profile reaches the NCP through graphics-broker IPC, but it
# does not itself guarantee that an arbitrary Wine child environment variable
# survives Ability startup on every Harmony build.
if ($PerfProfile -eq 'shadow-precise-dirty-ring-frame-assoc-trace' -and
    -not $D3DEnvironment.ContainsKey('WINEHUA_DXVK_TRACE_CAMERA')) {
    $D3DEnvironment['WINEHUA_DXVK_TRACE_CAMERA'] = '1'
}
if ($PerfProfile -eq 'shadow-precise-dirty-ring-present-image-trace') {
    if (-not $D3DEnvironment.ContainsKey('WINEHUA_DXVK_TRACE_PRESENT_IMAGE')) {
        $D3DEnvironment['WINEHUA_DXVK_TRACE_PRESENT_IMAGE'] = '1'
    }
    if (-not $D3DEnvironment.ContainsKey('DXVK_LOG_LEVEL')) {
        $D3DEnvironment['DXVK_LOG_LEVEL'] = 'info'
    }
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
    if ($key -notmatch '^(WINEDEBUG|DXVK_|VN_|VKR_|WINEHUA_DXVK_|WINEHUA_VKR_)[A-Za-z0-9_]*$') {
        throw "Unsupported D3D environment key: $key"
    }
    if ($value.Length -gt 1024) {
        throw "D3D environment value is too long: $key"
    }
}
$environmentPayload = @($environmentPairs | ForEach-Object {
    [ordered]@{
        key = [string]$_.Key
        value = [string]$_.Value
    }
})
$environmentJson = ConvertTo-Json -InputObject $environmentPayload -Compress
$encodedEnvironmentJson = [Uri]::EscapeDataString($environmentJson)

& $hdc -t $DeviceId shell aa force-stop $bundle | Out-Null
$stopped = $false
$processLines = @()
for ($attempt = 0; $attempt -lt 25; ++$attempt) {
    # pidof only reports the main Ability. Native Wine/VirGL children have
    # suffixed process names and can keep the GPU workload alive after the
    # Ability has disappeared, contaminating the next A/B run.
    $processLines = @(& $hdc -t $DeviceId shell ps -ef 2>$null |
        Where-Object { $_ -match [regex]::Escape($bundle) })
    if ($processLines.Count -eq 0) {
        $stopped = $true
        break
    }
    Start-Sleep -Milliseconds 200
}
if (-not $stopped) {
    throw "WineHua process tree did not stop before relaunch: $($processLines -join ' | ')"
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
        ConvertTo-Json -InputObject @($GameArguments) -Compress
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
        # A single encoded JSON value avoids aa start's finite Want-parameter
        # count silently dropping one or more environment key/value pairs.
        '--ps', 'winehua.d3d_env_json', $encodedEnvironmentJson)
    $startArguments += @(
        '--ps', 'winehua.game_args_json',
        [Uri]::EscapeDataString($gameArgumentsJson),
        '--ps', 'winehua.click_title_prefix', $ClickTitlePrefix,
        '--ps', 'winehua.click_button_text', $ClickButtonText,
        '--pi', 'winehua.click_delay_ms', [string]$ClickDelayMs,
        '--pi', 'winehua.click_client_x_permille', [string]$ClickClientXPermille,
        '--pi', 'winehua.click_client_y_permille', [string]$ClickClientYPermille)
    $output = & $hdc @startArguments
} else {
    $output = & $hdc -t $DeviceId shell aa start -a EntryAbility -b $bundle `
        --ps winehua.d3d_backend $D3DBackend `
        --ps winehua.perf_profile $PerfProfile
}
$startExitCode = $LASTEXITCODE
$outputText = ($output -join "`n").Trim()
if ($startExitCode -ne 0 -or $outputText -notmatch 'start ability successfully') {
    throw "WineHua start failed (exit=$startExitCode): $outputText"
}

$startedPid = ''
for ($attempt = 0; $attempt -lt 25; ++$attempt) {
    $pidOutput = @(& $hdc -t $DeviceId shell pidof $bundle 2>$null)
    if ($LASTEXITCODE -eq 0) {
        $startedPid = ($pidOutput -join ' ').Trim()
        if ($startedPid) { break }
    }
    Start-Sleep -Milliseconds 200
}
if (-not $startedPid) {
    throw "WineHua start reported success but no process appeared: $outputText"
}

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
        Write-Host "D3D environment transport: encoded JSON ($($environmentPairs.Count) entries)"
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
Write-Host "WineHua PID: $startedPid"
