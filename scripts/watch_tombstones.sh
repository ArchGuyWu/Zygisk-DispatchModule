#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# watch_tombstones.sh — 轮询 /data/tombstones/，集齐 N 条新墓碑后批量分析
#
# 用法:
#   ./scripts/watch_tombstones.sh --once
#   ./scripts/watch_tombstones.sh --threshold 2 --interval 30
#   ./scripts/watch_tombstones.sh --daemon
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TOMB_SRC="/data/tombstones"
STAGING="$WORKSPACE_DIR/tombstones_temp"
STATE_FILE="$STAGING/.watch_seen_ids"
QUEUE_FILE="$STAGING/.watch_pending_ids"
PARSER="$STAGING/parse_crashes.py"
REPORT_DIR="$WORKSPACE_DIR/docs/tombstone_batches"

THRESHOLD=2
INTERVAL=30
ONCE=0
DAEMON=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threshold) THRESHOLD="${2:-2}"; shift 2 ;;
        --interval)  INTERVAL="${2:-30}"; shift 2 ;;
        --once)      ONCE=1; shift ;;
        --daemon)    DAEMON=1; shift ;;
        -h|--help)
            grep '^#' "$0" | head -12 | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

mkdir -p "$STAGING" "$REPORT_DIR"
touch "$STATE_FILE" "$QUEUE_FILE"

if ! su -c "test -d '$TOMB_SRC'" 2>/dev/null; then
    echo "❌ 无法访问 $TOMB_SRC（需要 root/su）"
    exit 1
fi

ingest_new() {
    local id added=0
    while read -r id; do
        [[ -z "$id" ]] && continue
        if grep -qx "$id" "$STATE_FILE" 2>/dev/null; then
            continue
        fi
        if su -c "cp '$TOMB_SRC/tombstone_$id' '$STAGING/tombstone_$id' && chmod 644 '$STAGING/tombstone_$id'" 2>/dev/null; then
            echo "$id" >> "$STATE_FILE"
            echo "$id" >> "$QUEUE_FILE"
            echo "📥 tombstone_$id"
            added=$((added + 1))
        fi
    done < <(su -c "ls -1 '$TOMB_SRC'" | grep -E '^tombstone_[0-9]+$' | sed 's/tombstone_//' | sort -n)
    return 0
}

analyze_queue() {
    local ids=() id
    while read -r id; do
        [[ -n "$id" ]] && ids+=("$id")
    done < "$QUEUE_FILE"

    local count=${#ids[@]}
    if (( count < THRESHOLD )); then
        echo "⏳ 待分析队列: $count / $THRESHOLD"
        return 0
    fi

    local stamp report
    stamp="$(date '+%Y%m%d_%H%M%S')"
    report="$REPORT_DIR/batch_${stamp}.txt"

    {
        echo "Tombstone batch analysis"
        echo "Generated: $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo "Count: $count"
        echo "IDs: ${ids[*]}"
        echo
    } > "$report"

    WORKSPACE_DIR="$WORKSPACE_DIR" IDS="${ids[*]}" python3 <<'PY' >> "$report"
import importlib.util, os
from pathlib import Path

workspace = Path(os.environ['WORKSPACE_DIR'])
staging = workspace / 'tombstones_temp'
spec = importlib.util.spec_from_file_location('pc', staging / 'parse_crashes.py')
pc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc)
symbols = pc.load_symbols()
for token in os.environ.get('IDS', '').split():
    path = staging / f'tombstone_{token}'
    if not path.exists():
        print(f'--- tombstone_{token}: MISSING ---')
        continue
    res = pc.analyze_tombstone(str(path), symbols)
    if not res:
        print(f'--- tombstone_{token}: parse failed ---')
        continue
    print('=' * 72)
    print(f"{res['file']}  {res['timestamp']}")
    print(res['signal'])
    if res.get('cause'):
        print(res['cause'])
    if res.get('abort'):
        print(res['abort'])
    for fr in res['frames'][:4]:
        v = hex(fr['elf_vaddr']) if fr.get('elf_vaddr') else 'n/a'
        print(f"  #{fr['num']:02d} ELF={v} -> {fr['symbol']}")
PY

    : > "$QUEUE_FILE"
    echo "📊 报告已写入: $report"
    echo "==================== 摘要 ===================="
    sed -n '1,160p' "$report"
}

run_loop() {
    ingest_new
    analyze_queue
}

if (( ONCE )); then
    run_loop
    exit 0
fi

if (( DAEMON )); then
    nohup bash "$0" --threshold "$THRESHOLD" --interval "$INTERVAL" >> "$WORKSPACE_DIR/docs/tombstone_watch.log" 2>&1 &
    echo "🟢 后台监控已启动 (pid $!, interval=${INTERVAL}s, threshold=$THRESHOLD)"
    exit 0
fi

echo "👀 监控 $TOMB_SRC （每 ${INTERVAL}s，阈值 ${THRESHOLD}）— Ctrl+C 停止"
while true; do
    run_loop
    sleep "$INTERVAL"
done