import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const FILES_BASE = '/data/storage/el2/base/files';
const WINE_ROOT = `${FILES_BASE}/wine`;
const GUEST_VULKAN_ROOT = `${WINE_ROOT}/bin/guest_vulkan`;
const AUTOMATION_RESULTS = `${FILES_BASE}/automation/results`;
const CACHE_BASE = '/data/storage/el2/base/cache';
const TEMP_BASE = '/data/storage/el2/base/temp';

export interface SmokeLaunchRequest {
  mode: string;
  runId: string;
  suite: string;
  prefixMode: string;
  longSeconds: number;
}

interface SmokeTestSpec {
  id: string;
  architecture: string;
  executable: string;
  seconds: number;
  timeoutMs: number;
}

interface ProcessInfo {
  found: boolean;
  pid: number;
  status: string;
  startTimestamp: number;
  endTimestamp: number;
  exitCode: number | null;
  exitCodeSource: string;
}

interface SmokeResult {
  status: string;
  stage: string;
  message: string;
  heartbeatTimestampMs: number;
}

interface SmokeSuiteTestResult {
  schemaVersion?: number;
  runId?: string;
  testId?: string;
  status: string;
  stage: string;
  message: string;
  pid?: number;
  exitCode?: number;
  exitCodeSource?: string;
  heartbeatTimestampMs?: number;
}

interface SmokeSuiteSummary {
  schemaVersion: number;
  runId: string;
  suite: string;
  prefixMode: string;
  status: string;
  stage?: string;
  message?: string;
  completedTimestampMs?: number;
  tests: SmokeSuiteTestResult[];
}

function pathExists(path: string): boolean {
  try {
    return fs.accessSync(path);
  } catch (_) {
    return false;
  }
}

function ensureDirectory(path: string): void {
  if (!pathExists(path)) fs.mkdirSync(path, true);
}

function removeRecursive(path: string): void {
  if (!pathExists(path)) return;
  const stat = fs.lstatSync(path);
  if (stat.isSymbolicLink() || !stat.isDirectory()) {
    fs.unlinkSync(path);
    return;
  }
  for (const name of fs.listFileSync(path)) removeRecursive(`${path}/${name}`);
  fs.rmdirSync(path);
}

function copyRecursive(source: string, target: string): void {
  const stat = fs.statSync(source);
  if (!stat.isDirectory()) {
    const slash = target.lastIndexOf('/');
    if (slash > 0) ensureDirectory(target.substring(0, slash));
    fs.copyFileSync(source, target);
    return;
  }
  ensureDirectory(target);
  for (const name of fs.listFileSync(source)) copyRecursive(`${source}/${name}`, `${target}/${name}`);
}

function readText(path: string): string {
  try {
    return fs.readTextSync(path, { encoding: 'utf-8' }).trim();
  } catch (_) {
    return '';
  }
}

function writeAtomic(path: string, text: string): void {
  const slash = path.lastIndexOf('/');
  if (slash > 0) ensureDirectory(path.substring(0, slash));
  const temporary = `${path}.tmp.${Date.now()}`;
  const file = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(file.fd, text);
  } finally {
    fs.closeSync(file);
  }
  if (pathExists(path)) fs.unlinkSync(path);
  fs.renameSync(temporary, path);
}

function resetAutomationLogs(): void {
  const now = new Date();
  const twoDigits = (value: number): string => value < 10 ? `0${value}` : `${value}`;
  const day = `${now.getFullYear()}${twoDigits(now.getMonth() + 1)}${twoDigits(now.getDate())}`;
  const paths = [
    `${TEMP_BASE}/wine_stderr_${day}.log`,
    `${TEMP_BASE}/winehua_vtest_frontbuffer.log`,
    `${CACHE_BASE}/winehua_virgl_host.log`
  ];
  for (const path of paths) {
    try {
      const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      fs.closeSync(file);
    } catch (error) {
      hilog.warn(DOMAIN, 'SmokeRunner', 'log reset failed path=%{public}s error=%{public}s',
        path, JSON.stringify(error));
    }
  }
}

function safeId(value: string, fallback: string): string {
  const normalized = value.replace(/[^A-Za-z0-9_.-]/g, '_');
  return normalized.length > 0 ? normalized.substring(0, 80) : fallback;
}

function prefixRoot(prefixMode: string): string {
  return prefixMode === 'clean' ? `${FILES_BASE}/.wine-smoke` : `${FILES_BASE}/.wine`;
}

function hasReplayShader(directory: string): boolean {
  try {
    return fs.listFileSync(directory).some((name: string) =>
      name.indexOf('CS_') === 0 && name.indexOf('.remapped.spv') >= 0);
  } catch (_) {
    return false;
  }
}

/* A DXVK run writes its SPIR-V dumps under C:\smoke\results\<run-id>.
 * Replay is a separate process/suite so Venus can start from a fresh render
 * server after DXVK teardown; select the newest completed dump directory. */
function latestReplayShaderDirectory(prefix: string, current: string): string {
  const root = `${prefix}/drive_c/smoke/results`;
  try {
    const names = fs.listFileSync(root).sort().reverse();
    for (const name of names) {
      const directory = `${root}/${name}`;
      if (directory === current || !pathExists(directory)) continue;
      if (hasReplayShader(directory)) return directory;
    }
  } catch (_) {}
  return current;
}

export function defaultSmokeRequest(): SmokeLaunchRequest {
  return { mode: '', runId: '', suite: 'core', prefixMode: 'reuse', longSeconds: 3600 };
}

export function parseSmokeRequest(text: string): SmokeLaunchRequest {
  const fallback = defaultSmokeRequest();
  if (!text) return fallback;
  try {
    const value = JSON.parse(text) as SmokeLaunchRequest;
    return {
      mode: value.mode === 'smoke' || value.mode === 'game' ? value.mode : '',
      runId: safeId(value.runId, `${Date.now()}`),
      suite: safeId(value.suite, 'core').toLowerCase(),
      prefixMode: value.prefixMode === 'clean' ? 'clean' : 'reuse',
      longSeconds: value.longSeconds >= 60 && value.longSeconds <= 3600 ?
        Math.floor(value.longSeconds) : 3600
    };
  } catch (_) {
    return fallback;
  }
}

export function syncManagedSmoke(prefixMode: string): boolean {
  const source = `${WINE_ROOT}/smoke`;
  const target = `${prefixRoot(prefixMode)}/drive_c/smoke`;
  const sourceManifest = readText(`${source}/manifest.json`);
  if (!sourceManifest) {
    hilog.error(DOMAIN, 'SmokeRunner', 'managed smoke manifest missing: %{public}s', source);
    return false;
  }
  /* A previous interrupted extraction can leave a matching manifest but a
   * partially populated C:\smoke tree.  Validate the managed entry points
   * before taking the fast path; DXVK x64 otherwise fails as a misleading
   * "d3d11.dll not found" while the x86 slot happens to exist. */
  const required = [
    'x64/winehua_audio_smoke.exe', 'x64/winehua_graphics_smoke.exe',
    'x64/winehua_vulkan_smoke.exe', 'x64/winehua_d3d11_smoke.exe',
    'x64/winehua_d3d_switch_cube.exe', 'x64/winehua_win32_driver.exe',
    'x86/winehua_audio_smoke.exe', 'x86/winehua_graphics_smoke.exe',
    'x86/winehua_vulkan_smoke.exe', 'x86/winehua_d3d11_smoke.exe',
    'x86/winehua_d3d_switch_cube.exe', 'x86/winehua_win32_driver.exe',
    'assets/venus_storage_write.spv', 'assets/venus_storage_read.spv',
    'assets/venus_image_fetch.spv', 'assets/venus_combined_sample.spv',
    'assets/venus_separated_sample.spv',
  ];
  /* DXVK is a WineHua runtime overlay, not part of C:\\smoke.  Remove the
   * four files from older payloads, but only inside this managed directory. */
  for (const file of ['x64/d3d11.dll', 'x64/dxgi.dll', 'x86/d3d11.dll', 'x86/dxgi.dll']) {
    try {
      const stale = `${target}/${file}`;
      if (pathExists(stale)) fs.unlinkSync(stale);
    } catch (error) {
      hilog.warn(DOMAIN, 'SmokeRunner', 'stale managed DXVK cleanup failed path=%{public}s error=%{public}s',
        `${target}/${file}`, JSON.stringify(error));
    }
  }
  let complete = sourceManifest === readText(`${target}/manifest.json`);
  for (const file of required) {
    if (!pathExists(`${target}/${file}`)) {
      complete = false;
      break;
    }
  }
  if (complete) return true;

  try {
    removeRecursive(target);
    ensureDirectory(target);
    /* Keep the runtime-owned dxvk/ directory out of the prefix. */
    copyRecursive(`${source}/manifest.json`, `${target}/manifest.json`);
    for (const file of required) copyRecursive(`${source}/${file}`, `${target}/${file}`);
    if (pathExists(`${source}/assets`)) copyRecursive(`${source}/assets`, `${target}/assets`);
    ensureDirectory(`${target}/results`);
    hilog.info(DOMAIN, 'SmokeRunner', 'managed smoke synchronized prefix=%{public}s', prefixMode);
    return sourceManifest === readText(`${target}/manifest.json`);
  } catch (error) {
    hilog.error(DOMAIN, 'SmokeRunner', 'managed smoke sync failed: %{public}s', JSON.stringify(error));
    return false;
  }
}

/* Validate the versioned DXVK payload without injecting it into the prefix's
 * system32/syswow64 directories.  Native D3D DLLs belong to the selected game
 * or smoke application directory; the Wine loader supplies the normal
 * system/builtin fallback for every other process. */
function validateManagedDxvk(): boolean {
  const source = `${WINE_ROOT}/smoke/dxvk/legacy`;
  const required = [
    `${source}/x64/d3d11.dll`, `${source}/x64/dxgi.dll`,
    `${source}/x86/d3d11.dll`, `${source}/x86/dxgi.dll`
  ];
  try {
    for (const file of required) if (!pathExists(file)) return false;
    return true;
  } catch (error) {
    hilog.error(DOMAIN, 'SmokeRunner', 'managed DXVK payload validation failed: %{public}s', JSON.stringify(error));
    return false;
  }
}

function sleep(milliseconds: number): Promise<void> {
  return new Promise<void>((resolve) => setTimeout(resolve, milliseconds));
}

export class SmokeRunner {
  private request: SmokeLaunchRequest;
  private onStatus: (text: string) => void;
  private perfProfile: string;
  private prefix: string;
  private resultDirectory: string;

  constructor(request: SmokeLaunchRequest, onStatus: (text: string) => void,
              perfProfile: string = 'baseline') {
    this.request = request;
    this.onStatus = onStatus;
    this.perfProfile = perfProfile;
    this.prefix = prefixRoot(request.prefixMode);
    this.resultDirectory = `${this.prefix}/drive_c/smoke/results/${request.runId}`;
  }

  private specs(): SmokeTestSpec[] {
    const tests: SmokeTestSpec[] = [];
    const includeAudio = this.request.suite === 'core' || this.request.suite === 'audio' ||
      this.request.suite === 'all' || this.request.suite === 'long';
    const includeOpenGl = this.request.suite === 'core' || this.request.suite === 'opengl' ||
      this.request.suite === 'all' || this.request.suite === 'long';
    const includeVenus = this.request.suite === 'venus' || this.request.suite === 'all';
    const includeVenusSampled = this.request.suite === 'venus-sampled' || this.request.suite === 'all';
    /* queue-idle is a separate diagnostic.  Current Venus/vtest builds can
     * return VK_SUCCESS from queue/device idle before the mapped write is
     * observable; it must not poison the sampled-image qualification suite. */
    const includeVenusSampledIdle = this.request.suite === 'venus-sampled-idle';
    const includeVenusDepthCube = this.request.suite === 'venus-depth-cube';
    const includeVenusDepthCubeArray2D =
      this.request.suite === 'venus-depth-cube-array-2d-golden';
    const includeVenusDepthCubeGraphics = this.request.suite === 'venus-depth-cube-graphics';
    const includeWineVulkan = this.request.suite === 'wine-vulkan' ||
      this.request.suite === 'all';
    const includeWineVulkanPresent = this.request.suite === 'wine-vulkan-present' ||
      this.request.suite === 'all';
    const includeDxvk = this.request.suite === 'dxvk' || this.request.suite === 'all';
    const includeDxvkLong = this.request.suite === 'dxvk-long';
    const includeDxvkReplay = this.request.suite === 'dxvk-replay';
      const includeDxvkLayoutGeneral = this.request.suite === 'dxvk-layout-general';
      const includeDxvkCombined = this.request.suite === 'dxvk-combined';
    const includeDxvkDynamic = this.request.suite === 'dxvk-dynamic';
    if (includeAudio) {
      tests.push({ id: 'audio-x64', architecture: 'x64', executable: 'winehua_audio_smoke.exe',
        seconds: 3, timeoutMs: 45000 });
      tests.push({ id: 'audio-x86', architecture: 'x86', executable: 'winehua_audio_smoke.exe',
        seconds: 3, timeoutMs: 45000 });
    }
    if (includeOpenGl) {
      const seconds = this.request.suite === 'long' ? 3600 : 8;
      const timeout = this.request.suite === 'long' ? 3660000 : 60000;
      tests.push({ id: 'opengl-x64', architecture: 'x64', executable: 'winehua_graphics_smoke.exe',
        seconds: seconds, timeoutMs: timeout });
      tests.push({ id: 'opengl-x86', architecture: 'x86', executable: 'winehua_graphics_smoke.exe',
        seconds: seconds, timeoutMs: timeout });
    }
    if (includeVenus) {
      tests.push({ id: 'venus-offscreen-x64', architecture: 'guest-x64',
        executable: 'winehua_guest_vulkan_smoke', seconds: 0, timeoutMs: 60000 });
    }
    if (includeVenusSampled) {
      tests.push({ id: 'venus-sampled-normal-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-sampled-general-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-sampled-only-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
    }
    if (includeVenusSampledIdle) {
      tests.push({ id: 'venus-sampled-idle-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
    }
    if (includeVenusDepthCube) {
      tests.push({ id: 'venus-depth-cube-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 60000 });
      tests.push({ id: 'venus-depth-cube-array-2d-golden-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 60000 });
      tests.push({ id: 'venus-depth-cube-array-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 60000 });
    }
    if (includeVenusDepthCubeArray2D) {
      tests.push({ id: 'venus-depth-cube-array-2d-golden-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 60000 });
    }
    if (includeVenusDepthCubeGraphics) {
      tests.push({ id: 'venus-depth-cube-graphics-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-golden-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-golden-dxvk-contract-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-initialized-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-optimized-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-coordinate-trace-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-padded-dref-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'venus-depth-cube-graphics-no-float-controls-x64', architecture: 'guest-x64',
        executable: 'venus_sampled_image_probe', seconds: 0, timeoutMs: 90000 });
    }
    if (includeWineVulkan) {
      tests.push({ id: 'wine-vulkan-offscreen-x64', architecture: 'x64',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'wine-vulkan-offscreen-x86', architecture: 'x86',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'wine-vulkan-sampled-only-x64', architecture: 'x64',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
      tests.push({ id: 'wine-vulkan-sampled-only-x86', architecture: 'x86',
        executable: 'winehua_vulkan_smoke.exe', seconds: 0, timeoutMs: 90000 });
    }
    if (includeWineVulkanPresent) {
      tests.push({ id: 'wine-vulkan-present-x64', architecture: 'x64',
        executable: 'winehua_vulkan_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'wine-vulkan-present-x86', architecture: 'x86',
        executable: 'winehua_vulkan_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    if (includeDxvk) {
      /* On the current Harmony Venus renderer, creating an x64 DXVK device
       * and immediately creating the WoW64/x86 device can occasionally return
       * VK_ERROR_DEVICE_LOST while the host driver retires the first device.
       * Running the WoW64 case first is deterministic; the handoff wait above
       * still keeps both contexts strictly serial. */
      tests.push({ id: 'dxvk-legacy-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-legacy-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-cube-x64', architecture: 'x64',
        executable: 'winehua_d3d_switch_cube.exe', seconds: 8, timeoutMs: 180000 });
    }
    if (includeDxvkLong) {
      tests.push({ id: 'dxvk-long-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: this.request.longSeconds,
        timeoutMs: this.request.longSeconds * 1000 + 90000 });
    }
    if (includeDxvkReplay) {
      tests.push({ id: 'dxvk-exact-replay-cs', architecture: 'guest-x64',
        executable: 'venus_spirv_replay', seconds: 0, timeoutMs: 120000 });
    }
    if (includeDxvkLayoutGeneral) {
      tests.push({ id: 'dxvk-layout-general-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-layout-general-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    if (includeDxvkCombined) {
      tests.push({ id: 'dxvk-combined-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-combined-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    if (includeDxvkDynamic) {
      tests.push({ id: 'dxvk-dynamic-cb-x86', architecture: 'x86',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
      tests.push({ id: 'dxvk-dynamic-cb-x64', architecture: 'x64',
        executable: 'winehua_d3d11_smoke.exe', seconds: 5, timeoutMs: 180000 });
    }
    return tests;
  }

  private finalStatus(result: SmokeResult): boolean {
    return result.status === 'PASS' || result.status === 'FAIL' || result.status === 'SKIP' ||
      result.status === 'UNSUPPORTED';
  }

  private failureResult(spec: SmokeTestSpec, stage: string, message: string,
                        process: ProcessInfo | null): SmokeSuiteTestResult {
    return {
      schemaVersion: 1,
      runId: this.request.runId,
      testId: spec.id,
      status: 'FAIL',
      stage: stage,
      message: message,
      pid: process ? process.pid : -1,
      exitCode: process && process.exitCode !== null ? process.exitCode : -1,
      exitCodeSource: process ? process.exitCodeSource : 'unknown',
      heartbeatTimestampMs: Date.now()
    };
  }

  private async waitForFinal(spec: SmokeTestSpec, resultPath: string,
                             process: ProcessInfo): Promise<SmokeSuiteTestResult> {
    const deadline = Date.now() + spec.timeoutMs;
    while (Date.now() < deadline) {
      const text = readText(resultPath);
      if (text) {
        try {
          const result = JSON.parse(text) as SmokeResult;
          if (this.finalStatus(result)) {
            /* A result is authoritative, but it is not proof that the NCP
             * child has finished tearing down its Vulkan context.  In
             * particular, vkDestroyDevice may drain Venus work after the
             * smoke JSON has been atomically written.  Do not start the next
             * x86/x64 case while that context is still alive: concurrent
             * contexts can make the Harmony Venus ring report a fatal seqno
             * wait.  Give normal teardown a bounded grace period, then kill
             * the exact process returned by runWineProgram and wait for it to
             * disappear before returning the result.
             */
            for (let i = 0; i < 40; ++i) {
              const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
              if (!current.found || current.status !== 'running') break;
              await sleep(250);
            }
            const stillRunning = testNapi.queryWineProcess(process.pid) as ProcessInfo;
            if (stillRunning.found && stillRunning.status === 'running') {
              hilog.warn(DOMAIN, 'SmokeRunner',
                'final result arrived before process teardown pid=%{public}d test=%{public}s; terminating',
                process.pid, spec.id);
              testNapi.terminateWineProcess(process.pid);
              for (let i = 0; i < 20; ++i) {
                const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
                if (!current.found || current.status !== 'running') break;
                await sleep(250);
                }
            }
            if (spec.id.indexOf('wine-vulkan-present') >= 0 || spec.id.indexOf('dxvk-legacy') >= 0) {
              /* Process exit closes the vtest socket first; the Venus render
               * server frees the context asynchronously.  Leave a bounded
               * handoff window so the next architecture does not reuse the
               * ring while disconnect cleanup is still in flight. */
              await sleep(8000);
            }
            const parsed = JSON.parse(text) as SmokeSuiteTestResult;
            parsed.exitCodeSource = 'result';
            return parsed;
          }
        } catch (_) {}
      }
      const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
      if (!current.found || current.status !== 'running') {
        const detail = text ?
          'process disappeared with only a non-final result' :
          'process disappeared without an authoritative result';
        return this.failureResult(spec, 'crash', detail, current);
      }
      await sleep(500);
    }

    testNapi.terminateWineProcess(process.pid);
    for (let i = 0; i < 20; ++i) {
      const current = testNapi.queryWineProcess(process.pid) as ProcessInfo;
      if (!current.found || current.status !== 'running') break;
      await sleep(250);
    }
    if (spec.id.indexOf('wine-vulkan-present') >= 0 || spec.id.indexOf('dxvk-legacy') >= 0)
      await sleep(8000);
    return this.failureResult(spec, 'timeout', `test exceeded ${spec.timeoutMs} ms`, process);
  }

  private async runOne(spec: SmokeTestSpec): Promise<SmokeSuiteTestResult> {
    const resultPath = `${this.resultDirectory}/${spec.id}.json`;
    if (pathExists(resultPath)) fs.unlinkSync(resultPath);
    this.onStatus(`Smoke: ${spec.id}`);

    if (spec.architecture === 'guest-x64') {
      const executablePath = `${GUEST_VULKAN_ROOT}/bin/${spec.executable}`;
      if (!pathExists(`${GUEST_VULKAN_ROOT}/manifest.json`) || !pathExists(executablePath))
        return this.failureResult(spec, 'startup', 'managed Guest Vulkan runtime is missing', null);
      const currentReplayDirectory = `${this.prefix}/drive_c/smoke/results/${this.request.runId}`;
      const replaySpvDirectory = spec.executable === 'venus_spirv_replay' &&
        !hasReplayShader(currentReplayDirectory) ?
        latestReplayShaderDirectory(this.prefix, currentReplayDirectory) : currentReplayDirectory;
      const guestProcess = testNapi.runGuestProgram({
        executablePath: executablePath,
        argv: ['--automation', '--run-id', this.request.runId, '--test-id', spec.id,
          '--result', resultPath,
          ...(spec.id.indexOf('general') >= 0 ? ['--layout', 'general'] : []),
          ...(spec.id.indexOf('sampled-only') >= 0 ? ['--sampled-only'] : []),
          ...(spec.id.indexOf('idle') >= 0 ? ['--force-idle'] : []),
          ...(spec.id === 'venus-depth-cube-x64' ? ['--depth-cube'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-x64' ?
            ['--depth-cube-graphics'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-golden-x64' ?
            ['--depth-cube-graphics-golden'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-golden-dxvk-contract-x64' ?
            ['--depth-cube-graphics-golden-dxvk-contract'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-initialized-x64' ?
            ['--depth-cube-graphics-initialized'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-optimized-x64' ?
            ['--depth-cube-graphics-optimized'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-coordinate-trace-x64' ?
            ['--depth-cube-graphics-coordinate-trace'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-padded-dref-x64' ?
            ['--depth-cube-graphics-padded-dref'] : []),
          ...(spec.id === 'venus-depth-cube-graphics-no-float-controls-x64' ?
            ['--depth-cube-graphics-no-float-controls'] : []),
          ...(spec.id === 'venus-depth-cube-array-x64' ?
            ['--depth-cube-array'] : []),
          ...(spec.id === 'venus-depth-cube-array-2d-golden-x64' ?
            ['--depth-cube-array-2d-golden'] : []),
          ...(spec.executable === 'venus_spirv_replay' ? ['--spv-dir', replaySpvDirectory] : [])],
        environment: {
          'WINEHUA_SMOKE_RUN_ID': this.request.runId,
          'WINEHUA_SMOKE_TEST_ID': spec.id,
          /* Guest and Host map different shadow files. Functional probes use
           * explicit flush/invalidate for their tiny readback allocations. */
          'VN_WINEHUA_REMOTE_MEMORY_SYNC': '1'
        },
        workingDirectory: GUEST_VULKAN_ROOT,
        automationMode: true
      }) as ProcessInfo;
      if (!guestProcess.found || guestProcess.pid <= 0)
        return this.failureResult(spec, 'startup', 'runGuestProgram failed to return a PID', guestProcess);
      return this.waitForFinal(spec, resultPath, guestProcess);
    }

    const windowsResult = `C:\\smoke\\results\\${this.request.runId}\\${spec.id}.json`;
    const isWineVulkan = spec.executable === 'winehua_vulkan_smoke.exe';
    const isWineVulkanPresent = isWineVulkan && spec.id.indexOf('present') >= 0;
    const isDxvk = spec.executable === 'winehua_d3d11_smoke.exe' ||
      spec.executable === 'winehua_d3d_switch_cube.exe';
    let process: ProcessInfo;
    if (isWineVulkan || isDxvk) {
      const guestVulkanLibrary = `${GUEST_VULKAN_ROOT}/lib`;
      const guestVulkanIcd = `${GUEST_VULKAN_ROOT}/share/vulkan/icd.d/venus_icd.x86_64.json`;
      const box64LibraryPath = `${guestVulkanLibrary}:${WINE_ROOT}/bin/guest_gfx/lib:` +
        `${WINE_ROOT}/bin:${WINE_ROOT}/bin/x86_64-unix:${WINE_ROOT}/lib/x86_64`;
      const isPresent = isWineVulkanPresent || isDxvk;
      const smokeEnvironment: Record<string, string> = {
        'WINEHUA_SMOKE_RUN_ID': this.request.runId,
        'WINEHUA_SMOKE_TEST_ID': spec.id,
        'WINEHUA_SMOKE_ASSETS': 'C:\\smoke\\assets',
        'WINEHUA_VULKAN_RUNTIME': '1',
        'WINEHUA_VULKAN_LOADER_ARCH': 'x86_64',
        'WINEHUA_VENUS_ICD_ARCH': 'x86_64',
        'USE_LIBBOX64': '1',
        'BOX64_LOG': '1',
        'BOX64_LD_LIBRARY_PATH': box64LibraryPath,
        'BOX64_EMULATED_LIBS': 'libvulkan.so:libvulkan.so.1:' +
          'libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:' +
          'libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:' +
          'libwayland-client.so:libwayland-client.so.0:libwayland-server.so:' +
          'libwayland-server.so.0:libwayland-server.so.0:libwayland-egl.so:' +
          'libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8',
        'VK_DRIVER_FILES': guestVulkanIcd,
        'VK_ICD_FILENAMES': guestVulkanIcd,
        'VN_DEBUG': 'vtest',
        /* Host GPU feedback lands in the Host side of WineHua's explicit
         * shadow mapping. Use the synchronous Host query path so a stale
         * Guest availability word cannot hide a valid query result. */
        'VN_PERF': 'no_fence_feedback,no_query_feedback',
        /* Keep DLL load evidence for fallback detection without tracing every
         * Vulkan call. A caller can still override WINEDEBUG for a focused
         * diagnostic run through runWineProgram. */
        'WINEDEBUG': '+loaddll,+module'
      };
      if (isDxvk) {
        smokeEnvironment['BOX64_DYNAREC_WEAKBARRIER'] = '0';
        smokeEnvironment['WINEHUA_DXVK_VERSION'] = '1.10.3';
        smokeEnvironment['WINEHUA_DXVK_RELAXED_FEATURES'] = '1';
        smokeEnvironment['WINEDLLOVERRIDES'] = 'd3d11=n;dxgi=n';
        smokeEnvironment['DXVK_LOG_LEVEL'] = 'info';
        smokeEnvironment['DXVK_LOG_PATH'] = `C:\\smoke\\results\\${this.request.runId}`;
        smokeEnvironment['DXVK_WINEHUA_TRACE_SAMPLED'] = '1';
        smokeEnvironment['DXVK_WINEHUA_TRACE_QUERY'] = '1';
        /* Bounded to 4096 records in DXVK. This preserves clear/bind/update/
         * draw ordering evidence when the exhaustive D3D11 smoke exits in a
         * later feature case. */
        smokeEnvironment['DXVK_WINEHUA_TRACE_FLOW'] = '1';
        smokeEnvironment['DXVK_WINEHUA_COMMAND_QUERY_RESET'] = '1';
        smokeEnvironment['WINEHUA_D3D11_STENCIL_QUERY'] = '1';
        /* Capture the exact DXBC-derived SPIR-V used by this run. Disable
         * the state cache so every shader reaches D3D11 shader compilation,
         * making the dump deterministic and replayable. */
        smokeEnvironment['DXVK_SHADER_DUMP_PATH'] = `C:\\smoke\\results\\${this.request.runId}`;
        smokeEnvironment['DXVK_WINEHUA_DUMP_REMAPPED_SPIRV'] = '1';
        smokeEnvironment['DXVK_STATE_CACHE'] = '0';
        if (this.perfProfile === 'shadow-precise' ||
          this.perfProfile === 'shadow-precise-single-ring' ||
          this.perfProfile === 'shadow-precise-sync-submit' ||
          this.perfProfile === 'shadow-precise-strong-ring' ||
          this.perfProfile === 'shadow-precise-strong-ring-async-present' ||
          this.perfProfile === 'shadow-precise-strong-ring-fence-poll' ||
          this.perfProfile === 'shadow-precise-strong-ring-mailbox' ||
          this.perfProfile === 'shadow-precise-direct-fence' ||
          this.perfProfile === 'shadow-precise-retain-shmem') {
          smokeEnvironment['DXVK_WINEHUA_PRECISE_SHADOW'] = '1';
          smokeEnvironment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '1';
          smokeEnvironment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
          if (this.perfProfile === 'shadow-precise-single-ring')
            smokeEnvironment['VN_PERF'] =
              'no_fence_feedback,no_query_feedback,no_multi_ring';
          if (this.perfProfile === 'shadow-precise-sync-submit')
            smokeEnvironment['VN_PERF'] =
              'no_fence_feedback,no_query_feedback,no_async_queue_submit';
          if (this.perfProfile === 'shadow-precise-strong-ring' ||
            this.perfProfile === 'shadow-precise-strong-ring-async-present' ||
            this.perfProfile === 'shadow-precise-strong-ring-fence-poll' ||
            this.perfProfile === 'shadow-precise-strong-ring-mailbox')
            smokeEnvironment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
          if (this.perfProfile === 'shadow-precise-direct-fence')
            smokeEnvironment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
          if (this.perfProfile === 'shadow-precise-retain-shmem')
            smokeEnvironment['VN_WINEHUA_DEFER_SHMEM_UNREF'] = '1';
        }
        if (this.request.suite === 'dxvk-layout-general')
          smokeEnvironment['DXVK_WINEHUA_FORCE_SAMPLED_GENERAL'] = '1';
      }
      if (spec.id.indexOf('wine-vulkan-sampled-only') >= 0)
        smokeEnvironment['WINEHUA_VULKAN_SAMPLED_ONLY'] = '1';
        if (spec.id.indexOf('dxvk-dynamic-cb') >= 0) {
          smokeEnvironment['WINEHUA_D3D11_DYNAMIC_CB'] = '1';
          smokeEnvironment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '1';
          smokeEnvironment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
        }
        if (spec.id.indexOf('dxvk-combined') >= 0)
          smokeEnvironment['DXVK_STATE_CACHE'] = '0';
        if (spec.id.indexOf('dxvk-combined') >= 0)
          smokeEnvironment['WINEHUA_D3D11_COMBINED_SAMPLER'] = '1';
      process = testNapi.runWineProgram({
        windowsExePath: `C:\\smoke\\${spec.architecture}\\${spec.executable}`,
        argv: ['--automation', '--run-id', this.request.runId, '--test-id', spec.id,
          '--result', windowsResult, isPresent ? '--present' : '--offscreen',
          ...(isPresent ? ['--seconds', `${spec.seconds}`] : [])],
        environment: smokeEnvironment,
        /* DXVK lives in the WineHua runtime overlay, so the smoke process
         * still runs from its ordinary managed application directory. */
        workingDirectory: `C:\\smoke\\${spec.architecture}`,
        prefixMode: this.request.prefixMode,
        d3dBackend: isDxvk ? 'dxvk_legacy' : 'wined3d',
        presentBackend: isPresent ? 'venus_broker_present' : 'venus_offscreen',
        automationMode: true
      }) as ProcessInfo;
    } else {
      process = testNapi.runWineProgram({
        windowsExePath: `C:\\smoke\\${spec.architecture}\\${spec.executable}`,
        argv: ['--automation', '--run-id', this.request.runId, '--test-id', spec.id,
          '--result', windowsResult, '--seconds', `${spec.seconds}`, '--present'],
        environment: {
          'WINEHUA_SMOKE_RUN_ID': this.request.runId,
          'WINEHUA_SMOKE_TEST_ID': spec.id,
          /* Guest and Host map different shadow files. Functional probes use
           * explicit flush/invalidate for their tiny readback allocations. */
          'VN_WINEHUA_REMOTE_MEMORY_SYNC': '1'
        },
        workingDirectory: `C:\\smoke\\${spec.architecture}`,
        prefixMode: this.request.prefixMode,
        d3dBackend: 'wined3d',
        presentBackend: 'virgl_compositor',
        automationMode: true
      }) as ProcessInfo;
    }
    if (!process.found || process.pid <= 0)
      return this.failureResult(spec, 'startup', 'runWineProgram failed to return a PID', process);
    return this.waitForFinal(spec, resultPath, process);
  }

  async run(): Promise<void> {
    resetAutomationLogs();
    const stableDirectory = `${AUTOMATION_RESULTS}/${this.request.runId}`;
    ensureDirectory(stableDirectory);
    ensureDirectory(this.resultDirectory);
    writeAtomic(`${stableDirectory}/request.json`, JSON.stringify(this.request, null, 2));

    if (!syncManagedSmoke(this.request.prefixMode)) {
      const summary: SmokeSuiteSummary = {
        schemaVersion: 1, runId: this.request.runId, suite: this.request.suite,
        prefixMode: this.request.prefixMode, status: 'FAIL', stage: 'startup',
        message: 'managed C:\\smoke payload synchronization failed', tests: []
      };
      writeAtomic(`${stableDirectory}/suite-summary.json`, JSON.stringify(summary, null, 2));
      this.onStatus('Smoke: FAIL (sync)');
      return;
    }

      if ((this.request.suite === 'dxvk' || this.request.suite === 'dxvk-long' ||
        this.request.suite === 'dxvk-replay' || this.request.suite === 'dxvk-layout-general' ||
        this.request.suite === 'dxvk-combined' || this.request.suite === 'all') &&
        !validateManagedDxvk()) {
      const summary: SmokeSuiteSummary = {
        schemaVersion: 1, runId: this.request.runId, suite: this.request.suite,
        prefixMode: this.request.prefixMode, status: 'FAIL', stage: 'startup',
        message: 'managed DXVK payload validation failed', tests: []
      };
      writeAtomic(`${stableDirectory}/suite-summary.json`, JSON.stringify(summary, null, 2));
      this.onStatus('Smoke: FAIL (DXVK sync)');
      return;
    }

    const specs = this.specs();
    if (specs.length === 0) {
      const summary: SmokeSuiteSummary = {
        schemaVersion: 1, runId: this.request.runId, suite: this.request.suite,
        prefixMode: this.request.prefixMode, status: 'UNSUPPORTED', stage: 'startup',
        message: 'suite is not enabled by the phase2-vulkan-b3-v1 manifest', tests: []
      };
      const text = JSON.stringify(summary, null, 2);
      writeAtomic(`${stableDirectory}/suite-summary.json`, text);
      writeAtomic(`${this.resultDirectory}/suite-summary.json`, text);
      this.onStatus('Smoke: UNSUPPORTED');
      return;
    }

    const results: SmokeSuiteTestResult[] = [];
    for (const spec of specs) results.push(await this.runOne(spec));
    const passed = results.every((result) => result.status === 'PASS');
    const summary: SmokeSuiteSummary = {
      schemaVersion: 1,
      runId: this.request.runId,
      suite: this.request.suite,
      prefixMode: this.request.prefixMode,
      status: passed ? 'PASS' : 'FAIL',
      completedTimestampMs: Date.now(),
      tests: results
    };
    const text = JSON.stringify(summary, null, 2);
    writeAtomic(`${stableDirectory}/suite-summary.json`, text);
    writeAtomic(`${this.resultDirectory}/suite-summary.json`, text);
    this.onStatus(passed ? 'Smoke: PASS' : 'Smoke: FAIL');
  }
}
