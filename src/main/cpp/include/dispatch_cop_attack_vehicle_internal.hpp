#pragma once

#include <cstddef>
#include <cstdint>

#include "dispatch_cop_attack_internal.hpp"
#include "dispatch_types.hpp"
#include "game_types.hpp"

struct CopAttackVehicleSession {
    CVector veh_pos{};
    float v_dist = 0.0f;
    int64_t first_seen = 0;
    int64_t elapsed = 0;
    bool already_dispatched = false;

    CVector current_pos{};
    int64_t now_time = 0;
    StuckTracker tracker{};
    bool found_stuck = false;
    size_t stuck_idx = 0;
};

void cop_attack_vehicle_stuck_monitor(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos);

void cop_attack_vehicle_unstuck_intervene(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos);

void cop_attack_vehicle_initial_order(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos);