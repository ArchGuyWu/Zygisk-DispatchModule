#include "dispatch_reroute.hpp"

#include <algorithm>
#include <cmath>

#include "ecs_engine.hpp"
#include "dispatch_threat.hpp"
#include "dispatch_timing.hpp"
#include "mod_shared.hpp"

namespace dispatch_reroute {

int get_case_threat_tier(const CrimeEvent& crime) {
    int score = dispatch_threat::threat_level_score(
        dispatch_threat::get_case_max_threat_level(crime));
    if (score >= dispatch_threat::threat_level_score(ecs::CriminalThreatLevel::FIREARM_INACTIVE)) {
        return 2;
    }
    if (score >= dispatch_threat::threat_level_score(ecs::CriminalThreatLevel::MELEE_ACTIVE)) {
        return 1;
    }
    return crime.is_firearm ? 2 : 1;
}

std::shared_ptr<CrimeEvent> find_higher_threat_case_in_av(
    const std::shared_ptr<CrimeEvent>& from_case,
    CVector observer_pos,
    const std::vector<std::shared_ptr<CrimeEvent>>& crimes) {
    if (!from_case) return nullptr;

    int tier_a = get_case_threat_tier(*from_case);
    std::shared_ptr<CrimeEvent> best_case = nullptr;
    float best_dist = 999999.0f;

    for (const auto& case_b : crimes) {
        if (!case_b || case_b->cancelled || case_b == from_case) continue;
        if (!case_b->criminal || !is_ped_pointer_valid_safe(case_b->criminal)) continue;

        int tier_b = get_case_threat_tier(*case_b);
        if (tier_b < tier_a) continue;

        CVector anchor = get_crime_dispatch_position(*case_b);
        float dx = observer_pos.x - anchor.x;
        float dy = observer_pos.y - anchor.y;
        float dz = observer_pos.z - anchor.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        float av_range = dispatch_timing::get_av_range_for_crime(*case_b);

        if (dist <= av_range && dist < best_dist) {
            best_dist = dist;
            best_case = case_b;
        }
    }

    return best_case;
}

static std::shared_ptr<CrimeEvent> find_case_for_target(CPed* target) {
    if (!target) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (const auto& crime : g_active_crimes) {
        if (!crime || crime->cancelled) continue;
        if (crime->criminal == target) return crime;
        for (CPed* crim : crime->consolidated_criminals) {
            if (crim == target) return crime;
        }
    }
    return nullptr;
}

bool try_reroute_foot_cop(CPed* cop, CPed* current_target) {
    if (!cop || !current_target) return false;

    auto case_a = find_case_for_target(current_target);
    if (!case_a) return false;

    std::vector<std::shared_ptr<CrimeEvent>> crimes_snapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
        crimes_snapshot = g_active_crimes;
    }

    CVector cop_pos = get_entity_pos(cop);
    auto case_b = find_higher_threat_case_in_av(case_a, cop_pos, crimes_snapshot);
    if (!case_b || case_b == case_a) return false;

    LOGI("🚨 [EnRouteReroute] Foot cop %p Case %llu (tier=%d) -> Case %llu (tier=%d)",
         cop, (unsigned long long)case_a->case_id, get_case_threat_tier(*case_a),
         (unsigned long long)case_b->case_id, get_case_threat_tier(*case_b));

    make_single_cop_attack_criminal(cop, case_b->criminal, true);
    auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (combat_comp) {
        combat_comp->last_weapon_switch_time_ms = 0;
    }
    return true;
}

void apply_enroute_vehicle_reroutes(const std::vector<std::shared_ptr<CrimeEvent>>& crimes) {
    struct RerouteRecord {
        void* vehicle = nullptr;
        std::shared_ptr<CrimeEvent> from_case;
        std::shared_ptr<CrimeEvent> to_case;
        float distance = 0.0f;
    };
    std::vector<RerouteRecord> pending;

    {
        std::lock_guard<std::mutex> lock_sc(g_vehicles_mutex);
        for (const auto& case_a : crimes) {
            if (!case_a || case_a->cancelled) continue;

            for (void* veh : case_a->case_vehicles) {
                if (!veh || !is_vehicle_pointer_valid(veh)) continue;
                if (g_vehicles_ordered_to_scene.count(veh) == 0 || is_vehicle_emptied(veh)) continue;

                CVector veh_pos = get_entity_pos(veh);
                auto case_b = find_higher_threat_case_in_av(case_a, veh_pos, crimes);
                if (case_b && case_b != case_a) {
                    float dx = veh_pos.x - get_crime_dispatch_position(*case_b).x;
                    float dy = veh_pos.y - get_crime_dispatch_position(*case_b).y;
                    float dz = veh_pos.z - get_crime_dispatch_position(*case_b).z;
                    pending.push_back({veh, case_a, case_b, sqrtf(dx * dx + dy * dy + dz * dz)});
                }
            }
        }
    }

    for (const auto& record : pending) {
        void* veh = record.vehicle;
        auto case_a = record.from_case;
        auto case_b = record.to_case;
        if (!case_a || case_a->cancelled || !case_b || case_b->cancelled) continue;

        auto it_a = std::find(case_a->case_vehicles.begin(), case_a->case_vehicles.end(), veh);
        if (it_a != case_a->case_vehicles.end()) {
            case_a->case_vehicles.erase(it_a);
        }
        if (case_a->spawned_vehicle == veh) {
            case_a->spawned_vehicle = nullptr;
        }

        if (std::find(case_b->case_vehicles.begin(), case_b->case_vehicles.end(), veh) == case_b->case_vehicles.end()) {
            case_b->case_vehicles.push_back(veh);
        }
        if (!case_b->spawned_vehicle) {
            case_b->spawned_vehicle = veh;
        }

        LOGI("🚨 [EnRouteReroute] Vehicle %p Case %llu (tier=%d) -> Case %llu (tier=%d, dist=%.1fm)",
             veh, (unsigned long long)case_a->case_id, get_case_threat_tier(*case_a),
             (unsigned long long)case_b->case_id, get_case_threat_tier(*case_b), record.distance);

        command_cop_vehicle_to_scene(veh, get_crime_dispatch_position(*case_b));
        setup_dispatched_cops(veh, case_b->criminal);
    }
}

} // namespace dispatch_reroute