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
#include "dispatch_threat.hpp"

static bool try_mobilize_nearby_cops(const std::shared_ptr<CrimeEvent>& crime, int64_t cur_time, const char* reason) {
    if (!dispatch_timing::should_attempt_nearby_dispatch(*crime, cur_time)) {
        return false;
    }

    dispatch_timing::record_nearby_dispatch_attempt(*crime, cur_time);
    int mobilized = dispatch_nearby_available_cops_for_crime_auto(crime);
    if (mobilized > 0) {
        LOGI("🚓 [%s] Case %llu mobilized %d nearby officers (elapsed=%lldms)",
             reason, (unsigned long long)crime->case_id, mobilized,
             (long long)dispatch_timing::elapsed_since_dispatch_timer(*crime, cur_time));
        crime->dispatch_sent = true;
        crime->spawn_time_ms = cur_time;
        crime->on_scene_start = cur_time;
        crime->dispatch_state = STATE_ON_SCENE;
        return true;
    }
    return false;
}

static void schedule_emergency_vehicle_spawn(
    const std::shared_ptr<CrimeEvent>& crime,
    int density,
    bool swat_already,
    CVector target_pos) {
    crime->pending_tasks.push_back({
        now_ms(),
        [density, swat_already, target_pos, crime]() {
            if (crime->cancelled) return;

            CPed* criminal = crime->criminal;
            CVector loc = get_crime_dispatch_position(*crime);
            int64_t identify_delay = dispatch_timing::VEHICLE_IDENTIFY_DELAY_MS;
            int64_t stagger = dispatch_timing::VEHICLE_IDENTIFY_STAGGER_MS;

            if (density >= 6 && !swat_already) {
                LOGI("Heavy combat density (>=6) -> Dispatching 1 SWAT Enforcer + 1 Police Car for case %llu", (unsigned long long)crime->case_id);
                dispatch_spawn_emergency_car(MODEL_SWAT_VAN, target_pos);

                crime->pending_tasks.push_back({
                    now_ms() + identify_delay,
                    [target_pos, loc, criminal, stagger, crime]() {
                        if (crime->cancelled) return;
                        void* veh1 = find_closest_vehicle_to(target_pos, 25.0f);
                        if (veh1) {
                            crime->spawned_vehicle = veh1;
                            crime->case_vehicles.push_back(veh1);
                            register_spawned_swat(veh1);
                            command_cop_vehicle_to_scene(veh1, loc);
                            setup_dispatched_cops(veh1, criminal);
                        }

                        dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                        crime->pending_tasks.push_back({
                            now_ms() + stagger,
                            [target_pos, veh1, loc, criminal, crime]() {
                                if (crime->cancelled) return;
                                void* veh2 = find_closest_vehicle_to(target_pos, 25.0f, veh1);
                                if (veh2) {
                                    crime->case_vehicles.push_back(veh2);
                                    command_cop_vehicle_to_scene(veh2, loc);
                                    setup_dispatched_cops(veh2, criminal);
                                }
                            }
                        });
                    }
                });
            }
            else if (density >= 3 || (density >= 6 && swat_already)) {
                LOGI("Medium combat density -> Dispatching 2 Police Cars for case %llu", (unsigned long long)crime->case_id);
                dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                crime->pending_tasks.push_back({
                    now_ms() + identify_delay,
                    [target_pos, loc, criminal, stagger, crime]() {
                        if (crime->cancelled) return;
                        void* veh1 = find_closest_vehicle_to(target_pos, 25.0f);
                        if (veh1) {
                            crime->spawned_vehicle = veh1;
                            crime->case_vehicles.push_back(veh1);
                            command_cop_vehicle_to_scene(veh1, loc);
                            setup_dispatched_cops(veh1, criminal);
                        }

                        dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                        crime->pending_tasks.push_back({
                            now_ms() + stagger,
                            [target_pos, veh1, loc, criminal, crime]() {
                                if (crime->cancelled) return;
                                void* veh2 = find_closest_vehicle_to(target_pos, 25.0f, veh1);
                                if (veh2) {
                                    crime->case_vehicles.push_back(veh2);
                                    command_cop_vehicle_to_scene(veh2, loc);
                                    setup_dispatched_cops(veh2, criminal);
                                }
                            }
                        });
                    }
                });
            }
            else {
                LOGI("Light combat density -> Dispatching 1 Police Car for case %llu", (unsigned long long)crime->case_id);
                dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                crime->pending_tasks.push_back({
                    now_ms() + identify_delay,
                    [target_pos, loc, criminal, crime]() {
                        if (crime->cancelled) return;
                        void* veh = find_closest_vehicle_to(target_pos, 25.0f);
                        if (veh) {
                            crime->spawned_vehicle = veh;
                            crime->case_vehicles.push_back(veh);
                            command_cop_vehicle_to_scene(veh, loc);
                            setup_dispatched_cops(veh, criminal);
                        } else {
                            LOGW("⚠️ Failed to identify spawned vehicle near (%.1f, %.1f, %.1f) for case %llu",
                                 target_pos.x, target_pos.y, target_pos.z, (unsigned long long)crime->case_id);
                            if (g_active_crimes.empty()) {
                                g_tracked_criminal.store(nullptr);
                                ecs::EntityManager::get().clear();
                            }
                        }
                    }
                });
            }
        }
    });
}

void dispatch_tick_state_timing(const std::shared_ptr<CrimeEvent>& crime) {
    int64_t cur_time = now_ms();

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
        LOGI("Player too far from crime scene %llu (dist=%.1f) during timing, cancelling", (unsigned long long)crime->case_id, dist_to_player);
        crime->dispatch_state = STATE_CLEANUP;
        return;
    }

    float dist_to_cop = 9999.0f;
    if (g_FindDistToNearestCop) {
        dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, get_crime_dispatch_position(*crime));
    }
    if (dispatch_timing::is_cop_within_native_av(dist_to_cop, *crime)) {
        LOGI("Cop entered native AV for case %llu (nearest=%.1fm < %.0fm) -> STATE_ON_SCENE",
             (unsigned long long)crime->case_id, dist_to_cop,
             dispatch_timing::get_av_range_for_crime(*crime));
        crime->dispatch_sent = true;
        crime->on_scene_start = cur_time;
        crime->dispatch_state = STATE_ON_SCENE;
        make_cops_attack_criminal_immediate(crime->criminal);
        return;
    }

    int64_t elapsed = dispatch_timing::elapsed_since_dispatch_timer(*crime, cur_time);

    if (elapsed < crime->dispatch_delay_ms) {
        if (try_mobilize_nearby_cops(crime, cur_time, "NearbyRetry")) {
            return;
        }
        return;
    }

    // 兜底计时到期：最后一次附近调度，失败则刷增援车
    dispatch_timing::record_nearby_dispatch_attempt(*crime, cur_time);
    int mobilized = dispatch_nearby_available_cops_for_crime_auto(crime);
    if (mobilized > 0) {
        LOGI("Timer expired: nearby cop dispatch mobilized %d officers for case %llu (skipped emergency spawn)",
             mobilized, (unsigned long long)crime->case_id);
        crime->dispatch_sent = true;
        crime->spawn_time_ms = cur_time;
        crime->on_scene_start = cur_time;
        crime->dispatch_state = STATE_ON_SCENE;
        return;
    }

    LOGI("Timer expired: no nearby cops for case %llu after %lldms, falling back to emergency vehicle spawn",
         (unsigned long long)crime->case_id, (long long)elapsed);

    CVector dispatch_pos = get_crime_dispatch_position(*crime);
    int density = std::max(
        count_criminals_near(dispatch_pos, 40.0f),
        dispatch_threat::count_active_threats(*crime));
    CVector target_pos = get_spawn_target(dispatch_pos);
    crime->dispatch_sent = true;
    crime->spawn_time_ms = cur_time;
    crime->on_scene_start = cur_time;
    crime->dispatch_state = STATE_ON_SCENE;

    bool swat_already = false;
    if (density >= 6) {
        swat_already = is_swat_van_nearby(dispatch_pos, 150.0f);
        if (swat_already) {
            LOGI("SWAT density check for case %llu: SWAT vehicle already active nearby. Downgrading to 2 Police Cars.", (unsigned long long)crime->case_id);
        }
    }

    schedule_emergency_vehicle_spawn(crime, density, swat_already, target_pos);
}