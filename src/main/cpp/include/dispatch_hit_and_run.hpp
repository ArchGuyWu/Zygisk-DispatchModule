#pragma once

#include <cstdint>
#include <vector>
#include <mutex>

#include "dispatch_types.hpp"
#include "game_types.hpp"

struct PendingHitAndRunEntry {
    CPed* fugitive = nullptr;
    void* vehicle = nullptr;
    CVector scene_pos = {0.0f, 0.0f, 0.0f};
    uint64_t nearby_case_id = 0;
    int64_t report_time_ms = 0;
    eWeaponType weapon = WEAPON_UNARMED;
};

namespace dispatch_hit_and_run {

CPed* resolve_kill_perpetrator(const CEntity* killer, eWeaponType weapon);
bool is_case_criminal(const CrimeEvent& crime, CPed* ped);
bool is_vehicle_hit_kill(eWeaponType weapon, const CEntity* killer);

void handle_cop_death_near_case(
    const CPed* dead_cop,
    const CEntity* killer,
    eWeaponType weapon,
    CVector cop_pos);

void on_perpetrator_crime_report(CPed* perpetrator);
void tick_pending_wanted(int64_t cur_time);
void clear_all_pending();

bool is_pending_fugitive(CPed* ped);

} // namespace dispatch_hit_and_run