import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'PopupWM';

interface PopupEventData {
  popupId: number;
  x?: number;
  y?: number;
  w?: number;
  h?: number;
}

class PopupEntry {
  popupId: number = 0;
  parentToplevel: number = 0;
  subWin: window.Window | null = null;
  // Wine 逻辑坐标, 相对父窗口内容原点 (可为负/超出父窗口)
  offX: number = 0;
  offY: number = 0;
  w: number = 0;
  h: number = 0;
}

/**
 * PC 多窗口模式 popup (菜单/tooltip/下拉框) 子窗口管理。
 *
 * C++ compositor 把 Wine 的 subsurface popup 登记为伪 toplevel 并发
 * popup_show/move/resize/hide 事件 (挂在父 toplevel id 上)。
 * 这里为每个 popup 创建一个独立 OHOS 子窗口 (无边框/不可聚焦),
 * 由 pages/WinePopup 的 XComponent 渲染像素 — 不再受父窗口边缘裁剪。
 *
 * 注意: 不 import WineWindowManager (避免循环依赖), 所需对象全部由参数传入。
 */
export class PopupWindowManager {
  private static instance: PopupWindowManager | null = null;
  private popups: Map<number, PopupEntry> = new Map();
  // 创建进行中的 popupId (异步 createSubWindow 期间收到 hide 时延迟销毁)
  private creating: Set<number> = new Set();
  private pendingDestroy: Set<number> = new Set();
  // 父窗口 windowStage 未就绪时积压的事件: parentToplevel -> [event, data]
  private pendingEvents: Map<number, Array<[string, string]>> = new Map();

  static getInstance(): PopupWindowManager {
    if (!PopupWindowManager.instance) {
      PopupWindowManager.instance = new PopupWindowManager();
    }
    return PopupWindowManager.instance;
  }

  // -- 事件入口 (由 WineWindowManager 分发) --
  onPopupEvent(parentToplevel: number, event: string, data: string,
               stage: window.WindowStage | null, parentWin: window.Window | null,
               scale: number): void {
    /*
     * 风险标注 (P2): stage/parentWin 为空时所有事件都会积压, 包括 popup_hide。
     * hide 其实只需要 popupId 不需要 stage。当前由 WineWindowManager.unregisterAbility
     * 的级联销毁 (destroyPopupsForToplevel) 兜底, 不会泄漏子窗口;
     * 但 pendingEvents 会留一条永不 flush 的记录 (微量内存)。后续可改为
     * popup_hide 不经 stage 检查直接走 hide()。
     */
    if (!stage || !parentWin) {
      hilog.warn(DOMAIN, TAG, '[POPUP] #%{public}s parent=#%{public}d: stage/window not ready, queue',
        event, parentToplevel);
      let q = this.pendingEvents.get(parentToplevel);
      if (!q) { q = []; this.pendingEvents.set(parentToplevel, q); }
      q.push([event, data]);
      return;
    }
    this.dispatch(parentToplevel, event, data, stage, parentWin, scale);
  }

  // 父窗口就绪后 flush 积压事件 (WineWindowManager.flushPendingEvents 调用)
  flushPending(parentToplevel: number, stage: window.WindowStage,
               parentWin: window.Window, scale: number): void {
    const q = this.pendingEvents.get(parentToplevel);
    if (!q || q.length === 0) return;
    this.pendingEvents.delete(parentToplevel);
    hilog.info(DOMAIN, TAG, '[POPUP] flushPending parent=#%{public}d: %{public}d items',
      parentToplevel, q.length);
    for (const item of q) {
      this.dispatch(parentToplevel, item[0], item[1], stage, parentWin, scale);
    }
  }

  private dispatch(parentToplevel: number, event: string, data: string,
                   stage: window.WindowStage, parentWin: window.Window, scale: number): void {
    let parsed: PopupEventData;
    try {
      parsed = JSON.parse(data) as PopupEventData;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[POPUP] bad json parent=#%{public}d %{public}s: %{public}s',
        parentToplevel, event, data);
      return;
    }
    switch (event) {
      case 'popup_show':
        this.show(parentToplevel, parsed, stage, parentWin, scale);
        break;
      case 'popup_move':
        this.move(parsed, parentWin, scale);
        break;
      case 'popup_resize':
        this.resizePopup(parsed, parentWin, scale);
        break;
      case 'popup_hide':
        this.hide(parsed.popupId);
        break;
      default:
        hilog.warn(DOMAIN, TAG, '[POPUP] unknown event %{public}s', event);
        break;
    }
  }

  // -- popup_show: 创建子窗口 --
  private show(parentToplevel: number, data: PopupEventData, stage: window.WindowStage,
               parentWin: window.Window, scale: number): void {
    const popupId = data.popupId;
    if (this.popups.has(popupId) || this.creating.has(popupId)) {
      hilog.warn(DOMAIN, TAG, '[POPUP] show #%{public}d: already exists/creating, skip', popupId);
      return;
    }
    hilog.info(DOMAIN, TAG, '[POPUP] show #%{public}d parent=#%{public}d off=(%{public}d,%{public}d) %{public}dx%{public}d',
      popupId, parentToplevel, data.x ?? 0, data.y ?? 0, data.w ?? 0, data.h ?? 0);

    const entry = new PopupEntry();
    entry.popupId = popupId;
    entry.parentToplevel = parentToplevel;
    entry.offX = data.x ?? 0;
    entry.offY = data.y ?? 0;
    entry.w = data.w ?? 0;
    entry.h = data.h ?? 0;
    this.creating.add(popupId);

    // 让 pages/WinePopup 的 aboutToAppear 同步取到 popupId (FIFO, 与 WineWindow 同模式)
    testNapi.setPendingToplevel(popupId);

    stage.createSubWindowWithOptions(`wine_popup_${popupId}`, {
      title: '',
      decorEnabled: false,
      isModal: false
    }).then(async (subWin) => {
      try {
        await subWin.setWindowFocusable(false);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, '[POPUP] #%{public}d setWindowFocusable failed: %{public}s',
          popupId, JSON.stringify(e));
      }
      try {
        await subWin.loadContent('pages/WinePopup');
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[POPUP] #%{public}d loadContent FAILED: %{public}s',
          popupId, JSON.stringify(e));
        this.creating.delete(popupId);
        try { await subWin.destroyWindow(); } catch (_) { /* ignore */ }
        return;
      }
      try {
        subWin.setWindowCornerRadius(0);
      } catch (_) { /* ignore */ }
      try {
        subWin.setWindowBackgroundColor('#FF000000');
      } catch (_) { /* ignore */ }

      entry.subWin = subWin;
      this.creating.delete(popupId);

      // 创建期间收到 hide → 立即销毁
      if (this.pendingDestroy.has(popupId)) {
        this.pendingDestroy.delete(popupId);
        hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d destroy requested while creating', popupId);
        this.destroyEntry(entry);
        return;
      }

      this.popups.set(popupId, entry);
      subWin.on('windowEvent', (eventType: number) => {
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d WINDOW_DESTROYED', popupId);
          this.popups.delete(popupId);
        }
      });

      try {
        await subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                            Math.max(1, Math.round(entry.h * scale)));
        await this.positionEntry(entry, parentWin, scale);
        await subWin.showWindow();
        hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d shown', popupId);
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[POPUP] #%{public}d show failed: %{public}s',
          popupId, JSON.stringify(e));
        this.destroyEntry(entry);
      }
    }).catch((e: Error) => {
      this.creating.delete(popupId);
      hilog.error(DOMAIN, TAG, '[POPUP] #%{public}d createSubWindow FAILED: %{public}s',
        popupId, JSON.stringify(e));
    });
  }

  // -- popup_move / popup_resize --
  private move(data: PopupEventData, parentWin: window.Window, scale: number): void {
    const entry = this.popups.get(data.popupId);
    if (!entry || !entry.subWin) return;
    entry.offX = data.x ?? entry.offX;
    entry.offY = data.y ?? entry.offY;
    this.positionEntry(entry, parentWin, scale).catch(() => { /* ignore */ });
  }

  private resizePopup(data: PopupEventData, parentWin: window.Window, scale: number): void {
    const entry = this.popups.get(data.popupId);
    if (!entry || !entry.subWin) return;
    entry.w = data.w ?? entry.w;
    entry.h = data.h ?? entry.h;
    entry.subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                        Math.max(1, Math.round(entry.h * scale))).then(() => {
      return this.positionEntry(entry, parentWin, scale);
    }).catch(() => { /* ignore */ });
  }

  // -- popup_hide --
  private hide(popupId: number): void {
    if (this.creating.has(popupId)) {
      this.pendingDestroy.add(popupId);
      return;
    }
    const entry = this.popups.get(popupId);
    if (!entry) return;
    hilog.info(DOMAIN, TAG, '[POPUP] hide #%{public}d', popupId);
    this.destroyEntry(entry);
  }

  private destroyEntry(entry: PopupEntry): void {
    this.popups.delete(entry.popupId);
    if (entry.subWin) {
      entry.subWin.destroyWindow().catch((e: Error) => {
        hilog.warn(DOMAIN, TAG, '[POPUP] #%{public}d destroyWindow failed: %{public}s',
          entry.popupId, JSON.stringify(e));
      });
      entry.subWin = null;
    }
  }

  // 级联: 父窗口销毁时清理其全部 popup
  destroyPopupsForToplevel(parentToplevel: number): void {
    const doomed: PopupEntry[] = [];
    this.popups.forEach((entry) => {
      if (entry.parentToplevel === parentToplevel) doomed.push(entry);
    });
    for (const entry of doomed) {
      hilog.info(DOMAIN, TAG, '[POPUP] cascade destroy #%{public}d (parent #%{public}d destroyed)',
        entry.popupId, parentToplevel);
      this.destroyEntry(entry);
    }
  }

  // 父窗口移动/缩放 → 重新定位其全部 popup
  // (全局坐标 = 父窗口内容原点 + offset × scale, 无论 OHOS 是否自动跟随都收敛到正确位置)
  repositionPopups(parentToplevel: number, parentWin: window.Window, scale: number): void {
    this.popups.forEach((entry) => {
      if (entry.parentToplevel !== parentToplevel || !entry.subWin) return;
      this.positionEntry(entry, parentWin, scale).catch(() => { /* ignore */ });
    });
  }

  // 全局坐标 = 父窗口 windowRect (全局物理 px) + Wine 逻辑 offset × scale
  private async positionEntry(entry: PopupEntry, parentWin: window.Window, scale: number): Promise<void> {
    if (!entry.subWin) return;
    const rect = parentWin.getWindowProperties().windowRect;
    const gx = Math.round(rect.left + entry.offX * scale);
    const gy = Math.round(rect.top + entry.offY * scale);
    await entry.subWin.moveWindowToGlobal(gx, gy);
  }

  // 诊断: 某父窗口是否有活动 popup
  hasPopups(parentToplevel: number): boolean {
    let found = false;
    this.popups.forEach((entry) => {
      if (entry.parentToplevel === parentToplevel) found = true;
    });
    return found;
  }
}
