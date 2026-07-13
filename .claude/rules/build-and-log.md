# 构建与日志

## 构建

项目根目录 `/data/share/wineohos/Makefile` 是**唯一**构建入口。

**⚠️ 绝对禁止手动 `make -C build/wine-ohos` 或 `make -C build/wine-native`！** 必须通过顶层 `make`，因为它会：
- 通过 `scripts/env.sh` 正确设置 `NATIVE_ARCH`、交叉编译工具链
- 正确 configure Wine（`--host=x86_64-linux-ohos`、OHOS 头文件修复等）
- 通过 stamp 机制做增量构建，跳过无需重编的模块
- 正确运行 assemble → HAP → 签名 全流程

### 构建命令

```bash
# ==================== arm64 (当前调试目标) ====================

# 完整构建 (改任何源码后)
make NATIVE_ARCH=arm64-v8a

# 仅 HAP (只改 ArkTS 或 entry/src/main/cpp/ 时, 跳过 Wine/deps 重编译)
make NATIVE_ARCH=arm64-v8a hap

# 单个模块: deps | wine | box64 | native | assemble | hap

# ==================== x86_64 ====================
make NATIVE_ARCH=x86_64
```

**注意:**
- 默认 `make` = `make NATIVE_ARCH=x86_64` (x86_64 全量构建)
- 改 Wine C 源码 (`thirdparty/wine/`) 后 `make NATIVE_ARCH=arm64-v8a` 会自动检测并重编 Wine
- 改 Wine C 源码后，**必须 `make NATIVE_ARCH=arm64-v8a`（完整构建）**，因为 assemble 需要重新打包 wine-data.zip
- 只改 ArkTS → `make ... hap` 足够
- 只改 C++ (entry/src/main/cpp/) → `make ... hap` 足够

### Wine stamp 机制

Wine 构建使用 stamp 文件 + `find -newer` 检测源码变更：
- stamp 路径: `build/.stamps/wine-arm64-v8a`
- 任何 `thirdparty/wine/` 下的 .c/.h 文件比 stamp 新 → 触发 Wine 重编
- sentinel 检查: `build/wine-native/tools/winegcc/winegcc` 必须存在

### Pad 部署

```bash
# 设置设备地址
H="hdc -t <device_ip>"

# 完整部署流程 (改 Wine 后需卸载重装)
$H shell "bm uninstall -n app.hackeris.winehua"
$H file send "/data/share/wineohos/entry/build/default/outputs/default/entry-default-signed.hap" "/data/local/tmp/winehua.hap"
$H shell "bm install -p /data/local/tmp/winehua.hap"
$H shell "rm -rf /data/app/el2/100/base/app.hackeris.winehua/files/.wine /data/app/el2/100/base/app.hackeris.winehua/files/wine"
$H shell "aa start -a EntryAbility -b app.hackeris.winehua"

# 仅改 ArkTS/C++ (不改 Wine): 无需卸载, force-stop + install + start
$H shell "aa force-stop app.hackeris.winehua"
$H file send "..." "/data/local/tmp/winehua.hap"
$H shell "bm install -p /data/local/tmp/winehua.hap"
$H shell "aa start -a EntryAbility -b app.hackeris.winehua"
```

### Wine 子进程 stderr 日志

Wine 内部 stderr 输出被捕获到：
- **文件**: `/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_YYYYMMDD.log`
- **hilog tag**: `WineChild-stderr`

```bash
# 读取今天的 Wine stderr 日志
$H shell "cat /data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_$(date +%Y%m%d).log"

# 搜索特定内容
$H shell "grep -i '关键词' /data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_$(date +%Y%m%d).log"
```

**注意:** Wine TRACE 日志通过 `WINEDEBUG` 环境变量控制。默认 `WINEDEBUG=-all`（全部禁用）。如需开启特定 channel，修改 `wine_env.cpp` 中的 `WINEDEBUG` 值（如 `-all,+waylanddrv`），然后用 `fprintf(stderr, ...)` 替代 `TRACE(...)`（后者在 release build 中可能被优化掉）。

## hdc hilog 实时日志

### hdc 连接方式

```
# USB 直连
hdc -t <device_ip>

# 通过 hdc server 远程连接 (server 在另一台机器)
hdc -s <server_ip> -t <target_ip>
```

### 直接抓取（推荐）

```bash
hdc -t <device_ip> hilog

# 过滤 Wine 相关 tag
hdc -t <device_ip> hilog | grep -E 'CLICK-PIPE|KBD-PIPE|WineWM|MW-|WL_Plugin|WL_NAPI|WL_EGL|WL_Server|WL_Xdg|WL_Seat|WL_Input|WL-ERR|WL-STAT|Input-DROP|winehua|CRASH'
```

### 通过 shell 抓取

```bash
# 只取最后 N 行 app 日志
hdc -t <device_ip> shell "hilog -z 500 -t app"

# 抓取 crash 日志
hdc -t <device_ip> shell "hilog -z 200" | grep -iE 'crash|fault|SIGSEGV|SIGABRT|stack'
```

### 关键日志 tag 说明

| Tag | 来源 | 内容 |
|-----|------|------|
| `WWA` | ArkTS `WineWindowAbility.ets` | 子窗口 UIAbility 生命周期 |
| `WineWM` | ArkTS `WineWindowManager.ets` | 窗口生命周期、resize、title、surface 校准 |
| `CLICK-PIPE` | ArkTS `WineWindow.ets` | 鼠标事件: ArkTS→NAPI→Inject 完整链路 |
| `KBD-PIPE` | ArkTS `WineWindow.ets` | 键盘事件: ArkTS→NAPI→Inject 完整链路 |
| `WL_Plugin` | C++ `plugin_manager.cpp` | XComponent 注册、Surface 创建/销毁、renderer 管理 |
| `WL_EGL` | C++ `egl_renderer.cpp` | EGL 初始化、render loop |
| `WL_Server` | C++ `wayland_server.cpp` | Wayland compositor、帧 buffer、toplevel commit、资源快照 |
| `WL_Xdg` | C++ `xdg_shell.cpp` | xdg_toplevel 协议、window_geometry、min/max size |
| `WL_Input` | C++ `input_manager.cpp` | 事件注入 (InjectEnter/Button/Key)、丢帧统计 |
| `WL_Seat` | C++ `seat.cpp` | wl_seat 注册/绑定、pointer/keyboard 生命周期 |
| `WL_NAPI` | C++ `napi_init.cpp` | NAPI 桥接 (PIPE)、Wine/wineserver/wineboot 进程管理、crash 检测 |
| `MW-RNDR` | C++ `egl_renderer.cpp` render loop | viewport、surface size、frame size 对比 |
| `WL-ERR` | C++ `wayland_server.cpp` | Wayland 协议错误 (event loop dispatch 失败) |
| `WL-STAT` | C++ `wayland_server.cpp` | 定期资源快照 (toplevel/surface/renderer 数, 30s) |
| `Input-DROP` | C++ `input_manager.cpp` | 丢帧统计汇总 (60s, 分类 enter/button/key/motion) |
| `CRASH` | C++ `napi_init.cpp` | Wine/wineboot 进程崩溃信号 (SIGSEGV/SIGABRT 等) |
| `MW-SUBSURF` | C++ `wayland_server.cpp` | subsurface 生命周期: 创建/位置/存储/销毁/NULL buffer |
| `MW-MOVE` | C++ `wayland_server.cpp` | 交互式窗口移动: grab 开始/移动/结束 |
| `MW-TAKE` | C++ `wayland_server.cpp` | 帧合成输出: 尺寸、children/subsurfaces 数量 |
| `MW-DIAG` | C++ `wayland_server.cpp` | 临时诊断日志 (问题排查用) |
| `WineChild-stderr` | C++ `wine_child.cpp` | Wine 进程 stderr 转发 (TRACE/ERR/printf) |

### 事件流水线

**鼠标链路** (CLICK-PIPE / WL_NAPI [PIPE] / WL_Input):
```
[ArkTS] WineWindow.ets → [NAPI] napi_init.cpp SendPointerEvent → [Input] input_manager.cpp Inject*
```

**键盘链路** (KBD-PIPE / WL_NAPI [PIPE] / WL_Input):
```
[ArkTS] WineWindow.ets → [NAPI] napi_init.cpp SendKeyEvent → [Input] input_manager.cpp InjectKbd*
```

**滚轮链路** (CLICK-PIPE / WL_NAPI [PIPE] / WL_Input):
```
[ArkTS] WineWindow.ets (onAxisEvent) → [NAPI] napi_init.cpp SendScrollEvent → [Input] input_manager.cpp InjectAxis*
```

**修饰键链路** (WL_Input):
```
[ArkTS] WineWindow.ets (onKeyEvent) → [NAPI] SendKeyEvent → [Input] UpdateModifiers → EnqueueModifiers → InjectKbdModifiers
```
每次 Ctrl/Alt/Shift/Meta 按下/释放都会同步 `wl_keyboard_send_modifiers` 给 Wine。

### 日志过滤速查

```bash
# Pad 设备
H="hdc -t 192.168.1.6:33363"

# 完整事件流水线
$H hilog | grep -E 'CLICK-PIPE|KBD-PIPE|PIPE'

# 丢帧统计 (每 60s 一次汇总)
$H hilog | grep 'Input-DROP'

# 资源快照 (每 30s)
$H hilog | grep 'WL-STAT'

# 崩溃检测
$H hilog | grep -E 'CRASH|WL-ERR|SIGSEGV|SIGABRT'

# 事件注入详情 (含毫秒时间戳)
$H hilog | grep -E 'WL_Input.*Inject(Enter|Button|Key)'

# Wineboot 初始化诊断
$H shell "hilog -z 500 -t app" | grep -E 'wineboot-stderr|PAD-WB|Launch-Async|drive_c|wine\.inf|__wine_main|preloader_exec|spawn'
```

### 日志采集到本地

```bash
# Pad: 实时采集并保存
timeout 60 hdc -t 192.168.1.6:33363 hilog > /tmp/winehua.log 2>/dev/null

# Pad: 只取最近 N 行 app 日志
hdc -t 192.168.1.6:33363 shell "hilog -z 5000 -t app" > /tmp/winehua_dump.log

# 过滤关键 tag 存入文件
grep -E 'CLICK-PIPE|KBD-PIPE|PIPE|winehua|WineWM|WL_Plugin|WL_Seat|WL_Input|WWA|WL-ERR|WL-STAT|Input-DROP|CRASH|PAD-WB|wineboot-stderr' /tmp/winehua_dump.log > /tmp/winehua_filtered.log
```

## 输入事件（鼠标/键盘）调试工作流

涉及 UI 交互问题排查时，遵循以下流程：

### 1. 构建 + 部署 + 启动 App

```bash
cd /data/share/wineohos && bash build.sh hap && bash build.sh deploy <device_ip>
hdc -t <device_ip> shell "aa start -a EntryAbility -b app.hackeris.winehua"
```

### 2. 启动持续日志采集（后台运行）

```bash
# 清空旧日志，开始实时采集并过滤关键事件
: > /data/share/wineohos/.temp/live-monitor.log
hdc -t <device_ip> hilog 2>/dev/null | grep --line-buffered -E \
  'honwine.*(wl_seat.*registered|client bound|wl_pointer|wl_keyboard|keymap|CLICK-PIPE|KBD-PIPE|Seat\].*ERR|Seat\].*pipe|ptrRes=|needsEnter=|MW-FwdMouse|NAPI-Fwd|DROPPED|button dropped|motion dropped|InjectEnter|InjectButton|InjectKey|SCROLL|InjectAxis|InjectKbdModifiers)' \
  >> /data/share/wineohos/.temp/live-monitor.log
```

### 3. 用户操作

采集保持运行，用户在设备上操作（点击、键盘输入等）。

### 4. 停止采集 + 分析

```bash
# 分析采集到的日志
cat /data/share/wineohos/.temp/live-monitor.log
```

### 过滤关键词覆盖的排查维度

| 维度 | 关键词 | 排查问题 |
|------|--------|---------|
| Seat 生命周期 | `wl_seat.*registered`, `client bound`, `wl_pointer`, `wl_keyboard` | 资源反复创建/销毁 |
| 鼠标完整链路 | `CLICK-PIPE`, `MW-FwdMouse`, `ptrRes=`, `needsEnter=`, `SCROLL`, `InjectAxis` | action 值错位、enter 缺失、滚轮事件 |
| 键盘完整链路 | `KBD-PIPE`, `NAPI-Fwd`, `DROPPED`, `modifiers`, `InjectKbdModifiers` | enter/key 丢弃、修饰键状态 |
| 注入层错误 | `Seat].*ERR`, `button dropped`, `motion dropped` | Inject 失败原因 |
