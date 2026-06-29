# Crash Status (GTA SA DE Android)

> 数据来源：`crash_summary.txt`（50 条 tombstone，2026-06-28 采集）  
> 本文件描述**当前已知崩溃面**，不代表模组已完全修复。

## 摘要

| 指标 | 值 |
|------|-----|
| 样本数 | 50 |
| 主要信号 | SIGSEGV (Signal 11) |
| 已挂钩 ShadowHook 符号 | **39** 处（`shadowhook_hook_sym_name`） |
| 盲扫 `sanitize_task_pointers()` | **已禁用**（误伤 float/坐标成员导致二次崩溃） |

## 高频崩溃符号（Top）

| 次数 | 符号 | 模组对策 |
|------|------|----------|
| 12 | `CPed::ProcessBuoyancy` | Hook + 前置指针校验 |
| 11 | `u_strlen_64` (ICU) | Hook + 入参可读性过滤 |
| 3 | `CTaskManager::GetSimplestActiveTask` | 部分路径经 ManageTasks / FindActiveTaskByType 防护 |
| 3 | `CTaskManager::FindActiveTaskByType` | Hook + 任务链校验 |
| 2 | `CTaskComplexWanderStandard::LookForChatPartners` | 未单独挂钩（样本较少） |
| 2 | `CEventScanner::ScanForEvents` | 未单独挂钩 |
| 1 | `CPlayerPed::ProcessControl` | 未挂钩 |

完整列表见 `crash_summary.txt`。

## 已知限制

1. **净化器策略**：`sanitize_task_pointers()` 当前为空实现。盲目按 8 字节步长扫描会把非指针成员（如 `float` 限制值）误判为已析构对象并置 `nullptr`，曾引发 `fault addr 0x10` 类崩溃。若重新启用，必须基于 IDA/r2 的**精确偏移**而非全内存扫描。
2. **覆盖率 ≠ 零崩溃**：39 个 Hook 显著降低部分路径的 SIGSEGV，但引擎其他生命周期（相机、玩家控制、未挂钩任务虚表）仍可能崩溃。
3. **README 营销措辞**：对外文档已改为「缓解 / 降低概率」，避免「彻底根治」「100% 零开销」等表述。

## 验证建议

```bash
# 重新分析 tombstone（需 libUE4.so 与符号表）
python3 scripts/analyze_crashes.py   # 若存在
# 或手动对照 crash_summary.txt 与 module.cpp 中 g_stub_* 列表
rg 'shadowhook_hook_sym_name' src/main/cpp/module.cpp
```

## 更新记录

| 日期 | 说明 |
|------|------|
| 2026-06-29 | 初版：基于 50 条 tombstone 汇总，标注 sanitize 禁用状态 |