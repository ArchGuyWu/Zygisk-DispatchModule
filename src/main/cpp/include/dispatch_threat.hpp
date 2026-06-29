#pragma once

#include <cstdint>

#include "dispatch_types.hpp"
#include "ecs_engine.hpp"
#include "game_types.hpp"

namespace dispatch_threat {

struct ResponseQuota {
    int max_vehicles = 2;
    int max_foot_cops = 2;
};

int threat_level_score(ecs::CriminalThreatLevel level);
ecs::CriminalThreatLevel estimate_new_criminal_threat(bool is_firearm, int weapon_category);
ecs::CriminalThreatLevel get_criminal_threat_level(CPed* criminal, const CrimeEvent* crime = nullptr);
ecs::CriminalThreatLevel get_case_max_threat_level(const CrimeEvent& crime);
int compute_case_threat_score(const CrimeEvent& crime);
int count_active_threats(const CrimeEvent& crime);
int count_high_threats(const CrimeEvent& crime);

bool should_merge_into_case(
    const CrimeEvent& existing,
    CPed* perpetrator,
    bool new_firearm,
    int new_weapon_category);

CVector compute_dispatch_anchor(const CrimeEvent& crime);
void refresh_crime_dispatch_anchor(CrimeEvent& crime);

ResponseQuota compute_response_quota(const CrimeEvent& crime, int cops_killed);
int compute_nearby_dispatch_quota(const CrimeEvent& crime);

CPed* pick_criminal_target_for_cop(CPed* cop, const CrimeEvent& crime);

} // namespace dispatch_threat