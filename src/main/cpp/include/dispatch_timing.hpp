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
constexpr int64_t VEHICLE_IDENTIFY_RETRY_MS = 500;
constexpr int VEHICLE_IDENTIFY_MAX_ATTEMPTS = 3;
constexpr float VEHICLE_IDENTIFY_RADIUS_M = 40.0f;

// --- 视野外刷车等待：识别失败或警车赶路 ---
constexpr int64_t SPAWN_PENDING_TIMEOUT_MS = 45000;
constexpr int64_t SPAWN_ARRIVED_SCENE_TIMEOUT_MS = 60000;

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

// --- 救护/消防模组调度 ---
constexpr float EMERGENCY_STREAMING_MAX_DIST_M = 75.0f;
constexpr float EMERGENCY_STREAMING_TARGET_DIST_M = 60.0f;
constexpr float EMERGENCY_CAP_RANGE_M = 150.0f;
constexpr int MAX_AMBULANCE_NEAR_PLAYER = 2;
constexpr int MAX_FIRETRUCK_NEAR_PLAYER = 2;
constexpr float FIRE_DETECT_RANGE_M = 55.0f;
constexpr float CASUALTY_CASE_RANGE_M = 80.0f;
constexpr float GLOBAL_FIRE_PLAYER_RANGE_M = 75.0f;
constexpr int64_t EMERGENCY_EVAL_INTERVAL_MS = 2000;
constexpr int AMBULANCE_SPAWN_DELAY_MIN_MS = 5000;
constexpr int AMBULANCE_SPAWN_DELAY_MAX_MS = 8000;
constexpr int FIRETRUCK_SPAWN_DELAY_MIN_MS = 3000;
constexpr int FIRETRUCK_SPAWN_DELAY_MAX_MS = 5000;
constexpr int64_t EMERGENCY_REROUTE_INTERVAL_MS = 3000;
constexpr int64_t GLOBAL_FIRE_COOLDOWN_MS = 20000;
constexpr int EMS_REROUTE_PRIORITY_MARGIN = 8;
constexpr float EMS_PRIORITY_DISTANCE_PENALTY_M = 15.0f;

// --- 警用直升机空中支援 ---
constexpr float HELI_SPAWN_ALTITUDE_M = 80.0f;
constexpr float HELI_HOVER_ALTITUDE_M = 28.0f;
constexpr float HELI_GLOBAL_CAP_RANGE_M = 200.0f;
constexpr int MAX_POLICE_HELI_NEAR_PLAYER = 1;
constexpr int HELI_SPAWN_DELAY_MIN_MS = 10000;
constexpr int HELI_SPAWN_DELAY_MAX_MS = 15000;
constexpr int64_t HELI_REFRESH_INTERVAL_MS = 3000;

// --- 载具警到场下车 ---
constexpr float VEHICLE_STAGING_OFFSET_M = 32.0f;
constexpr float VEHICLE_STAGING_EXIT_MARGIN_M = 4.0f;
constexpr float VEHICLE_BIKE_STAGING_OFFSET_M = 20.0f;
constexpr int64_t VEHICLE_STUCK_EXIT_MS = 8000;
constexpr int64_t VEHICLE_MAX_APPROACH_MS = 15000;

// --- 步行转场 / 司乘 / 下车接战 ---
constexpr float NEARBY_FOOT_PREFER_DIST_M = 55.0f;
constexpr float ACTIVE_CRIME_FOOT_DUTY_RADIUS_M = 80.0f;
constexpr int64_t VEHICLE_ENTER_COMMAND_COOLDOWN_MS = 10000;
constexpr int64_t COP_EXIT_COMBAT_BLOCK_MS = 8000;

float get_av_range_for_crime(const CrimeEvent& crime);
int64_t get_natural_response_grace_ms(const CrimeEvent& crime);
bool is_cop_within_native_av(float dist_to_cop, const CrimeEvent& crime);
bool is_cop_within_any_active_crime_av(const CVector& cop_pos);

int compute_spawn_fallback_delay_ms(bool is_firearm);
int64_t elapsed_since_dispatch_timer(const CrimeEvent& crime, int64_t now_ms);
bool should_attempt_nearby_dispatch(const CrimeEvent& crime, int64_t now_ms);
void record_nearby_dispatch_attempt(CrimeEvent& crime, int64_t now_ms);

} // namespace dispatch_timing