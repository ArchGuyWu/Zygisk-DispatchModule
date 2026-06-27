#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# auto_commit_push.sh
# 每次完整修改后，自动编译、打包、提交并推送至远端仓库
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONTAINER_NAME="ubuntu-build"

echo "=== Zygisk-DispatchModule Auto Build & Auto Push System ==="
echo ""

# 1. 检测是否有未提交的更改
if [ -z "$(git -C "$SCRIPT_DIR" status --porcelain)" ]; then
    echo "ℹ️ No changes detected in the working tree. Nothing to build or push."
    exit 0
fi

echo "🚀 Changes detected. Starting complete build verification inside isolated container..."

# 2. 清理旧编译文件以保证“完整干净修改”
if [ -d "$SCRIPT_DIR/build" ]; then
    echo "🧹 Cleaning previous build directory..."
    rm -rf "$SCRIPT_DIR/build"
fi

# 3. 运行隔离容器进行编译打包
echo "🏗️  Compiling in container '${CONTAINER_NAME}'..."
if proot-distro login --isolated --bind "$WORKSPACE_DIR:/workspace" "$CONTAINER_NAME" -- bash "/workspace/Zygisk-DispatchModule/build_in_container.sh"; then
    echo "✅ Build completed successfully!"
else
    echo "❌ ERROR: Container build failed! Aborting auto-commit."
    exit 1
fi

# 4. Git 提交与推送
echo "📝 Staging changes and committing..."
git -C "$SCRIPT_DIR" add .

# 自动生成包含当前时间和更改摘要的 Commit Message
COMMIT_MSG="Auto-build & push: $(date '+%Y-%m-%d %H:%M:%S') - Code updates verified"
git -C "$SCRIPT_DIR" commit -m "$COMMIT_MSG"

# 自动运行 git push 推送至远端仓库
git -C "$SCRIPT_DIR" push origin master

echo "========================================================="
echo "🎉 SUCCESS: Complete build & remote push achieved!"
echo "========================================================="
