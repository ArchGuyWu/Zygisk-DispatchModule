#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# get_tombstone.sh
# 自动抓取 Android 系统最新生成的 tombstone 崩溃日志并输出简要分析
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-$WORKSPACE_DIR/tombstone_latest}"

echo "🔍 正在检索 /data/tombstones/ 下最新生成的墓碑文件..."

# 获取最新墓碑文件名
LATEST=$(su -c 'ls -t /data/tombstones/ | head -n 1' 2>/dev/null || true)

if [ -z "$LATEST" ]; then
    echo "❌ 未在 /data/tombstones/ 下找到任何墓碑文件，请确认设备已 Root 且发生过闪退。"
    exit 1
fi

echo "🎯 发现最新墓碑文件: $LATEST"
echo "📥 正在复制并授权至: $DEST ..."
su -c "cp /data/tombstones/$LATEST '$DEST' && chmod 644 '$DEST'"

echo "✅ 抓取成功！"
echo ""
echo "==================== 闪退日志摘要 (前 40 行) ===================="
head -n 40 "$DEST"
echo "==============================================================="
