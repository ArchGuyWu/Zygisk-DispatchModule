#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "dispatch_types.hpp"
#include "game_types.hpp"

struct CopAttackContext {
    CPed* criminal = nullptr;
    std::shared_ptr<CrimeEvent> crime_case;
    CVector crime_pos{};
    int pool_size = 0;
    char* byte_map = nullptr;

    bool is_active_firearm = false;
    int active_vehicles_count = 0;
    int active_foot_cops_count = 0;
    int max_vehicles = 2;
    int max_foot_cops = 2;

    float av_range_sq = 0.0f;

    std::vector<std::pair<void*, int64_t>> dispatched_vehicles_time_snapshot;
    std::vector<std::pair<void*, StuckTracker>> stuck_vehicles_snapshot;
    std::vector<void*> vehicles_emptied_snapshot;
    std::vector<void*> vehicles_ordered_to_scene_snapshot;
    std::vector<void*> vehicles_siren_awakened_snapshot;

    std::vector<std::pair<void*, int64_t>> pending_dispatched_vehicles_time;
    std::vector<void*> pending_vehicles_emptied;
    std::vector<void*> pending_vehicles_ordered_to_scene;
    std::vector<void*> pending_vehicles_siren_awakened;
    std::vector<std::pair<void*, StuckTracker>> pending_stuck_vehicles;
    std::vector<TemporaryRoadClosure> pending_temp_closures;
    std::vector<void*> counted_vehicles;

    void reset();
    bool vector_contains(const std::vector<void*>& vec, void* val) const;
};

void cop_attack_snapshot_globals(CopAttackContext& ctx);
void cop_attack_detect_firearm_threat(CopAttackContext& ctx);
void cop_attack_compute_quotas(CopAttackContext& ctx);
void cop_attack_single_pass_dispatch(CopAttackContext& ctx);
void cop_attack_dispatch_vehicle_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    CVector target_crime_pos,
    void* veh);
void cop_attack_dispatch_foot_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    CVector target_crime_pos,
    float dist_sq);
void cop_attack_commit_pending(CopAttackContext& ctx);