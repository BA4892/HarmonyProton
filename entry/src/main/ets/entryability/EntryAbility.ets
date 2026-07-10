import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // 沉浸模式必须在 WineWindowManager.init() 之前设置:
    // init() 会通过 getWindowAvoidArea 计算 output 尺寸, 需要此时 avoid area 已归零
    try {
      const mw = windowStage.getMainWindowSync();
      if (mw) {
        mw.setWindowLayoutFullScreen(true).catch((e: BusinessError) => {
          hilog.error(DOMAIN, 'testTag', 'fullScreen failed: %{public}s', JSON.stringify(e));
        });
        mw.setSpecificSystemBarEnabled('status', false).catch((e: BusinessError) => {
          hilog.error(DOMAIN, 'testTag', 'statusBar failed: %{public}s', JSON.stringify(e));
        });
        mw.setSpecificSystemBarEnabled('navigationIndicator', false).catch((e: BusinessError) => {
          hilog.error(DOMAIN, 'testTag', 'navBar failed: %{public}s', JSON.stringify(e));
        });
      }
    } catch (e) {
      hilog.error(DOMAIN, 'testTag', 'immersive ERR: %{public}s', JSON.stringify(e));
    }

    // 初始化多窗口管理器
    WineWindowManager.getInstance().init(windowStage, this.context);

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      let mainWin = windowStage.getMainWindowSync();
      if (mainWin) {
        mainWin.setWindowDecorVisible(false);
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}