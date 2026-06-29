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
// Emergency vehicle unstuck / navigation helpers
// =====================================================================
std::map<void*, StuckTracker> g_emergency_stuck_vehicles;
std::mutex g_emergency_stuck_vehicles_mutex;

std::vector<void*> g_emergency_vehicles_emptied;
std::mutex g_emergency_vehicles_emptied_mutex;

constexpr int PED_TYPE_MEDIC     = 18;
constexpr int PED_TYPE_FIREMAN   = 19;

void emergency_vehicles_tick() {
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
                                                    dispatch_tell_occupants_to_leave_car(veh);
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
                                                                dispatch_tell_occupants_to_leave_car(veh);
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
                                        dispatch_tell_occupants_to_leave_car(veh);
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
