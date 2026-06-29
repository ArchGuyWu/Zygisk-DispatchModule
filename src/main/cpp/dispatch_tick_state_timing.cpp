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


void dispatch_tick_state_timing(const std::shared_ptr<CrimeEvent>& crime) {
    float dist_to_player = 9999.0f;
    if (g_FindPlayerCoors) {
        CVector player_pos = g_FindPlayerCoors(0);
        float dx = player_pos.x - crime->location.x;
        float dy = player_pos.y - crime->location.y;
        float dz = player_pos.z - crime->location.z;
        dist_to_player = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    if (dist_to_player > 150.0f) {
        LOGI("Player too far from crime scene %llu (dist=%.1f) during timing, cancelling", (unsigned long long)crime->case_id, dist_to_player);
        crime->dispatch_state = STATE_CLEANUP;
        return;
    }

    float dist_to_cop = 9999.0f;
    if (g_FindDistToNearestCop) {
        dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, crime->location);
    }
    if (dist_to_cop < 80.0f) {
        LOGI("Natural police spawn detected for case %llu, cancelling dispatch", (unsigned long long)crime->case_id);
        crime->dispatch_sent = true;
        crime->dispatch_state = STATE_IDLE;
        return;
    }

    int64_t elapsed = now_ms() - crime->timer_start;

    // 计时等待期间周期性尝试调度附近可用警员（替代纯刷车增援）
    if (elapsed >= 1500 && elapsed < crime->dispatch_delay_ms) {
        int mobilized = dispatch_nearby_available_cops_for_crime_auto(crime);
        if (mobilized > 0) {
            LOGI("Nearby cop dispatch during timing for case %llu mobilized %d officers -> STATE_ON_SCENE",
                 (unsigned long long)crime->case_id, mobilized);
            crime->dispatch_sent = true;
            crime->spawn_time_ms = now_ms();
            crime->on_scene_start = now_ms();
            crime->dispatch_state = STATE_ON_SCENE;
            return;
        }
    }

    if (elapsed >= crime->dispatch_delay_ms) {
        int density = count_criminals_near(crime->location, 40.0f);
        LOGI("Dispatch timer expired. Target scene %llu (%.1f, %.1f, %.1f) has criminal density = %d",
             (unsigned long long)crime->case_id, crime->location.x, crime->location.y, crime->location.z, density);

        int mobilized = dispatch_nearby_available_cops_for_crime_auto(crime);
        if (mobilized > 0) {
            LOGI("Timer expired: nearby cop dispatch mobilized %d officers for case %llu (skipped emergency spawn)",
                 mobilized, (unsigned long long)crime->case_id);
            crime->dispatch_sent = true;
            crime->spawn_time_ms = now_ms();
            crime->on_scene_start = now_ms();
            crime->dispatch_state = STATE_ON_SCENE;
            return;
        }

        LOGI("Timer expired: no nearby cops for case %llu, falling back to emergency vehicle spawn",
             (unsigned long long)crime->case_id);

        CVector target_pos = get_spawn_target(crime->location);
        crime->dispatch_sent = true;
        crime->spawn_time_ms = now_ms();
        crime->on_scene_start = now_ms();
        crime->dispatch_state = STATE_ON_SCENE;

        bool swat_already = false;
        if (density >= 6) {
            swat_already = is_swat_van_nearby(crime->location, 150.0f);
            if (swat_already) {
                LOGI("SWAT density check for case %llu: SWAT vehicle already active nearby. Downgrading to 2 Police Cars.", (unsigned long long)crime->case_id);
            }
        }

        crime->pending_tasks.push_back({
            now_ms(),
            [density, swat_already, target_pos, crime]() {
                if (crime->cancelled) return;

                CPed* criminal = crime->criminal;
                CVector loc = crime->location;

                if (density >= 6 && !swat_already) {
                    LOGI("Heavy combat density (>=6) -> Dispatching 1 SWAT Enforcer + 1 Police Car for case %llu", (unsigned long long)crime->case_id);
                    dispatch_spawn_emergency_car(MODEL_SWAT_VAN, target_pos);

                    crime->pending_tasks.push_back({
                        now_ms() + 250,
                        [target_pos, loc, criminal, crime]() {
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
                                now_ms() + 250,
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
                        now_ms() + 250,
                        [target_pos, loc, criminal, crime]() {
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
                                now_ms() + 250,
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
                        now_ms() + 250,
                        [target_pos, loc, criminal, crime]() {
                            if (crime->cancelled) return;
                            void* veh = find_closest_vehicle_to(target_pos, 25.0f);
                            if (veh) {
                                crime->spawned_vehicle = veh;
                                crime->case_vehicles.push_back(veh);
                                command_cop_vehicle_to_scene(veh, loc);
                                setup_dispatched_cops(veh, criminal);
                            } else {
                                LOGW("⚠️ Failed to identify spawned vehicle near (%.1f, %.1f, %.1f) for case %llu", target_pos.x, target_pos.y, target_pos.z, (unsigned long long)crime->case_id);
                                // Fallback 清理层：如果当前没有任何活跃案件存在，则统一恢复/释放全局所有的向下兼容级 and 未分组表映射
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
}