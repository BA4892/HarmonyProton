import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

class HostVulkanController extends XComponentController {
  runId: string = '';
  started: boolean = false;

  onSurfaceCreated(surfaceId: string): void {
    hilog.info(DOMAIN, 'HostVulkan', 'surface created id=%{public}s run=%{public}s', surfaceId, this.runId);
    if (this.started || !this.runId) return;
    this.started = testNapi.runHostVulkanProbe(BigInt(surfaceId), this.runId) as boolean;
    if (!this.started) {
      hilog.error(DOMAIN, 'HostVulkan', 'native host Vulkan probe failed to start');
    }
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    hilog.info(DOMAIN, 'HostVulkan', 'surface changed id=%{public}s rect=%{public}s',
      surfaceId, JSON.stringify(rect));
  }

  onSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, 'HostVulkan', 'surface destroyed id=%{public}s', surfaceId);
    testNapi.stopHostVulkanProbe();
  }
}
@Entry
@Component
struct HostVulkanSmoke {
  @StorageLink('winehua.smoke.request') requestText: string = '';
  controller: HostVulkanController = new HostVulkanController();

  aboutToAppear(): void {
    try {
      const request = JSON.parse(this.requestText) as Record<string, string>;
      this.controller.runId = request.runId ?? `${Date.now()}`;
    } catch (_) {
      this.controller.runId = `${Date.now()}`;
    }
  }

  build() {
    Stack() {
      XComponent({
        id: 'host_vulkan_smoke_xcomponent',
        type: XComponentType.SURFACE,
        controller: this.controller
      })
        .width('100%')
        .height('100%')
        .backgroundColor('#000000')
    }
      .width('100%')
      .height('100%')
      .backgroundColor('#000000')
  }
}
