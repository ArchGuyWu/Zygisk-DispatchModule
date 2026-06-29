#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "dispatch_types.hpp"
#include "game_types.hpp"

namespace dispatch_police_spawn {

struct PoliceSpawnUnit {
    unsigned int model = 0;
    bool register_swat = false;
};

unsigned int get_local_patrol_model(CVector pos);

std::vector<PoliceSpawnUnit> build_initial_spawn_plan(
    const CrimeEvent& crime,
    int density,
    bool swat_already);

std::vector<PoliceSpawnUnit> build_reinforcement_spawn_plan(
    const CrimeEvent& crime,
    int reinforcement_wave,
    int density,
    bool swat_already);

void schedule_police_vehicle_spawns(
    const std::shared_ptr<CrimeEvent>& crime,
    CVector target_pos,
    CVector loc,
    CPed* criminal,
    const std::vector<PoliceSpawnUnit>& units,
    const char* reason);

bool is_swat_model(unsigned int model);
bool is_police_dispatch_model(unsigned int model);

} // namespace dispatch_police_spawn