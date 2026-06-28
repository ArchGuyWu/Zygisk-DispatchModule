#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# enter_sandbox.sh
# 自动将当前工作区挂载到 proot-distro 容器中，提供安全的隔离分析环境
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "📦 正在检索本地已安装的 proot-distro 容器..."
DISTROS=$(python3 -c "import subprocess, re; out = subprocess.getoutput('proot-distro list'); distros = [re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', line).strip('* \t') for line in out.splitlines() if '*' in line]; print('\n'.join(distros))")

if [ -z "$DISTROS" ]; then
    echo "❌ 未检测到任何已安装的 proot-distro 容器。"
    echo "💡 您可以使用 'proot-distro install ubuntu' 进行安装。"
    exit 1
fi

# 默认使用列表中的第一个容器（通常是 ubuntu 或 debian）
TARGET_DISTRO=$(echo "$DISTROS" | head -n 1)

echo "🎯 自动选择容器: $TARGET_DISTRO"
echo "📂 挂载宿主机目录: $WORKSPACE_DIR"
echo "➡️  映射至容器内部: /root/workspace"
echo "🚀 正在进入隔离分析沙箱..."
echo "💡 提示：输入 'exit' 可随时退出沙箱返回 Termux。"
echo ""

# 执行挂载并登录
proot-distro login "$TARGET_DISTRO" --bind "$WORKSPACE_DIR:/root/workspace" "$@"
