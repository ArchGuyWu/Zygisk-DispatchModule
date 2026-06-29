#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "dispatch_types.hpp"
#include "game_types.hpp"

namespace dispatch_emergency_services {

struct EmergencyServiceRecord {
    void* vehicle = nullptr;
    unsigned int model = 0;
    uint64_t case_id = 0;
    CVector target_anchor{};
    bool target_is_fire = false;
    int64_t spawn_time_ms = 0;
    int64_t last_reroute_ms = 0;
};

CVector clamp_spawn_to_streaming_range(CVector incident_anchor, CVector proposed_spawn);

void on_civilian_casualty_near_crime(const CPed* dead_ped, CVector death_pos);
void on_cop_casualty_near_crime(CVector death_pos);

void evaluate_prioritized_emergency_needs(
    const std::vector<std::shared_ptr<CrimeEvent>>& crimes,
    int64_t cur_time);
void evaluate_global_fire_dispatch(int64_t cur_time);

int compute_ambulance_priority(const CrimeEvent& crime, CVector reference_pos);
int compute_firetruck_priority(const CrimeEvent& crime, CVector reference_pos, bool has_fire);

void schedule_ambulance_for_crime(const std::shared_ptr<CrimeEvent>& crime, CVector incident_anchor);
void schedule_firetruck_for_crime(const std::shared_ptr<CrimeEvent>& crime, CVector fire_pos);

void setup_dispatched_emergency_vehicle(
    void* vehicle,
    unsigned int model,
    CVector target_anchor,
    const std::shared_ptr<CrimeEvent>& crime,
    bool target_is_fire);

void command_emergency_vehicle_to_scene(void* vehicle, unsigned int model, const CVector& target_loc);

void refresh_enroute_emergency_vehicles(
    const std::vector<std::shared_ptr<CrimeEvent>>& crimes,
    bool do_reroute_refresh);

void unregister_emergency_vehicle(void* vehicle);
void clear_case_emergency_records(uint64_t case_id);
void clear_all_emergency_records();

int count_active_emergency_vehicles_near(CVector pos, float range, unsigned int model);

void emergency_services_tick(const std::vector<std::shared_ptr<CrimeEvent>>& crimes, int64_t cur_time);

} // namespace dispatch_emergency_services