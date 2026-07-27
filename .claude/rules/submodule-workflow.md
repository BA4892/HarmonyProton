# Submodule 管理方案

## 分支约定

| 分支 | 用途 |
|------|------|
| `master` | 稳定，永远可构建 |
| `feature/*` | 日常开发，主仓库和 submodule **同名** |

## 流程一：Developer（提交 PR）

### 1. 准备

```bash
git checkout master
git pull
git submodule update --init --recursive
git checkout -b feature/<name>
```

### 2. 有 submodule 改动？

**没有** → 跳到第 3 步。

**有**：

```bash
# a) submodule 开同名分支
cd thirdparty/<name>
git checkout -b feature/<name>

# b) 改代码，commit
git add .
git commit -m "feat: xxx"

# c) 推到 submodule remote
git push origin feature/<name>

# d) 回到主仓库，登记指针（此时指向 feature/<name>）
cd ../..
git add thirdparty/<name>
git commit -m "submodule: <name> → feature/<name>"
```

### 3. 改主仓库代码

```bash
# 改代码，commit
git add .
git commit -m "feat: xxx"
```

### 4. 自测

```bash
make NATIVE_ARCH=arm64-v8a
# 部署测试
./scripts/check-submodules.sh   # 确认 submodule commit 已推送到 remote
```

### 5. 推分支，提 PR

```bash
git push origin feature/<name>
# 在 GitHub 提 PR: feature/<name> → master
```

Developer 的工作到此结束。Maintainer 接管后续。

---

## 流程二：Maintainer（审查并合并 PR）

### 1. 拉取 PR 分支

```bash
git fetch origin feature/<name>
git checkout feature/<name>
```

### 2. 检查 submodule 状态

```bash
./scripts/check-submodules.sh
```

对比 PR 分支和 master 的 submodule 指针。对每个有变更的 submodule：

**a) Developer 是否已把改动推到 remote？**

```bash
cd thirdparty/<name>
git ls-remote origin $(git rev-parse HEAD)
# 无输出 → Developer 忘了 push → 打回
```

**b) submodule commit 是否已在 default 分支上？**

```bash
git branch -r --contains HEAD origin/<default> 2>/dev/null
# 无输出 → submodule 的 feature 分支还没合
```

**c) 合入 submodule 的 default 分支：**

```bash
cd thirdparty/<name>
git fetch origin
git checkout <default>
git merge feature/<name>
git push origin <default>
cd ../..
```

**d) 更新主仓库 submodule 指针（指回 default 分支）：**

```bash
cd thirdparty/<name>
git checkout <default> && git pull
cd ../..
git add thirdparty/<name>
git commit -m "submodule: <name> 指针更新到 <default>"
```

### 3. 审查代码

```bash
git diff master...feature/<name>           # 主仓库代码变更
git log --oneline master..feature/<name>   # commit 历史
```

### 4. 构建测试

```bash
make NATIVE_ARCH=arm64-v8a
# 部署测试
```

### 5. 合并

```bash
git checkout master
git merge feature/<name> --ff-only
git push origin master
```

### 6. 清理

```bash
git branch -d feature/<name>
git push origin --delete feature/<name>    # 可选
```

---

## Maintainer 检查清单

| 检查项 | 命令 | 不通过 |
|--------|------|--------|
| submodule commit 在 remote | `git ls-remote` | 打回 |
| submodule 在 default 分支 | `git branch -r --contains` | Maintainer 合入 |
| 主仓库指针指向 default | `./scripts/check-submodules.sh` | Maintainer 更新指针 |
| 构建通过 | `make` | 打回 |
| 可 fast-forward | `git merge --ff-only` | 检查基线 |
