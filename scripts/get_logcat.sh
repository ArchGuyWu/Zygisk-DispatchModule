#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# get_logcat.sh
# 自动清除并抓取游戏 com.rockstargames.gtasa.de 的 logcat 运行日志
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-$WORKSPACE_DIR/logcat_latest.log}"

echo "🧹 正在清理历史 logcat 缓存..."
su -c "logcat -c"

echo "⏳ 正在实时抓取游戏运行日志，按 Ctrl+C 可提前结束..."
echo "📂 日志将输出至: $DEST"

# 检索游戏 PID
PID=$(su -c "pidof com.rockstargames.gtasa.de" || true)

if [ -n "$PID" ]; then
    echo "🎯 发现游戏正在运行 (PID: $PID)，正在过滤该进程的日志..."
    su -c "logcat -d --pid=$PID > '$DEST'" || true
else
    echo "⚠️ 游戏当前未运行，将捕获全局包含 'DispatchModule', 'shadowhook', 'policemod' 的日志..."
    su -c "logcat -d | grep -E '(DispatchModule|shadowhook|policemod)' > '$DEST'" || true
fi

echo "✅ 日志抓取完成！(共 $(wc -l < "$DEST") 行)"
