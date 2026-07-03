import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

class DesktopController extends XComponentController {
  onSurfaceCreated(surfaceId: string): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceCreated rootId=%{public}d surfaceId=%{public}s', rootId, surfaceId);
    if (rootId > 0) {
      try { testNapi.createRenderer(rootId, BigInt(surfaceId)); } catch (_) {}
    }
  }
  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceChanged rootId=%{public}d %{public}dx%{public}d', rootId, rect.surfaceWidth, rect.surfaceHeight);
    if (rootId > 0) {
      try {
        testNapi.resizeRenderer(rootId, rect.surfaceWidth, rect.surfaceHeight);
        const s = 2.0;
        testNapi.notifyToplevelResize(rootId,
          Math.round(rect.surfaceWidth / s), Math.round(rect.surfaceHeight / s));
      } catch (_) {}
    }
  }
  onSurfaceDestroyed(_s: string): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.warn(DOMAIN, 'LIFE', '[XComp] onSurfaceDestroyed rootId=%{public}d', rootId);
    if (rootId > 0) {
      try { testNapi.destroyRenderer(rootId); } catch (_) {}
    }
  }
}

@Entry
@Component
struct DesktopPage {
  controller: DesktopController = new DesktopController();
  // 触控状态机: IDLE → PENDING → (DRAGGING | 长按 | 点击)
  private touchPending: boolean = false;
  private isDragging: boolean = false;
  private touchStartTime: number = 0;
  private touchStartPx: number = 0;
  private touchStartPy: number = 0;
  private pendingTl: number = 0;
  private pendingPx: number = 0;
  private pendingPy: number = 0;
  private motionLogCounter: number = 0;
  private static readonly LONG_PRESS_MS: number = 500;
  private static readonly DRAG_THRESHOLD: number = 10; // 物理像素, 区分点击抖动和拖拽
  // 双击检测
  private lastTapTime: number = 0;
  private lastTapPx: number = 0;
  private lastTapPy: number = 0;
  private lastTapTl: number = 0;
  private static readonly DOUBLE_TAP_MS: number = 400;
  private static readonly DOUBLE_TAP_DIST: number = 30;

  build() {
    Stack() {
      XComponent({
        id: 'wine_desktop_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
      }).width('100%').height('100%').backgroundColor('#000').focusable(true)

      // ── 触控处理: Down/Up/Click → 左键, 长按 → 右键, 拖拽 → Press+Motion+Release ──
      .onTouch((ev: TouchEvent) => {
        try {
          const t = ev.changedTouches[0];
          if (!t) return;
          const px = Math.round(vp2px(t.x));
          const py = Math.round(vp2px(t.y));

          if (ev.type === TouchType.Down) {
            // 查找目标 toplevel
            const wpx = Math.round(px / 2);
            const wpy = Math.round(py / 2);
            const tl: number = testNapi.findToplevelAt(wpx, wpy) as number;
            if (tl <= 0) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH skipped (no toplevel at %{public}d,%{public}d)', wpx, wpy);
              return;
            }
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH tl=%{public}d px=(%{public}d,%{public}d)', tl, px, py);
            testNapi.raiseToplevel(tl);
            // 进入 PENDING 状态: 暂不发送事件, 等判定长按/拖拽/点击
            this.touchPending = true;
            this.isDragging = false;
            this.touchStartTime = Date.now();
            this.touchStartPx = px;
            this.touchStartPy = py;
            this.pendingTl = tl;
            this.pendingPx = px;
            this.pendingPy = py;
            this.motionLogCounter = 0;

          } else if (ev.type === TouchType.Move) {
            if (!this.touchPending && !this.isDragging) return;

            const dist = Math.abs(px - this.touchStartPx) + Math.abs(py - this.touchStartPy);

            if (this.touchPending && dist >= DesktopPage.DRAG_THRESHOLD) {
              // 进入拖拽模式: 补发 Press (Down 时未发)
              const btnLeft = mapMouseButton(1);
              this.touchPending = false;
              this.isDragging = true;
              // 打断双击序列 (拖拽不是点击)
              this.lastTapTime = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-START tl=%{public}d px=(%{public}d,%{public}d) dist=%{public}d',
                this.pendingTl, px, py, dist);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, btnLeft);
            } else if (this.isDragging) {
              // 拖拽移动: 持续发送 Motion
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, 0);
              if (++this.motionLogCounter % 10 === 0) {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-MOVE tl=%{public}d px=(%{public}d,%{public}d) frame=%{public}d',
                  this.pendingTl, px, py, this.motionLogCounter);
              }
            }
            // PENDING 且未超过阈值: 不做任何事 (等 Up 判定长按/点击)

          } else if (ev.type === TouchType.Up) {
            if (this.isDragging) {
              // 拖拽结束
              const btnLeft = mapMouseButton(1);
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-END tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, px, py);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, px, py, btnLeft);
              this.isDragging = false;

            } else if (this.touchPending) {
              // PENDING 状态: 按按下时长判定长按 vs 点击
              this.touchPending = false;
              const elapsed = Date.now() - this.touchStartTime;

              if (elapsed >= DesktopPage.LONG_PRESS_MS) {
                // 长按 → 右键
                const btnRight = mapMouseButton(2);
                this.lastTapTime = 0; // 长按打断双击序列
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] LONGPRESS->RIGHTCLICK tl=%{public}d elapsed=%{public}dms px=(%{public}d,%{public}d)',
                  this.pendingTl, elapsed, this.pendingPx, this.pendingPy);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnRight);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnRight);
              } else {
                // 短按 → 左键 或 双击
                const btnLeft = mapMouseButton(1);
                const now = Date.now();
                const dist = Math.abs(this.pendingPx - this.lastTapPx) + Math.abs(this.pendingPy - this.lastTapPy);
                const isDoubleTap = (this.lastTapTime > 0 && (now - this.lastTapTime) < DesktopPage.DOUBLE_TAP_MS
                                    && dist < DesktopPage.DOUBLE_TAP_DIST && this.pendingTl === this.lastTapTl);

                if (isDoubleTap) {
                  // 双击: 两个连续点击使用同一位置
                  this.lastTapTime = 0;
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DOUBLE-TAP tl=%{public}d px=(%{public}d,%{public}d)',
                    this.pendingTl, this.lastTapPx, this.lastTapPy);
                } else {
                  // 单击
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
                  this.lastTapTime = now;
                  this.lastTapPx = this.pendingPx;
                  this.lastTapPy = this.pendingPy;
                  this.lastTapTl = this.pendingTl;
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TAP->LEFTCLICK tl=%{public}d px=(%{public}d,%{public}d)',
                    this.pendingTl, this.pendingPx, this.pendingPy);
                }
              }
            }
          } else if (ev.type === TouchType.Cancel) {
            // 系统取消触摸 (手势/通知等): 如果正在拖拽则补发 Release
            if (this.isDragging) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL-DRAG tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, px, py);
              const btnLeft = mapMouseButton(1);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, px, py, btnLeft);
            } else {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL tl=%{public}d (no event sent)', this.pendingTl);
            }
            this.touchPending = false;
            this.isDragging = false;
          }
        } catch (e) {
          hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH handler exception: %{public}s', JSON.stringify(e));
        }
      })

      // ── 键盘事件 ──
      .onKeyEvent((ev: KeyEvent) => {
        const rootId = WineWindowManager.getInstance().getDesktopRootId();
        if (rootId <= 0) return;
        ev.stopPropagation();
        const code = OH2EVDEV[ev.keyCode] ?? 0;
        if (code <= 0) return;
        testNapi.sendKeyEvent(rootId, code, ev.type === KeyType.Down);
      })

      // ── 滚轮事件 ──
      .onAxisEvent((ev?: AxisEvent) => {
        const rootId = WineWindowManager.getInstance().getDesktopRootId();
        if (rootId <= 0 || !ev) return;
        const wpx = Math.round(vp2px(ev.x) / 2);
        const wpy = Math.round(vp2px(ev.y) / 2);
        const vert = ev.getVerticalAxisValue();
        const horiz = ev.getHorizontalAxisValue();
        const step = ev.scrollStep ?? 0;
        const tl: number = testNapi.findToplevelAt(wpx, wpy) as number;
        if (vert !== 0) testNapi.sendScrollEvent(tl, 0, vert, step, wpx, wpy);
        if (horiz !== 0) testNapi.sendScrollEvent(tl, 1, horiz, step, wpx, wpy);
      })
    }
    .width('100%').height('100%').backgroundColor('#000')
  }
}
