#include <jni.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <vector>
#include <map>
#include <unordered_map>
#include <fstream>
#include <string>
#include <cinttypes>
#include <set>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <random>
#include <functional>
#include <cmath>
#include <algorithm>
#include <cstdio>

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
#include "ecs_engine.hpp"
#include "dispatch_tick_internal.hpp"
#include "dispatch_timing.hpp"
#include "dispatch_police_spawn.hpp"
#include "dispatch_heli_support.hpp"
#include "dispatch_emergency_services.hpp"


void dispatch_tick_state_on_scene(const std::shared_ptr<CrimeEvent>& crime) {
    int64_t cur_time = now_ms();
    bool dispatch_now = false;

    float dist_to_cop = 9999.0f;
    if (g_FindDistToNearestCop) {
        dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, get_crime_dispatch_position(*crime));
    }
    if (dispatch_timing::is_cop_within_native_av(dist_to_cop, *crime)) {
        dispatch_now = true;
    } else if (crime->last_on_scene_dispatch_ms <= 0 ||
               (cur_time - crime->last_on_scene_dispatch_ms) >= dispatch_timing::ON_SCENE_DISPATCH_INTERVAL_MS) {
        dispatch_now = true;
    }

    if (g_orig_tell_occupants_leave_car) {
        for (void* veh : crime->case_vehicles) {
            if (veh && is_vehicle_pointer_valid(veh) && !is_vehicle_emptied(veh)) {
                if (get_entity_model_index(veh) == MODEL_POLICE_HELI) continue;
                CVector veh_pos = get_entity_pos(veh);
                CVector crime_pos = get_crime_dispatch_position(*crime);
                float dx = veh_pos.x - crime_pos.x;
                float dy = veh_pos.y - crime_pos.y;
                float dz = veh_pos.z - crime_pos.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                int64_t elapsed = now_ms() - crime->spawn_time_ms;

                if (dist < 32.0f || elapsed > 15000) {
                    LOGI("Ordering occupants of vehicle %p to leave car (dist=%.1f, elapsed=%lld ms) for case %llu (Bulk Guard)",
                         veh, dist, (long long)elapsed, (unsigned long long)crime->case_id);
                    dispatch_tell_occupants_to_leave_car(veh);
                    add_vehicle_emptied(veh);
                }
            }
        }
        if (crime->spawned_vehicle) {
            if (!is_vehicle_pointer_valid(crime->spawned_vehicle)) {
                crime->spawned_vehicle = nullptr;
            } else if (is_vehicle_emptied(crime->spawned_vehicle)) {
                crime->occupants_ordered_out = true;
            }
        }
    }

    if (dispatch_now) {
        make_cops_attack_criminal(crime->criminal);
        crime->last_on_scene_dispatch_ms = cur_time;
    }

    float dist_to_player = 9999.0f;
    if (g_FindPlayerCoors) {
        CVector player_pos = g_FindPlayerCoors(0);
        CVector dispatch_pos = get_crime_dispatch_position(*crime);
        float dx = player_pos.x - dispatch_pos.x;
        float dy = player_pos.y - dispatch_pos.y;
        float dz = player_pos.z - dispatch_pos.z;
        dist_to_player = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    if (dist_to_player > 150.0f) {
        LOGI("Player too far from crime scene %llu (dist=%.1f) on scene, cancelling", (unsigned long long)crime->case_id, dist_to_player);
        crime->dispatch_state = STATE_CLEANUP;
        return;
    }

    if (crime->cops_killed > crime->last_cops_killed) {
        int new_deaths = crime->cops_killed - crime->last_cops_killed;
        crime->last_cops_killed = crime->cops_killed;

        for (int i = 0; i < new_deaths; i++) {
            if (crime->reinforcements_sent < 3) {
                crime->reinforcements_sent++;
                int r = crime->reinforcements_sent;

                int spawn_delay = 0;
                if (new_deaths >= 2) {
                    spawn_delay = get_random_range(
                        dispatch_timing::REINFORCE_SPAWN_HEAVY_MIN_MS,
                        dispatch_timing::REINFORCE_SPAWN_HEAVY_MAX_MS);
                    LOGI("⚠️ Heavy casualties (%d dead) case %llu: nearby@%lldms spawn@%dms",
                         new_deaths, (unsigned long long)crime->case_id,
                         (long long)dispatch_timing::REINFORCE_NEARBY_ATTEMPT_MS, spawn_delay);
                } else {
                    switch (r) {
                        case 1:
                            spawn_delay = get_random_range(
                                dispatch_timing::REINFORCE_SPAWN_LIGHT_MIN_MS,
                                dispatch_timing::REINFORCE_SPAWN_LIGHT_MAX_MS);
                            break;
                        case 2:
                            spawn_delay = get_random_range(
                                dispatch_timing::REINFORCE_SPAWN_MEDIUM_MIN_MS,
                                dispatch_timing::REINFORCE_SPAWN_MEDIUM_MAX_MS);
                            break;
                        default:
                            spawn_delay = get_random_range(
                                dispatch_timing::REINFORCE_SPAWN_LIGHT_MIN_MS,
                                dispatch_timing::REINFORCE_SPAWN_LIGHT_MAX_MS);
                            break;
                    }
                }

                LOGI("Cop casualty -> reinforcement #%d for case %llu (nearby@%lldms, spawn_fallback@%dms)",
                     r, (unsigned long long)crime->case_id,
                     (long long)dispatch_timing::REINFORCE_NEARBY_ATTEMPT_MS, spawn_delay);

                crime->pending_tasks.push_back({
                    now_ms() + dispatch_timing::REINFORCE_NEARBY_ATTEMPT_MS,
                    [r, crime]() {
                        if (crime->cancelled) return;
                        int mobilized = dispatch_nearby_available_cops_for_crime_auto(crime);
                        if (mobilized > 0) {
                            LOGI("Reinforcement #%d: mobilized %d nearby cops for case %llu (skipped vehicle spawn)",
                                 r, mobilized, (unsigned long long)crime->case_id);
                        }
                    }
                });

                crime->pending_tasks.push_back({
                    now_ms() + spawn_delay,
                    [r, crime]() {
                        if (crime->cancelled) {
                            LOGI("Reinforcement #%d cancelled because crime event is no longer active for case %llu", r, (unsigned long long)crime->case_id);
                            return;
                        }

                        int mobilized = dispatch_nearby_available_cops_for_crime_auto(crime);
                        if (mobilized > 0) {
                            LOGI("Reinforcement #%d: late nearby mobilized %d for case %llu (skipped spawn)",
                                 r, mobilized, (unsigned long long)crime->case_id);
                            return;
                        }

                        CVector loc = get_crime_dispatch_position(*crime);
                        CVector target_pos = dispatch_emergency_services::clamp_spawn_to_streaming_range(
                            loc, get_spawn_target(loc));
                        int density = count_criminals_near(loc, 40.0f);
                        bool swat_already = is_swat_van_nearby(loc, 150.0f);
                        auto plan = dispatch_police_spawn::build_reinforcement_spawn_plan(
                            *crime, r, density, swat_already);
                        char reason_buf[48];
                        snprintf(reason_buf, sizeof(reason_buf), "Reinforcement#%d", r);
                        dispatch_police_spawn::schedule_police_vehicle_spawns(
                            crime, target_pos, loc, crime->criminal, plan, reason_buf);
                    }
                });
            }
        }
    }

    dispatch_heli_support::evaluate_case_heli_support(crime, cur_time);
    dispatch_heli_support::refresh_active_helis(crime);

    int64_t cur_scene_time = now_ms();
    if (crime->vehicle_spawn_pending && crime->case_vehicles.empty()) {
        int64_t pending_elapsed = cur_scene_time - crime->spawn_time_ms;
        if (pending_elapsed > dispatch_timing::SPAWN_PENDING_TIMEOUT_MS) {
            LOGI("Spawn pending timeout for case %llu after %lldms with no identified vehicles",
                 (unsigned long long)crime->case_id, (long long)pending_elapsed);
            crime->dispatch_state = STATE_CLEANUP;
        }
        return;
    }

    if (crime->on_scene_start <= 0) {
        return;
    }

    int64_t scene_elapsed = cur_scene_time - crime->on_scene_start;
    int64_t scene_timeout_ms = crime->vehicle_spawn_pending
                                   ? dispatch_timing::SPAWN_ARRIVED_SCENE_TIMEOUT_MS
                                   : 30000;
    if (scene_elapsed > scene_timeout_ms) {
        LOGI("Scene timeout reached for case %llu after %lldms",
             (unsigned long long)crime->case_id, (long long)scene_elapsed);
        crime->dispatch_state = STATE_CLEANUP;
    }
}