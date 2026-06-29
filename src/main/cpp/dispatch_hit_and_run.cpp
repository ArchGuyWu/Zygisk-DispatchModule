#include <cmath>
#include <algorithm>

#include "dispatch_hit_and_run.hpp"
#include "dispatch_emergency_services.hpp"
#include "game_config.hpp"
#include "dispatch_tick_internal.hpp"
#include "ecs_engine.hpp"
#include "mod_shared.hpp"
#include "pointer_sanitizer.hpp"

namespace dispatch_hit_and_run {

static std::vector<PendingHitAndRunEntry> g_pending_hit_and_run;
static std::mutex g_pending_hit_and_run_mutex;

static constexpr int64_t PENDING_WANTED_TTL_MS = 600000;
static constexpr int MAX_CONCURRENT_CASES = 4;
static constexpr float IDLE_COP_SEARCH_RADIUS_M = 120.0f;

static float distance_3d(CVector a, CVector b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool is_vehicle_hit_kill(eWeaponType weapon, const CEntity* killer) {
    if (weapon == WEAPON_RAMMEDBYCAR || weapon == WEAPON_RUNOVERBYCAR) {
        return true;
    }
    if (killer && is_vehicle_pointer_valid(const_cast<CEntity*>(killer))) {
        return true;
    }
    return false;
}

CPed* resolve_kill_perpetrator(const CEntity* killer, eWeaponType weapon) {
    if (!killer) return nullptr;

    if (is_ped_pointer_valid_safe(const_cast<CEntity*>(killer))) {
        return const_cast<CPed*>(reinterpret_cast<const CPed*>(killer));
    }

    if (is_vehicle_pointer_valid(const_cast<CEntity*>(killer))) {
        return get_vehicle_driver_ped(const_cast<void*>(
            reinterpret_cast<const void*>(killer)));
    }

    (void)weapon;
    return nullptr;
}

bool is_case_criminal(const CrimeEvent& crime, CPed* ped) {
    if (!ped) return false;
    if (crime.criminal == ped) return true;
    for (CPed* c : crime.consolidated_criminals) {
        if (c == ped) return true;
    }
    return false;
}

static void register_pending_hit_and_run(
    CPed* driver,
    void* vehicle,
    CVector scene_pos,
    uint64_t nearby_case_id,
    eWeaponType weapon) {
    if (!driver || !is_ped_pointer_valid_safe(driver)) return;
    if (g_GetPedType) {
        int ped_type = g_GetPedType(driver);
        if (ped_type == PED_TYPE_COP || ped_type == PED_TYPE_PLAYER) {
            return;
        }
    }

    std::lock_guard<std::mutex> lock(g_pending_hit_and_run_mutex);
    for (const auto& entry : g_pending_hit_and_run) {
        if (entry.fugitive == driver) {
            LOGI("🚗 [HitAndRun] Fugitive %p already on pending wanted list (case %llu)",
                 driver, (unsigned long long)nearby_case_id);
            return;
        }
    }

    g_pending_hit_and_run.push_back({
        driver,
        vehicle,
        scene_pos,
        nearby_case_id,
        now_ms(),
        weapon,
    });

    LOGI("🚗 [HitAndRun] Queued fugitive %p on pending wanted list "
         "(vehicle=%p, scene case=%llu, weapon=%d) — waits for idle police",
         driver, vehicle, (unsigned long long)nearby_case_id, (int)weapon);
}

static int count_active_crime_cases() {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    int count = 0;
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            count++;
        }
    }
    return count;
}

static int count_on_scene_cases() {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    int count = 0;
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled && crime->dispatch_state == STATE_ON_SCENE) {
            count++;
        }
    }
    return count;
}

static bool any_case_deploying_resources() {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (const auto& crime : g_active_crimes) {
        if (!crime || crime->cancelled) continue;
        if (crime->dispatch_state == STATE_TIMING) return true;
        if (!crime->pending_tasks.empty()) return true;
    }
    return false;
}

static bool is_cop_busy_with_active_case(CPed* cop) {
    if (!cop) return false;

    if (g_GetWeaponLockOnTarget) {
        CEntity* lock_target = g_GetWeaponLockOnTarget(cop);
        if (lock_target && is_ped_pointer_valid_safe(lock_target)) {
            if (find_crime_containing_criminal(reinterpret_cast<CPed*>(lock_target))) {
                return true;
            }
        }
    }

    auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (combat && combat->target_entity && is_ped_pointer_valid_safe(combat->target_entity)) {
        if (find_crime_containing_criminal(static_cast<CPed*>(combat->target_entity))) {
            return true;
        }
    }

    void* veh = find_vehicle_of_cop_cached(cop);
    if (veh && is_vehicle_ordered_to_scene(veh)) {
        return true;
    }

    return false;
}

static int count_idle_cops_near(CVector pos, float radius) {
    if (!g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return 0;

    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return 0;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return 0;

    float radius_sq = radius * radius;
    int idle = 0;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag < 0) continue;

        int handle = (i << 8) | flag;
        CPed* ped = g_GetPoolPed(handle);
        if (!ped || !is_ped_pointer_valid_safe(ped)) continue;
        if (g_IsAlive && !g_IsAlive(ped)) continue;
        if (g_GetPedType(ped) != PED_TYPE_COP) continue;
        if (is_cop_busy_with_active_case(ped)) continue;

        CVector cop_pos = get_entity_pos(ped);
        float dx = cop_pos.x - pos.x;
        float dy = cop_pos.y - pos.y;
        float dz = cop_pos.z - pos.z;
        if ((dx * dx + dy * dy + dz * dz) <= radius_sq) {
            idle++;
        }
    }
    return idle;
}

static bool has_idle_police_capacity(CVector reference_pos) {
    if (count_active_crime_cases() >= MAX_CONCURRENT_CASES) {
        return false;
    }
    if (any_case_deploying_resources()) {
        return false;
    }

    int idle_nearby = count_idle_cops_near(reference_pos, IDLE_COP_SEARCH_RADIUS_M);
    if (idle_nearby >= 1) {
        return true;
    }

    // 无到场案件时允许走刷车兜底
    return count_on_scene_cases() == 0;
}

static bool activate_fugitive_case(const PendingHitAndRunEntry& entry, int64_t cur_time) {
    if (!entry.fugitive || !is_ped_pointer_valid_safe(entry.fugitive)) return false;
    if (g_IsAlive && !g_IsAlive(entry.fugitive)) return false;
    if (find_crime_containing_criminal(entry.fugitive)) return true;

    CVector pos = get_entity_pos(entry.fugitive);
    if (!has_idle_police_capacity(pos)) {
        return false;
    }
    if (!should_activate_or_hijack_crime(pos, false)) {
        LOGI("🚗 [HitAndRun] Fugitive %p still queued — case slot/policy blocked",
             entry.fugitive);
        return false;
    }

    LOGI("🚨 [HitAndRun] Idle police available -> pursuing pending fugitive %p "
         "(orig case=%llu, pos=%.1f,%.1f,%.1f)",
         entry.fugitive, (unsigned long long)entry.nearby_case_id,
         pos.x, pos.y, pos.z);

    ecs::EventDispatcher::get().dispatch(ecs::CrimeReportEvent(
        entry.fugitive, nullptr, pos, false, 1, cur_time));
    return true;
}

bool is_pending_fugitive(CPed* ped) {
    if (!ped) return false;
    std::lock_guard<std::mutex> lock(g_pending_hit_and_run_mutex);
    for (const auto& entry : g_pending_hit_and_run) {
        if (entry.fugitive == ped) return true;
    }
    return false;
}

void on_perpetrator_crime_report(CPed* perpetrator) {
    if (!perpetrator || !is_pending_fugitive(perpetrator)) return;
    LOGI("🚗 [HitAndRun] Pending fugitive %p re-offended — remains queued until idle police",
         perpetrator);
}

void tick_pending_wanted(int64_t cur_time) {
    PendingHitAndRunEntry candidate;
    bool has_candidate = false;

    {
        std::lock_guard<std::mutex> lock(g_pending_hit_and_run_mutex);
        for (auto it = g_pending_hit_and_run.begin(); it != g_pending_hit_and_run.end(); ) {
            if (!it->fugitive || !is_ped_pointer_valid_safe(it->fugitive) ||
                (g_IsAlive && !g_IsAlive(it->fugitive))) {
                LOGI("🚗 [HitAndRun] Fugitive %p gone — removed from pending wanted list",
                     it->fugitive);
                it = g_pending_hit_and_run.erase(it);
                continue;
            }

            if (cur_time - it->report_time_ms > PENDING_WANTED_TTL_MS) {
                LOGI("🚗 [HitAndRun] Fugitive %p expired from pending wanted list",
                     it->fugitive);
                it = g_pending_hit_and_run.erase(it);
                continue;
            }

            if (find_crime_containing_criminal(it->fugitive)) {
                LOGI("🚗 [HitAndRun] Fugitive %p already in active case — removed from pending list",
                     it->fugitive);
                it = g_pending_hit_and_run.erase(it);
                continue;
            }

            if (!has_candidate) {
                candidate = *it;
                has_candidate = true;
            }
            ++it;
        }
    }

    if (!has_candidate) return;

    if (activate_fugitive_case(candidate, cur_time)) {
        std::lock_guard<std::mutex> lock(g_pending_hit_and_run_mutex);
        for (auto it = g_pending_hit_and_run.begin(); it != g_pending_hit_and_run.end(); ) {
            if (it->fugitive == candidate.fugitive) {
                it = g_pending_hit_and_run.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void clear_all_pending() {
    std::lock_guard<std::mutex> lock(g_pending_hit_and_run_mutex);
    g_pending_hit_and_run.clear();
}

void handle_cop_death_near_case(
    const CPed* dead_cop,
    const CEntity* killer,
    eWeaponType weapon,
    CVector cop_pos) {
    (void)dead_cop;

    std::shared_ptr<CrimeEvent> nearest_crime;
    float nearest_dist = 80.0f;

    {
        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
        float min_dist_sq = 80.0f * 80.0f;

        for (auto& crime : g_active_crimes) {
            if (!crime || crime->cancelled) continue;
            CVector anchor = get_crime_dispatch_position(*crime);
            float dx = cop_pos.x - anchor.x;
            float dy = cop_pos.y - anchor.y;
            float dz = cop_pos.z - anchor.z;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            if (dist_sq < min_dist_sq) {
                min_dist_sq = dist_sq;
                nearest_crime = crime;
                nearest_dist = sqrtf(dist_sq);
            }
        }
    }

    dispatch_emergency_services::on_cop_casualty_near_crime(cop_pos);

    if (!nearest_crime) {
        LOGI("📡 [dispatchCenter] Cop killed far from any active case -> no case attribution");
        return;
    }

    CPed* killer_ped = resolve_kill_perpetrator(killer, weapon);
    void* killer_vehicle = nullptr;
    if (killer && is_vehicle_pointer_valid(const_cast<CEntity*>(killer))) {
        killer_vehicle = const_cast<void*>(reinterpret_cast<const void*>(killer));
    } else if (killer_ped) {
        killer_vehicle = find_vehicle_of_cop(killer_ped);
    }

    if (killer_ped && is_ped_pointer_valid_safe(killer_ped) &&
        is_case_criminal(*nearest_crime, killer_ped)) {
        nearest_crime->cops_killed++;
        LOGI("📡 [dispatchCenter] Cop killed BY case criminal %p at case %llu "
             "(criminal-kills=%d, dist=%.1f) -> reinforcement eligible",
             killer_ped, (unsigned long long)nearest_crime->case_id,
             nearest_crime->cops_killed, nearest_dist);
        return;
    }

    if (is_vehicle_hit_kill(weapon, killer)) {
        if (killer_ped && is_ped_pointer_valid_safe(killer_ped)) {
            register_pending_hit_and_run(
                killer_ped,
                killer_vehicle,
                cop_pos,
                nearest_crime->case_id,
                weapon);
            LOGI("📡 [dispatchCenter] Cop killed by hit-and-run driver %p near case %llu "
                 "(weapon=%d) -> pending wanted, NOT counted on criminal",
                 killer_ped, (unsigned long long)nearest_crime->case_id, (int)weapon);
        } else {
            LOGI("📡 [dispatchCenter] Vehicle cop kill near case %llu but no driver identified "
                 "-> ignored for criminal stats",
                 (unsigned long long)nearest_crime->case_id);
        }
        return;
    }

    const char* killer_tag = killer_ped ? "unrelated ped" : "unknown";
    LOGI("📡 [dispatchCenter] Cop killed by %s near case %llu (weapon=%d) "
         "-> NOT attributed to case criminal",
         killer_tag, (unsigned long long)nearest_crime->case_id, (int)weapon);
}

} // namespace dispatch_hit_and_run