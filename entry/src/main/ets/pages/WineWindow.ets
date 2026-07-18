import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;

// 自定义 XComponentController: 通过 surfaceId 管理 XComponent 生命周期
// 不再使用 libraryname='entry', 从根本上消除多窗口 XComponent exports 冲突
class WineXComponentController extends XComponentController {
  toplevelId: number = 0;

  onSurfaceCreated(surfaceId: string): void {
    hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceCreated tl=%{public}d surfaceId=%{public}s',
      this.toplevelId, surfaceId);
    try {
      testNapi.createRenderer(this.toplevelId, BigInt(surfaceId));
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceCreated createRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceChanged tl=%{public}d surfaceId=%{public}s rect=%{public}s',
      this.toplevelId, surfaceId, JSON.stringify(rect));
    try {
      testNapi.resizeRenderer(this.toplevelId, rect.surfaceWidth, rect.surfaceHeight);
      // surface 是物理像素, Wine 需要逻辑坐标 (effectiveScale=2.0)
      const w = Math.round(rect.surfaceWidth / 2.0);
      const h = Math.round(rect.surfaceHeight / 2.0);
      hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceChanged tl=%{public}d → notifyToplevelResize(%{public}d, %{public}d)',
        this.toplevelId, w, h);
      testNapi.notifyToplevelResize(this.toplevelId, w, h);
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceChanged resizeRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceDestroyed tl=%{public}d surfaceId=%{public}s',
      this.toplevelId, surfaceId);
    try {
      testNapi.destroyRenderer(this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceDestroyed destroyRenderer: %{public}s', JSON.stringify(e));
    }
  }
}

@Entry
@Component
struct WineWindow {
  controller: WineXComponentController = new WineXComponentController();
  @State toplevelId: number = 0;
  @State surfacePxW: number = 1;
  @State surfacePxH: number = 1;

  aboutToAppear(): void {
    // 关键: aboutToAppear 是 loadContent 的同步调用, 在下一个 Ability 覆盖前读取
    this.toplevelId = testNapi.getCurrentToplevelId() as number;
    // 同步设置给 controller: 保证 onSurfaceCreated 拿到正确的 toplevelId
    this.controller.toplevelId = this.toplevelId;
    hilog.info(DOMAIN, 'WineWindow', 'aboutToAppear tl=%{public}d', this.toplevelId);
  }

  build() {
    Stack() {
      XComponent({
        id: 'wine_sub_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
        // 不再设置 libraryname: 使用自定义 Controller 的 surfaceId 回调
      })
        .width('100%')
        .height('100%')
        // 背景透明: 与 '#000' 等价 (surface 不透明覆盖全部区域),
        // 为后续 ARGB 异型窗口方案 (子窗口路线) 预留
        .backgroundColor('#00000000')
        .focusable(false)
        // -- 滚轮事件 (轴事件, API 17+) --
        // 轴事件不冒泡 (AxisEvent.propagation 需显式激活), 挂外层 Stack 收不到,
        // 必须挂在命中链顶端的 XComponent 上
        .onAxisEvent((event?: AxisEvent) => {
          if (this.toplevelId <= 0 || !event) return;
          const px = vp2px(event.x);
          const py = vp2px(event.y);
          const vert = event.getVerticalAxisValue();
          const horiz = event.getHorizontalAxisValue();
          const step = event.scrollStep ?? 0;
          if (vert !== 0) {
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL tl=%{public}d vert=%{public}d step=%{public}d px=(%{public}d,%{public}d)',
              this.toplevelId, Math.round(vert), step, Math.round(px), Math.round(py));
            testNapi.sendScrollEvent(this.toplevelId, 0, vert, step, px, py);
          }
          if (horiz !== 0) {
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL tl=%{public}d horiz=%{public}d step=%{public}d px=(%{public}d,%{public}d)',
              this.toplevelId, Math.round(horiz), step, Math.round(px), Math.round(py));
            testNapi.sendScrollEvent(this.toplevelId, 1, horiz, step, px, py);
          }
        })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#00000000')
    .focusable(true)
    .defaultFocus(true)

    // -- 鼠标事件 --
    .onMouse((event: MouseEvent) => {
      if (this.toplevelId <= 0) return;
      // Wine 发起 xdg_toplevel.move → 下一个 MOVE 时调用 OHOS 原生 startMoving()
      if (event.action === MouseAction.Move && WineWindowManager.getInstance().shouldStartMoving(this.toplevelId)) {
        try {
          const win = WineWindowManager.getInstance().getWindow(this.toplevelId);
          if (win) {
            win.startMoving();
            return; // OHOS 接管拖拽, 不再转发给 Wine
          }
        } catch (e) { /* fall through to normal path */ }
      }
      const px = vp2px(event.x);
      const py = vp2px(event.y);
      const btn = mapMouseButton(event.button);
      if (event.action !== MouseAction.Move) {
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE tl=%{public}d action=%{public}d px=(%{public}d,%{public}d) btn=0x%{public}x',
          this.toplevelId, event.action, Math.round(px), Math.round(py), btn);
      }
      testNapi.sendPointerEvent(this.toplevelId, event.action, px, py, btn);
    })

    // -- 键盘事件 --
    .onKeyEvent((event: KeyEvent) => {
      hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] onKeyEvent keyCode=%{public}d type=%{public}d tl=%{public}d',
        event.keyCode, event.type, this.toplevelId);
      if (this.toplevelId <= 0) return;
      event.stopPropagation();
      const evdevCode = OH2EVDEV[event.keyCode] ?? 0;
      if (evdevCode === 0) {
        hilog.warn(DOMAIN, 'KBD-PIPE', '[ArkTS] KEY unmapped ohosCode=%{public}d', event.keyCode);
        return;
      }
      const pressed = (event.type === KeyType.Down);
      hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] KEY ohos=%{public}d->evdev=%{public}d %{public}s',
        this.toplevelId, event.keyCode, evdevCode, pressed ? 'DOWN' : 'UP');
      testNapi.sendKeyEvent(this.toplevelId, evdevCode, pressed);
    })

    .onAreaChange((_o: Area, n: Area) => {
      this.surfacePxW = vp2px(n.width as number);
      this.surfacePxH = vp2px(n.height as number);
      hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onAreaChange tl=%{public}d w=%{public}d h=%{public}d',
        this.toplevelId, Math.round(this.surfacePxW), Math.round(this.surfacePxH));
    })
  }
}
