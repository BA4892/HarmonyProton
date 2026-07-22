[CmdletBinding()]
param(
    [ValidateSet('baseline', 'direct-fence-wait', 'no-remote-sync', 'no-dynamic-flush', 'fence-feedback', 'shadow-none', 'shadow-trace', 'shadow-to-host-explicit', 'shadow-precise', 'shadow-precise-single-ring', 'shadow-precise-sync-submit', 'shadow-precise-strong-ring', 'shadow-precise-strong-ring-async-present', 'shadow-precise-strong-ring-fence-poll', 'shadow-precise-strong-ring-mailbox', 'shadow-precise-direct-fence', 'shadow-precise-retain-shmem')]
    [string]$PerfProfile = 'shadow-precise-strong-ring',
    [string]$GamePath = 'C:\smoke\x64\winehua_d3d_switch_cube.exe',
    [ValidateRange(8, 120)]
    [int]$Samples = 40,
    [ValidateRange(50, 2000)]
    [int]$IntervalMs = 120,
    [ValidateRange(5, 120)]
    [int]$StartupTimeoutSeconds = 45,
    [string]$DeviceId = '5KPBB25818203996',
    [string]$OutputRoot = 'D:\MyProject\winehua-logs\automation'
)

$ErrorActionPreference = 'Stop'
$hdc = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
$bundle = 'app.hackeris.winehua'
$startScript = Join-Path $PSScriptRoot 'Start-WineHuaGameTest.ps1'
$runId = 'frame-order-{0}' -f (Get-Date -Format 'yyyyMMdd-HHmmss')
$output = Join-Path $OutputRoot $runId
$frames = Join-Path $output 'frames'
$remoteRoot = '/data/local/tmp/winehua-frame-order'

if (-not (Test-Path -LiteralPath $hdc)) { throw "Windows HDC not found: $hdc" }
if (-not (Test-Path -LiteralPath $startScript)) { throw "Launcher not found: $startScript" }
New-Item -ItemType Directory -Path $frames -Force | Out-Null
Add-Type -AssemblyName System.Drawing

function Invoke-Hdc {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $result = & $hdc -t $DeviceId @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "hdc failed ($LASTEXITCODE): $($Arguments -join ' ')`n$($result -join "`n")"
    }
    return $result
}

function Get-FrameMarker {
    param([Parameter(Mandatory)][string]$Path)

    $bitmap = [System.Drawing.Bitmap]::new($Path)
    try {
        # The marker is a blue-dominant quad in the upper-left part of the D3D
        # client. R and G store the high/low nibbles as 4 + nibble * 8.
        $maxX = [Math]::Min($bitmap.Width - 1, [Math]::Max(260, [int]($bitmap.Width * 0.22)))
        # The OHNativeImage transform currently displays D3D positive Y in the
        # lower half of the Wine client, so the marker appears at lower-left.
        $minY = [Math]::Max(90, [int]($bitmap.Height * 0.60))
        $maxY = [Math]::Min($bitmap.Height - 1, [int]($bitmap.Height * 0.90))
        [long]$sumR = 0
        [long]$sumG = 0
        [long]$sumB = 0
        [int]$count = 0
        # Keep clear of the Harmony status bar and desktop taskbar. Small blue
        # app icons must never be accepted during runtime extraction.
        for ($y = $minY; $y -le $maxY; $y += 2) {
            for ($x = 0; $x -le $maxX; $x += 2) {
                $pixel = $bitmap.GetPixel($x, $y)
                if ($pixel.B -ge 180 -and
                    ($pixel.B - $pixel.R) -ge 70 -and
                    ($pixel.B - $pixel.G) -ge 70) {
                    $sumR += $pixel.R
                    $sumG += $pixel.G
                    $sumB += $pixel.B
                    ++$count
                }
            }
        }
        if ($count -lt 500) {
            return [pscustomobject]@{ valid = $false; pixels = $count; value = $null }
        }

        $averageR = [double]$sumR / $count
        $averageG = [double]$sumG / $count
        $averageB = [double]$sumB / $count
        $high = [Math]::Max(0, [Math]::Min(15, [int][Math]::Round(($averageR - 4.0) / 8.0)))
        $low = [Math]::Max(0, [Math]::Min(15, [int][Math]::Round(($averageG - 4.0) / 8.0)))
        return [pscustomobject]@{
            valid = $true
            pixels = $count
            value = (($high -shl 4) -bor $low)
            averageR = [Math]::Round($averageR, 2)
            averageG = [Math]::Round($averageG, 2)
            averageB = [Math]::Round($averageB, 2)
        }
    } finally {
        $bitmap.Dispose()
    }
}

function Save-Snapshot {
    param([Parameter(Mandatory)][string]$Name)
    $remote = "$remoteRoot-$Name.jpeg"
    $local = Join-Path $frames "$Name.jpeg"
    Invoke-Hdc -Arguments @('shell', 'snapshot_display', '-f', $remote) | Out-Null
    Invoke-Hdc -Arguments @('file', 'recv', $remote, $local) | Out-Null
    Invoke-Hdc -Arguments @('shell', 'rm', '-f', $remote) | Out-Null
    return [pscustomobject]@{ path = $local; marker = Get-FrameMarker -Path $local }
}

$records = [System.Collections.Generic.List[object]]::new()
$started = $false
$summary = $null
try {
    Invoke-Hdc -Arguments @('shell', 'hilog', '-r') | Out-Null
    & $startScript -D3DBackend dxvk_legacy -PerfProfile $PerfProfile `
        -GamePath $GamePath -DeviceId $DeviceId | Tee-Object -FilePath (Join-Path $output 'launch.txt')
    if ($LASTEXITCODE -ne 0) { throw 'WineHua game launcher failed' }
    $started = $true

    $profileObserved = $false
    $profileDeadline = (Get-Date).AddSeconds(10)
    $profileNeedle = "host shadow profile=$PerfProfile "
    do {
        Start-Sleep -Milliseconds 300
        $profileLog = @(Invoke-Hdc -Arguments @('shell', 'hilog', '-x'))
        if (($profileLog -join "`n").Contains($profileNeedle)) {
            $profileObserved = $true
            break
        }
    } while ((Get-Date) -lt $profileDeadline)
    if (-not $profileObserved) {
        throw "Requested performance profile was not observed: $PerfProfile"
    }

    $deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    $startupIndex = 0
    do {
        Start-Sleep -Milliseconds 800
        $snapshot = Save-Snapshot -Name ('startup-{0:d2}' -f $startupIndex++)
        if ($snapshot.marker.valid) { break }
    } while ((Get-Date) -lt $deadline)
    if (-not $snapshot.marker.valid) {
        throw "Frame marker was not found within $StartupTimeoutSeconds seconds"
    }

    for ($i = 0; $i -lt $Samples; ++$i) {
        $capturedAt = [DateTimeOffset]::Now
        $snapshot = Save-Snapshot -Name ('sample-{0:d3}' -f $i)
        $records.Add([pscustomobject]@{
            index = $i
            capturedAt = $capturedAt.ToString('o')
            markerValid = [bool]$snapshot.marker.valid
            marker = $snapshot.marker.value
            pixels = $snapshot.marker.pixels
            averageR = $snapshot.marker.averageR
            averageG = $snapshot.marker.averageG
            averageB = $snapshot.marker.averageB
            image = [IO.Path]::GetFileName($snapshot.path)
        })
        Start-Sleep -Milliseconds $IntervalMs
    }

    $valid = @($records | Where-Object markerValid)
    $regressions = [System.Collections.Generic.List[object]]::new()
    $duplicates = 0
    $forwardDeltas = [System.Collections.Generic.List[int]]::new()
    for ($i = 1; $i -lt $valid.Count; ++$i) {
        $previous = [int]$valid[$i - 1].marker
        $current = [int]$valid[$i].marker
        $delta = ($current - $previous + 256) % 256
        if ($delta -eq 0) {
            ++$duplicates
        } elseif ($delta -gt 128) {
            $regressions.Add([pscustomobject]@{
                fromIndex = $valid[$i - 1].index
                toIndex = $valid[$i].index
                previous = $previous
                current = $current
                moduloDelta = $delta
            })
        } else {
            $forwardDeltas.Add($delta)
        }
    }

    $minimumValid = [Math]::Ceiling($Samples * 0.75)
    $trailingInvalid = 0
    for ($i = $records.Count - 1; $i -ge 0 -and -not $records[$i].markerValid; --$i) {
        ++$trailingInvalid
    }
    $status = if ($valid.Count -lt $minimumValid) {
        'INCONCLUSIVE'
    } elseif ($regressions.Count -gt 0 -or $trailingInvalid -gt 2) {
        'FAIL'
    } else {
        'PASS'
    }
    $summary = [ordered]@{
        schemaVersion = 1
        runId = $runId
        status = $status
        perfProfile = $PerfProfile
        gamePath = $GamePath
        samplesRequested = $Samples
        samplesValid = $valid.Count
        duplicates = $duplicates
        trailingInvalid = $trailingInvalid
        regressions = @($regressions)
        averageForwardDelta = if ($forwardDeltas.Count) {
            [Math]::Round(($forwardDeltas | Measure-Object -Average).Average, 2)
        } else { $null }
        maxForwardDelta = if ($forwardDeltas.Count) {
            ($forwardDeltas | Measure-Object -Maximum).Maximum
        } else { $null }
        records = @($records)
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding UTF8
} catch {
    $summary = [ordered]@{
        schemaVersion = 1
        runId = $runId
        status = 'INFRA_ERROR'
        perfProfile = $PerfProfile
        gamePath = $GamePath
        message = $_.Exception.Message
        records = @($records)
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding UTF8
} finally {
    try {
        $hilog = Invoke-Hdc -Arguments @('shell', 'hilog', '-x')
        $hilog | Set-Content -LiteralPath (Join-Path $output 'hilog.txt') -Encoding UTF8
    } catch {
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $output 'hilog-error.txt') -Encoding UTF8
    }
    if ($started) {
        try { Invoke-Hdc -Arguments @('shell', 'aa', 'force-stop', $bundle) | Out-Null } catch {}
    }
}

$summary | ConvertTo-Json -Depth 4
Write-Host "Frame-order archive: $output"
if ($summary.status -eq 'PASS') { exit 0 }
if ($summary.status -eq 'FAIL') { exit 1 }
exit 2
