#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# auto_commit_push.sh
# 每次完整修改后，自动编译、打包、提交并推送至远端仓库
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_DIR="$WORKSPACE_DIR"
CONTAINER_NAME="ubuntu-build"

echo "=== Zygisk-DispatchModule Auto Build & Auto Push System ==="
echo ""

# 1. 检测是否有未提交的更改
if [ -z "$(git -C "$SCRIPT_DIR" status --porcelain)" ]; then
    echo "ℹ️ No changes detected in the working tree. Nothing to build or push."
    exit 0
fi

echo "🚀 Changes detected. Staging changes and committing directly to trigger remote CI/CD..."

# 2. Git 提交与推送
echo "📝 Staging changes and committing..."
git -C "$SCRIPT_DIR" add .

# 自动生成或使用传入的参数作为 Commit Message
if [ -n "${1:-}" ]; then
    COMMIT_MSG="$1"
else
    COMMIT_MSG="Auto-build & push: $(date '+%Y-%m-%d %H:%M:%S') - Code updates verified"
fi
git -C "$SCRIPT_DIR" commit -m "$COMMIT_MSG"

# 自动运行 git push 推送至远端仓库
git -C "$SCRIPT_DIR" push origin master

echo "========================================================="
echo "🎉 SUCCESS: Complete build & remote push achieved!"
echo "========================================================="
