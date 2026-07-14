#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# collect_tombstones.sh — 采集 /data/tombstones/ 墓碑并解析符号
#
# 用法:
#   ./scripts/collect_tombstones.sh                 # 最新 1 条
#   ./scripts/collect_tombstones.sh --count 2       # 最新 2 条
#   ./scripts/collect_tombstones.sh --new           # 仅采集未记录的新墓碑
#   ./scripts/collect_tombstones.sh 15 16         # 指定 tombstone ID
#   ./scripts/collect_tombstones.sh --analyze-only  # 只分析 staging 已有文件
#
# 采集成功后自动清理：
#   - tombstones_temp/ 中不在本批次的旧副本
#   - /data/tombstones/  中不在本批次的系统墓碑（需 root/su）
# =====================================================================
set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TOMB_SRC="/data/tombstones"
STAGING="$WORKSPACE_DIR/tombstones_temp"
STATE_FILE="$STAGING/.watch_seen_ids"
PARSER="$STAGING/parse_crashes.py"
REPORT_DIR="$WORKSPACE_DIR/docs/tombstone_batches"

COUNT=1
NEW_ONLY=0
ANALYZE_ONLY=0
IDS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --count)
            COUNT="${2:-1}"
            shift 2
            ;;
        --new)
            NEW_ONLY=1
            shift
            ;;
        --analyze-only)
            ANALYZE_ONLY=1
            shift
            ;;
        -h|--help)
            sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            if [[ "$1" =~ ^[0-9]+$ ]]; then
                IDS+=("$1")
                shift
            else
                echo "Unknown option: $1"
                exit 1
            fi
            ;;
    esac
done

mkdir -p "$STAGING" "$REPORT_DIR"
touch "$STATE_FILE"

if [[ ! -f "$PARSER" ]]; then
    echo "❌ 缺少解析器: $PARSER"
    exit 1
fi

is_gtasa_tombstone() {
    local id="$1"
    local src="$TOMB_SRC/tombstone_$id"
    local cmdline
    cmdline="$(su -c "grep -m1 '^Cmdline:' '$src' 2>/dev/null" | sed 's/^Cmdline:[[:space:]]*//')"
    [[ "$cmdline" == *"gtasa"* ]]
}

copy_tombstone() {
    local id="$1"
    local src="$TOMB_SRC/tombstone_$id"
    local dest="$STAGING/tombstone_$id"

    if ! su -c "test -r '$src'" 2>/dev/null; then
        echo "⚠️  无法读取 $src"
        return 1
    fi

    if ! is_gtasa_tombstone "$id"; then
        local cmdline
        cmdline="$(su -c "grep -m1 '^Cmdline:' '$src' 2>/dev/null" | sed 's/^Cmdline:[[:space:]]*//')"
        echo "⏭️  跳过非游戏墓碑 tombstone_$id (Cmdline: ${cmdline:-unknown})"
        return 1
    fi

    su -c "cp '$src' '$dest' && chmod 644 '$dest'"
    local ts
    ts="$(su -c "grep -m1 '^Timestamp:' '$src' 2>/dev/null" | sed 's/^Timestamp:[[:space:]]*//')"
    [[ -z "$ts" ]] && ts="unknown"
    local key="${id}|${ts}"
    if ! grep -qxF "$key" "$STATE_FILE" 2>/dev/null; then
        echo "$key" >> "$STATE_FILE"
    fi
    echo "📥 tombstone_$id ($ts)"
}

prune_old_tombstones() {
    local -a keep_ids=("$@")
    if ((${#keep_ids[@]} == 0)); then
        return 0
    fi

    local f id kid keep

    shopt -s nullglob
    for f in "$STAGING"/tombstone_*; do
        [[ -f "$f" ]] || continue
        id="${f##*/tombstone_}"
        keep=0
        for kid in "${keep_ids[@]}"; do
            if [[ "$id" == "$kid" ]]; then
                keep=1
                break
            fi
        done
        if (( ! keep )); then
            rm -f "$f"
            echo "🗑️  已删 staging: tombstone_$id"
        fi
    done
    shopt -u nullglob

    if ! su -c "test -d '$TOMB_SRC'" 2>/dev/null; then
        return 0
    fi

    local all_ids=()
    mapfile -t all_ids < <(list_all_ids)
    for id in "${all_ids[@]}"; do
        [[ -z "$id" ]] && continue
        keep=0
        for kid in "${keep_ids[@]}"; do
            if [[ "$id" == "$kid" ]]; then
                keep=1
                break
            fi
        done
        if (( ! keep )); then
            if su -c "rm -f '$TOMB_SRC/tombstone_$id'" 2>/dev/null; then
                echo "🗑️  已删 system: tombstone_$id"
            fi
        fi
    done
}

list_all_ids() {
    su -c "ls -1 '$TOMB_SRC'" 2>/dev/null \
        | grep -E '^tombstone_[0-9]+$' \
        | sed 's/tombstone_//' \
        | sort -n
}

list_latest_ids() {
    local n="$1"
    list_all_ids | tail -n "$n"
}

collect_ids() {
    local id ts key
    COLLECTED=()

    if ((${#IDS[@]} > 0)); then
        COLLECTED=("${IDS[@]}")
        return 0
    fi

    if (( NEW_ONLY )); then
        while read -r id; do
            [[ -z "$id" ]] && continue
            if ! is_gtasa_tombstone "$id"; then
                continue
            fi
            ts="$(su -c "grep -m1 '^Timestamp:' '$TOMB_SRC/tombstone_$id' 2>/dev/null" | sed 's/^Timestamp:[[:space:]]*//')"
            [[ -z "$ts" ]] && ts="unknown"
            key="${id}|${ts}"
            if grep -qxF "$key" "$STATE_FILE" 2>/dev/null; then
                continue
            fi
            COLLECTED+=("$id")
        done < <(list_all_ids)
        return 0
    fi

    while read -r id; do
        [[ -z "$id" ]] && continue
        if is_gtasa_tombstone "$id"; then
            COLLECTED+=("$id")
        fi
    done < <(list_latest_ids "$COUNT")
}

analyze_ids() {
    local ids=("$@")
    if ((${#ids[@]} == 0)); then
        echo "ℹ️  没有可分析的墓碑"
        return 0
    fi

    local stamp report
    stamp="$(date '+%Y%m%d_%H%M%S')"
    report="$REPORT_DIR/batch_${stamp}.txt"

    {
        echo "Tombstone batch analysis"
        echo "Generated: $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo "Count: ${#ids[@]}"
        echo "IDs: ${ids[*]}"
        echo
    } > "$report"

    WORKSPACE_DIR="$WORKSPACE_DIR" IDS="${ids[*]}" python3 <<'PY' >> "$report"
import importlib.util
import os
from pathlib import Path

workspace = Path(os.environ["WORKSPACE_DIR"])
staging = workspace / "tombstones_temp"
spec = importlib.util.spec_from_file_location("pc", staging / "parse_crashes.py")
pc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc)
symbols = pc.load_symbols()
for token in os.environ.get("IDS", "").split():
    path = staging / f"tombstone_{token}"
    if not path.exists():
        print(f"--- tombstone_{token}: MISSING ---")
        continue
    res = pc.analyze_tombstone(str(path), symbols)
    if not res:
        print(f"--- tombstone_{token}: parse failed (not GTA SA DE?) ---")
        continue
    print(pc.format_report(res))
PY

    echo "📊 报告: $report"
    echo "==================== 摘要 ===================="
    sed -n '1,200p' "$report"
}

if (( ! ANALYZE_ONLY )); then
    if ! su -c "test -d '$TOMB_SRC'" 2>/dev/null; then
        echo "❌ 无法访问 $TOMB_SRC（需要 root/su）"
        exit 1
    fi

    collect_ids
    if ((${#COLLECTED[@]} == 0)); then
        echo "ℹ️  没有新的墓碑需要采集"
        exit 0
    fi

    echo "🔍 采集 ${#COLLECTED[@]} 条墓碑 → $STAGING"
    for id in "${COLLECTED[@]}"; do
        copy_tombstone "$id" || true
    done
    prune_old_tombstones "${COLLECTED[@]}"
else
    mapfile -t COLLECTED < <(ls -1 "$STAGING" 2>/dev/null | grep -E '^tombstone_[0-9]+$' | sed 's/tombstone_//' | sort -n)
fi

analyze_ids "${COLLECTED[@]}"