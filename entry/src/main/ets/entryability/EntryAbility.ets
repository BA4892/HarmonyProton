import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  private contentPage: string = 'pages/Index';

  private publishLaunchRequest(want: Want): void {
    const parameters = want.parameters;
    const value = (key: string, fallback: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return fallback;
      return `${parameters[key]}`;
    };
    const mode = value('winehua.mode', '');
    const suite = value('winehua.suite', 'core');
    const requestedD3d = value('winehua.d3d_backend', 'wined3d');
    const d3dBackend = requestedD3d === 'dxvk_legacy' ? 'dxvk_legacy' : 'wined3d';
    const gamePath = value('winehua.game_path', '');
    this.contentPage = mode === 'smoke' && suite === 'host-vulkan' ?
      'pages/HostVulkanSmoke' : 'pages/Index';
    WineWindowManager.getInstance().setAutomationMode(mode === 'smoke');
    const request = JSON.stringify({
      mode: mode,
      runId: value('winehua.run_id', `${Date.now()}`),
      suite: suite,
      prefixMode: value('winehua.prefix', 'reuse')
    });
    AppStorage.setOrCreate<string>('winehua.smoke.request', request);
    AppStorage.set<string>('winehua.smoke.request', request);
    AppStorage.setOrCreate<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.set<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.setOrCreate<string>('winehua.game.path', gamePath);
    AppStorage.set<string>('winehua.game.path', gamePath);
    hilog.info(DOMAIN, 'SmokeRunner', 'launch mode=%{public}s request=%{public}s', mode, request);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    this.publishLaunchRequest(want);
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.publishLaunchRequest(want);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // 初始化多窗口管理器
    WineWindowManager.getInstance().init(windowStage, this.context);

    windowStage.loadContent(this.contentPage, (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      // loadContent 成功后隐藏窗口装饰
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
