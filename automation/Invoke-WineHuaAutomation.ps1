[CmdletBinding()]
param(
    [ValidateSet('core', 'audio', 'opengl', 'host-vulkan', 'host-heaven', 'host-heaven-material-depth', 'host-heaven-inputs', 'venus', 'venus-sampled', 'venus-sampled-idle', 'venus-depth-cube', 'venus-depth-cube-array-2d-golden', 'venus-depth-cube-graphics', 'venus-heaven-material', 'venus-heaven-material-depth', 'venus-heaven-captured', 'venus-heaven-inputs', 'venus-heaven-captured-ab', 'venus-heaven-discard-ab', 'venus-heaven-material-layout', 'venus-heaven-draw0', 'venus-heaven-draw170', 'venus-heaven-f647', 'capabilities', 'wine-vulkan', 'wine-vulkan-present', 'dxvk', 'dxvk-long', 'dxvk-replay', 'dxvk-layout-general', 'dxvk-combined', 'dxvk-dynamic', 'all', 'long')]
    [string]$Suite = 'core',
    [ValidateSet('reuse', 'clean')]
    [string]$Prefix = 'reuse',
    [ValidateSet('baseline', 'direct-fence-wait', 'no-remote-sync', 'no-dynamic-flush', 'fence-feedback', 'shadow-none', 'shadow-trace', 'shadow-to-host-explicit', 'shadow-precise', 'shadow-precise-single-ring', 'shadow-precise-sync-submit', 'shadow-precise-strong-ring', 'shadow-precise-legacy-host-sync', 'shadow-precise-strong-ring-trace', 'shadow-precise-strong-ring-perf', 'shadow-precise-dirty-ring', 'shadow-precise-dirty-ring-perf', 'shadow-precise-dirty-ring-no-merge', 'shadow-precise-dirty-ring-no-upload', 'shadow-precise-dirty-ring-no-upload-fast', 'shadow-precise-dirty-ring-inline-upload', 'shadow-precise-dirty-ring-inline-upload-coverage-sort', 'shadow-precise-dirty-ring-inline-upload-serialized', 'shadow-precise-dirty-ring-inline-upload-descriptor-serialized', 'shadow-precise-strong-ring-async-present', 'shadow-precise-strong-ring-fence-poll', 'shadow-precise-strong-ring-mailbox', 'shadow-precise-direct-fence', 'shadow-precise-retain-shmem', 'shadow-precise-cpu-upload')]
    [string]$PerfProfile = 'shadow-precise-dirty-ring-inline-upload',
    [int]$Runs = 1,
    [ValidateRange(60, 3600)]
    [int]$LongSeconds = 3600,
    [switch]$Gate,
    [switch]$SkipBuild,
    [string]$DeviceId = '',
    [string]$ReplayFragmentSpv = '',
    [string]$ReplayVertexSpv = '',
    [string]$ArchiveRoot = 'D:\MyProject\winehua-logs\automation',
    [int]$TimeoutMinutes = 15
)

$ErrorActionPreference = 'Stop'
$RepoWsl = '/home/maple/Work/WineHua-build'
$Container = 'winehua-master-ext4'
$ContainerRepo = '/data/src/winehua'
$Bundle = 'app.hackeris.winehua'
$Ability = 'EntryAbility'
$Hdc = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
$HapWsl = "$RepoWsl/entry/build/default/outputs/default/entry-default-signed.hap"
$HapWindows = '\\wsl.localhost\Ubuntu\home\maple\Work\WineHua-build\entry\build\default\outputs\default\entry-default-signed.hap'
$DeviceSandbox = "/data/app/el2/100/base/$Bundle"

function Invoke-NativeChecked {
    param([scriptblock]$Command, [string]$Description)
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE" }
}

function Invoke-Hdc {
    & $Hdc -t $script:DeviceId @args
}

function Get-DeviceText {
    param([string]$RemotePath)
    $text = & $Hdc -t $script:DeviceId shell cat $RemotePath 2>$null
    if ($LASTEXITCODE -ne 0) { return '' }
    $joined = ($text -join "`n").Trim()
    $start = $joined.IndexOf('{')
    $end = $joined.LastIndexOf('}')
    if ($start -lt 0 -or $end -lt $start) { return '' }
    return $joined.Substring($start, $end - $start + 1)
}

function Save-DeviceFile {
    param([string]$RemotePath, [string]$LocalPath)
    $parent = Split-Path -Parent $LocalPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    & $Hdc -t $script:DeviceId file recv $RemotePath $LocalPath *> $null
}

function Test-FixedFrame {
    param([string]$ImagePath, [string]$JsonPath)
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::new($ImagePath)
    try {
        $classes = @{
            red    = @{ Count = 0L; X = 0L; Y = 0L }
            green  = @{ Count = 0L; X = 0L; Y = 0L }
            blue   = @{ Count = 0L; X = 0L; Y = 0L }
            yellow = @{ Count = 0L; X = 0L; Y = 0L }
        }
        $step = 4
        for ($y = 0; $y -lt $bitmap.Height; $y += $step) {
            for ($x = 0; $x -lt $bitmap.Width; $x += $step) {
                $pixel = $bitmap.GetPixel($x, $y)
                $name = $null
                if ($pixel.R -gt 170 -and $pixel.G -lt 100 -and $pixel.B -lt 120) { $name = 'red' }
                elseif ($pixel.G -gt 150 -and $pixel.R -lt 120 -and $pixel.B -lt 130) { $name = 'green' }
                elseif ($pixel.B -gt 160 -and $pixel.R -lt 130 -and $pixel.G -lt 140) { $name = 'blue' }
                elseif ($pixel.R -gt 170 -and $pixel.G -gt 140 -and $pixel.B -lt 120) { $name = 'yellow' }
                if ($name) {
                    $classes[$name].Count++
                    $classes[$name].X += $x
                    $classes[$name].Y += $y
                }
            }
        }
        $sampleCount = [math]::Ceiling($bitmap.Width / $step) * [math]::Ceiling($bitmap.Height / $step)
        $minimum = [math]::Max(80, [int]($sampleCount * 0.003))
        $centroids = @{}
        $enough = $true
        foreach ($name in @('red', 'green', 'blue', 'yellow')) {
            $entry = $classes[$name]
            if ($entry.Count -lt $minimum) { $enough = $false }
            $centroids[$name] = @{
                count = $entry.Count
                x = if ($entry.Count) { [double]$entry.X / $entry.Count } else { -1 }
                y = if ($entry.Count) { [double]$entry.Y / $entry.Count } else { -1 }
            }
        }
        # The OHOS presentation transform follows the display's native orientation.
        # A landscape snapshot can therefore contain a 90/180/270 degree rotation of
        # the canonical Vulkan framebuffer.  Require the exact four-colour topology,
        # but accept rotations; a reflection or duplicated/missing quadrant still
        # fails the visual gate.
        $centerX = ($centroids.red.x + $centroids.green.x + $centroids.blue.x + $centroids.yellow.x) / 4
        $centerY = ($centroids.red.y + $centroids.green.y + $centroids.blue.y + $centroids.yellow.y) / 4
        $quadrants = @{}
        foreach ($name in @('red', 'green', 'blue', 'yellow')) {
            $column = if ($centroids[$name].x -lt $centerX) { 'L' } else { 'R' }
            $row = if ($centroids[$name].y -lt $centerY) { 'T' } else { 'B' }
            $quadrants["$row$column"] = $name
        }
        $layouts = @(
            @{ name = 'identity';  TL = 'red';    TR = 'green';  BL = 'blue';   BR = 'yellow' },
            @{ name = 'rotate90';  TL = 'blue';   TR = 'red';    BL = 'yellow'; BR = 'green' },
            @{ name = 'rotate180'; TL = 'yellow'; TR = 'blue';   BL = 'green';  BR = 'red' },
            @{ name = 'rotate270'; TL = 'green';  TR = 'yellow'; BL = 'red';    BR = 'blue' }
        )
        $detectedTransform = $null
        foreach ($layout in $layouts) {
            if ($quadrants.Count -eq 4 -and
                $quadrants.TL -eq $layout.TL -and $quadrants.TR -eq $layout.TR -and
                $quadrants.BL -eq $layout.BL -and $quadrants.BR -eq $layout.BR) {
                $detectedTransform = $layout.name
                break
            }
        }
        $xValues = @($centroids.red.x, $centroids.green.x, $centroids.blue.x, $centroids.yellow.x)
        $yValues = @($centroids.red.y, $centroids.green.y, $centroids.blue.y, $centroids.yellow.y)
        $separatedColumns = (($xValues | Measure-Object -Maximum).Maximum - ($xValues | Measure-Object -Minimum).Minimum) -gt ($bitmap.Width * 0.08)
        $separatedRows = (($yValues | Measure-Object -Maximum).Maximum - ($yValues | Measure-Object -Minimum).Minimum) -gt ($bitmap.Height * 0.08)
        $pass = $enough -and $separatedColumns -and $separatedRows -and $null -ne $detectedTransform
        [ordered]@{
            schemaVersion = 1
            status = if ($pass) { 'PASS' } else { 'FAIL' }
            validator = 'rgba-quadrants-v1-rotations'
            image = $ImagePath
            width = $bitmap.Width
            height = $bitmap.Height
            minimumSamplesPerColor = $minimum
            detectedTransform = $detectedTransform
            quadrants = $quadrants
            centroids = $centroids
        } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $JsonPath -Encoding UTF8
        return $pass
    }
    finally {
        $bitmap.Dispose()
    }
}

function Test-D3D11CubeFrame {
    param([string]$ImagePath, [string]$JsonPath)
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::new($ImagePath)
    try {
        $step = 4
        $colored = 0L
        $dark = 0L
        $buckets = @{ red = 0L; green = 0L; blue = 0L }
        $minX = $bitmap.Width
        $minY = $bitmap.Height
        $maxX = -1
        $maxY = -1
        for ($y = 0; $y -lt $bitmap.Height; $y += $step) {
            for ($x = 0; $x -lt $bitmap.Width; $x += $step) {
                $pixel = $bitmap.GetPixel($x, $y)
                $maximum = [math]::Max($pixel.R, [math]::Max($pixel.G, $pixel.B))
                $minimum = [math]::Min($pixel.R, [math]::Min($pixel.G, $pixel.B))
                if ($maximum -lt 55) { $dark++ }
                if ($maximum -gt 100 -and ($maximum - $minimum) -gt 55) {
                    $colored++
                    $minX = [math]::Min($minX, $x)
                    $minY = [math]::Min($minY, $y)
                    $maxX = [math]::Max($maxX, $x)
                    $maxY = [math]::Max($maxY, $y)
                    if ($pixel.R -eq $maximum) { $buckets.red++ }
                    elseif ($pixel.G -eq $maximum) { $buckets.green++ }
                    else { $buckets.blue++ }
                }
            }
        }
        $sampleCount = [math]::Ceiling($bitmap.Width / $step) * [math]::Ceiling($bitmap.Height / $step)
        $minimumColored = [math]::Max(500, [int]($sampleCount * 0.005))
        $activeBuckets = @($buckets.Values | Where-Object { $_ -gt ($minimumColored * 0.08) }).Count
        $boxWidth = if ($maxX -ge $minX) { $maxX - $minX + 1 } else { 0 }
        $boxHeight = if ($maxY -ge $minY) { $maxY - $minY + 1 } else { 0 }
        $pass = $colored -ge $minimumColored -and $activeBuckets -ge 3 -and
            $boxWidth -gt ($bitmap.Width * 0.08) -and $boxHeight -gt ($bitmap.Height * 0.08) -and
            $dark -gt ($sampleCount * 0.03)
        [ordered]@{
            schemaVersion = 1
            status = if ($pass) { 'PASS' } else { 'FAIL' }
            validator = 'd3d11-cube-color-depth-v1'
            image = $ImagePath
            width = $bitmap.Width
            height = $bitmap.Height
            coloredSamples = $colored
            minimumColoredSamples = $minimumColored
            darkSamples = $dark
            activeColorBuckets = $activeBuckets
            colorBuckets = $buckets
            coloredBounds = @{ x = $minX; y = $minY; width = $boxWidth; height = $boxHeight }
        } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $JsonPath -Encoding UTF8
        return $pass
    }
    finally {
        $bitmap.Dispose()
    }
}

function Get-D3D11Coverage {
    param([object]$Summary, [string]$RunSuite)
    $entries = @()
    foreach ($test in @($Summary.tests)) {
        $m = $test.metrics
        # Visual examples (for example dxvk-cube-x64) prove device creation
        # and presentation but intentionally do not duplicate the exhaustive
        # feature metrics emitted by winehua_d3d11_smoke.
        if ($null -eq $m -or $null -eq $m.rgba8SampleMatrix) { continue }
        $checks = [ordered]@{
            featureLevel11 = ($m.featureLevel -eq '11.0')
            shaderModel5 = [bool]$m.shaderModel5
            cubeGeometry = [bool]$m.cubeGeometry
            drawIndexedInstanced = [bool]$m.drawIndexedInstanced
            depthStencil = [bool]$m.depthStencil
            alphaBlend = [bool]$m.alphaBlend
            rasterizerState = [bool]$m.rasterizerState
            constantBuffer = [bool]$m.constantBuffer
            dynamicConstantBuffer = ($RunSuite -ne 'dxvk-dynamic' -or [bool]$m.dynamicConstantBuffer)
            dynamicConstantReadback = ($RunSuite -ne 'dxvk-dynamic' -or [bool]$m.dynamicConstantReadback)
            textureUpdate = [bool]$m.textureUpdate
            textureUploadReadback = [bool]$m.textureUploadReadback
            textureSamplingFunctional = [bool]$m.textureSampling
            rgba8LoadPs = [bool]$m.rgba8SampleMatrix.loadPs.pass
            rgba8LoadCs = [bool]$m.rgba8SampleMatrix.loadCs.pass
            rgba8PointPs = [bool]$m.rgba8SampleMatrix.pointPs.pass
            rgba8PointCs = [bool]$m.rgba8SampleMatrix.pointCs.pass
            rgba8LinearPs = [bool]$m.rgba8SampleMatrix.linearPs.pass
            rgba8LinearCs = [bool]$m.rgba8SampleMatrix.linearCs.pass
            rgba8UpdatedUpload = [bool]$m.rgba8SampleMatrix.updated.uploadPass
            rgba8UpdatedLoadPs = [bool]$m.rgba8SampleMatrix.updated.loadPs.pass
            rgba8UpdatedLoadCs = [bool]$m.rgba8SampleMatrix.updated.loadCs.pass
            rgba8UpdatedPointPs = [bool]$m.rgba8SampleMatrix.updated.pointPs.pass
            rgba8UpdatedPointCs = [bool]$m.rgba8SampleMatrix.updated.pointCs.pass
            descriptorIdentity = [bool]$m.descriptorMatrix.initial.pass
            descriptorRebindDirtyState = [bool]$m.descriptorMatrix.rebind.pass
            descriptorUnbound = [bool]$m.descriptorMatrix.unbound.pass
            descriptorLifetime = [bool]$m.descriptorMatrix.lifetime.pass
            subresourceArrayLayers = [bool]$m.subresourceMatrix.arrayLayers
            subresourceMipLevels = [bool]$m.subresourceMatrix.mipLevels
            subresourceExplicitLod = [bool]$m.subresourceMatrix.explicitLod
            subresourceBarrierUpdate = [bool]$m.subresourceMatrix.barrierUpdate
            subresourceMatrix = [bool]$m.subresourceMatrix.pass
            texture3dCreated = [bool]$m.texture3dMatrix.created
            texture3dUpload = [bool]$m.texture3dMatrix.upload
            texture3dSingleDispatch = [bool]$m.texture3dMatrix.singleDispatch
            texture3dUavToSrvBarrier = [bool]$m.texture3dMatrix.uavToSrvBarrier
            texture3dPingPong = [bool]$m.texture3dMatrix.pingPong
            heavenCubeMatrix = [bool]$m.heavenResourceMatrix.cube.pass
            heavenTexture3dR8 = [bool]$m.heavenResourceMatrix.texture3d.r8.pass
            heavenTexture3dRg8 = [bool]$m.heavenResourceMatrix.texture3d.rg8.pass
            heavenD32DepthComparison = [bool]$m.heavenResourceMatrix.depthComparisonSampler.pass
            heavenD24S8DepthComparison = [bool]$m.heavenResourceMatrix.d24s8DepthComparisonSampler.pass
            heavenD24S8ExtendedMatrix = [bool]$m.heavenResourceMatrix.d24s8ExtendedMatrix.pass
            heavenResourceMatrix = [bool]$m.heavenResourceMatrix.pass
            bcTextureCreated = ($m.bcTextureTest -eq 'created_sampled')
            bcSamplingSubmitted = [bool]$m.bcSamplingSubmitted
            bcSamplingFunctional = [bool]$m.bcSamplingFunctional
            offscreenRenderTarget = [bool]$m.offscreenRenderTarget
            msaa4xSupported = [bool]$m.msaa4xSupported
            msaaResolveFunctional = [bool]$m.msaaResolveFunctional
            stencilQueryEnabled = [bool]$m.stencilQueryEnabled
            stencilPixelFunctional = [bool]$m.stencilPixelFunctional
            stencilQueryFunctional = [bool]$m.stencilFunctional
            computeShaderDispatch = [bool]$m.computeShaderDispatch
            computeUavSubmitted = [bool]$m.computeUavSubmitted
            computeUavFunctional = [bool]$m.computeUavFunctional
            computeSampledImageFunctional = [bool]$m.computeSampledImageFunctional
            longWallClock = ($RunSuite -ne 'dxvk-long' -or
                [int64]$m.durationMs -ge ([int64]$LongSeconds * 1000 - 2000))
            present60Frames = ([int]$m.presentFrames -ge 60)
            presentResultSuccess = ([int]$m.presentResult -eq 0)
            cpuFullFrameReadbackZero = ([int]$m.cpuReadBytes -eq 0)
            cpuFullFrameUploadZero = ([int]$m.cpuUploadBytes -eq 0)
            perFrameDeviceWaitIdleZero = ([int]$m.perFrameDeviceWaitIdle -eq 0)
            noFallback = (-not [bool]$m.fallbackDetected)
        }
        $missing = @($checks.GetEnumerator() | Where-Object { -not [bool]$_.Value } | ForEach-Object { $_.Key })
        $submittedOnly = @()
        if ([bool]$m.bcSamplingSubmitted -and -not [bool]$m.bcSamplingFunctional) { $submittedOnly += 'bcSampling' }
        if ([bool]$m.computeUavSubmitted -and -not [bool]$m.computeUavFunctional) { $submittedOnly += 'computeUav' }
        $entries += [ordered]@{
            testId = $test.testId
            appStatus = $test.status
            requiredPass = ($missing.Count -eq 0)
            missingRequired = $missing
            submittedOnly = $submittedOnly
            metrics = [ordered]@{
                presentFrames = [int]$m.presentFrames
                queueSubmitCount = [int]$m.queueSubmitCount
                featureProbeReadBytes = [int]$m.featureProbeReadBytes
                featureProbeGpuCopies = [int]$m.featureProbeGpuCopies
                durationMs = [int64]$m.durationMs
                rgba8SampleMatrix = $m.rgba8SampleMatrix
                descriptorMatrix = $m.descriptorMatrix
                subresourceMatrix = $m.subresourceMatrix
                heavenResourceMatrix = $m.heavenResourceMatrix
                cpuReadBytes = [int]$m.cpuReadBytes
                cpuUploadBytes = [int]$m.cpuUploadBytes
            }
        }
    }
    $requiredPass = ($entries.Count -gt 0 -and
        @($entries | Where-Object { -not $_.requiredPass }).Count -eq 0)
    return [ordered]@{
        schemaVersion = 1
        suite = $RunSuite
        status = if ($requiredPass) { 'PASS' } else { 'FAIL' }
        tests = $entries
        policy = 'required API/object/RGBA8 Load-POINT-LINEAR PS-CS/descriptor/subresource array-mip-explicit-LOD-update/texture sampling/D24S8 2D-array-per-view-cube-cube-array-linear-border/present/readback coverage; ordinary R32_FLOAT comparison, optional MSAA resolve, and stencil query are reported separately'
    }
}

function Capture-D3D11Frame {
    param([string]$RemoteImage, [string]$LocalImage, [string]$JsonPath)
    $lastJson = $null
    for ($attempt = 0; $attempt -lt 4; $attempt++) {
        if ($attempt -gt 0) { Start-Sleep -Milliseconds 750 }
        Invoke-Hdc shell snapshot_display -f $RemoteImage | Out-Null
        Save-DeviceFile $RemoteImage $LocalImage
        Invoke-Hdc shell rm $RemoteImage | Out-Null
        $attemptJson = "$JsonPath.attempt$attempt"
        $lastJson = $attemptJson
        if (Test-D3D11CubeFrame -ImagePath $LocalImage -JsonPath $attemptJson) {
            Copy-Item -LiteralPath $attemptJson -Destination $JsonPath -Force
            return $true
        }
    }
    if ($lastJson -and (Test-Path -LiteralPath $lastJson)) {
        Copy-Item -LiteralPath $lastJson -Destination $JsonPath -Force
    }
    return $false
}

function Assert-BuildEnvironment {
    $mountsText = wsl -d Ubuntu -- docker inspect $Container --format '{{json .Mounts}}'
    if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect build container' }
    $mounts = $mountsText | ConvertFrom-Json
    $source = $mounts | Where-Object { $_.Destination -eq $ContainerRepo -and $_.Source -eq $RepoWsl -and $_.RW }
    $sdk = $mounts | Where-Object { $_.Destination -eq '/apps/harmony' -and -not $_.RW }
    if (-not $source -or -not $sdk -or $mounts.Count -ne 2) {
        throw 'winehua-master-ext4 mounts do not match the isolated build contract'
    }
}

function Invoke-Build {
    param([string]$LogPath)
    Assert-BuildEnvironment
    wsl -d Ubuntu -- docker start $Container | Out-Null
    $output = & wsl -d Ubuntu -- docker exec $Container bash -lc "cd $ContainerRepo && make NATIVE_ARCH=arm64-v8a" 2>&1
    $exitCode = $LASTEXITCODE
    $redacted = $output | ForEach-Object {
        $_ -replace '(-keyPwd\s+)\S+', '$1<redacted>' -replace '(-keystorePwd\s+)\S+', '$1<redacted>'
    }
    $redacted | Set-Content -LiteralPath $LogPath -Encoding UTF8
    if ($exitCode -ne 0) { throw "Docker build failed; see $LogPath" }
}

function Get-ArtifactMetadata {
    param([string]$OutputDirectory)
    $stat = wsl -d Ubuntu -- stat -c '%y %s' $HapWsl
    if ($LASTEXITCODE -ne 0) { throw 'Signed HAP does not exist' }
    $hapHash = ((wsl -d Ubuntu -- sha256sum $HapWsl) -split '\s+')[0]
    $rawHash = ((wsl -d Ubuntu -- sha256sum "$RepoWsl/entry/src/main/resources/rawfile/wine-data.zip") -split '\s+')[0]
    $embeddedHash = ((wsl -d Ubuntu -- bash -lc "unzip -p '$HapWsl' resources/rawfile/wine-data.zip | sha256sum") -split '\s+')[0]
    if ($rawHash -ne $embeddedHash) { throw 'HAP embedded wine-data.zip hash does not match assembled payload' }

    $smokeList = wsl -d Ubuntu -- unzip -l "$RepoWsl/entry/src/main/resources/rawfile/wine-data.zip"
    foreach ($required in @('smoke/manifest.json', 'smoke/x64/winehua_audio_smoke.exe',
        'smoke/x86/winehua_audio_smoke.exe', 'smoke/x64/winehua_graphics_smoke.exe',
        'smoke/x86/winehua_graphics_smoke.exe', 'smoke/x64/winehua_vulkan_smoke.exe',
        'smoke/x86/winehua_vulkan_smoke.exe',
        'smoke/x64/winehua_d3d11_smoke.exe', 'smoke/x86/winehua_d3d11_smoke.exe',
        'dxvk/manifest.json',
        'dxvk/legacy/x64/d3d11.dll', 'dxvk/legacy/x64/dxgi.dll',
        'dxvk/legacy/x86/d3d11.dll', 'dxvk/legacy/x86/dxgi.dll',
        'bin/guest_vulkan/lib/libvulkan.so.1',
        'bin/guest_vulkan/lib/libvulkan_virtio.so',
        'bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json')) {
        if (-not ($smokeList -match [regex]::Escape($required))) { throw "Payload missing $required" }
    }

    $guestArch = wsl -d Ubuntu -- bash -lc "unzip -p '$RepoWsl/entry/src/main/resources/rawfile/wine-data.zip' bin/guest_gfx/lib/libEGL.so.1 | file -"
    $hostArch = wsl -d Ubuntu -- bash -lc "unzip -p '$HapWsl' libs/arm64-v8a/libentry.so | file -"
    if ($guestArch -notmatch 'x86-64') { throw "Guest EGL architecture invalid: $guestArch" }
    if ($hostArch -notmatch 'ARM aarch64') { throw "Host libentry architecture invalid: $hostArch" }

    $mainCommit = wsl -d Ubuntu -- git -C $RepoWsl rev-parse HEAD
    $submodules = wsl -d Ubuntu -- git -C $RepoWsl submodule status --recursive
    $dirty = wsl -d Ubuntu -- git -C $RepoWsl status --short
    $metadata = [ordered]@{
        schemaVersion = 1
        hap = $HapWsl
        hapTimestampAndSize = $stat
        hapSha256 = $hapHash
        rawfileSha256 = $rawHash
        mainCommit = $mainCommit
        submodules = @($submodules)
        dirtySummary = @($dirty)
        guestArchitecture = $guestArch
        hostArchitecture = $hostArch
    }
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'artifact.json') -Encoding UTF8
    return $metadata
}

function Get-CanonicalCapabilities {
    param([object]$Capabilities)
    return [ordered]@{
        deviceApiVersion = [string]$Capabilities.deviceApiVersion
        pushConstantBytes = [int]$Capabilities.pushConstantBytes
        geometryShader = [bool]$Capabilities.geometryShader
        tessellationShader = [bool]$Capabilities.tessellationShader
        multiDrawIndirect = [bool]$Capabilities.multiDrawIndirect
        descriptorIndexing = [bool]$Capabilities.descriptorIndexing
        scalarBlockLayout = [bool]$Capabilities.scalarBlockLayout
        robustness2 = [bool]$Capabilities.robustness2
        transformFeedback = [bool]$Capabilities.transformFeedback
        shaderInt8 = [bool]$Capabilities.shaderInt8
        shaderInt16 = [bool]$Capabilities.shaderInt16
        shaderInt64 = [bool]$Capabilities.shaderInt64
        timelineSemaphore = [bool]$Capabilities.timelineSemaphore
        synchronization2 = [bool]$Capabilities.synchronization2
        dynamicRendering = [bool]$Capabilities.dynamicRendering
        maintenance4 = [bool]$Capabilities.maintenance4
        maintenance5 = [bool]$Capabilities.maintenance5
        maintenance6 = [bool]$Capabilities.maintenance6
        presentWait = [bool]$Capabilities.presentWait
        swapchainMaintenance = [bool]$Capabilities.swapchainMaintenance
        customBorderColorExtension = [bool]$Capabilities.customBorderColorExtension
        customBorderColors = [bool]$Capabilities.customBorderColors
        customBorderColorWithoutFormat = [bool]$Capabilities.customBorderColorWithoutFormat
        bc1 = [bool]$Capabilities.bc1
        bc2 = [bool]$Capabilities.bc2
        bc3 = [bool]$Capabilities.bc3
        bc4 = [bool]$Capabilities.bc4
        bc5 = [bool]$Capabilities.bc5
        bc6 = [bool]$Capabilities.bc6
        bc7 = [bool]$Capabilities.bc7
        etc2 = [bool]$Capabilities.etc2
        astc4x4 = [bool]$Capabilities.astc4x4
        astc8x8 = [bool]$Capabilities.astc8x8
    }
}

function Get-TextSha256 {
    param([string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function Write-CapabilityMatrix {
    param([string]$RootDirectory, [object[]]$RunRecords)
    $hostRun = $RunRecords | Where-Object { $_.suite -eq 'host-vulkan' -and $_.passed } | Select-Object -Last 1
    $venusRun = $RunRecords | Where-Object { $_.suite -eq 'venus' -and $_.passed } | Select-Object -Last 1
    if (-not $hostRun -or -not $venusRun) { throw 'Capability matrix requires passing Host Vulkan and Venus runs' }

    $hostPath = Join-Path $RootDirectory "$($hostRun.runId)\device-results\host-vulkan.json"
    $venusPath = Join-Path $RootDirectory "$($venusRun.runId)\device-results\venus-offscreen-x64.json"
    $hostResult = Get-Content -Raw -LiteralPath $hostPath | ConvertFrom-Json
    $venusResult = Get-Content -Raw -LiteralPath $venusPath | ConvertFrom-Json
    $hostCanonical = Get-CanonicalCapabilities $hostResult.capabilities
    $venusCanonical = Get-CanonicalCapabilities $venusResult.capabilities
    $hostJson = $hostCanonical | ConvertTo-Json -Compress
    $venusJson = $venusCanonical | ConvertTo-Json -Compress
    $hostHash = Get-TextSha256 $hostJson
    $venusHash = Get-TextSha256 $venusJson
    Write-Utf8NoBom (Join-Path $RootDirectory 'host-capabilities.canonical.json') $hostJson
    Write-Utf8NoBom (Join-Path $RootDirectory 'venus-capabilities.canonical.json') $venusJson

    $differences = @()
    foreach ($property in $hostCanonical.Keys) {
        $hostValue = $hostCanonical[$property]
        $venusValue = $venusCanonical[$property]
        if ("$hostValue" -ne "$venusValue") {
            $differences += [ordered]@{ capability = $property; host = $hostValue; venus = $venusValue }
        }
    }
    $matrix = [ordered]@{
        schemaVersion = 1
        status = 'PASS'
        host = [ordered]@{
            deviceName = $hostResult.capabilities.deviceName
            driverVersion = $hostResult.capabilities.driverVersion
            capabilityHash = $hostHash
            canonical = $hostCanonical
        }
        venus = [ordered]@{
            deviceName = $venusResult.capabilities.deviceName
            driverVersion = $venusResult.capabilities.driverVersion
            capabilityHash = $venusHash
            canonical = $venusCanonical
        }
        differences = $differences
    }
    $matrix | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $RootDirectory 'capability-matrix.json') -Encoding UTF8
    return $matrix
}

function Invoke-OneRun {
    param([string]$RunSuite, [string]$RunPrefix, [string]$RunId, [string]$RootDirectory)
    $runDirectory = Join-Path $RootDirectory $RunId
    New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
    $remoteStable = "$DeviceSandbox/files/automation/results/$RunId/suite-summary.json"
    $remoteHostResults = "$DeviceSandbox/files/automation/results/$RunId"
    $remotePrefix = if ($RunPrefix -eq 'clean') { '.wine-smoke' } else { '.wine' }
    $remoteResults = "$DeviceSandbox/files/$remotePrefix/drive_c/smoke/results/$RunId"

    Invoke-Hdc shell aa force-stop $Bundle | Out-Null
    # HDC shell cannot remove application-owned sandbox files. EntryAbility
    # performs and verifies the clean-prefix reset under the App UID before
    # starting Wayland, wineserver or Wine.
    Invoke-Hdc shell 'power-shell wakeup' | Out-Null
    Invoke-Hdc shell 'hilog -x' | Out-Null
    $startCommand = "aa start -a $Ability -b $Bundle --ps winehua.mode smoke --ps winehua.run_id $RunId --ps winehua.suite $RunSuite --ps winehua.prefix $RunPrefix --ps winehua.perf_profile $PerfProfile --ps winehua.long_seconds $LongSeconds"
    $startOutput = Invoke-Hdc shell $startCommand
    if (($startOutput -join "`n") -match '10106102') {
        # Devices without a credential can be dismissed with one deterministic
        # swipe.  A credential-protected lock remains an infrastructure error.
        Invoke-Hdc shell 'uitest uiInput swipe 1280 1350 1280 300 1200' | Out-Null
        $startOutput = Invoke-Hdc shell $startCommand
    }
    $startOutput | Set-Content -LiteralPath (Join-Path $runDirectory 'start.log') -Encoding UTF8
    if (($startOutput -join "`n") -notmatch 'start ability successfully') {
        throw "Want start failed: $($startOutput -join ' ')"
    }

    $runTimeoutMinutes = if ($RunSuite -eq 'dxvk-long') {
        [Math]::Max($TimeoutMinutes, [Math]::Ceiling($LongSeconds / 60.0) + 5)
    } else { $TimeoutMinutes }
    $deadline = (Get-Date).AddMinutes($runTimeoutMinutes)
    $captured = @{}
    $summaryText = ''
    while ((Get-Date) -lt $deadline) {
        if ($RunSuite -in @('core', 'opengl', 'all', 'long')) {
            foreach ($testId in @('opengl-x64', 'opengl-x86')) {
                if ($captured.ContainsKey($testId)) { continue }
                $remoteResult = "$remoteResults/$testId.json"
                $resultText = Get-DeviceText $remoteResult
                if ($resultText -match '"message"\s*:\s*"fixed-frame"') {
                    $remoteImage = "/data/local/tmp/winehua-$RunId-$testId.jpeg"
                    $localImage = Join-Path $runDirectory "$testId.jpeg"
                    Invoke-Hdc shell snapshot_display -f $remoteImage | Out-Null
                    Save-DeviceFile $remoteImage $localImage
                    Invoke-Hdc shell rm $remoteImage | Out-Null
                    $visualJson = Join-Path $runDirectory "$testId-visual.json"
                    $captured[$testId] = Test-FixedFrame -ImagePath $localImage -JsonPath $visualJson
                }
            }
        }
        if ($RunSuite -in @('dxvk', 'dxvk-long', 'dxvk-dynamic', 'all')) {
            $dxvkTests = if ($RunSuite -eq 'dxvk-dynamic') {
                @('dxvk-dynamic-cb-x86', 'dxvk-dynamic-cb-x64')
            } elseif ($RunSuite -eq 'dxvk-long') {
                @('dxvk-long-x64')
            } else {
                @('dxvk-legacy-x64', 'dxvk-legacy-x86')
            }
            foreach ($testId in $dxvkTests) {
                if ($captured.ContainsKey($testId)) { continue }
                $remoteResult = "$remoteResults/$testId.json"
                $resultText = Get-DeviceText $remoteResult
                if ($resultText -match '"message"\s*:\s*"fixed-frame"') {
                    $remoteImage = "/data/local/tmp/winehua-$RunId-$testId.jpeg"
                    $localImage = Join-Path $runDirectory "$testId.jpeg"
                    Invoke-Hdc shell snapshot_display -f $remoteImage | Out-Null
                    Save-DeviceFile $remoteImage $localImage
                    Invoke-Hdc shell rm $remoteImage | Out-Null
                    $visualJson = Join-Path $runDirectory "$testId-visual.json"
                    $captured[$testId] = Capture-D3D11Frame -RemoteImage $remoteImage -LocalImage $localImage -JsonPath $visualJson
                }
            }
        }
        if ($RunSuite -eq 'host-vulkan' -and -not $captured.ContainsKey('host-vulkan')) {
            $hostResultText = Get-DeviceText "$remoteHostResults/host-vulkan.json"
            if ($hostResultText -match '"message"\s*:\s*"fixed-frame"') {
                $remoteImage = "/data/local/tmp/winehua-$RunId-host-vulkan.jpeg"
                $localImage = Join-Path $runDirectory 'host-vulkan.jpeg'
                Invoke-Hdc shell snapshot_display -f $remoteImage | Out-Null
                Save-DeviceFile $remoteImage $localImage
                Invoke-Hdc shell rm $remoteImage | Out-Null
                $visualJson = Join-Path $runDirectory 'host-vulkan-visual.json'
                $captured['host-vulkan'] = Test-FixedFrame -ImagePath $localImage -JsonPath $visualJson
            }
        }
        $summaryText = Get-DeviceText $remoteStable
        if ($summaryText) { break }
        Start-Sleep -Milliseconds 500
    }

    if (-not $summaryText) { throw "Suite $RunId timed out without suite-summary.json" }
    $summaryPath = Join-Path $runDirectory 'suite-summary.json'
    $summaryText | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $summary = $summaryText | ConvertFrom-Json

    if ($RunSuite -in @('core', 'opengl', 'all', 'long')) {
        foreach ($testId in @('opengl-x64', 'opengl-x86')) {
            if (-not $captured.ContainsKey($testId)) { $captured[$testId] = $false }
        }
    }
    if ($RunSuite -in @('dxvk', 'dxvk-long', 'dxvk-dynamic', 'all')) {
        $dxvkTests = if ($RunSuite -eq 'dxvk-dynamic') {
            @('dxvk-dynamic-cb-x86', 'dxvk-dynamic-cb-x64')
        } elseif ($RunSuite -eq 'dxvk-long') {
            @('dxvk-long-x64')
        } else {
            @('dxvk-legacy-x64', 'dxvk-legacy-x86')
        }
        foreach ($testId in $dxvkTests) {
            if (-not $captured.ContainsKey($testId)) { $captured[$testId] = $false }
        }
    }
    if ($RunSuite -eq 'host-vulkan' -and -not $captured.ContainsKey('host-vulkan')) {
        $captured['host-vulkan'] = $false
    }

    (& $Hdc -t $script:DeviceId shell 'hilog -z 10000 -t app') | Set-Content -LiteralPath (Join-Path $runDirectory 'hilog.txt') -Encoding UTF8
    Save-DeviceFile "$DeviceSandbox/temp/wine_stderr_$(Get-Date -Format yyyyMMdd).log" (Join-Path $runDirectory 'wine-stderr.log')
    Save-DeviceFile "$DeviceSandbox/cache/winehua_virgl_host.log" (Join-Path $runDirectory 'virgl-host.log')
    Save-DeviceFile "$DeviceSandbox/temp/winehua_vtest_frontbuffer.log" (Join-Path $runDirectory 'vtest-frontbuffer.log')
    if ($RunSuite -eq 'host-vulkan') {
        Save-DeviceFile $remoteHostResults (Join-Path $runDirectory 'device-results')
    } else {
        Save-DeviceFile $remoteResults (Join-Path $runDirectory 'device-results')
    }

    $customBorderSelections = @()
    $wineStderrPath = Join-Path $runDirectory 'wine-stderr.log'
    if (Test-Path -LiteralPath $wineStderrPath) {
        foreach ($match in Select-String -LiteralPath $wineStderrPath -Pattern 'custom-border path=([a-z-]+) reason=([a-zA-Z0-9_-]+)') {
            $path = $match.Matches[0].Groups[1].Value
            $reason = $match.Matches[0].Groups[2].Value
            $key = "$path|$reason"
            if (-not ($customBorderSelections | Where-Object { $_.key -eq $key })) {
                $customBorderSelections += [ordered]@{ key = $key; path = $path; reason = $reason }
            }
        }
    }

    $visualPass = -not ($captured.Values -contains $false)
    $coverage = if ($RunSuite -in @('dxvk', 'dxvk-long', 'dxvk-dynamic', 'all')) {
        Get-D3D11Coverage -Summary $summary -RunSuite $RunSuite
    } else { $null }
    $coveragePass = $null -eq $coverage -or $coverage.status -eq 'PASS'
    $hostSummary = [ordered]@{
        schemaVersion = 1
        runId = $RunId
        suite = $RunSuite
        prefix = $RunPrefix
        perfProfile = $PerfProfile
        appStatus = $summary.status
        visualStatus = if ($visualPass) { 'PASS' } else { 'FAIL' }
        coverageStatus = if ($null -eq $coverage) { 'NOT_APPLICABLE' } else { $coverage.status }
        visuals = $captured
        coverage = $coverage
        customBorderSelections = @($customBorderSelections | ForEach-Object {
            [ordered]@{ path = $_.path; reason = $_.reason }
        })
        status = if ($summary.status -eq 'PASS' -and $visualPass -and $coveragePass) { 'PASS' } else { 'FAIL' }
    }
    $hostSummary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDirectory 'host-summary.json') -Encoding UTF8
    return $hostSummary.status -eq 'PASS'
}

if (-not (Test-Path -LiteralPath $Hdc)) { throw "Windows HDC not found: $Hdc" }
if (-not (Test-Path -LiteralPath $HapWindows) -and $SkipBuild) { throw 'Signed HAP missing while -SkipBuild was requested' }
if ($Runs -lt 1) { throw '-Runs must be at least 1' }

if (-not $DeviceId) {
    $targets = @(& $Hdc list targets | ForEach-Object { "$($_)".Trim() } |
        Where-Object { $_ -and $_ -notmatch '^\[' })
    # Prefer a physical target when HDC also exposes the local forwarding/emulator
    # target. An ARM64 HAP is intentionally rejected by the x86 localhost target.
    $physicalTargets = @($targets | Where-Object {
        $_ -notmatch '^(127\.0\.0\.1|localhost)(:|$)'
    })
    $DeviceId = if ($physicalTargets.Count -gt 0) {
        $physicalTargets[0]
    } elseif ($targets.Count -gt 0) {
        $targets[0]
    } else {
        ''
    }
}
if (-not $DeviceId) { throw 'No HDC device is connected' }
$script:DeviceId = $DeviceId

$sessionId = "phase2-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$sessionDirectory = Join-Path $ArchiveRoot $sessionId
New-Item -ItemType Directory -Force -Path $sessionDirectory | Out-Null

if (-not $SkipBuild) { Invoke-Build -LogPath (Join-Path $sessionDirectory 'build.log') }
$artifact = Get-ArtifactMetadata -OutputDirectory $sessionDirectory
$installOutput = & $Hdc -t $DeviceId install -r $HapWindows 2>&1
$installOutput | Set-Content -LiteralPath (Join-Path $sessionDirectory 'install.log') -Encoding UTF8
if ($LASTEXITCODE -ne 0 -or ($installOutput -join "`n") -notmatch 'install bundle successfully') {
    throw 'HAP overwrite install did not report install bundle successfully'
}

if ($Suite -in @('venus-heaven-material', 'venus-heaven-material-layout')) {
    if (-not $ReplayFragmentSpv -or -not (Test-Path -LiteralPath $ReplayFragmentSpv)) {
        throw 'venus-heaven-material requires -ReplayFragmentSpv pointing to the captured final SPIR-V'
    }
    if (-not $ReplayVertexSpv -or -not (Test-Path -LiteralPath $ReplayVertexSpv)) {
        throw 'venus-heaven-material requires -ReplayVertexSpv pointing to the captured/remapped VS SPIR-V'
    }
    # /data/local/tmp is visible to the HDC shell but not to the App's
    # sandboxed native child. Stage the captured shaders in the app-owned temp
    # directory and let SmokeRunner use its logical storage alias.
    $remoteReplaySpv = "$DeviceSandbox/temp/winehua_heaven_final_fs.spv"
    $remoteReplayVertexSpv = "$DeviceSandbox/temp/winehua_heaven_final_vs.spv"
    $sendOutput = & $Hdc -t $DeviceId file send $ReplayFragmentSpv $remoteReplaySpv 2>&1
    $sendOutput | Set-Content -LiteralPath (Join-Path $sessionDirectory 'replay-shader-send.log') -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { throw 'Failed to stage the external replay SPIR-V on the device' }
    $localReplayHash = (Get-FileHash -LiteralPath $ReplayFragmentSpv -Algorithm SHA256).Hash.ToLowerInvariant()
    $deviceReplayHashLine = (& $Hdc -t $DeviceId shell sha256sum $remoteReplaySpv 2>$null | Select-Object -First 1)
    $deviceReplayHash = if ($deviceReplayHashLine) { ("$deviceReplayHashLine" -split '\s+')[0].ToLowerInvariant() } else { '' }
    if ($deviceReplayHash -and $deviceReplayHash -match '^[0-9a-f]{64}$' -and
        $deviceReplayHash -ne $localReplayHash) {
        throw "External replay SPIR-V hash mismatch: local=$localReplayHash device=$deviceReplayHash"
    }
    $sendVertexOutput = & $Hdc -t $DeviceId file send $ReplayVertexSpv $remoteReplayVertexSpv 2>&1
    $sendVertexOutput | Set-Content -LiteralPath (Join-Path $sessionDirectory 'replay-vertex-send.log') -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { throw 'Failed to stage the external replay vertex SPIR-V on the device' }
    $localVertexHash = (Get-FileHash -LiteralPath $ReplayVertexSpv -Algorithm SHA256).Hash.ToLowerInvariant()
    $deviceVertexHashLine = (& $Hdc -t $DeviceId shell sha256sum $remoteReplayVertexSpv 2>$null | Select-Object -First 1)
    $deviceVertexHash = if ($deviceVertexHashLine) { ("$deviceVertexHashLine" -split '\s+')[0].ToLowerInvariant() } else { '' }
    if ($deviceVertexHash -and $deviceVertexHash -match '^[0-9a-f]{64}$' -and
        $deviceVertexHash -ne $localVertexHash) {
        throw "External replay vertex SPIR-V hash mismatch: local=$localVertexHash device=$deviceVertexHash"
    }
    Copy-Item -LiteralPath $ReplayFragmentSpv -Destination (Join-Path $sessionDirectory 'heaven-final-fragment.spv')
    Copy-Item -LiteralPath $ReplayVertexSpv -Destination (Join-Path $sessionDirectory 'heaven-final-vertex.spv')
}

$matrix = @()
if ($Gate) {
    1..3 | ForEach-Object { $matrix += ,@('core', 'reuse') }
    $matrix += ,@('core', 'clean')
} elseif ($Suite -eq 'capabilities') {
    $matrix += ,@('host-vulkan', 'reuse')
    $matrix += ,@('venus', 'reuse')
} else {
    1..$Runs | ForEach-Object { $matrix += ,@($Suite, $Prefix) }
}

$allPassed = $true
$runRecords = @()
$index = 0
foreach ($entry in $matrix) {
    $index++
    $runSuite = $entry[0]
    $runPrefix = $entry[1]
    $runId = "$sessionId-$('{0:D2}' -f $index)-$runSuite-$runPrefix"
    try {
        $passed = Invoke-OneRun -RunSuite $runSuite -RunPrefix $runPrefix -RunId $runId -RootDirectory $sessionDirectory
    } catch {
        $passed = $false
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $sessionDirectory "$runId-infrastructure-error.txt") -Encoding UTF8
    } finally {
        # Smoke uses the singleton EntryAbility with winehua.mode=smoke.  If it
        # remains alive, a later icon launch is delivered through onNewWant and
        # can retain the automation page/window state instead of rebuilding the
        # normal Tablet desktop.  Stop only our test app after all result and
        # screenshot collection for this run has completed; the next run (or a
        # normal user launch) will then start from the correct mode boundary.
        Invoke-Hdc shell aa force-stop $Bundle | Out-Null
    }
    $runRecords += [ordered]@{ runId = $runId; suite = $runSuite; prefix = $runPrefix; passed = $passed }
    if (-not $passed) { $allPassed = $false }
}

$capabilityMatrix = $null
if ($Suite -eq 'capabilities') {
    try {
        $capabilityMatrix = Write-CapabilityMatrix -RootDirectory $sessionDirectory -RunRecords $runRecords
    } catch {
        $allPassed = $false
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $sessionDirectory 'capability-matrix-error.txt') -Encoding UTF8
    }
}

[ordered]@{
    schemaVersion = 1
    sessionId = $sessionId
    deviceId = $DeviceId
    hapSha256 = $artifact.hapSha256
    gate = [bool]$Gate
    perfProfile = $PerfProfile
    status = if ($allPassed) { 'PASS' } else { 'FAIL' }
    runs = $runRecords
    capabilityHashes = if ($capabilityMatrix) {
        [ordered]@{ host = $capabilityMatrix.host.capabilityHash; venus = $capabilityMatrix.venus.capabilityHash }
    } else { $null }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $sessionDirectory 'automation-summary.json') -Encoding UTF8

$finalLabel = if ($allPassed) { 'PASS' } else { 'FAIL' }
Write-Host "Automation ${finalLabel}: $sessionDirectory"
if (-not $allPassed) { exit 1 }
