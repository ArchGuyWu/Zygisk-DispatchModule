#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# auto_commit_push.sh
# 提交前可选编译验证，再推送至远端仓库
# 跳过编译: SKIP_BUILD=1 ./auto_commit_push.sh
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_DIR="$WORKSPACE_DIR"
CONTAINER_NAME="ubuntu-build"

echo "=== Zygisk-DispatchModule Auto Build & Push ==="
echo ""

# 1. 检测是否有未提交的更改
if [ -z "$(git -C "$SCRIPT_DIR" status --porcelain)" ]; then
    echo "ℹ️ No changes detected in the working tree. Nothing to build or push."
    exit 0
fi

# 2. 编译验证（默认开启，可用 SKIP_BUILD=1 跳过）
if [ "${SKIP_BUILD:-}" != "1" ]; then
    echo "🔨 Running pre-push build verification..."
    if ! bash "$SCRIPT_DIR/build_in_container.sh"; then
        echo "❌ Build failed. Fix errors before pushing, or set SKIP_BUILD=1 to bypass."
        exit 1
    fi
    echo "✅ Build verification passed."
else
    echo "⚠️ SKIP_BUILD=1 — skipping compile verification."
fi

# 3. Git 提交与推送
echo "📝 Staging changes and committing..."
git -C "$SCRIPT_DIR" add .

if [ -n "${1:-}" ]; then
    COMMIT_MSG="$1"
else
    COMMIT_MSG="Auto-build & push: $(date '+%Y-%m-%d %H:%M:%S') - Build verified"
fi
git -C "$SCRIPT_DIR" commit -m "$COMMIT_MSG"
git -C "$SCRIPT_DIR" push origin master

echo "========================================================="
echo "🎉 SUCCESS: Build verified, committed, and pushed."
echo "========================================================="