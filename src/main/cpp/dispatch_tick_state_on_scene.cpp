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


void dispatch_tick_state_on_scene(const std::shared_ptr<CrimeEvent>& crime) {
    make_cops_attack_criminal(crime->criminal);

    if (g_orig_tell_occupants_leave_car) {
        for (void* veh : crime->case_vehicles) {
            if (veh && is_vehicle_pointer_valid(veh) && !is_vehicle_emptied(veh)) {
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
                    if (g_GetCarToGoToCoors) {
                        g_GetCarToGoToCoors(veh, &veh_pos, 0, false);
                    }
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

                        CPed* criminal = crime->criminal;
                        CVector loc = get_crime_dispatch_position(*crime);
                        CVector target_pos = get_spawn_target(loc);

                        int density = count_criminals_near(loc, 40.0f);
                        bool swat_already = is_swat_van_nearby(loc, 150.0f);
                        int64_t identify_delay = dispatch_timing::VEHICLE_IDENTIFY_DELAY_MS;
                        int64_t stagger = dispatch_timing::VEHICLE_IDENTIFY_STAGGER_MS;

                        if (r == 3 && density >= 5 && !swat_already) {
                            LOGI("Reinforcement #%d (Heavy SWAT) for case %llu: Dispatching SWAT Enforcer. (density=%d)", r, (unsigned long long)crime->case_id, density);
                            dispatch_spawn_emergency_car(MODEL_SWAT_VAN, target_pos);

                            crime->pending_tasks.push_back({
                                now_ms() + identify_delay,
                                [target_pos, loc, criminal, r, crime]() {
                                    if (crime->cancelled) return;
                                    void* veh = find_closest_vehicle_to(target_pos, 25.0f);
                                    if (veh) {
                                        crime->case_vehicles.push_back(veh);
                                        register_spawned_swat(veh);
                                        command_cop_vehicle_to_scene(veh, loc);
                                        setup_dispatched_cops(veh, criminal);
                                        LOGI("✅ Reinforcement #%d: SWAT configured and driving to scene for case %llu", r, (unsigned long long)crime->case_id);
                                    }
                                }
                            });
                        }
                        else if (density >= 3) {
                            LOGI("Reinforcement #%d (Medium) for case %llu: Deploying 2 Police Cars. (density=%d)", r, (unsigned long long)crime->case_id, density);
                            dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                            crime->pending_tasks.push_back({
                                now_ms() + identify_delay,
                                [target_pos, loc, criminal, r, stagger, crime]() {
                                    if (crime->cancelled) return;
                                    void* veh1 = find_closest_vehicle_to(target_pos, 25.0f);
                                    if (veh1) {
                                        crime->case_vehicles.push_back(veh1);
                                        command_cop_vehicle_to_scene(veh1, loc);
                                        setup_dispatched_cops(veh1, criminal);
                                    }

                                    dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                                    crime->pending_tasks.push_back({
                                        now_ms() + stagger,
                                        [target_pos, veh1, loc, criminal, r, crime]() {
                                            if (crime->cancelled) return;
                                            void* veh2 = find_closest_vehicle_to(target_pos, 25.0f, veh1);
                                            if (veh2) {
                                                crime->case_vehicles.push_back(veh2);
                                                command_cop_vehicle_to_scene(veh2, loc);
                                                setup_dispatched_cops(veh2, criminal);
                                                LOGI("✅ Reinforcement #%d: 2 Police Cars configured and driving to scene for case %llu", r, (unsigned long long)crime->case_id);
                                            }
                                        }
                                    });
                                }
                            });
                        }
                        else {
                            LOGI("Reinforcement #%d (Light) for case %llu: Deploying 1 Police Car. (density=%d)", r, (unsigned long long)crime->case_id, density);
                            dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                            crime->pending_tasks.push_back({
                                now_ms() + identify_delay,
                                [target_pos, loc, criminal, r, crime]() {
                                    if (crime->cancelled) return;
                                    void* veh = find_closest_vehicle_to(target_pos, 25.0f);
                                    if (veh) {
                                        crime->case_vehicles.push_back(veh);
                                        command_cop_vehicle_to_scene(veh, loc);
                                        setup_dispatched_cops(veh, criminal);
                                        LOGI("✅ Reinforcement #%d: 1 Police Car configured and driving to scene for case %llu", r, (unsigned long long)crime->case_id);
                                    }
                                }
                            });
                        }
                    }
                });
            }
        }
    }

    int64_t scene_elapsed = now_ms() - crime->on_scene_start;
    if (scene_elapsed > 30000) {
        LOGI("Scene timeout reached for case %llu", (unsigned long long)crime->case_id);
        crime->dispatch_state = STATE_CLEANUP;
    }
}