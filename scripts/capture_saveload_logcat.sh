#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# capture_saveload_logcat.sh — 读档/闪退专用 logcat 抓取（dispatchCenter SaveLoad 时间线）
#
# 用法（重启刷模组后）:
#   ./scripts/capture_saveload_logcat.sh          # 清缓冲 → 实时抓，Ctrl+C 结束
#   ./scripts/capture_saveload_logcat.sh --dump   # 不清缓冲，闪退后一次性导出
#   ./scripts/capture_saveload_logcat.sh --no-clear   # 实时抓但保留已有缓冲
#
# 输出目录: logcat_watch/saveload_YYYYMMDD_HHMMSS.{full,summary}.log
# 把 *.summary.log + tombstones_temp/tombstone_XX 发给分析即可。
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$WORKSPACE_DIR/logcat_watch"
STAMP="$(date '+%Y%m%d_%H%M%S')"
TAG="dispatchCenter"
GAME_PKG="com.rockstargames.gtasa.de"

MODE="watch"
CLEAR=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dump)      MODE="dump"; CLEAR=0; shift ;;
        --no-clear)  CLEAR=0; shift ;;
        --watch)     MODE="watch"; shift ;;
        -h|--help)
            sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

mkdir -p "$OUT_DIR"
FULL="$OUT_DIR/saveload_${STAMP}.full.log"
SUMMARY="$OUT_DIR/saveload_${STAMP}.summary.log"

logcat_cmd() {
    if command -v su >/dev/null 2>&1 && su -c "true" 2>/dev/null; then
        su -c "$*"
    else
        eval "$@"
    fi
}

FILTER_RE='GenericLoad|LoadDataInSlot|LoadGameFromSlot|ms_bLoading|Session begin|Session end|Hydration|Skip pipeline|u_strlen|ms_bFailed|deserialize|Left frontend|Menu read-save|RestoreForStartLoad|All hooks installed|VanillaQoL|SaveLoad'

echo "=== SaveLoad logcat capture ==="
echo "Mode: $MODE | clear buffer: $CLEAR"
echo "Full log:    $FULL"
echo "Summary log: $SUMMARY"
echo ""

if (( CLEAR )); then
    echo "🧹 Clearing logcat buffer..."
    logcat_cmd "logcat -c" || true
    echo "✅ Buffer cleared."
    echo "   1) 启动游戏，等 logcat 出现「All hooks installed」"
    echo "   2) 主菜单 Load Game → 选 slot（不要点继续）"
    echo ""
fi

if [[ "$MODE" == "dump" ]]; then
    echo "📥 Dumping current logcat (dispatchCenter)..."
    logcat_cmd "logcat -d -v time -s ${TAG}:I ${TAG}:W" > "$FULL" || true
else
    echo "⏳ Live capture (dispatchCenter). Reproduce load-save crash, then Ctrl+C."
    echo "   Tip: run in another terminal: ./scripts/watch_tombstones.sh --once"
    echo ""
    trap 'echo ""; echo "⏹ Stopping capture..."' INT TERM
    logcat_cmd "logcat -v time -s ${TAG}:I ${TAG}:W" | tee "$FULL" || true
fi

if [[ ! -s "$FULL" ]]; then
    echo "⚠️ No dispatchCenter lines captured. Trying game PID fallback..."
    PID="$(logcat_cmd "pidof $GAME_PKG" 2>/dev/null || true)"
    if [[ -n "$PID" ]]; then
        logcat_cmd "logcat -d -v time --pid=$PID" > "$FULL" || true
    else
        logcat_cmd "logcat -d -v time" > "$FULL" || true
    fi
fi

rg -i "$FILTER_RE" "$FULL" > "$SUMMARY" 2>/dev/null || grep -Ei "$FILTER_RE" "$FULL" > "$SUMMARY" || true

FULL_LINES=$(wc -l < "$FULL" | tr -d ' ')
SUM_LINES=$(wc -l < "$SUMMARY" | tr -d ' ')

echo ""
echo "✅ Done — full: ${FULL_LINES} lines, summary: ${SUM_LINES} lines"
echo ""
echo "==================== SaveLoad summary ===================="
if [[ -s "$SUMMARY" ]]; then
    cat "$SUMMARY"
else
    echo "(no SaveLoad keywords — send full log instead)"
    head -n 30 "$FULL" || true
fi
echo "========================================================="
echo ""
echo "📤 Send for analysis:"
echo "   $SUMMARY"
echo "   (+ tombstones_temp/tombstone_XX if crashed)"