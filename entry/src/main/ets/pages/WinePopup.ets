import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

// PC 模式 popup (菜单/tooltip/下拉框) 子窗口页面。
// 与 WineWindow 同模式: 自定义 Controller 通过 surfaceId 管理渲染器。
// 区别: 不调 notifyToplevelResize (popup 无 xdg_toplevel, 不能发 configure),
//       不处理键盘 (焦点留在父窗口), 不处理 startMoving。
class PopupXComponentController extends XComponentController {
  popupId: number = 0;

  onSurfaceCreated(surfaceId: string): void {
    hilog.info(DOMAIN, 'WinePopup', '[PopupCtrl] onSurfaceCreated popup=%{public}d surfaceId=%{public}s',
      this.popupId, surfaceId);
    try {
      testNapi.createRenderer(this.popupId, BigInt(surfaceId));
    } catch (e) {
      hilog.error(DOMAIN, 'WinePopup', 'ERR onSurfaceCreated createRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    hilog.info(DOMAIN, 'WinePopup', '[PopupCtrl] onSurfaceChanged popup=%{public}d rect=%{public}s',
      this.popupId, JSON.stringify(rect));
    try {
      testNapi.resizeRenderer(this.popupId, rect.surfaceWidth, rect.surfaceHeight);
    } catch (e) {
      hilog.error(DOMAIN, 'WinePopup', 'ERR onSurfaceChanged resizeRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, 'WinePopup', '[PopupCtrl] onSurfaceDestroyed popup=%{public}d', this.popupId);
    try {
      testNapi.destroyRenderer(this.popupId);
    } catch (e) {
      hilog.error(DOMAIN, 'WinePopup', 'ERR onSurfaceDestroyed destroyRenderer: %{public}s', JSON.stringify(e));
    }
  }
}

@Entry
@Component
struct WinePopup {
  controller: PopupXComponentController = new PopupXComponentController();
  @State popupId: number = 0;

  aboutToAppear(): void {
    // loadContent 同步调用, 与 setPendingToplevel 配对 (同 WineWindow)
    this.popupId = testNapi.getCurrentToplevelId() as number;
    this.controller.popupId = this.popupId;
    hilog.info(DOMAIN, 'WinePopup', 'aboutToAppear popup=%{public}d', this.popupId);
  }

  build() {
    Stack() {
      XComponent({
        id: 'wine_popup_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
      })
        .width('100%')
        .height('100%')
        .backgroundColor('#000')
        .focusable(false)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
    .focusable(false)

    // -- 鼠标事件 --
    .onMouse((event: MouseEvent) => {
      if (this.popupId <= 0) return;
      const px = vp2px(event.x);
      const py = vp2px(event.y);
      const btn = mapMouseButton(event.button);
      if (event.action !== MouseAction.Move) {
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-MOUSE popup=%{public}d action=%{public}d px=(%{public}d,%{public}d) btn=0x%{public}x',
          this.popupId, event.action, Math.round(px), Math.round(py), btn);
      }
      testNapi.sendPointerEvent(this.popupId, event.action, px, py, btn);
    })

    // -- 滚轮事件 (菜单滚动条等) --
    .onAxisEvent((event?: AxisEvent) => {
      if (this.popupId <= 0 || !event) return;
      const px = vp2px(event.x);
      const py = vp2px(event.y);
      const vert = event.getVerticalAxisValue();
      const horiz = event.getHorizontalAxisValue();
      const step = event.scrollStep ?? 0;
      if (vert !== 0) {
        testNapi.sendScrollEvent(this.popupId, 0, vert, step, px, py);
      }
      if (horiz !== 0) {
        testNapi.sendScrollEvent(this.popupId, 1, horiz, step, px, py);
      }
    })
  }
}
