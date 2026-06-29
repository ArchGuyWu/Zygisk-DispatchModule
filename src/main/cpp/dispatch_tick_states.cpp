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


void dispatch_tick_process_crime(
    const std::shared_ptr<CrimeEvent>& crime,
    int64_t cur_time,
    bool do_siren_refresh) {
        // 刷新各个案件独立拥有的警车驾驶路径与警笛
        if (do_siren_refresh) {
            std::lock_guard<std::mutex> lock_sc(g_vehicles_mutex);
            for (void* veh : crime->case_vehicles) {
                if (is_vehicle_pointer_valid(veh) && !is_vehicle_emptied(veh)) {
                    command_cop_vehicle_to_scene(veh, crime->location);
                }
            }
        }

        // 检查活动犯罪分子是否依然有效 (并案机制：遍历并清理已失效或已死亡的犯罪NPC)
        {
            auto& list = crime->consolidated_criminals;
            auto& is_fire_list = crime->criminal_is_firearm;
            
            // 1. 清理列表中所有已失效 (despawned) 的 NPC
            for (auto it = list.begin(); it != list.end(); ) {
                if (!*it || !is_ped_pointer_valid_safe(*it)) {
                    size_t idx = std::distance(list.begin(), it);
                    LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Consolidated criminal NPC %p is no longer valid (despawned) -> removing from case", 
                         (unsigned long long)crime->case_id, *it);
                    it = list.erase(it);
                    if (idx < is_fire_list.size()) {
                        is_fire_list.erase(is_fire_list.begin() + idx);
                    }
                } else {
                    ++it;
                }
            }
            
            // 2. 如果当前 primary criminal 已经不合法，从并案列表中转移主犯
            if (crime->criminal && !is_ped_pointer_valid_safe(crime->criminal)) {
                if (!list.empty()) {
                    crime->criminal = list.front();
                    if (crime == get_primary_active_crime()) {
                        g_tracked_criminal.store(list.front());
                    }
                    LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Primary criminal was despawned. Shifted primary tracking to %p.", 
                         (unsigned long long)crime->case_id, crime->criminal);
                } else {
                    LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Active criminal NPC is no longer valid (despawned) and no other consolidated criminals -> cancelling crime event", 
                         (unsigned long long)crime->case_id);
                    crime->cancelled = true;
                }
            } else if (list.empty()) {
                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: All consolidated criminals are invalid/despawned -> cancelling crime event", 
                     (unsigned long long)crime->case_id);
                crime->cancelled = true;
            }
        }

        // 轮询并执行过期的异步挂起任务 (串行，线程安全，并在回调中避免迭代器失效)
        if (!crime->cancelled) {
            int64_t now = now_ms();
            std::vector<CrimeEvent::DelayedTask> tasks_to_execute;

            for (auto it = crime->pending_tasks.begin(); it != crime->pending_tasks.end(); ) {
                if (now >= it->execute_time_ms) {
                    tasks_to_execute.push_back(*it);
                    it = crime->pending_tasks.erase(it);
                } else {
                    ++it;
                }
            }

            for (const auto& task : tasks_to_execute) {
                if (!crime->cancelled) {
                    task.callback();
                }
            }
        }

        // 对每一个案件独立运行其调度状态机
        if (!crime->cancelled) {
            switch (crime->dispatch_state) {
                case STATE_IDLE: {
                    // 🚧 [动态警戒禁行区]：自动为活跃犯罪现场设置 50 米的路障/封路禁行，阻挡平民 NPC 车辆冲入现场
                    if (!crime->road_closure_active) {
                        if (g_ThePaths && g_SwitchRoadsOffInArea) {
                            CVector center = crime->location;
                            g_SwitchRoadsOffInArea(
                                g_ThePaths,
                                center.x - 50.0f, center.y - 50.0f, center.z - 15.0f,
                                center.x + 50.0f, center.y + 50.0f, center.z + 15.0f,
                                true, true, false
                            );
                            crime->road_closure_active = true;
                            crime->road_closure_center = center;
                            LOGI("🚧 [dispatchCenter - Cordon] Established dynamic police road closure within 50m around (%.1f, %.1f, %.1f) for case %llu", 
                                 center.x, center.y, center.z, (unsigned long long)crime->case_id);
                        }
                    }

                    if (!crime->dispatch_sent) {
                        float dist_to_cop = 9999.0f;
                        if (g_FindDistToNearestCop) {
                            dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, crime->location);
                        }

                        if (dist_to_cop < 50.0f) {
                            LOGI("Cops nearby (dist=%.1f) for case %llu, transition to STATE_ON_SCENE directly", dist_to_cop, (unsigned long long)crime->case_id);
                            crime->dispatch_sent = true;
                            crime->on_scene_start = now_ms();
                            crime->dispatch_state = STATE_ON_SCENE;
                            crime->last_cops_killed = 0;
                        } else {
                            // 缩短调度计时器以让警车快速抵达：枪械犯罪 4~7 秒，近战/非枪械犯罪 8~12 秒，防止战斗提前完结导致警察“不响应”
                            crime->dispatch_delay_ms = crime->is_firearm ? 
                                get_random_range(4000, 7000) : get_random_range(8000, 12000);
                            crime->timer_start = now_ms();
                            crime->dispatch_state = STATE_TIMING;
                            crime->last_cops_killed = 0;
                            LOGI("No cops nearby for case %llu, starting dispatch timer: %d ms", (unsigned long long)crime->case_id, crime->dispatch_delay_ms);
                        }
                    }
                    break;
                }

                case STATE_TIMING: {
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
                        break;
                    }

                    float dist_to_cop = 9999.0f;
                    if (g_FindDistToNearestCop) {
                        dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, crime->location);
                    }
                    if (dist_to_cop < 80.0f) {
                        LOGI("Natural police spawn detected for case %llu, cancelling dispatch", (unsigned long long)crime->case_id);
                        crime->dispatch_sent = true;
                        crime->dispatch_state = STATE_IDLE;
                        break;
                    }

                    int64_t elapsed = now_ms() - crime->timer_start;
                    if (elapsed >= crime->dispatch_delay_ms) {
                        int density = count_criminals_near(crime->location, 40.0f);
                        LOGI("Dispatch timer expired. Target scene %llu (%.1f, %.1f, %.1f) has criminal density = %d",
                             (unsigned long long)crime->case_id, crime->location.x, crime->location.y, crime->location.z, density);

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
                    break;
                }

                case STATE_ON_SCENE: {
                    make_cops_attack_criminal(crime->criminal);

                    if (g_TellOccupantsToLeaveCar) {
                        for (void* veh : crime->case_vehicles) {
                            if (veh && is_vehicle_pointer_valid(veh) && !is_vehicle_emptied(veh)) {
                                CVector veh_pos = get_entity_pos(veh);
                                CVector crime_pos = crime->location;
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
                                    g_TellOccupantsToLeaveCar(veh);
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
                        float dx = player_pos.x - crime->location.x;
                        float dy = player_pos.y - crime->location.y;
                        float dz = player_pos.z - crime->location.z;
                        dist_to_player = sqrtf(dx * dx + dy * dy + dz * dz);
                    }
                    if (dist_to_player > 150.0f) {
                        LOGI("Player too far from crime scene %llu (dist=%.1f) on scene, cancelling", (unsigned long long)crime->case_id, dist_to_player);
                        crime->dispatch_state = STATE_CLEANUP;
                        break;
                    }

                    if (crime->cops_killed > crime->last_cops_killed) {
                        int new_deaths = crime->cops_killed - crime->last_cops_killed;
                        crime->last_cops_killed = crime->cops_killed;

                        for (int i = 0; i < new_deaths; i++) {
                            if (crime->reinforcements_sent < 3) {
                                crime->reinforcements_sent++;
                                int r = crime->reinforcements_sent;

                                int delay = 10000;
                                if (new_deaths >= 2) {
                                    delay = get_random_range(2500, 3500);
                                    LOGI("⚠️ Heavy casualties detected (%d dead) for case %llu! Activating emergency reinforcement delay: %d ms", new_deaths, (unsigned long long)crime->case_id, delay);
                                } else {
                                    switch (r) {
                                        case 1: delay = get_random_range(8500, 11500); break;
                                        case 2: delay = get_random_range(7000, 9000); break;
                                        case 3: delay = get_random_range(8500, 11500); break;
                                        default: delay = get_random_range(8500, 11500); break;
                                    }
                                }

                                LOGI("Cop casualty -> reinforcement #%d scheduled in %d ms for case %llu", r, delay, (unsigned long long)crime->case_id);

                                crime->pending_tasks.push_back({
                                    now_ms() + delay,
                                    [r, crime]() {
                                        if (crime->cancelled) {
                                            LOGI("Reinforcement #%d cancelled because crime event is no longer active for case %llu", r, (unsigned long long)crime->case_id);
                                            return;
                                        }

                                        CPed* criminal = crime->criminal;
                                        CVector loc = crime->location;
                                        CVector target_pos = get_spawn_target(loc);

                                        int density = count_criminals_near(loc, 40.0f);
                                        bool swat_already = is_swat_van_nearby(loc, 150.0f);

                                        if (r == 3 && density >= 5 && !swat_already) {
                                            LOGI("Reinforcement #%d (Heavy SWAT) for case %llu: Dispatching SWAT Enforcer. (density=%d)", r, (unsigned long long)crime->case_id, density);
                                            dispatch_spawn_emergency_car(MODEL_SWAT_VAN, target_pos);

                                            crime->pending_tasks.push_back({
                                                now_ms() + 250,
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
                                                now_ms() + 250,
                                                [target_pos, loc, criminal, r, crime]() {
                                                    if (crime->cancelled) return;
                                                    void* veh1 = find_closest_vehicle_to(target_pos, 25.0f);
                                                    if (veh1) {
                                                        crime->case_vehicles.push_back(veh1);
                                                        command_cop_vehicle_to_scene(veh1, loc);
                                                        setup_dispatched_cops(veh1, criminal);
                                                    }

                                                    dispatch_spawn_emergency_car(MODEL_POLICE_CAR, target_pos);

                                                    crime->pending_tasks.push_back({
                                                        now_ms() + 250,
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
                                                now_ms() + 250,
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
                    break;
                }

                case STATE_CLEANUP: {
                    LOGI("Cleaning up crime event for case %llu", (unsigned long long)crime->case_id);
                    cleanup_single_case_vehicles(crime);
                    crime->cancelled = true;
                    break;
                }

                default:
                    break;
            }
        }

}
