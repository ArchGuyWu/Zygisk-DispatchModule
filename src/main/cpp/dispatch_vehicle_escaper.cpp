#include "dispatch_vehicle_escaper.hpp"

#include <cmath>
#include <mutex>
#include <unordered_map>

#include "dispatch_emergency_services.hpp"
#include "dispatch_timing.hpp"
#include "game_config.hpp"
#include "mod_shared.hpp"

namespace dispatch_vehicle_escaper {

static std::unordered_map<void*, int64_t> g_sideline_traffic_vehicles;
static std::mutex g_sideline_traffic_mutex;

void reset_stuck_tracker(StuckTracker& tracker) {
    tracker.stuck_since = 0;
    tracker.last_intervention_time = now_ms();
    tracker.spin_count = 0;
    tracker.last_dir_x = 0.0f;
    tracker.last_dir_y = 0.0f;
}

void clear_vehicle_stuck_trackers(void* vehicle) {
    if (!vehicle) return;
    {
        std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
        g_stuck_vehicles.erase(vehicle);
    }
    {
        std::lock_guard<std::mutex> lock(g_emergency_stuck_vehicles_mutex);
        g_emergency_stuck_vehicles.erase(vehicle);
    }
}

bool is_warp_visible_to_player(CVector pos) {
    return is_pos_visible_to_player_camera(pos);
}

bool should_trigger_stage2_warp(
    int64_t stuck_duration_ms,
    bool warp_visible,
    float dist_to_target_m) {
    if (warp_visible) return false;
    if (stuck_duration_ms < dispatch_timing::VEHICLE_STUCK_STAGE2_MS) return false;
    if (dist_to_target_m > dispatch_timing::VEHICLE_STAGE2_MIN_DIST_M) return true;
    return stuck_duration_ms >= dispatch_timing::VEHICLE_STAGE2_LONG_STUCK_MS;
}

float get_vehicle_staging_exit_radius(void* vehicle) {
    bool is_bike = vehicle && get_entity_model_index(vehicle) == MODEL_POLICE_BIKE;
    float offset = is_bike ? dispatch_timing::VEHICLE_BIKE_STAGING_OFFSET_M
                           : dispatch_timing::VEHICLE_STAGING_OFFSET_M;
    return offset + dispatch_timing::VEHICLE_STAGING_EXIT_MARGIN_M;
}

bool anti_spin_should_bulk_exit(float dist_to_crime_m, void* vehicle) {
    float staging_radius = get_vehicle_staging_exit_radius(vehicle);
    if (dist_to_crime_m <= staging_radius + 4.0f) {
        return false;
    }
    return dist_to_crime_m < dispatch_timing::ANTI_SPIN_CLOSE_RANGE_M;
}

bool is_mod_managed_emergency_vehicle(void* vehicle) {
    if (!vehicle || !is_vehicle_pointer_valid(vehicle)) return false;
    unsigned int model = get_entity_model_index(vehicle);
    if (model != MODEL_AMBULANCE && model != MODEL_FIRETRUCK) return false;
    if (is_vehicle_ordered_to_scene(vehicle)) return true;

    {
        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
        for (const auto& crime : g_active_crimes) {
            if (!crime || crime->cancelled) continue;
            for (void* veh : crime->case_vehicles) {
                if (veh == vehicle) return true;
            }
        }
    }
    return dispatch_emergency_services::is_registered_emergency_vehicle(vehicle);
}

static bool closure_center_near(
    CVector a,
    CVector b,
    float max_dist_m) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy) <= max_dist_m;
}

bool queue_deduped_temp_closure(
    CVector center,
    std::vector<TemporaryRoadClosure>& pending_out) {
    float radius = dispatch_timing::TEMP_CLOSURE_RADIUS_M;
    int64_t reopen_time_ms = now_ms() + dispatch_timing::TEMP_CLOSURE_TTL_MS;

    {
        std::lock_guard<std::mutex> lock(g_temp_closures_mutex);
        for (auto& existing : g_temp_road_closures) {
            if (closure_center_near(existing.center, center, dispatch_timing::TEMP_CLOSURE_DEDUP_M)) {
                if (reopen_time_ms > existing.reopen_time_ms) {
                    existing.reopen_time_ms = reopen_time_ms;
                }
                return false;
            }
        }
        for (auto& pending : pending_out) {
            if (closure_center_near(pending.center, center, dispatch_timing::TEMP_CLOSURE_DEDUP_M)) {
                if (reopen_time_ms > pending.reopen_time_ms) {
                    pending.reopen_time_ms = reopen_time_ms;
                }
                return false;
            }
        }
    }

    if (g_ThePaths && g_SwitchRoadsOffInArea) {
        g_SwitchRoadsOffInArea(
            g_ThePaths,
            center.x - radius, center.y - radius, center.z - 8.0f,
            center.x + radius, center.y + radius, center.z + 8.0f,
            true, true, false);
    }
    pending_out.push_back({center, radius, reopen_time_ms});
    return true;
}

void sideline_traffic_obstacle(void* other_vehicle, CVector rescuer_pos) {
    if (!other_vehicle || !is_vehicle_pointer_valid(other_vehicle)) return;

    CVector other_pos = get_entity_pos(other_vehicle);
    float dx = other_pos.x - rescuer_pos.x;
    float dy = other_pos.y - rescuer_pos.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.1f) {
        dx = 1.0f;
        dy = 0.0f;
        len = 1.0f;
    }
    float side_sign = (((uintptr_t)other_vehicle) & 1) ? 1.0f : -1.0f;
    float perp_x = (-dy / len) * side_sign;
    float perp_y = (dx / len) * side_sign;
    float shift = dispatch_timing::TRAFFIC_SIDELINE_SHIFT_M;

    CVector sideline = {
        other_pos.x + perp_x * shift,
        other_pos.y + perp_y * shift,
        other_pos.z
    };
    set_entity_pos(other_vehicle, sideline);

    std::lock_guard<std::mutex> lock(g_sideline_traffic_mutex);
    g_sideline_traffic_vehicles[other_vehicle] = now_ms();
}

bool is_sideline_traffic_vehicle(void* vehicle) {
    if (!vehicle) return false;
    std::lock_guard<std::mutex> lock(g_sideline_traffic_mutex);
    return g_sideline_traffic_vehicles.count(vehicle) > 0;
}

void prune_sideline_traffic_vehicles() {
    std::lock_guard<std::mutex> lock(g_sideline_traffic_mutex);
    for (auto it = g_sideline_traffic_vehicles.begin(); it != g_sideline_traffic_vehicles.end(); ) {
        if (!is_vehicle_pointer_valid(it->first)) {
            it = g_sideline_traffic_vehicles.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace dispatch_vehicle_escaper