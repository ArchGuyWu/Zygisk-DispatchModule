#pragma once

#include <cstdint>
#include <memory>

#include "dispatch_types.hpp"

// =====================================================================
// 调度时序常量（单一数据源，避免各文件魔法数字不一致）
// =====================================================================
namespace dispatch_timing {

// --- 距离阈值（与 dispatch_logic 视听范围对齐）---
constexpr float AV_RANGE_FIREARM_M = 75.0f;
constexpr float AV_RANGE_MELEE_M = 35.0f;
constexpr float COP_ON_SCENE_IMMEDIATE_M = 50.0f;   // 最近警距 < 此值：直接视为到场
constexpr float COP_NATURAL_RESPONSE_M = 60.0f;     // 自然响应/取消刷车阈值
constexpr float NEARBY_SEARCH_FIREARM_M = 150.0f;
constexpr float NEARBY_SEARCH_MELEE_M = 80.0f;

// --- 阶段 1：留给引擎视听/原生 AI 的自然响应窗口 ---
constexpr int64_t NATURAL_RESPONSE_GRACE_MS = 3000;

// --- 阶段 2：附近警员轮询调度间隔 ---
constexpr int64_t NEARBY_RETRY_INTERVAL_MS = 3000;
constexpr int64_t NEARBY_ASSIGN_DEDUP_MS = 8000;

// --- 阶段 3：刷增援车兜底（进入 TIMING 后额外等待，不含 grace 前静默）---
constexpr int SPAWN_FALLBACK_FIREARM_MIN_MS = 8000;
constexpr int SPAWN_FALLBACK_FIREARM_MAX_MS = 11000;
constexpr int SPAWN_FALLBACK_MELEE_MIN_MS = 12000;
constexpr int SPAWN_FALLBACK_MELEE_MAX_MS = 16000;

// --- 刷车后识别/配置载具 ---
constexpr int64_t VEHICLE_IDENTIFY_DELAY_MS = 400;
constexpr int64_t VEHICLE_IDENTIFY_STAGGER_MS = 600;

// --- 到场后伤亡增援：先附近调度，再刷车 ---
constexpr int64_t REINFORCE_NEARBY_ATTEMPT_MS = 4000;
constexpr int REINFORCE_SPAWN_LIGHT_MIN_MS = 9000;
constexpr int REINFORCE_SPAWN_LIGHT_MAX_MS = 12000;
constexpr int REINFORCE_SPAWN_MEDIUM_MIN_MS = 7500;
constexpr int REINFORCE_SPAWN_MEDIUM_MAX_MS = 10000;
constexpr int REINFORCE_SPAWN_HEAVY_MIN_MS = 3000;
constexpr int REINFORCE_SPAWN_HEAVY_MAX_MS = 4500;

// --- 地面警战斗分派限频 ---
constexpr int64_t FOOT_ASSIGN_FIREARM_MS = 4000;
constexpr int64_t FOOT_ASSIGN_MELEE_MS = 12000;
constexpr int64_t FOOT_ASSIGN_ACTIVE_WINDOW_MS = 15000;

// --- 载具警到场下车 ---
constexpr int64_t VEHICLE_STUCK_EXIT_MS = 8000;
constexpr int64_t VEHICLE_MAX_APPROACH_MS = 15000;

int compute_spawn_fallback_delay_ms(bool is_firearm);
int64_t elapsed_since_dispatch_timer(const CrimeEvent& crime, int64_t now_ms);
bool should_attempt_nearby_dispatch(const CrimeEvent& crime, int64_t now_ms);
void record_nearby_dispatch_attempt(CrimeEvent& crime, int64_t now_ms);

} // namespace dispatch_timing