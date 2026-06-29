#pragma once

#include <memory>
#include <vector>

#include "dispatch_types.hpp"
#include "game_types.hpp"

namespace dispatch_reroute {

int get_case_threat_tier(const CrimeEvent& crime);

std::shared_ptr<CrimeEvent> find_higher_threat_case_in_av(
    const std::shared_ptr<CrimeEvent>& from_case,
    CVector observer_pos,
    const std::vector<std::shared_ptr<CrimeEvent>>& crimes);

bool try_reroute_foot_cop(CPed* cop, CPed* current_target);

void apply_enroute_vehicle_reroutes(const std::vector<std::shared_ptr<CrimeEvent>>& crimes);

} // namespace dispatch_reroute