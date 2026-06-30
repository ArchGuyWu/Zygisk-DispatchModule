#pragma once

#include <cstdint>
#include <vector>

#include "dispatch_types.hpp"
#include "game_types.hpp"

namespace dispatch_vehicle_escaper {

void reset_stuck_tracker(StuckTracker& tracker);
void clear_vehicle_stuck_trackers(void* vehicle);

bool is_warp_visible_to_player(CVector pos);
bool should_trigger_stage2_warp(int64_t stuck_duration_ms, bool warp_visible, float dist_to_target_m);

float get_vehicle_staging_exit_radius(void* vehicle);
bool anti_spin_should_bulk_exit(float dist_to_crime_m, void* vehicle);

bool is_mod_managed_emergency_vehicle(void* vehicle);

bool queue_deduped_temp_closure(
    CVector center,
    std::vector<TemporaryRoadClosure>& pending_out);

void sideline_traffic_obstacle(void* other_vehicle, CVector rescuer_pos);
bool is_sideline_traffic_vehicle(void* vehicle);
void prune_sideline_traffic_vehicles();

} // namespace dispatch_vehicle_escaper