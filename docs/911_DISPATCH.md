# 911 / 对讲机调度架构

## 总览

```
市民/玩家目击
    → 部门信号 (police_911 / ems_911 / fire_911)
    → 统一进线 (civilian_911_intake，占线/排队/重拨)
    → PSAP intake 完成 ([911Connect])
    → 分部门转接 ([911Transfer])

警员目击
    → 对讲机 (police_radio / ems_radio / fire_radio)
    → 不占 911 线路，直接授权派单

拖车
    → tow_radio（独立队列，不走 911）
```

## 911 时序

| 常量 | 值 | 含义 |
|------|-----|------|
| `CIVILIAN_911_CONNECT_BASE_MS` | 3500 | 有空线时 intake 时长 |
| `CIVILIAN_911_BUSY_MAX_WAIT_MS` | 12000 | 占线最长等待 |
| `CIVILIAN_911_MAX_REDIAL_ATTEMPTS` | 3 | 占线超时后最多重拨次数 |
| `SERVICE_REPORT_TTL_MS` | 90000 | 目击信号有效期 |

## 日志标签

| 标签 | 触发条件 |
|------|----------|
| `[911Busy]` | 两路并发占满，新呼叫排队 |
| `[911Answer]` | 排队呼叫分配到空线 |
| `[911Connect]` | PSAP intake 完成 |
| `[911Transfer]` | 分部门线路接通 |
| `[911Redial]` | 占线超时，市民重拨 |
| `[911Drop]` | 排队中目击丢失，挂断 |
| `[911GiveUp]` | 超过最大重拨次数 |
| `[911Revoke]` | 转接后部门目击丢失，撤销出动授权 |
| `[EMSBackupWait]` | 同案救护+消防批次等待（消防车延迟齐发） |
| `[PursuitRoadblock]` | 高热度追捕临时封路 |
| `[Radio] air-support` | 直升机支援电台请求 |
| `[HeliSupport]` | 警用直升机调度 |
| `[NearbyCopDispatch]` | 附近警员出动（步行/车内分流） |
| `[DriverRecruit]` | 无司机警车招募步行警员上车 |

## 感知分层（Perception v2）

| 层级 | 通道 | 行为 |
|------|------|------|
| AV | `HEARD` | 仅调查锚点；玩家 3s / NPC 8s 反应延迟 |
| AV | `SEEN`（非活跃威胁） | 调查模式；玩家 1.5s / NPC 5s 延迟 |
| 确认威胁 | `CONFIRMED` 或活跃威胁 | 立即追击/ mobilize |

警员对讲机不占 911 线路；市民 911 `SEEN` 目击仍可直接授权警方出动。

## 玩家报案规则

- **警方**：玩家非本案嫌犯时可报；嫌犯（含 `consolidated_criminals`）不报。
- **EMS**：玩家造成伤亡需求时不报（含追捕案、最近伤人记录、并案重叠）。
- **火警**：玩家纵火时不报（含本案嫌犯 + 最近纵火坐标记录）。
- **追捕案** (`is_player_pursuit`)：不走 911，走电台追捕链路。

## 目击与转接校验

1. 每 tick 对部门信号做 TTL 清理（`scrub_stale_department_911`）。
2. 部门信号全失效时，同步清除未接通的 `civilian_911_intake` 并移出队列。
3. `[911Transfer]` 前对每个部门重扫目击（`revalidate_department_witnesses`）。
4. 转接要求 `last_refresh_ms` 仍在 TTL 内。
5. 转接后每 tick `revoke_stale_911_authorizations`；目击丢失 → `[911Revoke]`。
6. 排队 intake 期间 `call_witness_still_valid` 重扫部门信号，失效 → `[911Drop]`。

## 火警 orphan 挂案

全局火情（玩家附近、50m 内无优先案件）→ `ingest_orphan_fire_awareness`：

1. 优先挂玩家追捕案（若最近纵火记录匹配）。
2. 其次挂已有火情信号的案件。
3. 最后按 120m 最近案件兜底。

## 测试场景清单

1. 市民目击枪械案 → 911 排队 → 接通 → 警方出动。
2. 两案同时报案 → 第二通 `[911Busy]` → 超时 `[911Redial]`。
3. 排队中目击离开 → `[911Drop]`。
4. 占线三次失败 → `[911GiveUp]`。
5. 警员对讲机目击 → 不经 911 直接 mobilize。
6. 玩家伤人 → 仅_civilian_目击时 EMS 出动；玩家自己不报 EMS。
7. 玩家纵火 → 仅_civilian_/警员目击时消防出动；玩家自己不报火警。
8. 玩家目击 NPC 斗殴 → 可报警方 911，不报 EMS（若伤亡为玩家造成）。
9. Orphan fire 挂追捕案 vs 最近 NPC 案。
10. 转接前目击消失 → 对应部门不应 `[911Transfer]`。
11. 排队 intake 期间目击消失 → `[911Drop]`。
12. 转接后目击消失 → `[911Revoke]`，对应部门停止出动。
13. 120m 外孤立火情 → 新建 environmental fire case，仅消防/EMS 链路。
14. 玩家燃烧瓶纵火 → 玩家不报火警；市民目击仍可报。
15. 高热度追捕 → `[PursuitRoadblock]` 临时封路。
16. 直升机支援 → `[Radio] air-support` + `[HeliSupport]` 出动。