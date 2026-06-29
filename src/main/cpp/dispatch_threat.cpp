#include "dispatch_threat.hpp"

#include <algorithm>
#include <cmath>

#include "mod_shared.hpp"

namespace dispatch_threat {

int threat_level_score(ecs::CriminalThreatLevel level) {
    return static_cast<int>(level);
}

ecs::CriminalThreatLevel estimate_new_criminal_threat(bool is_firearm, int weapon_category) {
    if (is_firearm || weapon_category == 2) {
        return ecs::CriminalThreatLevel::FIREARM_ACTIVE;
    }
    if (weapon_category == 1) {
        return ecs::CriminalThreatLevel::MELEE_ACTIVE;
    }
    return ecs::CriminalThreatLevel::UNARMED_INACTIVE;
}

ecs::CriminalThreatLevel get_criminal_threat_level(CPed* criminal, const CrimeEvent* crime) {
    if (!criminal || !is_ped_pointer_valid_safe(criminal)) {
        return ecs::CriminalThreatLevel::UNARMED_INACTIVE;
    }
    if (g_IsAlive && !g_IsAlive(criminal)) {
        return ecs::CriminalThreatLevel::UNARMED_INACTIVE;
    }

    auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(criminal);
    if (crim_comp) {
        return crim_comp->get_threat_level();
    }

    if (crime) {
        auto it = crime->criminal_states.find(criminal);
        if (it != crime->criminal_states.end()) {
            const auto& state = it->second;
            if (state.current_threat_category >= 2) {
                if (state.fleeing || !state.is_active) {
                    return ecs::CriminalThreatLevel::FIREARM_INACTIVE;
                }
                if (state.shooting_air) {
                    return ecs::CriminalThreatLevel::FIREARM_AIR_SHOOT;
                }
                return ecs::CriminalThreatLevel::FIREARM_ACTIVE;
            }
            if (state.current_threat_category == 1) {
                return state.is_active
                    ? ecs::CriminalThreatLevel::MELEE_ACTIVE
                    : ecs::CriminalThreatLevel::MELEE_INACTIVE;
            }
            return state.is_active
                ? ecs::CriminalThreatLevel::UNARMED_ACTIVE
                : ecs::CriminalThreatLevel::UNARMED_INACTIVE;
        }

        for (size_t idx = 0; idx < crime->consolidated_criminals.size(); ++idx) {
            if (crime->consolidated_criminals[idx] != criminal) continue;
            if (idx < crime->criminal_is_firearm.size() && crime->criminal_is_firearm[idx]) {
                return ecs::CriminalThreatLevel::FIREARM_ACTIVE;
            }
            return ecs::CriminalThreatLevel::MELEE_ACTIVE;
        }
    }

    return ecs::CriminalThreatLevel::UNARMED_INACTIVE;
}

ecs::CriminalThreatLevel get_case_max_threat_level(const CrimeEvent& crime) {
    ecs::CriminalThreatLevel max_level = ecs::CriminalThreatLevel::UNARMED_INACTIVE;
    for (CPed* criminal : crime.consolidated_criminals) {
        ecs::CriminalThreatLevel level = get_criminal_threat_level(criminal, &crime);
        if (threat_level_score(level) > threat_level_score(max_level)) {
            max_level = level;
        }
    }
    if (crime.is_firearm &&
        threat_level_score(max_level) < threat_level_score(ecs::CriminalThreatLevel::FIREARM_INACTIVE)) {
        max_level = ecs::CriminalThreatLevel::FIREARM_INACTIVE;
    }
    return max_level;
}

int compute_case_threat_score(const CrimeEvent& crime) {
    int total = 0;
    for (CPed* criminal : crime.consolidated_criminals) {
        total += threat_level_score(get_criminal_threat_level(criminal, &crime));
    }
    if (crime.is_firearm && total == 0) {
        total += threat_level_score(ecs::CriminalThreatLevel::FIREARM_INACTIVE);
    }
    return total;
}

int count_active_threats(const CrimeEvent& crime) {
    int count = 0;
    for (CPed* criminal : crime.consolidated_criminals) {
        ecs::CriminalThreatLevel level = get_criminal_threat_level(criminal, &crime);
        if (threat_level_score(level) >= threat_level_score(ecs::CriminalThreatLevel::MELEE_ACTIVE)) {
            count++;
        }
    }
    return count;
}

int count_high_threats(const CrimeEvent& crime) {
    int count = 0;
    for (CPed* criminal : crime.consolidated_criminals) {
        ecs::CriminalThreatLevel level = get_criminal_threat_level(criminal, &crime);
        if (threat_level_score(level) >= threat_level_score(ecs::CriminalThreatLevel::FIREARM_AIR_SHOOT)) {
            count++;
        }
    }
    return count;
}

bool should_merge_into_case(
    const CrimeEvent& existing,
    CPed* perpetrator,
    bool new_firearm,
    int new_weapon_category) {
    if (new_firearm || new_weapon_category == 2) {
        return true;
    }

    ecs::CriminalThreatLevel new_level = estimate_new_criminal_threat(new_firearm, new_weapon_category);
    int new_score = threat_level_score(new_level);
    int case_max = threat_level_score(get_case_max_threat_level(existing));

    if (existing.is_firearm && new_score < threat_level_score(ecs::CriminalThreatLevel::MELEE_ACTIVE)) {
        LOGI("📡 [CaseMerge] Rejected merge of %p into firearm case %llu (new_threat=%d)",
             perpetrator, (unsigned long long)existing.case_id, new_score);
        return false;
    }

    if (case_max >= threat_level_score(ecs::CriminalThreatLevel::MELEE_ACTIVE) &&
        new_score < threat_level_score(ecs::CriminalThreatLevel::MELEE_ACTIVE)) {
        LOGI("📡 [CaseMerge] Rejected merge of %p into active case %llu (case_max=%d, new_threat=%d)",
             perpetrator, (unsigned long long)existing.case_id, case_max, new_score);
        return false;
    }

    if (case_max >= threat_level_score(ecs::CriminalThreatLevel::FIREARM_AIR_SHOOT) &&
        new_score <= threat_level_score(ecs::CriminalThreatLevel::MELEE_INACTIVE)) {
        LOGI("📡 [CaseMerge] Rejected merge of %p into high-threat case %llu (case_max=%d, new_threat=%d)",
             perpetrator, (unsigned long long)existing.case_id, case_max, new_score);
        return false;
    }

    return true;
}

CVector compute_dispatch_anchor(const CrimeEvent& crime) {
    CVector weighted_sum{0.0f, 0.0f, 0.0f};
    float total_weight = 0.0f;

    for (CPed* criminal : crime.consolidated_criminals) {
        if (!criminal || !is_ped_pointer_valid_safe(criminal)) continue;
        if (g_IsAlive && !g_IsAlive(criminal)) continue;

        int weight = threat_level_score(get_criminal_threat_level(criminal, &crime));
        if (weight <= 0) {
            weight = 1;
        }

        CVector pos = get_entity_pos(criminal);
        weighted_sum.x += pos.x * static_cast<float>(weight);
        weighted_sum.y += pos.y * static_cast<float>(weight);
        weighted_sum.z += pos.z * static_cast<float>(weight);
        total_weight += static_cast<float>(weight);
    }

    if (total_weight <= 0.0f) {
        return crime.location;
    }

    return CVector{
        weighted_sum.x / total_weight,
        weighted_sum.y / total_weight,
        weighted_sum.z / total_weight
    };
}

void refresh_crime_dispatch_anchor(CrimeEvent& crime) {
    crime.dispatch_anchor = compute_dispatch_anchor(crime);
}

ResponseQuota compute_response_quota(const CrimeEvent& crime, int cops_killed) {
    ResponseQuota quota;
    int max_threat = threat_level_score(get_case_max_threat_level(crime));
    int threat_score = compute_case_threat_score(crime);
    int active_threats = count_active_threats(crime);
    int high_threats = count_high_threats(crime);
    int total_criminals = static_cast<int>(crime.consolidated_criminals.size());

    if (cops_killed > 0) {
        quota.max_vehicles = 3;
        quota.max_foot_cops = 4;
        LOGI("📊 [Quota] Casualties -> max (V=%d, F=%d, killed=%d)",
             quota.max_vehicles, quota.max_foot_cops, cops_killed);
        return quota;
    }

    if (max_threat >= threat_level_score(ecs::CriminalThreatLevel::FIREARM_ACTIVE) || high_threats >= 2) {
        quota.max_vehicles = 3;
        quota.max_foot_cops = 4;
    } else if (max_threat >= threat_level_score(ecs::CriminalThreatLevel::FIREARM_AIR_SHOOT)) {
        quota.max_vehicles = 3;
        quota.max_foot_cops = 3;
    } else if (max_threat >= threat_level_score(ecs::CriminalThreatLevel::FIREARM_INACTIVE)) {
        quota.max_vehicles = 2;
        quota.max_foot_cops = (active_threats >= 2) ? 3 : 2;
    } else if (max_threat >= threat_level_score(ecs::CriminalThreatLevel::MELEE_ACTIVE)) {
        quota.max_vehicles = (total_criminals <= 1) ? 1 : 2;
        quota.max_foot_cops = (total_criminals <= 1) ? 1 : 2;
    } else {
        quota.max_vehicles = 1;
        quota.max_foot_cops = 1;
    }

    if (threat_score >= 20) {
        quota.max_foot_cops = std::max(quota.max_foot_cops, 4);
        quota.max_vehicles = std::max(quota.max_vehicles, 3);
    } else if (threat_score >= 12) {
        quota.max_foot_cops = std::max(quota.max_foot_cops, 3);
        quota.max_vehicles = std::max(quota.max_vehicles, 2);
    }

    LOGI("📊 [Quota] Threat-based quota (max=%d, score=%d, active=%d, high=%d, total=%d) -> V=%d F=%d",
         max_threat, threat_score, active_threats, high_threats, total_criminals,
         quota.max_vehicles, quota.max_foot_cops);
    return quota;
}

int compute_nearby_dispatch_quota(const CrimeEvent& crime) {
    int threat_score = compute_case_threat_score(crime);
    int active_threats = count_active_threats(crime);
    CVector pos = get_crime_dispatch_position(crime);
    int density = count_criminals_near(pos, 40.0f);

    int quota = 1;
    if (threat_score >= 20 || active_threats >= 4 || density >= 6) {
        quota = 4;
    } else if (threat_score >= 12 || active_threats >= 2 || density >= 3) {
        quota = 2;
    }

    LOGI("📊 [NearbyQuota] Case %llu threat_score=%d active=%d density=%d -> quota=%d",
         (unsigned long long)crime.case_id, threat_score, active_threats, density, quota);
    return quota;
}

CPed* pick_criminal_target_for_cop(CPed* cop, const CrimeEvent& crime) {
    if (!cop || !is_ped_pointer_valid_safe(cop)) return nullptr;

    CVector cop_pos = get_entity_pos(cop);
    CPed* best_criminal = nullptr;
    float best_score = -999999.0f;

    constexpr float kThreatWeight = 45.0f;
    constexpr float kNearPreferRadius = 15.0f;

    for (CPed* criminal : crime.consolidated_criminals) {
        if (!criminal || !is_ped_pointer_valid_safe(criminal)) continue;
        if (g_IsAlive && !g_IsAlive(criminal)) continue;

        CVector crim_pos = get_entity_pos(criminal);
        float dx = cop_pos.x - crim_pos.x;
        float dy = cop_pos.y - crim_pos.y;
        float dz = cop_pos.z - crim_pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        int threat = threat_level_score(get_criminal_threat_level(criminal, &crime));
        float score = static_cast<float>(threat) * kThreatWeight - dist;
        if (dist <= kNearPreferRadius) {
            score += static_cast<float>(threat) * 10.0f;
        }

        if (score > best_score) {
            best_score = score;
            best_criminal = criminal;
        }
    }

    return best_criminal ? best_criminal : crime.criminal;
}

} // namespace dispatch_threat