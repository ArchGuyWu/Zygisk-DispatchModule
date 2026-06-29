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
// Dispatch tick (runs from CTheScripts::Process hook)
//
// State machine: IDLE -> TIMING -> ON_SCENE -> CLEANUP
// =====================================================================
static int64_t g_last_tick_time_ms = 0;

void cleanup_single_case_vehicles(std::shared_ptr<CrimeEvent> crime) {
    if (!crime) return;
    
    // 1. 解除封路
    if (crime->road_closure_active) {
        if (g_ThePaths && g_SwitchRoadsOffInArea) {
            CVector center = crime->road_closure_center;
            g_SwitchRoadsOffInArea(
                g_ThePaths,
                center.x - 50.0f, center.y - 50.0f, center.z - 15.0f,
                center.x + 50.0f, center.y + 50.0f, center.z + 15.0f,
                false, true, false
            );
            crime->road_closure_active = false;
            LOGI("🚧 [dispatchCenter - Cordon] Lifted road closure for case %llu", (unsigned long long)crime->case_id);
        }
    }
    
    // 2. 精准擦除当前案件的警车绑定
    std::lock_guard<std::mutex> lock_emp(g_vehicles_emptied_mutex);
    std::lock_guard<std::mutex> lock_veh(g_vehicles_mutex);
    std::lock_guard<std::mutex> lock_dis(g_dispatched_vehicles_time_mutex);
    std::lock_guard<std::mutex> lock_sc(g_vehicles_siren_awakened_mutex);
    std::lock_guard<std::mutex> lock_spawn(g_spawned_cop_vehicles_mutex);
    
    for (void* veh : crime->case_vehicles) {
        if (veh) {
            g_vehicles_emptied.erase(veh);
            g_vehicles_ordered_to_scene.erase(veh);
            g_dispatched_vehicles_time.erase(veh);
            g_vehicles_siren_awakened.erase(veh);
            
            auto it = std::find(g_spawned_cop_vehicles.begin(), g_spawned_cop_vehicles.end(), veh);
            if (it != g_spawned_cop_vehicles.end()) {
                g_spawned_cop_vehicles.erase(it);
            }
        }
    }
    crime->case_vehicles.clear();
    LOGI("📡 [dispatchCenter - GC] Cleaned up vehicles and route closures for case %llu", (unsigned long long)crime->case_id);
}

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
// Emergency vehicle unstuck / navigation helpers
// =====================================================================
std::map<void*, StuckTracker> g_emergency_stuck_vehicles;
std::mutex g_emergency_stuck_vehicles_mutex;

std::vector<void*> g_emergency_vehicles_emptied;
std::mutex g_emergency_vehicles_emptied_mutex;

constexpr int PED_TYPE_MEDIC     = 18;
constexpr int PED_TYPE_FIREMAN   = 19;

static void emergency_vehicles_tick() {
    if (!g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return;

    void* ped_pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!ped_pool) return;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(ped_pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(ped_pool) + 16);
    if (!byte_map) return;

    int64_t now_time = now_ms();

    // 1. 定期清理已失效的离车标记
    {
        std::lock_guard<std::mutex> emptied_lock(g_emergency_vehicles_emptied_mutex);
        for (auto it = g_emergency_vehicles_emptied.begin(); it != g_emergency_vehicles_emptied.end(); ) {
            if (!*it || !is_vehicle_pointer_valid(*it)) {
                it = g_emergency_vehicles_emptied.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. 遍历 Ped 池，找出医护人员 (18) 与消防员 (19) 的载具
    std::vector<void*> processed_vehicles; // 避免同一车辆有多个乘员时重复处理
    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            if (ped && is_ped_pointer_valid_safe(ped)) {
                if (g_IsAlive && !g_IsAlive(ped)) {
                    continue;
                }
                int ped_type = g_GetPedType(ped);
                if (ped_type == PED_TYPE_MEDIC || ped_type == PED_TYPE_FIREMAN) {
                    void* veh = find_vehicle_of_cop(ped); // 此辅助函数能安全返回 ped 的当前载具
                    if (veh && is_vehicle_pointer_valid(veh)) {
                        unsigned int model = get_entity_model_index(veh);
                        if (model == 416 || model == 407) { // Ambulance or Firetruck
                            if (std::find(processed_vehicles.begin(), processed_vehicles.end(), veh) != processed_vehicles.end()) {
                                continue;
                            }
                            processed_vehicles.push_back(veh);

                            CVector current_pos = get_entity_pos(veh);
                            
                            // 提取车辆当前的朝向向量 (Up Vector)
                            float dir_x = 0.0f;
                            float dir_y = 0.0f;
                            float dir_z = 0.0f;
                            if (g_GetMatrix) {
                                CMatrix* mat = g_GetMatrix(veh);
                                if (mat) {
                                    dir_x = mat->up_x;
                                    dir_y = mat->up_y;
                                    dir_z = mat->up_z;
                                }
                            }
                            float d_len = sqrtf(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);
                            if (d_len > 0.01f) {
                                dir_x /= d_len;
                                dir_y /= d_len;
                                dir_z /= d_len;
                            } else {
                                dir_x = 0.0f;
                                dir_y = 1.0f;
                                dir_z = 0.0f;
                            }

                            // A. 扶正由于物理倾倒引起的异常 (比如重型消防车侧翻)
                            if (g_GetMatrix) {
                                CMatrix* mat = g_GetMatrix(veh);
                                if (mat && mat->at_z < 0.8f) { // Tilting > 36.8 degrees
                                    mat->up_x = dir_x;
                                    mat->up_y = dir_y;
                                    mat->up_z = 0.0f;
                                    mat->at_x = 0.0f;
                                    mat->at_y = 0.0f;
                                    mat->at_z = 1.0f;
                                    mat->right_x = dir_y;
                                    mat->right_y = -dir_x;
                                    mat->right_z = 0.0f;
                                    
                                    CVector upright_pos = {current_pos.x, current_pos.y, current_pos.z + 0.3f};
                                    set_entity_pos(veh, upright_pos);
                                    LOGI("🚒🚑 [Emergency Escaper - Stabilize] Uprighted flipped vehicle %p", veh);
                                }
                            }

                            // 获取或初始化 StuckTracker
                            StuckTracker tracker;
                            bool found_stuck = false;
                            {
                                std::lock_guard<std::mutex> stuck_lock(g_emergency_stuck_vehicles_mutex);
                                auto it = g_emergency_stuck_vehicles.find(veh);
                                if (it != g_emergency_stuck_vehicles.end()) {
                                    tracker = it->second;
                                    found_stuck = true;
                                }
                            }

                            if (!found_stuck) {
                                tracker.last_pos = current_pos;
                                tracker.last_check_time = now_time;
                                tracker.stuck_since = 0;
                                tracker.last_intervention_time = 0;
                                tracker.spin_count = 0;
                                tracker.last_dir_x = 0.0f;
                                tracker.last_dir_y = 0.0f;

                                std::lock_guard<std::mutex> stuck_lock(g_emergency_stuck_vehicles_mutex);
                                g_emergency_stuck_vehicles[veh] = tracker;
                            } else {
                                float time_diff = (now_time - tracker.last_check_time) / 1000.0f;
                                if (time_diff >= 1.0f) { // 每秒心跳
                                    float dx_s = current_pos.x - tracker.last_pos.x;
                                    float dy_s = current_pos.y - tracker.last_pos.y;
                                    float dz_s = current_pos.z - tracker.last_pos.z;
                                    float dist_moved = sqrtf(dx_s * dx_s + dy_s * dy_s + dz_s * dz_s);
                                    float speed = dist_moved / time_diff;

                                    // B. [Proactive Water Escape]：主动防落水溺亡救援
                                    if (speed >= 3.0f && dist_moved > 0.1f) {
                                        float lookahead = speed * 1.5f;
                                        CVector p_pos = {
                                            current_pos.x + (dx_s / dist_moved) * lookahead,
                                            current_pos.y + (dy_s / dist_moved) * lookahead,
                                            current_pos.z + (dz_s / dist_moved) * lookahead
                                        };
                                        bool will_fall = (p_pos.z < 1.0f && current_pos.z >= 2.0f);
                                        if (will_fall) {
                                            bool already_emptied = false;
                                            {
                                                std::lock_guard<std::mutex> emptied_lock(g_emergency_vehicles_emptied_mutex);
                                                already_emptied = std::find(g_emergency_vehicles_emptied.begin(), g_emergency_vehicles_emptied.end(), veh) != g_emergency_vehicles_emptied.end();
                                            }
                                            if (!already_emptied) {
                                                if (g_TellOccupantsToLeaveCar) {
                                                    g_TellOccupantsToLeaveCar(veh);
                                                }
                                                std::lock_guard<std::mutex> emptied_lock(g_emergency_vehicles_emptied_mutex);
                                                g_emergency_vehicles_emptied.push_back(veh);
                                                LOGW("🌊 [Emergency Escaper - Water rescue] Cinematic emergency exit triggered for %p to avoid drowning!", veh);
                                            }
                                        }
                                    }

                                    // C. [Anti-Spin Guard]：反画圆死锁机制
                                    float cur_dir_x = 0.0f;
                                    float cur_dir_y = 0.0f;
                                    if (dist_moved > 0.05f) {
                                        cur_dir_x = dx_s / dist_moved;
                                        cur_dir_y = dy_s / dist_moved;
                                    }
                                    if (dist_moved > 0.5f) {
                                        if (tracker.last_dir_x != 0.0f || tracker.last_dir_y != 0.0f) {
                                            float dir_dot = cur_dir_x * tracker.last_dir_x + cur_dir_y * tracker.last_dir_y;
                                            if (dir_dot < 0.85f) { // 画圆判断
                                                tracker.spin_count++;
                                            } else {
                                                tracker.spin_count = 0;
                                            }
                                        }
                                        tracker.last_dir_x = cur_dir_x;
                                        tracker.last_dir_y = cur_dir_y;
                                    } else {
                                        tracker.spin_count = 0;
                                    }

                                    if (tracker.spin_count >= 3) {
                                        bool is_seen = is_cop_visible_to_player(veh, current_pos.x, current_pos.y, current_pos.z);
                                        if (g_GetMatrix) {
                                            CMatrix* mat = g_GetMatrix(veh);
                                            if (mat && !is_seen) {
                                                // 物理旋转 120 度打破惯性
                                                float new_up_x = mat->up_x * (-0.5f) - mat->up_y * 0.866f;
                                                float new_up_y = mat->up_x * 0.866f + mat->up_y * (-0.5f);
                                                float new_right_x = mat->right_x * (-0.5f) - mat->right_y * 0.866f;
                                                float new_right_y = mat->right_x * 0.866f + mat->right_y * (-0.5f);
                                                mat->up_x = new_up_x;
                                                mat->up_y = new_up_y;
                                                mat->right_x = new_right_x;
                                                mat->right_y = new_right_y;
                                            }
                                        }
                                        CVector reverse_pos;
                                        if (is_seen) {
                                            reverse_pos = {
                                                current_pos.x - dir_x * 3.5f,
                                                current_pos.y - dir_y * 3.5f,
                                                current_pos.z + 0.3f
                                            };
                                        } else {
                                            reverse_pos = {
                                                current_pos.x - dir_x * 4.5f,
                                                current_pos.y - dir_y * 4.5f,
                                                current_pos.z + 0.5f
                                            };
                                        }
                                        set_entity_pos(veh, reverse_pos);
                                        LOGW("🔄 [Emergency Escaper - Anti-Spin] Circle spinning detected. Applied yaw rotation and nudged reverse (seen=%d)", is_seen);
                                        tracker.spin_count = 0;
                                        tracker.last_intervention_time = now_time;
                                    }

                                    // D. [ACC Unified Fleet Control]：急救车辆智能距离避让，绝对不追尾
                                    bool car_blocked = false;
                                    if (g_ms_pVehiclePool && g_GetPoolVehicle) {
                                        void* v_pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
                                        if (v_pool) {
                                            char* v_byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(v_pool) + 8);
                                            int v_size = *reinterpret_cast<int*>(reinterpret_cast<char*>(v_pool) + 16);
                                            if (v_byte_map) {
                                                for (int j = 0; j < v_size; j++) {
                                                    signed char v_flag = v_byte_map[j];
                                                    if (v_flag >= 0) {
                                                        int v_handle = (j << 8) | v_flag;
                                                        void* other_veh = g_GetPoolVehicle(v_handle);
                                                        if (other_veh && other_veh != veh && is_vehicle_pointer_valid(other_veh)) {
                                                            CVector other_pos = get_entity_pos(other_veh);
                                                            float ox = other_pos.x - current_pos.x;
                                                            float oy = other_pos.y - current_pos.y;
                                                            float oz = other_pos.z - current_pos.z;
                                                            float dist_v = sqrtf(ox * ox + oy * oy + oz * oz);

                                                            if (dist_v < 10.0f) {
                                                                float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                                if (dot_p > 0.0f && dot_p < 10.0f) {
                                                                    float lat_dist_sq = (dist_v * dist_v) - (dot_p * dot_p);
                                                                    if (lat_dist_sq < 4.84f) { // 横向偏离小于 2.2米
                                                                        car_blocked = true;
                                                                        if ((speed < 1.2f || dist_moved < 1.2f) && dist_v < 8.0f) {
                                                                            // 开启智能偏斜避让
                                                                            float side_sign = (((uintptr_t)veh) & 1) ? 1.0f : -1.0f;
                                                                            float lx = -dir_y * side_sign;
                                                                            float ly = dir_x * side_sign;
                                                                            CVector detour_pos = {
                                                                                current_pos.x + lx * 1.8f + dir_x * 0.8f,
                                                                                current_pos.y + ly * 1.8f + dir_y * 0.8f,
                                                                                current_pos.z
                                                                            };
                                                                            
                                                                            set_entity_pos(veh, detour_pos);
                                                                            
                                                                            LOGI("[Emergency Escaper - ACC Bypass] Detoured blocked vehicle %p", veh);
                                                                        }
                                                                        break; // 每秒处理一个避让源
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // E. [Fire Avoidance]：仅对救护车 (Ambulance) 开启火源智能避让。消防车需要逆火而行不避让！
                                    if (!car_blocked && model == 416 && g_FireManager && g_FindNearestFire) {
                                        void* nearest_fire = g_FindNearestFire(g_FireManager, current_pos, true, true);
                                        if (nearest_fire) {
                                            CVector fire_pos;
                                            if (get_fire_position(nearest_fire, fire_pos)) {
                                                float fx = fire_pos.x - current_pos.x;
                                                float fy = fire_pos.y - current_pos.y;
                                                float fz = fire_pos.z - current_pos.z;
                                                float fire_dist = sqrtf(fx * fx + fy * fy + fz * fz);

                                                if (fire_dist < 15.0f) {
                                                    if (fire_dist < 6.5f && speed < 2.0f) {
                                                        // 距离过近，强制退出保护
                                                        bool already_emptied = false;
                                                        {
                                                            std::lock_guard<std::mutex> emptied_lock(g_emergency_vehicles_emptied_mutex);
                                                            already_emptied = std::find(g_emergency_vehicles_emptied.begin(), g_emergency_vehicles_emptied.end(), veh) != g_emergency_vehicles_emptied.end();
                                                        }
                                                        if (!already_emptied) {
                                                            if (g_TellOccupantsToLeaveCar) {
                                                                g_TellOccupantsToLeaveCar(veh);
                                                            }
                                                            std::lock_guard<std::mutex> emptied_lock(g_emergency_vehicles_emptied_mutex);
                                                            g_emergency_vehicles_emptied.push_back(veh);
                                                            LOGW("🔥 [Emergency Escaper - Ambulance Fire Exit] Trapped near fire, emergency exit triggered.");
                                                        }
                                                    } else {
                                                        // 正常避火
                                                        float dot_p = fx * dir_x + fy * dir_y + fz * dir_z;
                                                        if (dot_p > 0.0f && dot_p < 15.0f) {
                                                            float lat_dist_sq = (fire_dist * fire_dist) - (dot_p * dot_p);
                                                            if (lat_dist_sq < 25.0f) {
                                                                float lx = -dir_y;
                                                                float ly = dir_x;
                                                                float side_dot = fx * lx + fy * ly;
                                                                float side_sign = (side_dot > 0.0f) ? -1.0f : 1.0f;

                                                                CVector detour_pos = {
                                                                    current_pos.x + lx * side_sign * 5.5f - dir_x * 1.5f,
                                                                    current_pos.y + ly * side_sign * 5.5f - dir_y * 1.5f,
                                                                    current_pos.z
                                                                };
                                                                
                                                                set_entity_pos(veh, detour_pos);
                                                                
                                                                LOGI("[Emergency Escaper - Ambulance Fire Detour] Detoured around fire source.");
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // F. 双保险卡死检测计时
                                    if (speed < 0.8f || dist_moved < 1.2f) {
                                        if (tracker.stuck_since == 0) {
                                            tracker.stuck_since = now_time;
                                        }
                                    } else {
                                        tracker.stuck_since = 0;
                                    }

                                    tracker.last_pos = current_pos;
                                    tracker.last_check_time = now_time;

                                    {
                                        std::lock_guard<std::mutex> stuck_lock(g_emergency_stuck_vehicles_mutex);
                                        g_emergency_stuck_vehicles[veh] = tracker;
                                    }
                                }
                            }

                            // G. [Multi-Stage Unstucking System]：多阶段高级脱困
                            int64_t stuck_duration = (tracker.stuck_since > 0) ? (now_time - tracker.stuck_since) : 0;
                            
                            // B1. 落水检测二次熔断
                            if (current_pos.z < 1.0f) {
                                bool already_emptied = false;
                                {
                                    std::lock_guard<std::mutex> emptied_lock(g_emergency_vehicles_emptied_mutex);
                                    already_emptied = std::find(g_emergency_vehicles_emptied.begin(), g_emergency_vehicles_emptied.end(), veh) != g_emergency_vehicles_emptied.end();
                                }
                                if (!already_emptied) {
                                    if (g_TellOccupantsToLeaveCar) {
                                        g_TellOccupantsToLeaveCar(veh);
                                    }
                                    std::lock_guard<std::mutex> emptied_lock(g_emergency_vehicles_emptied_mutex);
                                    g_emergency_vehicles_emptied.push_back(veh);
                                    LOGW("[Emergency Escaper - Water escape] Low elevation Z < 1.0m. Evacuated emergency personnel.");
                                }
                            }

                            bool trigger_stage1 = false;
                            bool trigger_stage2 = false;

                            bool is_seen = is_cop_visible_to_player(veh, current_pos.x, current_pos.y, current_pos.z);

                            if (stuck_duration >= 7000 && !is_seen) {
                                trigger_stage2 = true;
                            } else if (stuck_duration > 3500) {
                                if (now_time - tracker.last_intervention_time > 6000) {
                                    trigger_stage1 = true;
                                }
                            }

                            // STAGE 2: 7秒以上且玩家看不见：高通过率跃迁 20m 尝试传送到更近位置以解除卡住
                            if (trigger_stage2) {
                                tracker.last_intervention_time = now_time;
                                tracker.stuck_since = 0; // 重置

                                {
                                    std::lock_guard<std::mutex> stuck_lock(g_emergency_stuck_vehicles_mutex);
                                    g_emergency_stuck_vehicles[veh] = tracker;
                                }

                                CVector warp_pos = {
                                    current_pos.x + dir_x * 20.0f,
                                    current_pos.y + dir_y * 20.0f,
                                    current_pos.z + dir_z * 20.0f + 0.80f // 加上安全防陷落高度
                                };
                                set_entity_pos(veh, warp_pos);
                                LOGI("🚒🚑 [Emergency Escaper - Stage 2 Warp] Unseen warped vehicle %p forward 20m", veh);
                            }
                            // STAGE 1: 3.5秒以上卡死：温柔物理推开 / 看不见时中等跃迁 12m
                            else if (trigger_stage1) {
                                tracker.last_intervention_time = now_time;
                                {
                                    std::lock_guard<std::mutex> stuck_lock(g_emergency_stuck_vehicles_mutex);
                                    g_emergency_stuck_vehicles[veh] = tracker;
                                }

                                LOGI("🚒🚑 [Emergency Escaper - Stage 1 Nudge] Initiated rescue for %p...", veh);

                                // 1. 临时关闭路段 20m，强迫 AI 避开当前卡住的死路规划新路线
                                if (g_ThePaths && g_SwitchRoadsOffInArea) {
                                    g_SwitchRoadsOffInArea(
                                        g_ThePaths,
                                        current_pos.x - 20.0f, current_pos.y - 20.0f, current_pos.z - 8.0f,
                                        current_pos.x + 20.0f, current_pos.y + 20.0f, current_pos.z + 8.0f,
                                        true, true, false
                                    );
                                    std::lock_guard<std::mutex> temp_lock(g_temp_closures_mutex);
                                    g_temp_road_closures.push_back({current_pos, 20.0f, now_time + 15000});
                                }

                                // 2. 地下掩埋清除 15m 内阻碍的非紧急平民车辆
                                if (g_ms_pVehiclePool && g_GetPoolVehicle) {
                                    void* v_pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
                                    if (v_pool) {
                                        char* v_byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(v_pool) + 8);
                                        int v_size = *reinterpret_cast<int*>(reinterpret_cast<char*>(v_pool) + 16);
                                        if (v_byte_map) {
                                            for (int j = 0; j < v_size; j++) {
                                                signed char v_flag = v_byte_map[j];
                                                if (v_flag >= 0) {
                                                    int v_handle = (j << 8) | v_flag;
                                                    void* other_veh = g_GetPoolVehicle(v_handle);
                                                    if (other_veh && other_veh != veh && is_vehicle_pointer_valid(other_veh)) {
                                                        unsigned int o_model = get_entity_model_index(other_veh);
                                                        if (o_model != 416 && o_model != 407 && o_model != MODEL_POLICE_CAR && o_model != MODEL_SWAT_VAN && o_model != MODEL_POLICE_BIKE) {
                                                            CVector other_pos = get_entity_pos(other_veh);
                                                            float ov_dx = other_pos.x - current_pos.x;
                                                            float ov_dy = other_pos.y - current_pos.y;
                                                            float ov_dz = other_pos.z - current_pos.z;
                                                            float ov_dist = sqrtf(ov_dx * ov_dx + ov_dy * ov_dy + ov_dz * ov_dz);

                                                            if (ov_dist < 15.0f) {
                                                                if (!is_pos_visible_to_player_camera(other_pos)) {
                                                                    CVector underground = {other_pos.x, other_pos.y, other_pos.z - 50.0f};
                                                                    set_entity_pos(other_veh, underground);
                                                                    LOGI("   +- Trapped road blockage %p buried underground", other_veh);
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // 3. 物理微调：看得见时反向倒车，看不见时正向向前穿墙中跃迁 12m
                                CVector nudged_pos;
                                if (is_seen) {
                                    nudged_pos.x = current_pos.x - dir_x * 3.5f;
                                    nudged_pos.y = current_pos.y - dir_y * 3.5f;
                                    nudged_pos.z = current_pos.z + 0.60f; // 抬升高度推开阻挡
                                } else {
                                    nudged_pos.x = current_pos.x + dir_x * 12.0f;
                                    nudged_pos.y = current_pos.y + dir_y * 12.0f;
                                    nudged_pos.z = current_pos.z + dir_z * 12.0f + 0.75f;
                                }

                                set_entity_pos(veh, nudged_pos);
                                LOGI("   +- Applied Stage 1 Nudge (seen=%d)", is_seen);
                            }
                        }
                    }
                }
            }
        }
    }
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

static void on_main_thread_tick() {
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

void proxy_the_scripts_process() {
    SHADOWHOOK_STACK_SCOPE();

    if (g_orig_the_scripts_process) {
        g_orig_the_scripts_process();
    }

    on_main_thread_tick();
}

// =====================================================================
// Wanted-system spawn interception
// =====================================================================
std::atomic<bool> g_in_wanted_update{false};

void* g_stub_wanted_update = nullptr;
fn_WantedUpdate_t g_orig_wanted_update = nullptr;

void proxy_wanted_update(void* this_wanted) {
    SHADOWHOOK_STACK_SCOPE();
    g_in_wanted_update.store(true);
    SHADOWHOOK_CALL_PREV(proxy_wanted_update, this_wanted);
    g_in_wanted_update.store(false);
}

void* g_stub_add_ped = nullptr;
fn_AddPed_t g_orig_add_ped = nullptr;

CPed* proxy_add_ped(int pedType, unsigned int modelIndex, const CVector& pos, bool bUnknown) {
    SHADOWHOOK_STACK_SCOPE();

    if (pedType == 6 && g_in_wanted_update.load()) { // PED_TYPE_COP = 6
        LOGI("🚫 [trueDispatch] Intercepted and blocked wanted-system forced cop spawn! Model: %u, Pos: (%.1f, %.1f, %.1f)", modelIndex, pos.x, pos.y, pos.z);
        return nullptr;
    }

    return SHADOWHOOK_CALL_PREV(proxy_add_ped, pedType, modelIndex, pos, bUnknown);
}

// =====================================================================
// =====================================================================
// Emergency vehicle spawn distance workaround (mobile draw distance)
// =====================================================================
static bool is_police_vehicle_model(unsigned int model) {
    return (model == 596 || model == 597 || model == 598 || model == 599 ||  // Cop Cars (LSPD, SFPD, LVPD, Ranger)
            model == 523 ||                                                 // Cop Bike
            model == 427 || model == 601 ||                                 // SWAT Enforcer, SWAT Water Cannon
            model == 490 || model == 528 ||                                 // FBI Rancher, FBI Truck
            model == 433 || model == 432);                                  // Barracks (Army Truck), Rhino (Tank)
}

static int count_active_police_vehicles_near_player(float range) {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle || !g_FindPlayerCoors) return 0;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool || !is_pointer_readable(pool)) return 0;

    char** p_byte_map = reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    if (!is_pointer_readable(p_byte_map)) return 0;
    char* byte_map = *p_byte_map;
    if (!byte_map || !is_pointer_readable(byte_map)) return 0;

    int* p_size = reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!is_pointer_readable(p_size)) return 0;
    int size = *p_size;

    CVector player_pos = g_FindPlayerCoors(0);
    int count = 0;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            void* veh = g_GetPoolVehicle(handle);
            if (veh && is_pointer_readable(veh)) {
                unsigned short model = get_entity_model_index(veh);
                if (is_police_vehicle_model(model)) {
                    CVector pos = get_entity_pos(veh);
                    float dx = pos.x - player_pos.x;
                    float dy = pos.y - player_pos.y;
                    float dz = pos.z - player_pos.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist < range) {
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

static bool relocate_police_car_spawn(unsigned int model, CVector& pos) {
    int wanted_level = g_player_wanted_level.load();
    if (wanted_level <= 0) {
        return true; // Allow ambient/parked police cars to spawn normally without relocation or blocking
    }

    int max_cars = 0;
    if (wanted_level == 1 || wanted_level == 2) {
        max_cars = 1;
    } else if (wanted_level == 3 || wanted_level == 4) {
        max_cars = 2;
    } else if (wanted_level >= 5) {
        max_cars = 3;
    }

    int active_cars = count_active_police_vehicles_near_player(150.0f);
    if (active_cars >= max_cars) {
        LOGI("🚫 [trueDispatch] Blocked native cop spawn (Cap reached: %d/%d). Model: %u", active_cars, max_cars, model);
        return false;
    }

    static std::atomic<uint64_t> last_spawn_time_ms{0};
    uint64_t now = now_ms();
    if (now - last_spawn_time_ms.load() < 15000) { // 15s cooldown
        LOGI("🚫 [trueDispatch] Blocked native cop spawn (Cooldown active). Model: %u", model);
        return false;
    }

    CVector player_pos = g_FindPlayerCoors ? g_FindPlayerCoors(0) : pos;
    CVector target_pos = get_spawn_target(player_pos);

    float dx = target_pos.x - player_pos.x;
    float dy = target_pos.y - player_pos.y;
    float dz = target_pos.z - player_pos.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    if (dist > 65.0f) {
        float scale = 60.0f / dist;
        target_pos.x = player_pos.x + dx * scale;
        target_pos.y = player_pos.y + dy * scale;
        target_pos.z = player_pos.z + dz * scale;
    }

    LOGI("🚓 [trueDispatch] Relocating native cop spawn! Model: %u, original pos: (%.1f, %.1f, %.1f), relocated to (%.1f, %.1f, %.1f), dist: %.1f m",
         model, pos.x, pos.y, pos.z, target_pos.x, target_pos.y, target_pos.z, dist);

    last_spawn_time_ms.store(now);
    pos = target_pos;
    return true;
}

void* g_stub_generate_one_emergency_car = nullptr;
fn_GenOneEmergencyCar_t g_orig_generate_one_emergency_car = nullptr;

void proxy_generate_one_emergency_car(unsigned int model, CVector pos) {
    SHADOWHOOK_STACK_SCOPE();

    if (is_police_vehicle_model(model)) {
        if (!g_is_generating_custom_dispatch.load()) {
            if (!relocate_police_car_spawn(model, pos)) {
                return; // Intercept and block
            }
        }
    }

    if ((model == 416 || model == 407) && g_FindPlayerCoors) { // MODEL_AMBULANCE = 416, MODEL_FIRETRUCK = 407
        CVector player_pos = g_FindPlayerCoors(0);
        float dx = pos.x - player_pos.x;
        float dy = pos.y - player_pos.y;
        float dz = pos.z - player_pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        // 如果生成距离太远（比如大于 75 米），在移动端极易因为超出 Streaming Clip Distance 被秒删（Despawn）
        // 我们实施 Workaround：将生成距离缩减比例（Scale Down Ratio），控制在 55 到 65 米的安全加载视距内
        if (dist > 75.0f) {
            float target_dist = 60.0f; // 黄金视距：既在移动端加载范围内，又不至于让玩家眼睁睁看着刷出
            float scale = target_dist / dist;
            CVector scaled_pos = {
                player_pos.x + dx * scale,
                player_pos.y + dy * scale,
                player_pos.z + dz * scale // 保持原有的高度关系
            };
            LOGI("🚑🚒 [Emergency Workaround] Mobile draw distance scale applied! Model=%u, Original spawn dist=%.1f m, scaled to %.1f m (pos: %.1f, %.1f, %.1f)", 
                 model, dist, target_dist, scaled_pos.x, scaled_pos.y, scaled_pos.z);
            pos = scaled_pos;
        }
    }

    SHADOWHOOK_CALL_PREV(proxy_generate_one_emergency_car, model, pos);
}

void* g_stub_script_generate_one_emergency_car = nullptr;
fn_ScriptGenEmergencyCar_t g_orig_script_generate_one_emergency_car = nullptr;

void proxy_script_generate_one_emergency_car(unsigned int model, CVector pos) {
    SHADOWHOOK_STACK_SCOPE();

    // We do NOT block or relocate scripted police cars to ensure 100% mission compatibility

    if ((model == 416 || model == 407) && g_FindPlayerCoors) { // MODEL_AMBULANCE = 416, MODEL_FIRETRUCK = 407
        CVector player_pos = g_FindPlayerCoors(0);
        float dx = pos.x - player_pos.x;
        float dy = pos.y - player_pos.y;
        float dz = pos.z - player_pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (dist > 75.0f) {
            float target_dist = 60.0f;
            float scale = target_dist / dist;
            CVector scaled_pos = {
                player_pos.x + dx * scale,
                player_pos.y + dy * scale,
                player_pos.z + dz * scale
            };
            LOGI("🚑🚒 [Emergency Script Workaround] Mobile draw distance scale applied! Model=%u, Original spawn dist=%.1f m, scaled to %.1f m (pos: %.1f, %.1f, %.1f)", 
                 model, dist, target_dist, scaled_pos.x, scaled_pos.y, scaled_pos.z);
            pos = scaled_pos;
        }
    }

    SHADOWHOOK_CALL_PREV(proxy_script_generate_one_emergency_car, model, pos);
}

void* g_stub_tell_occupants_leave_car = nullptr;
fn_TellOccupantsToLeaveCar_t g_orig_tell_occupants_leave_car = nullptr;

void proxy_tell_occupants_leave_car(void* vehicle) {
    SHADOWHOOK_STACK_SCOPE();
    bind_vehicle_occupants(vehicle); // Bind them here before they leave!
    record_exit_start_for_occupants(vehicle);
    SHADOWHOOK_CALL_PREV(proxy_tell_occupants_leave_car, vehicle);
}
