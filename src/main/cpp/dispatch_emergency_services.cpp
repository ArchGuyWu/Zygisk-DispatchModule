#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "dispatch_emergency_services.hpp"
#include "dispatch_tick_internal.hpp"
#include "dispatch_threat.hpp"
#include "dispatch_timing.hpp"
#include "game_config.hpp"
#include "mod_shared.hpp"

namespace dispatch_emergency_services {

static std::vector<EmergencyServiceRecord> g_spawned_emergency_services;
static std::mutex g_spawned_emergency_services_mutex;
static std::atomic<int64_t> g_last_global_fire_dispatch_ms{0};

struct PendingGlobalFireSpawn {
    CVector spawn_pos;
    CVector fire_pos;
    int64_t execute_at;
};
static std::vector<PendingGlobalFireSpawn> g_pending_global_fire_spawns;
static int64_t g_last_priority_eval_ms = 0;

static bool is_emergency_services_model(unsigned int model) {
    return model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK;
}

static float distance_3d(CVector a, CVector b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int proximity_bonus(const CrimeEvent& crime, CVector reference_pos) {
    if (!g_FindPlayerCoors) return 0;
    CVector player_pos = g_FindPlayerCoors(0);
    CVector anchor = get_crime_dispatch_position(crime);
    float dist_player = distance_3d(player_pos, anchor);
    float av_range = dispatch_timing::get_av_range_for_crime(crime);

    int bonus = 0;
    if (dist_player <= av_range) {
        bonus += 15;
    } else if (dist_player <= av_range * 1.5f) {
        bonus += 6;
    }
    bonus += static_cast<int>((150.0f - std::min(dist_player, 150.0f)) / 12.0f);

    float dist_ref = distance_3d(reference_pos, anchor);
    bonus -= static_cast<int>(dist_ref / dispatch_timing::EMS_PRIORITY_DISTANCE_PENALTY_M);
    return bonus;
}

int compute_ambulance_priority(const CrimeEvent& crime, CVector reference_pos) {
    int score = dispatch_threat::compute_case_threat_score(crime);
    score += crime.civilian_casualties_recorded * 8;
    score += crime.cops_killed * 12;
    if (crime.is_firearm) {
        score += 6;
    }
    score += proximity_bonus(crime, reference_pos);
    return score;
}

int compute_firetruck_priority(const CrimeEvent& crime, CVector reference_pos, bool has_fire) {
    int score = dispatch_threat::compute_case_threat_score(crime);
    if (has_fire) {
        score += 25;
    }
    score += crime.civilian_casualties_recorded * 4;
    if (crime.is_firearm && has_fire) {
        score += 10;
    }
    score += proximity_bonus(crime, reference_pos);
    return score;
}

static std::shared_ptr<CrimeEvent> find_priority_crime_near(
    CVector pos,
    float range,
    bool for_ambulance) {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    std::shared_ptr<CrimeEvent> best;
    int best_score = -999999;
    float range_sq = range * range;

    for (const auto& crime : g_active_crimes) {
        if (!crime || crime->cancelled) continue;
        CVector anchor = get_crime_dispatch_position(*crime);
        float dx = anchor.x - pos.x;
        float dy = anchor.y - pos.y;
        float dz = anchor.z - pos.z;
        float dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq > range_sq) continue;

        int score = for_ambulance
                        ? compute_ambulance_priority(*crime, pos)
                        : compute_firetruck_priority(*crime, pos, true);
        if (score > best_score) {
            best_score = score;
            best = crime;
        }
    }
    return best;
}

static std::shared_ptr<CrimeEvent> find_crime_by_id(
    const std::vector<std::shared_ptr<CrimeEvent>>& crimes,
    uint64_t case_id) {
    for (const auto& crime : crimes) {
        if (crime && !crime->cancelled && crime->case_id == case_id) {
            return crime;
        }
    }
    return nullptr;
}

static void prune_invalid_records_locked() {
    g_spawned_emergency_services.erase(
        std::remove_if(
            g_spawned_emergency_services.begin(),
            g_spawned_emergency_services.end(),
            [](const EmergencyServiceRecord& record) {
                return !record.vehicle || !is_vehicle_pointer_valid(record.vehicle);
            }),
        g_spawned_emergency_services.end());
}

CVector clamp_spawn_to_streaming_range(CVector incident_anchor, CVector proposed_spawn) {
    if (!g_FindPlayerCoors) return proposed_spawn;

    CVector player_pos = g_FindPlayerCoors(0);
    float dx = proposed_spawn.x - player_pos.x;
    float dy = proposed_spawn.y - player_pos.y;
    float dist_xy = sqrtf(dx * dx + dy * dy);

    if (dist_xy <= dispatch_timing::EMERGENCY_STREAMING_MAX_DIST_M) {
        return proposed_spawn;
    }

    float scale = dispatch_timing::EMERGENCY_STREAMING_TARGET_DIST_M / dist_xy;
    CVector clamped = {
        player_pos.x + dx * scale,
        player_pos.y + dy * scale,
        proposed_spawn.z
    };

    float anchor_dx = clamped.x - incident_anchor.x;
    float anchor_dy = clamped.y - incident_anchor.y;
    float anchor_dist = sqrtf(anchor_dx * anchor_dx + anchor_dy * anchor_dy);
    if (anchor_dist > dispatch_timing::EMERGENCY_STREAMING_TARGET_DIST_M + 25.0f) {
        float pull = (dispatch_timing::EMERGENCY_STREAMING_TARGET_DIST_M + 10.0f) / anchor_dist;
        clamped.x = incident_anchor.x + anchor_dx * pull;
        clamped.y = incident_anchor.y + anchor_dy * pull;
    }

    return clamped;
}

int count_active_emergency_vehicles_near(CVector pos, float range, unsigned int model) {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle) return 0;

    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool) return 0;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return 0;

    int count = 0;
    float range_sq = range * range;
    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag < 0) continue;

        int handle = (i << 8) | flag;
        void* veh = g_GetPoolVehicle(handle);
        if (!veh || !is_vehicle_pointer_valid(veh)) continue;
        if (get_entity_model_index(veh) != model) continue;

        CVector veh_pos = get_entity_pos(veh);
        float dx = veh_pos.x - pos.x;
        float dy = veh_pos.y - pos.y;
        float dz = veh_pos.z - pos.z;
        if (dx * dx + dy * dy + dz * dz <= range_sq) {
            count++;
        }
    }
    return count;
}

static bool can_spawn_emergency_model(unsigned int model, CVector incident_anchor) {
    CVector player_pos = g_FindPlayerCoors ? g_FindPlayerCoors(0) : incident_anchor;
    int cap = (model == MODEL_AMBULANCE)
                  ? dispatch_timing::MAX_AMBULANCE_NEAR_PLAYER
                  : dispatch_timing::MAX_FIRETRUCK_NEAR_PLAYER;
    int active = count_active_emergency_vehicles_near(
        player_pos, dispatch_timing::EMERGENCY_CAP_RANGE_M, model);
    return active < cap;
}

void command_emergency_vehicle_ai(void* vehicle, unsigned int model, const CVector& target_loc, float dist) {
    if (!g_GetCarToGoToCoors || !vehicle) return;

    float stop_dist = (model == MODEL_FIRETRUCK) ? 28.0f : 22.0f;
    if (dist < stop_dist) {
        CVector veh_pos = get_entity_pos(vehicle);
        g_GetCarToGoToCoors(vehicle, &veh_pos, 4, false);
        dispatch_tell_occupants_to_leave_car(vehicle);
        add_vehicle_emptied(vehicle);
        return;
    }

    g_GetCarToGoToCoors(vehicle, const_cast<CVector*>(&target_loc), 2, true);
}

void command_emergency_vehicle_to_scene(void* vehicle, unsigned int model, const CVector& target_loc) {
    if (!vehicle) return;

    if (!is_vehicle_occupied_by_driver(vehicle)) {
        LOGW("⚠️ [ModEMS] Vehicle %p (model=%u) has no driver after spawn", vehicle, model);
    }

    if (is_vehicle_occupied_by_driver(vehicle)) {
        CVector veh_pos = get_entity_pos(vehicle);
        float dx = veh_pos.x - target_loc.x;
        float dy = veh_pos.y - target_loc.y;
        float dz = veh_pos.z - target_loc.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        command_emergency_vehicle_ai(vehicle, model, target_loc, dist);
        add_vehicle_ordered_to_scene(vehicle);
        LOGI("🚑🚒 [ModEMS] Commanded vehicle %p (model=%u) to scene (dist=%.1f)", vehicle, model, dist);
    }
}

void setup_dispatched_emergency_vehicle(
    void* vehicle,
    unsigned int model,
    CVector target_anchor,
    const std::shared_ptr<CrimeEvent>& crime,
    bool target_is_fire) {
    if (!vehicle || !is_emergency_services_model(model)) return;

    bind_vehicle_occupants(vehicle);

    if (crime) {
        if (std::find(crime->case_vehicles.begin(), crime->case_vehicles.end(), vehicle) == crime->case_vehicles.end()) {
            crime->case_vehicles.push_back(vehicle);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_spawned_emergency_services_mutex);
        prune_invalid_records_locked();
        bool found = false;
        for (auto& record : g_spawned_emergency_services) {
            if (record.vehicle == vehicle) {
                record.model = model;
                record.case_id = crime ? crime->case_id : 0;
                record.target_anchor = target_anchor;
                record.target_is_fire = target_is_fire;
                record.spawn_time_ms = now_ms();
                found = true;
                break;
            }
        }
        if (!found) {
            g_spawned_emergency_services.push_back({
                vehicle,
                model,
                crime ? crime->case_id : 0,
                target_anchor,
                target_is_fire,
                now_ms(),
                0
            });
        }
    }

    command_emergency_vehicle_to_scene(vehicle, model, target_anchor);

    if (!is_vehicle_siren_awakened(vehicle)) {
        if (g_VehicleInflictDamage) {
            CVector veh_pos = get_entity_pos(vehicle);
            g_VehicleInflictDamage(vehicle, nullptr, WEAPON_UNARMED, 0.0f, veh_pos);
        }
        add_vehicle_siren_awakened(vehicle);
    }
}

static void finalize_spawn_task(
    const std::shared_ptr<CrimeEvent>& crime,
    unsigned int model,
    CVector spawn_pos,
    CVector incident_anchor,
    bool target_is_fire) {
    if (crime && crime->cancelled) return;

    void* veh = find_closest_vehicle_to(spawn_pos, 25.0f);
    if (!veh) {
        LOGW("⚠️ [ModEMS] Failed to identify spawned vehicle model=%u near (%.1f, %.1f, %.1f)",
             model, spawn_pos.x, spawn_pos.y, spawn_pos.z);
        if (crime) {
            if (model == MODEL_AMBULANCE) crime->mod_ambulance_dispatched = 0;
            if (model == MODEL_FIRETRUCK) crime->mod_firetruck_dispatched = 0;
        }
        return;
    }

    setup_dispatched_emergency_vehicle(veh, model, incident_anchor, crime, target_is_fire);
    LOGI("✅ [ModEMS] Case %llu configured %s at %p",
         crime ? (unsigned long long)crime->case_id : 0ULL,
         model == MODEL_AMBULANCE ? "ambulance" : "firetruck",
         veh);
}

void schedule_ambulance_for_crime(const std::shared_ptr<CrimeEvent>& crime, CVector incident_anchor) {
    if (!crime || crime->cancelled || crime->mod_ambulance_dispatched > 0) return;
    if (!can_spawn_emergency_model(MODEL_AMBULANCE, incident_anchor)) {
        LOGI("🚫 [ModEMS] Ambulance cap reached near player for case %llu (priority=%d, deferred)",
             (unsigned long long)crime->case_id,
             compute_ambulance_priority(*crime, incident_anchor));
        return;
    }

    crime->mod_ambulance_dispatched = 1;
    CVector spawn_pos = clamp_spawn_to_streaming_range(incident_anchor, get_spawn_target(incident_anchor));
    int delay = get_random_range(
        dispatch_timing::AMBULANCE_SPAWN_DELAY_MIN_MS,
        dispatch_timing::AMBULANCE_SPAWN_DELAY_MAX_MS);

    LOGI("🚑 [ModEMS] Scheduling ambulance for case %llu in %dms (casualties=%d, priority=%d)",
         (unsigned long long)crime->case_id, delay, crime->civilian_casualties_recorded,
         compute_ambulance_priority(*crime, incident_anchor));

    crime->pending_tasks.push_back({
        now_ms() + delay,
        [crime, spawn_pos, incident_anchor]() {
            if (crime->cancelled) {
                crime->mod_ambulance_dispatched = 0;
                return;
            }
            dispatch_spawn_emergency_car(MODEL_AMBULANCE, spawn_pos, incident_anchor);
            crime->pending_tasks.push_back({
                now_ms() + dispatch_timing::VEHICLE_IDENTIFY_DELAY_MS,
                [crime, spawn_pos, incident_anchor]() {
                    finalize_spawn_task(crime, MODEL_AMBULANCE, spawn_pos, incident_anchor, false);
                }
            });
        }
    });
}

void schedule_firetruck_for_crime(const std::shared_ptr<CrimeEvent>& crime, CVector fire_pos) {
    if (!crime || crime->cancelled || crime->mod_firetruck_dispatched > 0) return;
    if (!can_spawn_emergency_model(MODEL_FIRETRUCK, fire_pos)) {
        LOGI("🚫 [ModEMS] Firetruck cap reached near player for case %llu (priority=%d, deferred)",
             (unsigned long long)crime->case_id,
             compute_firetruck_priority(*crime, fire_pos, true));
        return;
    }

    crime->mod_firetruck_dispatched = 1;
    CVector spawn_pos = clamp_spawn_to_streaming_range(fire_pos, get_spawn_target(fire_pos));
    int delay = get_random_range(
        dispatch_timing::FIRETRUCK_SPAWN_DELAY_MIN_MS,
        dispatch_timing::FIRETRUCK_SPAWN_DELAY_MAX_MS);

    LOGI("🚒 [ModEMS] Scheduling firetruck for case %llu in %dms (fire at %.1f, %.1f, %.1f, priority=%d)",
         (unsigned long long)crime->case_id, delay, fire_pos.x, fire_pos.y, fire_pos.z,
         compute_firetruck_priority(*crime, fire_pos, true));

    crime->pending_tasks.push_back({
        now_ms() + delay,
        [crime, spawn_pos, fire_pos]() {
            if (crime->cancelled) {
                crime->mod_firetruck_dispatched = 0;
                return;
            }
            dispatch_spawn_emergency_car(MODEL_FIRETRUCK, spawn_pos, fire_pos);
            crime->pending_tasks.push_back({
                now_ms() + dispatch_timing::VEHICLE_IDENTIFY_DELAY_MS,
                [crime, spawn_pos, fire_pos]() {
                    finalize_spawn_task(crime, MODEL_FIRETRUCK, spawn_pos, fire_pos, true);
                }
            });
        }
    });
}

void on_civilian_casualty_near_crime(const CPed* dead_ped, CVector death_pos) {
    if (!dead_ped || !g_GetPedType) return;

    int ped_type = g_GetPedType(const_cast<CPed*>(dead_ped));
    if (ped_type == PED_TYPE_PLAYER || ped_type == PED_TYPE_COP ||
        ped_type == PED_TYPE_MEDIC || ped_type == PED_TYPE_FIREMAN) {
        return;
    }

    auto crime = find_priority_crime_near(death_pos, dispatch_timing::CASUALTY_CASE_RANGE_M, true);
    if (!crime) return;

    if (crime->criminal == dead_ped) return;
    for (CPed* crim : crime->consolidated_criminals) {
        if (crim == dead_ped) return;
    }

    crime->civilian_casualties_recorded++;
    crime->last_emergency_eval_ms = 0;
    LOGI("🚑 [ModEMS] Civilian casualty -> case %llu (total=%d, priority=%d)",
         (unsigned long long)crime->case_id, crime->civilian_casualties_recorded,
         compute_ambulance_priority(*crime, death_pos));
}

void on_cop_casualty_near_crime(CVector death_pos) {
    auto crime = find_priority_crime_near(death_pos, dispatch_timing::CASUALTY_CASE_RANGE_M, true);
    if (!crime) return;

    crime->civilian_casualties_recorded++;
    crime->last_emergency_eval_ms = 0;
    LOGI("🚑 [ModEMS] Cop casualty -> case %llu (priority=%d)",
         (unsigned long long)crime->case_id,
         compute_ambulance_priority(*crime, death_pos));
}

static bool find_fire_near(CVector pos, float range, CVector& out_fire_pos) {
    if (!g_FireManager || !g_FindNearestFire) return false;

    void* fire = g_FindNearestFire(g_FireManager, pos, true, true);
    if (!fire) return false;

    CVector fire_pos;
    if (!get_fire_position(fire, fire_pos)) return false;

    float dx = fire_pos.x - pos.x;
    float dy = fire_pos.y - pos.y;
    float dz = fire_pos.z - pos.z;
    if (sqrtf(dx * dx + dy * dy + dz * dz) > range) return false;

    out_fire_pos = fire_pos;
    return true;
}

struct FireCandidate {
    std::shared_ptr<CrimeEvent> crime;
    CVector fire_pos;
    int priority = 0;
};

struct AmbulanceCandidate {
    std::shared_ptr<CrimeEvent> crime;
    CVector anchor;
    int priority = 0;
};

void evaluate_prioritized_emergency_needs(
    const std::vector<std::shared_ptr<CrimeEvent>>& crimes,
    int64_t cur_time) {
    if (cur_time - g_last_priority_eval_ms < dispatch_timing::EMERGENCY_EVAL_INTERVAL_MS) {
        return;
    }
    g_last_priority_eval_ms = cur_time;

    CVector player_pos = g_FindPlayerCoors ? g_FindPlayerCoors(0) : CVector{0, 0, 0};

    std::vector<FireCandidate> fire_candidates;
    std::vector<AmbulanceCandidate> ambulance_candidates;

    for (const auto& crime : crimes) {
        if (!crime || crime->cancelled) continue;
        crime->last_emergency_eval_ms = cur_time;

        CVector anchor = get_crime_dispatch_position(*crime);

        if (crime->mod_firetruck_dispatched == 0) {
            CVector fire_pos;
            if (find_fire_near(anchor, dispatch_timing::FIRE_DETECT_RANGE_M, fire_pos)) {
                fire_candidates.push_back({
                    crime,
                    fire_pos,
                    compute_firetruck_priority(*crime, player_pos, true)
                });
            }
        }

        if (crime->civilian_casualties_recorded > 0 && crime->mod_ambulance_dispatched == 0) {
            ambulance_candidates.push_back({
                crime,
                anchor,
                compute_ambulance_priority(*crime, player_pos)
            });
        }
    }

    std::sort(fire_candidates.begin(), fire_candidates.end(),
              [](const FireCandidate& a, const FireCandidate& b) {
                  return a.priority > b.priority;
              });
    std::sort(ambulance_candidates.begin(), ambulance_candidates.end(),
              [](const AmbulanceCandidate& a, const AmbulanceCandidate& b) {
                  return a.priority > b.priority;
              });

    int fire_slots = dispatch_timing::MAX_FIRETRUCK_NEAR_PLAYER -
                     count_active_emergency_vehicles_near(
                         player_pos, dispatch_timing::EMERGENCY_CAP_RANGE_M, MODEL_FIRETRUCK);
    for (const auto& candidate : fire_candidates) {
        if (fire_slots <= 0) {
            LOGI("🚒 [EMSPriority] Firetruck slot full — case %llu deferred (priority=%d)",
                 (unsigned long long)candidate.crime->case_id, candidate.priority);
            continue;
        }
        LOGI("🚒 [EMSPriority] Allocating firetruck to case %llu (priority=%d)",
             (unsigned long long)candidate.crime->case_id, candidate.priority);
        schedule_firetruck_for_crime(candidate.crime, candidate.fire_pos);
        fire_slots--;
    }

    int amb_slots = dispatch_timing::MAX_AMBULANCE_NEAR_PLAYER -
                    count_active_emergency_vehicles_near(
                        player_pos, dispatch_timing::EMERGENCY_CAP_RANGE_M, MODEL_AMBULANCE);
    for (const auto& candidate : ambulance_candidates) {
        if (amb_slots <= 0) {
            LOGI("🚑 [EMSPriority] Ambulance slot full — case %llu deferred (priority=%d)",
                 (unsigned long long)candidate.crime->case_id, candidate.priority);
            continue;
        }
        LOGI("🚑 [EMSPriority] Allocating ambulance to case %llu (priority=%d)",
             (unsigned long long)candidate.crime->case_id, candidate.priority);
        schedule_ambulance_for_crime(candidate.crime, candidate.anchor);
        amb_slots--;
    }
}

void evaluate_global_fire_dispatch(int64_t cur_time) {
    if (!g_FindPlayerCoors || !g_FireManager || !g_FindNearestFire) return;

    int64_t last = g_last_global_fire_dispatch_ms.load();
    if (cur_time - last < dispatch_timing::GLOBAL_FIRE_COOLDOWN_MS) return;

    CVector player_pos = g_FindPlayerCoors(0);
    CVector fire_pos;
    if (!find_fire_near(player_pos, dispatch_timing::GLOBAL_FIRE_PLAYER_RANGE_M, fire_pos)) {
        return;
    }

    if (find_priority_crime_near(fire_pos, 50.0f, false)) {
        return;
    }

    if (!can_spawn_emergency_model(MODEL_FIRETRUCK, fire_pos)) return;

    g_last_global_fire_dispatch_ms.store(cur_time);
    CVector spawn_pos = clamp_spawn_to_streaming_range(fire_pos, get_spawn_target(fire_pos));

    LOGI("🚒 [ModEMS] Global fire dispatch near player at (%.1f, %.1f, %.1f)",
         fire_pos.x, fire_pos.y, fire_pos.z);

    dispatch_spawn_emergency_car(MODEL_FIRETRUCK, spawn_pos, fire_pos);
    g_pending_global_fire_spawns.push_back({
        spawn_pos,
        fire_pos,
        now_ms() + dispatch_timing::VEHICLE_IDENTIFY_DELAY_MS
    });
}

static void transfer_emergency_vehicle_case(
    void* vehicle,
    const std::shared_ptr<CrimeEvent>& from_case,
    const std::shared_ptr<CrimeEvent>& to_case) {
    if (!vehicle || !to_case) return;

    if (from_case) {
        auto it = std::find(from_case->case_vehicles.begin(), from_case->case_vehicles.end(), vehicle);
        if (it != from_case->case_vehicles.end()) {
            from_case->case_vehicles.erase(it);
        }
        if (from_case->mod_ambulance_dispatched > 0 && get_entity_model_index(vehicle) == MODEL_AMBULANCE) {
            from_case->mod_ambulance_dispatched = 0;
        }
        if (from_case->mod_firetruck_dispatched > 0 && get_entity_model_index(vehicle) == MODEL_FIRETRUCK) {
            from_case->mod_firetruck_dispatched = 0;
        }
    }

    if (std::find(to_case->case_vehicles.begin(), to_case->case_vehicles.end(), vehicle) == to_case->case_vehicles.end()) {
        to_case->case_vehicles.push_back(vehicle);
    }
    if (get_entity_model_index(vehicle) == MODEL_AMBULANCE) {
        to_case->mod_ambulance_dispatched = 1;
    }
    if (get_entity_model_index(vehicle) == MODEL_FIRETRUCK) {
        to_case->mod_firetruck_dispatched = 1;
    }
}

void refresh_enroute_emergency_vehicles(
    const std::vector<std::shared_ptr<CrimeEvent>>& crimes,
    bool do_reroute_refresh) {
    if (!do_reroute_refresh) return;

    std::lock_guard<std::mutex> lock(g_spawned_emergency_services_mutex);
    prune_invalid_records_locked();

    int64_t cur_time = now_ms();
    CVector player_pos = g_FindPlayerCoors ? g_FindPlayerCoors(0) : CVector{0, 0, 0};

    for (auto& record : g_spawned_emergency_services) {
        if (!record.vehicle || is_vehicle_emptied(record.vehicle)) continue;
        if (cur_time - record.last_reroute_ms < dispatch_timing::EMERGENCY_REROUTE_INTERVAL_MS) {
            continue;
        }

        auto from_case = find_crime_by_id(crimes, record.case_id);
        CVector veh_pos = get_entity_pos(record.vehicle);

        std::shared_ptr<CrimeEvent> better_case;
        int best_priority = -999999;
        int current_priority = 0;

        if (from_case) {
            current_priority = (record.model == MODEL_AMBULANCE)
                                   ? compute_ambulance_priority(*from_case, player_pos)
                                   : compute_firetruck_priority(*from_case, player_pos, record.target_is_fire);
        }

        for (const auto& crime : crimes) {
            if (!crime || crime->cancelled) continue;
            if (from_case && crime->case_id == from_case->case_id) continue;

            CVector anchor = get_crime_dispatch_position(*crime);
            float dist = distance_3d(veh_pos, anchor);
            float av_range = dispatch_timing::get_av_range_for_crime(*crime);
            if (dist > av_range) continue;

            int candidate_priority = 0;
            CVector candidate_target = anchor;
            bool valid = false;

            if (record.model == MODEL_AMBULANCE) {
                if (crime->civilian_casualties_recorded <= 0) continue;
                candidate_priority = compute_ambulance_priority(*crime, player_pos);
                valid = true;
            } else if (record.model == MODEL_FIRETRUCK) {
                CVector fire_pos;
                if (!find_fire_near(anchor, dispatch_timing::FIRE_DETECT_RANGE_M, fire_pos)) continue;
                candidate_priority = compute_firetruck_priority(*crime, player_pos, true);
                candidate_target = fire_pos;
                valid = true;
            }

            if (!valid) continue;
            if (candidate_priority <= current_priority + dispatch_timing::EMS_REROUTE_PRIORITY_MARGIN) {
                continue;
            }
            if (candidate_priority > best_priority) {
                best_priority = candidate_priority;
                better_case = crime;
                record.target_anchor = candidate_target;
                record.target_is_fire = (record.model == MODEL_FIRETRUCK);
            }
        }

        CVector target = record.target_anchor;
        if (better_case && better_case != from_case) {
            LOGI("🚨 [EMSPriorityReroute] Vehicle %p case %llu (prio=%d) -> case %llu (prio=%d)",
                 record.vehicle,
                 from_case ? (unsigned long long)from_case->case_id : 0ULL,
                 current_priority,
                 (unsigned long long)better_case->case_id,
                 best_priority);
            transfer_emergency_vehicle_case(record.vehicle, from_case, better_case);
            record.case_id = better_case->case_id;
            target = record.target_anchor;
        } else if (from_case) {
            if (record.target_is_fire) {
                CVector fire_pos;
                if (find_fire_near(get_crime_dispatch_position(*from_case),
                                   dispatch_timing::FIRE_DETECT_RANGE_M, fire_pos)) {
                    target = fire_pos;
                } else {
                    target = get_crime_dispatch_position(*from_case);
                }
            } else {
                target = get_crime_dispatch_position(*from_case);
            }
            record.target_anchor = target;
        }

        record.last_reroute_ms = cur_time;
        command_emergency_vehicle_to_scene(record.vehicle, record.model, target);
    }
}

void unregister_emergency_vehicle(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_spawned_emergency_services_mutex);
    g_spawned_emergency_services.erase(
        std::remove_if(
            g_spawned_emergency_services.begin(),
            g_spawned_emergency_services.end(),
            [vehicle](const EmergencyServiceRecord& record) { return record.vehicle == vehicle; }),
        g_spawned_emergency_services.end());
}

void clear_case_emergency_records(uint64_t case_id) {
    std::lock_guard<std::mutex> lock(g_spawned_emergency_services_mutex);
    g_spawned_emergency_services.erase(
        std::remove_if(
            g_spawned_emergency_services.begin(),
            g_spawned_emergency_services.end(),
            [case_id](const EmergencyServiceRecord& record) { return record.case_id == case_id; }),
        g_spawned_emergency_services.end());
}

void clear_all_emergency_records() {
    std::lock_guard<std::mutex> lock(g_spawned_emergency_services_mutex);
    g_spawned_emergency_services.clear();
}

void emergency_services_tick(const std::vector<std::shared_ptr<CrimeEvent>>& crimes, int64_t cur_time) {
    evaluate_prioritized_emergency_needs(crimes, cur_time);
    evaluate_global_fire_dispatch(cur_time);

    for (auto it = g_pending_global_fire_spawns.begin(); it != g_pending_global_fire_spawns.end(); ) {
        if (cur_time >= it->execute_at) {
            finalize_spawn_task(nullptr, MODEL_FIRETRUCK, it->spawn_pos, it->fire_pos, true);
            it = g_pending_global_fire_spawns.erase(it);
        } else {
            ++it;
        }
    }

    refresh_enroute_emergency_vehicles(crimes, true);
}

} // namespace dispatch_emergency_services