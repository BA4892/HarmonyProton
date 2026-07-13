#!/bin/bash
# 检查 submodule 状态：HEAD vs remote default branch
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "=== Submodule 状态检查 ==="
echo ""

for sm in wine box64 libepoxy mesa virglrenderer libdrm; do
    path="thirdparty/$sm"
    HEAD=$(git ls-tree HEAD "$path" | awk '{print $3}')
    
    # 获取 default branch
    branch=$(git config -f .gitmodules --get "submodule.$path.branch" 2>/dev/null || echo "master")
    
    # 获取 remote HEAD
    cd "$path"
    remote_url=$(git remote get-url origin)
    remote_sha=$(git ls-remote origin "$branch" 2>/dev/null | awk '{print $1}')
    
    echo "--- $sm ($branch) ---"
    echo "  tracked: $HEAD"
    echo "  remote:  $remote_sha"
    
    if [ "$HEAD" = "$remote_sha" ]; then
        echo "  ✅ 与 remote 一致"
    else
        # 检查 tracked commit 是否在 remote 历史中
        if git branch -r --contains "$HEAD" 2>/dev/null | grep -q .; then
            echo "  ⚠️  tracked commit 在 remote 分支中, 但不在 $branch 尖端"
        else
            echo "  ❌ tracked commit 不在任何 remote 分支中! 需要推送!"
        fi
    fi
    echo ""
    cd "$ROOT"
done
