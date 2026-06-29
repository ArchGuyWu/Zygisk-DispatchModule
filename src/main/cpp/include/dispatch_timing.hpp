#pragma once

#include <cstdint>
#include <memory>

#include "dispatch_types.hpp"
#include "game_types.hpp"

// =====================================================================
// 调度时序常量（与 GTA 原生视听 AV 对齐）
// =====================================================================
namespace dispatch_timing {

// --- 原生视听范围（与 dispatch_logic / 引擎一致）---
constexpr float AV_RANGE_FIREARM_M = 75.0f;
constexpr float AV_RANGE_MELEE_M = 35.0f;

// --- 视听外「附近调度」搜索半径（略大于 AV，供强制增援用）---
constexpr float NEARBY_SEARCH_FIREARM_M = 150.0f;
constexpr float NEARBY_SEARCH_MELEE_M = 80.0f;

// --- 阶段 1：视听范围内的自然响应缓冲（按案件类型）---
constexpr int64_t NATURAL_RESPONSE_GRACE_FIREARM_MS = 2500;
constexpr int64_t NATURAL_RESPONSE_GRACE_MELEE_MS = 4000;

// --- 阶段 2：视听外附近警员轮询间隔 ---
constexpr int64_t NEARBY_RETRY_INTERVAL_MS = 3000;
constexpr int64_t NEARBY_ASSIGN_DEDUP_MS = 8000;

// --- 阶段 3：刷增援车兜底（进入 TIMING 后，自 timer_start 起算）---
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

// --- on_scene 分派心跳（视听外降频，视听内由调用方即时触发）---
constexpr int64_t ON_SCENE_DISPATCH_INTERVAL_MS = 2500;

// --- 载具警到场下车 ---
constexpr int64_t VEHICLE_STUCK_EXIT_MS = 8000;
constexpr int64_t VEHICLE_MAX_APPROACH_MS = 15000;

float get_av_range_for_crime(const CrimeEvent& crime);
int64_t get_natural_response_grace_ms(const CrimeEvent& crime);
bool is_cop_within_native_av(float dist_to_cop, const CrimeEvent& crime);
bool is_cop_within_any_active_crime_av(const CVector& cop_pos);

int compute_spawn_fallback_delay_ms(bool is_firearm);
int64_t elapsed_since_dispatch_timer(const CrimeEvent& crime, int64_t now_ms);
bool should_attempt_nearby_dispatch(const CrimeEvent& crime, int64_t now_ms);
void record_nearby_dispatch_attempt(CrimeEvent& crime, int64_t now_ms);

} // namespace dispatch_timing