# Submodule 可维护性专项报告

> 日期：2026-07-31
> 分支：评审时基于 `feature/render-element-completeness`，该分支已合入 master（PR #47）
> 目的：作为分支评审（长期可维护性维度）的依据文档。记录各 submodule 与上游的合并风险现状、改进路线图与验收标准。
> 关联文档：[PHASE2_DXVK_STATUS_MEMO.md](PHASE2_DXVK_STATUS_MEMO.md) / [DXVK_MODERN_UPGRADE_READINESS.md](DXVK_MODERN_UPGRADE_READINESS.md) / [CODE_IMPROVEMENT_PLAN.md](CODE_IMPROVEMENT_PLAN.md)

---

## 0. 总览：重合并风险矩阵

合并成本 = 侵入深度 × 上游活跃度 × 与我们基线的差距。

| Submodule | 上游基线 | 侵入模式 | 新增文件 | 合并方向 | 风险 |
|-----------|---------|---------|---------|---------|------|
| virglrenderer | freedesktop `main` | **深嵌**：`vkr_device_memory.c` +2275（8 hunks、46 处 winehua 引用、`#ifdef __OHOS__`）；`vkr_queue.c` +1134（数十个 `vkr_ohos_perf_*` atomic） | 少 | 我们合上游（上行） | 🔴 高 |
| mesa | **OpenHarmony mesa**（`OpenHarmony-6.0-Beta1`），不是 mesa main | 中：`vn_ring.c` +423，**512 槽 perf 数组直接嵌进 `struct vn_ring` 热路径**；`vn_renderer_vtest.c` +393 为新文件（好模式） | 部分 | 我们合上游（上行） | 🔴 高 |
| dxvk | doitsujin `v1.10.3` tag（**上游死线**） | 深嵌：`dxvk_context.cpp` +1834、`dxbc_compiler.cpp` +507；已有独立层先例：`dxvk_winehua_trace.h` +442、`d3d11_bc.cpp` +203 | 6 个（含 `WINEHUA_FORK.md`） | **上游 backport 进来（下行）** | 🟡 中-高 |
| wine | wine `master` | **良好**：以新增 smoke 程序为主，修改仅 `win32u/vulkan.c` +634、`ntdll/loader.c` +145 | 多 | 我们合上游（上行） | 🟢 低 |

**关键认知**：

1. mesa 的"上游"是 OpenHarmony 官方 mesa 分支（`.gitmodules` 中 `libdrm.branch = OpenHarmony-6.0-Beta1` 印证），不是 mesa main。升级跟踪 OH 的 mesa 维护节奏，与 mesa 主线无关。
2. dxvk 是唯一"下行合并"（上游 → 我们）：v1.10.3 已冻结，无新修复可拉，fork 处于"稳定但停滞"状态；未来切 2.x 时 5567 行 WineHua 改动是**重写不是合并**。
3. virglrenderer / mesa 上游活跃 → 深嵌改动每合一次都是人工手术，是本分支最大负债。

---

## 一、Fork/Upstream 重合并

### 1.1 上游识别与基线锁定（改进 C，P1）

| Submodule | 上游仓库/分支 | 基线记录（当前快照） | 合并节奏建议 |
|-----------|--------------|---------------------|-------------|
| virglrenderer | freedesktop/virglrenderer `main` | merge-base `8cb58e478`（2026-06-10） | **每季度尝试一次**，落后超两个大版本不可收拾 |
| mesa | OpenHarmony mesa（OH 分支） | tag `OpenHarmony-v6.0-Beta1`（merge-base e5d8c3f2，2025-06-13） | 跟随 OH 发布节奏，记录 OH 的升级周期 |
| wine | wine `master` | merge-base `13289668fd1`（2026-06-07，winehua 独有 89 commit） | 每次合 wine 时**先合 WineHua 侧新文件**（与上游无冲突，先落地减少 diff 堆积），再处理修改文件 |
| dxvk | doitsujin/dxvk `v1.10.3` | tag `v1.10.3`（merge-base e4fd5e9，2022-08-02） | 无需节奏；但每次 backport 上游 2.x bugfix 记录来源 commit |

**基线为派生数据（merge-base 可随时重算），本表只记录当前快照**：每次合并/升级后更新。升级本身是项目级事件（涉及构建 + 回归验证），不是 git 操作。重算命令：

```bash
MB=$(git -C thirdparty/<name> merge-base HEAD upstream/<branch>)
git -C thirdparty/<name> log -1 --format="%h %ad %s" --date=short $MB
git -C thirdparty/<name> rev-list --count $MB..HEAD   # winehua 独有 commit 数
```

上游 remote 用 `./scripts/setup-upstream-remotes.sh` 配置（新 clone 环境必跑，URL 与本表一致）。

### 1.2 侵入度量化（评审依据）

- **virglrenderer**（+6215 行 / 34 文件）：
  - `vkr_device_memory.c`：上游 ~57 行 → +2275 行，分布在 **8 个 hunk**，46 处 winehua 引用，`#ifdef __OHOS__` 守卫
  - `vkr_queue.c`：+1134 行，文件顶部数十个 `vkr_ohos_perf_*` atomic 计数器
  - `vkr_descriptor_set.c` +515 / `vkr_command_buffer.c` +449 / `vkr_renderer.c` +331 / `vkr_pipeline.c` +244
- **mesa**（+1619 / 16 文件）：`vn_ring.c` +423（含 VN_RING_PERF_COMMAND_TYPE_COUNT 512 槽数组嵌入 ring 热路径结构体）、`vn_queue.c` +413、`vn_renderer_vtest.c` +393（**新文件，好模式**）、`vtest_protocol.h` +26
- **dxvk**（+5567 / 60 文件）：仅 6 个新文件，核心改动深嵌 `dxvk_context.cpp` +1834、`dxbc_compiler.cpp` +507、`dxvk_cmdlist.cpp` +209、`dxvk_shader.cpp` +207
- **wine**（+8411 / 24 文件）：`programs/winehua_d3d11_smoke/main.c` 单文件 +5862，修改文件少（`win32u/vulkan.c` +634、`ntdll/loader.c` +145）

### 1.3 物理隔离：新代码进新文件（改进 A，P0）

缓解合并冲突的唯一有效机制是**物理隔离**。dxvk 已证明可行（`dxvk_winehua_trace.h`、`d3d11_bc.cpp` 独立成文件），virglrenderer 未做到。

**目标结构**（不改行为，纯结构重构，A/B 构建验证）：

```
src/venus/vkr_winehua_shadow.c/.h   ← shadow alloc / dirty-tracking / coverage-sort / msync 全部移入
vkr_device_memory.c                 ← 每文件仅保留 ≤3 处 hook：
    #ifdef __OHOS__
    vkr_winehua_shadow_init_for_allocation(mem, alloc_info);
    #endif
src/venus/vkr_winehua_perf.h        ← 性能计数器独立层，产品代码只留 VKR_PERF_INC(name)
```

**mesa 必须做**：`vn_ring.c` 的 512 槽 perf 数组移出热路径结构体（当前每个 ring 都背着 512×3 的数组，且合并冲突噪音大）。

**试点顺序**：`vkr_device_memory.c` → `vn_ring.c` → `vkr_queue.c`。

**验收标准**：上游升级时，每个文件只需确认 1-3 个 hook 位置，冲突从"手术级"降为"核对级"。

### 1.4 补丁清单：patch manifest（改进 B，P0）

合并冲突时最痛苦的是不知道"这个 hunk 对应我们的哪个改动、为什么存在、丢了它的后果"。

```
docs/submodule-patches/<submodule>.md
├── 每个改动条目：文件:函数 | 为什么存在 | 依赖的上游行为 | 不变式
│   └── 例：vkr_device_memory.c 分配走 SHM shadow 的判定
│       不变式：shadow 模式下 host-visible allocation 必须落在 SHM 上，
│       否则 Guest vkMapMemory 不可见 host 写入（表现为贴图花屏）
└── 每条记录对应的验证方法（哪个 smoke 测试 / 游戏场景能回归它）
```

**不变式是清单的灵魂**——合并时只要不变式不破坏，实现怎么改都可以。清单同时是新人入职文档与合并操作手册。

### 1.5 dxvk 双轨战略（改进 D，P1）

- 当前 v1.10.3 是上游死线，fork 稳定但停滞；`DXVK_MODERN_UPGRADE_READINESS.md` 已评估 920 可试点 2.x，但 2.x 与 1.10.3 结构差异巨大，**现有改动是重写不是迁移**。
- 正式化两条通道：
  - `dxvk-legacy-1.10.3`（产品分支）：只收 cherry-pick 的上游 bugfix，每次记录来源 commit
  - `feature/dxvk-modern`（2.x 重写试点）：独立 manifest profile（如 `DXVK_MODERN`），走 `DXVK_MODERN → DXVK_LEGACY → WineD3D` fallback 链（合并决策记录见 [archive/PHASE2_DXVK_MERGE_REPORT.md](archive/PHASE2_DXVK_MERGE_REPORT.md)）
- **禁止两条通道在同一分支共存**——那会把一次升级变成两次迁移。

---

## 二、持续迭代：降低每次改动的成本

### 2.1 Profile 映射单一数据源（改进 E，P0）

**问题**：`entry/src/main/cpp/napi_init.cpp:SetHostShadowProfile`（~150 行条件分支、30+ bool 变量）与 `entry/src/main/cpp/wine_launch.cpp:AppendStableDesktopDxvkEnv`（~40 行）**各自维护一份 profile名→行为映射**。两侧语义必须一致，但没有机制保证。一次 profile 演进会同时改两个文件，漏改即静默行为漂移（guest 期望 A、host 给 B——0x887a0004 类事故的温床）。

**方案**：单一注册表（常量数组），两侧共同 include：

```cpp
struct ShadowProfile {
    const char* name;           // "shadow-precise-dirty-ring-inline-upload-coverage-sort"
    const char* shadowMode;
    const char* shadowSelector;
    bool mergeRanges;
    bool gpuUpload;
    bool uploadWait;
    const char* presentMode;
    bool isProductDefault;
    bool isDiagnosticOnly;
};
constexpr ShadowProfile kShadowProfiles[] = { /* 一行一个 profile */ };

const ShadowProfile* FindShadowProfile(const char* name);
void ApplyProfile(const ShadowProfile*, VirglHostConfig*);   // napi_init 侧
void ApplyProfileEnv(const ShadowProfile*);                  // wine_launch 侧（guest env）
```

**验收**：`profile_config_test` 对每个 profile 断言"napi_init 侧输出 == wine_launch 侧输出"；新增 profile 只加一行数据。

### 2.2 Env Var 目录与 guest/host 握手（改进 F，P1）

- **现状**：约 30 个 env var 无 schema、无版本。fingerprint 已接入两处（`graphics_broker.cpp:235` 启动日志、`:1205` 运行中配置变更强制报错"App restart required"）——但它是 **host 侧自检**，解决不了"guest 侧 DXVK 收到的 env 与 host 侧配置漂移"（两侧由不同代码生成，各自算出各自的 hash，握手不上去）。
- **第一步（低成本）**：建 `docs/ENV_VAR_CATALOG.md`——全部 env var 表格：读取者（guest DXVK / host virglrenderer / box64 / 主仓库）、默认值、由哪个 profile 字段派生、引入版本。这张表本身就是升级手册。
- **第二步（中期）**：把 `VirglHostLaunchConfig.fingerprint` 升级为跨 IPC 握手：guest 侧把 DXVK 实际生效的 env 组合回传，host 对比期望值，不匹配打 `WL-ERR` 级错误而不是静默跑。

### 2.3 诊断与产品代码分离（改进 G，P1）

- **现状**：`vn_ring.c` 的 512 槽 perf 数组、`vkr_queue.c` 顶部数十个 `vkr_ohos_perf_*` atomic、`VKR_WINEHUA_UBO_TRACE_LIMIT 200000` 等 trace 常量全部嵌在产品热路径里。
- **问题**：读代码需跳过它们；perf 层 bug 可能污染产品路径；且它们出现在合并 diff 中增加冲突噪音（上游改热路径时 perf 行与逻辑行纠缠在同一个 hunk）。
- **方案**：独立 `*_winehua_perf.*` 文件 + 条件编译宏，产品代码只留零成本 `VKR_PERF_INC()` 调用（同改进 A）。

---

## 三、质量保证

### 3.1 现状盘点

| 项 | 状态 |
|----|------|
| CI 构建门禁 | ✅ `build.yml` 已有 `pull_request: branches: [master]` + push + tag + workflow_dispatch |
| KNOWN_GOOD/UNKNOWN/REJECTED 分类系统 | ✅ STATUS_MEMO 机制，业界少见，**必须保留** |
| submodule 一致性检查入 CI | ❌ `scripts/check-submodules.sh` 只在本地跑 |
| 已知坑自动化回归 | ❌ 0x887a0004 / Crysis3 DllPath / steam_api64 SAFEFLAGS 依赖人工记忆 |
| Smoke 测试结构 | ⚠️ 单文件 5862 行（winehua_d3d11_smoke/main.c），无测试清单文档 |

### 3.2 CI submodule 门禁 job（改进 H，P0）

纯脚本，可立即落地：

```yaml
jobs:
  submodule-check:   # 快速 job，与 build 并行，PR/push 都跑
    steps:
      - checkout（recursive）
      - ./scripts/check-submodules.sh            # gitlink 可达性 + 指针一致性
      - 分支约定检查：每个 submodule 当前指针在
        .gitmodules 声明分支（wine=master, box64=main, virglrenderer=master,
        libepoxy=master, mesa=main, libdrm=OpenHarmony-6.0-Beta1,
        dxvk=dxvk-legacy-1.10.3）的远程历史中
      - submodule 工作树干净检查（无未提交改动）
```

这直接把 Maintainer 检查清单的"打回"条件（见 `.claude/rules/submodule-workflow.md`）变成机器判断。

### 3.3 已知坑回归脚本（改进 I，P1）

```
scripts/qa/known-regressions.sh
├── 01_env_protocol：验证 SPAWN\n 协议 env 注入生效（回归 0x887a0004）
├── 02_crysis_dllpath：启动 Crysis 3 → 断言无 0xC0000135
├── 03_safeflags：启动 steam_api64 目标 → 断言无 AV
└── 每次运行把 hilog 证据 + HAP hash 归档到带时间戳目录
```

配合 STATUS_MEMO 已立的"KNOWN_GOOD 归档签名 HAP"规则，落成脚本即流程闭环。

### 3.4 Smoke 测试治理（改进 J，P2）

5862 行单文件不要求立刻拆（拆动可能引入新 bug 且无功能收益），但定规则：**新测试进新文件**（按主题 present/shadow/format/sync），并在清单里记录"每个测试验证的不变式"——与补丁清单（1.4）呼应：**测试是补丁清单不变式的执行者**。

---

## 四、路线图与优先级

| 阶段 | 动作 | 成本 | 解决 |
|------|------|------|------|
| **合并前** | B：补丁清单 ×4 submodule；F-1：env var 目录；C：基线文档；D：dxvk 双轨决策 | 文档工作 | 重合并可操作性 |
| **合并前** | H：CI submodule job | 低（纯 yml + 脚本） | 质量门禁 |
| **下个迭代** | E：profile 单一数据源 + 一致性测试；I：known-regressions 脚本 | 中 | 迭代成本 + 回归 |
| **Q3** | A：物理隔离试点（vkr_device_memory → vkr_winehua_shadow.c，然后 vn_ring） | 中-高（不改行为，A/B 验证） | 重合并根治 |
| **长期** | D：dxvk-modern 试点通道；C：合并节奏制度化（每季度） | 高 | dxvk 2.x 迁移 |

**顺序逻辑**：文档类先做（成本≈0、可立即合并、为后续重构提供语义锚点）；代码类按合并窗口排序——virglrenderer/mesa 上游一旦发新版本，物理隔离（A）的紧迫性从 Q3 提到当下。

---

## 五、证据索引

评审核对用，全部为本分支实际代码：

- `entry/src/main/cpp/napi_init.cpp` — `SetHostShadowProfile`（~150 行条件分支）
- `entry/src/main/cpp/wine_launch.cpp` — `AppendStableDesktopDxvkEnv`（profile 映射重复实现）、`CopyFileIfNeeded`（mkstemp+fsync+rename 原子写）、`EnsureWow64Files`
- `entry/src/main/cpp/graphics_broker.cpp:235` — fingerprint 启动日志；`:1205` — 配置变更强制校验
- `entry/src/main/cpp/virgl_host_config.h:26,31-33` — `fingerprint` 字段 + Validate/Fingerprint/Build 三函数
- `entry/src/main/cpp/virgl_child.cpp:478` — launch fingerprint 透传
- `thirdparty/virglrenderer` `d4446dd` — +6215 行 / 34 文件（见 1.2）
- `thirdparty/mesa` `f1447e05` — +1619 行 / 16 文件（见 1.2）
- `thirdparty/dxvk` `abe71bc0` — +5567 行 / 60 文件，基线 `v1.10.3`（merge-base e4fd5e9）；`WINEHUA_FORK.md` 已记录 fork 策略
- `thirdparty/wine` `3a69dcad` — +8411 行 / 24 文件
- `.gitmodules` — branch 约定：wine=master, box64=main, virglrenderer=master, libepoxy=master, mesa=main, libdrm=OpenHarmony-6.0-Beta1, dxvk=dxvk-legacy-1.10.3
- `.github/workflows/build.yml` — 已含 PR 触发
- `scripts/check-submodules.sh` — gitlink 可达性 + 指针一致性（本地脚本，未入 CI）
- `docs/DXVK_MODERN_UPGRADE_READINESS.md` — 设备能力矩阵（920 phone Vulkan 1.3.309 vs 910 tablet 1.2.275）
