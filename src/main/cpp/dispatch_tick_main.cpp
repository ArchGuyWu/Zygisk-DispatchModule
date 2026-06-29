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


// =====================================================================
// Main dispatch tick (state machine + spawn orchestration)
// =====================================================================
static int64_t g_last_tick_time_ms = 0;
// =====================================================================
// Custom dispatch vehicle spawn wrapper
// =====================================================================
static void dispatch_spawn_emergency_car(unsigned int model, CVector pos) {
    g_is_generating_custom_dispatch.store(true);
    if (g_ScriptGenEmergencyCar) {
        g_ScriptGenEmergencyCar(model, pos);
    } else if (g_GenOneEmergencyCar) {
        g_GenOneEmergencyCar(model, pos);
    }
    g_is_generating_custom_dispatch.store(false);
}

// =====================================================================
// Civilian vehicle avoidance near emergency units
// =====================================================================
static bool is_emergency_vehicle_model(unsigned int model) {
    return (model == 416 || model == 407 || 
            model == 596 || model == 597 || model == 598 || model == 599 || 
            model == 601 || model == 528 || model == 490 || model == 523);
}

// 🚗💨 平民车辆避让时的惊慌按喇叭与急刹车冷却定时器
std::unordered_map<void*, int64_t> g_civilian_panic_timers;
static int64_t g_last_cleanup_panic_timers = 0;

static bool is_vehicle_driven_by_player(void* veh) {
    if (!veh || !g_FindPlayerPed || !g_IsDriver) return false;
    void* player = g_FindPlayerPed(0);
    if (!player) return false;
    return g_IsDriver(veh, reinterpret_cast<const CPed*>(player));
}

static void apply_civilian_avoidance_field() {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle || !g_GetMatrix) return;

    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool) return;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return;

    int64_t now = now_ms();

    // 垃圾回收：每 10 秒清理一次已经失效的车辆指针，防止 Map 无限膨胀
    if (now - g_last_cleanup_panic_timers > 10000) {
        g_last_cleanup_panic_timers = now;
        for (auto it = g_civilian_panic_timers.begin(); it != g_civilian_panic_timers.end(); ) {
            if (!is_vehicle_pointer_valid(it->first)) {
                it = g_civilian_panic_timers.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 1. 搜集所有处于活跃行驶状态（有司机）的调度/应急车辆
    struct ActiveEmergencyVeh {
        void* veh;
        CVector pos;
        CVector dir; // 前向向量 (Up Vector)
        CVector right; // 右向向量 (Right Vector)
    };
    std::vector<ActiveEmergencyVeh> active_evs;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            void* veh = g_GetPoolVehicle(handle);
            if (veh && is_vehicle_pointer_valid(veh)) {
                unsigned int model = get_entity_model_index(veh);
                if (is_emergency_vehicle_model(model)) {
                    if (is_vehicle_occupied_by_driver(veh)) {
                        CVector ev_pos = get_entity_pos(veh);
                        CVector ev_dir = {0.0f, 1.0f, 0.0f};
                        CVector ev_right = {1.0f, 0.0f, 0.0f};
                        CMatrix* mat = g_GetMatrix(veh);
                        if (mat) {
                            ev_dir = {mat->up_x, mat->up_y, mat->up_z};
                            ev_right = {mat->right_x, mat->right_y, mat->right_z};
                        }
                        float len_d = sqrtf(ev_dir.x * ev_dir.x + ev_dir.y * ev_dir.y + ev_dir.z * ev_dir.z);
                        if (len_d > 0.01f) {
                            ev_dir.x /= len_d; ev_dir.y /= len_d; ev_dir.z /= len_d;
                        } else {
                            ev_dir = {0.0f, 1.0f, 0.0f};
                        }
                        float len_r = sqrtf(ev_right.x * ev_right.x + ev_right.y * ev_right.y + ev_right.z * ev_right.z);
                        if (len_r > 0.01f) {
                            ev_right.x /= len_r; ev_right.y /= len_r; ev_right.z /= len_r;
                        } else {
                            ev_right = {1.0f, 0.0f, 0.0f};
                        }
                        active_evs.push_back({veh, ev_pos, ev_dir, ev_right});
                    }
                }
            }
        }
    }

    if (active_evs.empty()) return;

    // 2. 遍历所有非应急、且有司机的平民车辆，施加“恐惧/避让”电磁场
    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            void* veh = g_GetPoolVehicle(handle);
            if (veh && is_vehicle_pointer_valid(veh)) {
                
                // 玩家驾驶的载具本身绝不参与避让
                if (is_vehicle_driven_by_player(veh)) continue;

                unsigned int model = get_entity_model_index(veh);
                if (!is_emergency_vehicle_model(model) && is_vehicle_occupied_by_driver(veh)) {
                    CVector civ_pos = get_entity_pos(veh);
                    
                    // 寻找最近且可能产生阻碍的应急车辆
                    for (const auto& ev : active_evs) {
                        if (ev.veh == veh) continue;

                        float dx = civ_pos.x - ev.pos.x;
                        float dy = civ_pos.y - ev.pos.y;
                        float dz = civ_pos.z - ev.pos.z;
                        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

                        // 恐惧场作用范围：前方 22 米，横向 4.5 米
                        if (dist < 22.0f && dist > 0.1f) {
                            // 投影到应急车的前向向量 and 右向向量
                            float dot_forward = dx * ev.dir.x + dy * ev.dir.y + dz * ev.dir.z;
                            float dot_right = dx * ev.right.x + dy * ev.right.y + dz * ev.right.z;

                            // 如果平民车处于应急车的前方，且横向距离较近
                            if (dot_forward > 0.0f && dot_forward < 22.0f && fabsf(dot_right) < 4.5f) {
                                float avoid_sign = (dot_right >= 0.0f) ? 1.0f : -1.0f;
                                
                                // 平民车越近，避让力度越强。使用渐进的平滑系数
                                float intensity = (22.0f - dot_forward) / 22.0f; // 0.0 -> 1.0
                                
                                // 每帧微小的平滑偏移（高频更新，杜绝瞬移感与悬空）
                                float side_shove = intensity * 0.035f; 
                                float brake_shove = intensity * 0.025f;

                                CMatrix* mat = g_GetMatrix(veh);
                                float civ_up_x = 0.0f;
                                float civ_up_y = 0.0f;
                                if (mat) {
                                    civ_up_x = mat->up_x;
                                    civ_up_y = mat->up_y;
                                }

                                CVector new_civ_pos = civ_pos;
                                // 1. 施加横向避让位移
                                new_civ_pos.x += ev.right.x * avoid_sign * side_shove;
                                new_civ_pos.y += ev.right.y * avoid_sign * side_shove;
                                // 2. 施加物理急刹车拖拽（退后位置分量，降低实际前进速度）
                                new_civ_pos.x -= civ_up_x * brake_shove;
                                new_civ_pos.y -= civ_up_y * brake_shove;
                                // 移除 Z 轴硬性提升，防止车辆在避让时产生悬空感，让物理引擎自身贴地

                                // 施加微弱的 Yaw 角度旋转，让其车头指向避让方向
                                if (mat) {
                                    float rot_angle = avoid_sign * intensity * 0.006f; 
                                    float cos_a = cosf(rot_angle);
                                    float sin_a = sinf(rot_angle);

                                    float new_up_x = mat->up_x * cos_a - mat->right_x * sin_a;
                                    float new_up_y = mat->up_y * cos_a - mat->right_y * sin_a;
                                    float new_right_x = mat->up_x * sin_a + mat->right_x * cos_a;
                                    float new_right_y = mat->up_y * sin_a + mat->right_y * cos_a;

                                    mat->up_x = new_up_x;
                                    mat->up_y = new_up_y;
                                    mat->right_x = new_right_x;
                                    mat->right_y = new_right_y;
                                }

                                set_entity_pos(veh, new_civ_pos);
                                break; // 响应最近的一个避让源即可
                            }
                        }
                    }
                }
            }
        }
    }
}

void on_main_thread_tick() {
    // 1. 每帧高频平滑避让更新（无感避让）
    apply_civilian_avoidance_field();

    int64_t cur_time = now_ms();
    if (cur_time - g_last_tick_time_ms < 250) {
        return;
    }
    g_last_tick_time_ms = cur_time;

    ecs::EventDispatcher::get().dispatch(ecs::TickEvent(cur_time));

    if (!g_FindPlayerPed || !g_FindPlayerPed(0)) {
        return; // 不在游戏内
    }

    // 运行急救与消防车辆的自主脱困、避障与高级物理救援心跳
    emergency_vehicles_tick();

    // 周期性释放已到期的临时道路关闭
    {
        std::lock_guard<std::mutex> temp_lock(g_temp_closures_mutex);
        for (auto it = g_temp_road_closures.begin(); it != g_temp_road_closures.end(); ) {
            if (cur_time >= it->reopen_time_ms) {
                if (g_ThePaths && g_SwitchRoadsOffInArea) {
                    g_SwitchRoadsOffInArea(
                        g_ThePaths,
                        it->center.x - it->radius, it->center.y - it->radius, it->center.z - 8.0f,
                        it->center.x + it->radius, it->center.y + it->radius, it->center.z + 8.0f,
                        false, true, false
                    );
                    LOGI("🚧 [dispatchCenter - TempClosure] Reopened route section around (%.1f, %.1f, %.1f) after delay", 
                         it->center.x, it->center.y, it->center.z);
                }
                it = g_temp_road_closures.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);

    // 判断是否有任意活动案件
    bool any_active_case = false;
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            any_active_case = true;
            break;
        }
    }

    // Periodically bind occupants of active vehicles (only for our custom spawned initial response & reinforcement vehicles)
    if (any_active_case) {
        std::lock_guard<std::mutex> lock_sp(g_spawned_cop_vehicles_mutex);
        for (void* veh : g_spawned_cop_vehicles) {
            if (is_vehicle_pointer_valid(veh)) {
                bind_vehicle_occupants(veh);
            }
        }
    }

    static int64_t last_siren_refresh = 0;
    bool do_siren_refresh = (cur_time - last_siren_refresh > 1500);
    if (do_siren_refresh) {
        last_siren_refresh = cur_time;
    }

    // 复制一份 shared_ptr 数组快照，绝对规避在派发和回调过程中（同一线程上重入时）导致 `g_active_crimes` 扩容而引发的迭代器失效 crash
    std::vector<std::shared_ptr<CrimeEvent>> crimes_snapshot = g_active_crimes;

    // =====================================================================
    // 👻 [dispatchCenter - GhostVehicleGuard] 幽灵车急停锁定保护（防止半路警员跌落摩托，空车狂奔）
    // =====================================================================
    if (any_active_case) {
        for (const auto& crime : crimes_snapshot) {
            if (!crime || crime->cancelled) continue;
            for (void* veh : crime->case_vehicles) {
                if (is_vehicle_pointer_valid(veh) && !is_vehicle_emptied(veh)) {
                    if (!is_vehicle_occupied_by_driver(veh)) {
                        CVector veh_pos = get_entity_pos(veh);
                        if (g_GetCarToGoToCoors) {
                            g_GetCarToGoToCoors(veh, &veh_pos, 4, false); // Mode 4 (DF_STOP_CAR) 瞬间手刹锁死
                        }
                        add_vehicle_emptied(veh);
                        LOGW("⚠️ [dispatchCenter - GhostVehicleGuard] Handbrake-locked ghost vehicle %p (no driver). Marked as emptied.", veh);
                    }
                }
            }
        }
    }

    // =====================================================================
    // 🚨 [dispatchCenter - EnRouteRerouting] 警车响应调度沿途发生的同级及以上活跃犯罪 (Reroute en-route cops to overlapping crimes)
    // =====================================================================
    struct RerouteRecord {
        void* vehicle;
        std::shared_ptr<CrimeEvent> from_case;
        std::shared_ptr<CrimeEvent> to_case;
        float distance;
    };
    std::vector<RerouteRecord> pending_reroutes;

    {
        std::lock_guard<std::mutex> lock_sc(g_vehicles_mutex);
        for (auto& case_A : crimes_snapshot) {
            if (!case_A || case_A->cancelled) continue;

            for (void* veh : case_A->case_vehicles) {
                if (!veh || !is_vehicle_pointer_valid(veh)) continue;

                // 仅重定向正处于行驶赶往现场途中 (g_vehicles_ordered_to_scene) 且尚未下车的警车
                if (g_vehicles_ordered_to_scene.count(veh) > 0 && !is_vehicle_emptied(veh)) {
                    CVector veh_pos = get_entity_pos(veh);
                    std::shared_ptr<CrimeEvent> best_case_B = nullptr;
                    float best_dist = 999999.0f;

                    for (auto& case_B : crimes_snapshot) {
                        if (!case_B || case_B->cancelled || case_B == case_A) continue;
                        if (!case_B->criminal || !is_ped_pointer_valid_safe(case_B->criminal)) continue;

                        // 1. 同级及以上威胁度等级限制
                        // 枪击大案(threat=2), 近战/非枪击(threat=1)
                        int threat_A = case_A->is_firearm ? 2 : 1;
                        int threat_B = case_B->is_firearm ? 2 : 1;

                        if (threat_B >= threat_A) {
                            float dx = veh_pos.x - case_B->location.x;
                            float dy = veh_pos.y - case_B->location.y;
                            float dz = veh_pos.z - case_B->location.z;
                            float dist = sqrtf(dx * dx + dy * dy + dz * dz);

                            // 2. 动态视听 AV 响应范围：枪械 75 米，近战/空手 35 米
                            float av_range = case_B->is_firearm ? 75.0f : 35.0f;

                            if (dist <= av_range && dist < best_dist) {
                                best_dist = dist;
                                best_case_B = case_B;
                            }
                        }
                    }

                    if (best_case_B) {
                        pending_reroutes.push_back({veh, case_A, best_case_B, best_dist});
                    }
                }
            }
        }
    }

    // 执行重定向，将警车无缝过户给新案件
    for (const auto& record : pending_reroutes) {
        void* veh = record.vehicle;
        auto case_A = record.from_case;
        auto case_B = record.to_case;

        if (!case_A || case_A->cancelled || !case_B || case_B->cancelled) continue;

        // A. 从原案件中移除该车
        auto it_A = std::find(case_A->case_vehicles.begin(), case_A->case_vehicles.end(), veh);
        if (it_A != case_A->case_vehicles.end()) {
            case_A->case_vehicles.erase(it_A);
        }
        if (case_A->spawned_vehicle == veh) {
            case_A->spawned_vehicle = nullptr;
        }

        // B. 将该车追加至新案件
        auto it_B = std::find(case_B->case_vehicles.begin(), case_B->case_vehicles.end(), veh);
        if (it_B == case_B->case_vehicles.end()) {
            case_B->case_vehicles.push_back(veh);
        }
        if (!case_B->spawned_vehicle) {
            case_B->spawned_vehicle = veh;
        }

        LOGI("🚨 [dispatchCenter - EnRouteReroute] Dispatched vehicle %p (originally for Case %llu, threat: %d) encountered Case %llu (threat: %d) en route! Rerouting to Case %llu (dist: %.1fm).",
             veh, (unsigned long long)case_A->case_id, case_A->is_firearm ? 2 : 1,
             (unsigned long long)case_B->case_id, case_B->is_firearm ? 2 : 1,
             (unsigned long long)case_B->case_id, record.distance);

        // C. 给新案件重置车辆的导航目的地
        command_cop_vehicle_to_scene(veh, case_B->location);

        // D. 重置乘员的唤醒与绑定，使司机一脚油门驶向新案件嫌犯
        setup_dispatched_cops(veh, case_B->criminal);
    }

    for (auto& crime : crimes_snapshot) {
        if (!crime || crime->cancelled) {
            continue;
        }

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

    // 垃圾回收阶段：清理所有标记为已取消 (cancelled) 或已经转为 STATE_CLEANUP 的案件
    for (auto it = g_active_crimes.begin(); it != g_active_crimes.end(); ) {
        if ((*it)->cancelled || (*it)->dispatch_state == STATE_CLEANUP) {
            LOGI("📡 [dispatchCenter - CaseGC] Erasing case %llu from active crimes list", (unsigned long long)(*it)->case_id);
            cleanup_single_case_vehicles(*it);
            it = g_active_crimes.erase(it);
        } else {
            ++it;
        }
    }

    // Fallback 清理层：如果当前没有任何活跃案件存在，则统一恢复/释放全局所有的向下兼容级和未分组表映射
    if (g_active_crimes.empty()) {
        g_tracked_criminal.store(nullptr);

        g_player_stray_bullet_flag.store(false);
        g_player_stray_bullet_time.store(0);
        g_friendly_fire_cop_hits.store(0);
        g_last_friendly_fire_cop_time.store(0);
        g_player_friendly_fire_blocked.store(false);
        {
            std::lock_guard<std::mutex> lock_emp(g_vehicles_emptied_mutex);
            g_vehicles_emptied.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
            g_dispatched_vehicles_time.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
            g_cop_attack_assign_time.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
            g_armed_cops_time.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
            g_cop_assigned_weapon.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
            g_stuck_vehicles.clear();
        }
        {
            std::lock_guard<std::mutex> lock_sc(g_vehicles_mutex);
            g_vehicles_ordered_to_scene.clear();
        }
        {
            std::lock_guard<std::mutex> lock_sa(g_vehicles_siren_awakened_mutex);
            g_vehicles_siren_awakened.clear();
        }
        {
            std::lock_guard<std::mutex> lock_swat(g_spawned_swats_mutex);
            g_spawned_swats.clear();
        }
        {
            std::lock_guard<std::mutex> lock_bind(g_bindings_mutex);
            g_cop_vehicle_bindings.clear();
        }
        {
            std::lock_guard<std::mutex> lock_ex(g_exits_mutex);
            g_cop_exits.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_spawned_cop_vehicles_mutex);
            g_spawned_cop_vehicles.clear();
        }
    }
}

