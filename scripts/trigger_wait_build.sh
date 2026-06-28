#!/data/data/com.termux/files/usr/bin/bash
# =====================================================================
# trigger_wait_build.sh
# 自动触发编译推送，并阻塞等待 GitHub Action 完成，若失败则自动打印错误日志
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# 1. 如果有未提交的改动，先运行 auto_commit_push.sh
if [ -n "$(git -C "$SCRIPT_DIR/.." status --porcelain)" ]; then
    echo "📦 检测到本地修改，正在触发自动编译与推送..."
    "$SCRIPT_DIR/../auto_commit_push.sh"
else
    echo "ℹ️ 本地工作区干净，将直接追踪当前最新提交..."
fi

# 2. 获取当前本地提交的 SHA
SHA=$(git -C "$SCRIPT_DIR/.." rev-parse HEAD)
echo "🎯 开始追踪提交 $SHA 的 GitHub Actions 构建状态..."

# 3. 循环等待并监控构建
while true; do
    # 使用 pueue 运行 gh 避免 Termux 库加载冲突
    TASK_ID=$(pueue add --print-task-id -- "gh run list --commit $SHA")
    pueue wait "$TASK_ID" >/dev/null 2>&1
    
    # 提取构建流信息
    RUN_INFO=$(pueue log "$TASK_ID" | grep -E "(completed|in_progress|queued|waiting)")
    
    if [ -n "$RUN_INFO" ]; then
        STATUS=$(echo "$RUN_INFO" | awk '{print $1}')
        CONCLUSION=$(echo "$RUN_INFO" | awk '{print $2}')
        RUN_ID=$(echo "$RUN_INFO" | awk '{print $8}')
        
        echo "⏳ 构建状态: [$STATUS] | 结果: [${CONCLUSION:-running}] | Run ID: $RUN_ID"
        
        if [ "$STATUS" = "completed" ]; then
            if [ "$CONCLUSION" = "success" ]; then
                echo "✅ GitHub Action 构建成功！Artifact 已生成。"
                exit 0
            else
                echo "❌ GitHub Action 构建失败！正在抓取详细错误日志..."
                LOG_TASK=$(pueue add --print-task-id -- "gh run view $RUN_ID --log-failed")
                pueue wait "$LOG_TASK" >/dev/null 2>&1
                echo "==================== 编译错误日志 ===================="
                pueue log --full "$LOG_TASK" | sed -n '/output:/,$p' | tail -n +2
                echo "===================================================="
                exit 1
            fi
        fi
    else
        echo "⏳ 等待 GitHub Actions 接收并初始化构建任务..."
    fi
    sleep 10
done
