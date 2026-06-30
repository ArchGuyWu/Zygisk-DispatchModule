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
#include "dispatch_vehicle_escaper.hpp"


// =====================================================================
// Civilian vehicle avoidance near emergency units
// =====================================================================
static bool is_emergency_vehicle_model(unsigned int model) {
    return (model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK ||
            model == MODEL_POLICE_CAR || model == 597 || model == 598 || model == 599 ||
            model == MODEL_SWAT_VAN || model == 528 || model == 490 || model == MODEL_POLICE_BIKE);
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

void apply_civilian_avoidance_field() {
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
                    if (dispatch_vehicle_escaper::is_sideline_traffic_vehicle(veh)) continue;

                    auto panic_it = g_civilian_panic_timers.find(veh);
                    if (panic_it != g_civilian_panic_timers.end() && now < panic_it->second) {
                        continue;
                    }

                    CVector civ_pos = get_entity_pos(veh);

                    float scene_weaken = 1.0f;
                    {
                        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
                        for (const auto& crime : g_active_crimes) {
                            if (!crime || crime->cancelled) continue;
                            CVector anchor = get_crime_dispatch_position(*crime);
                            float sdx = civ_pos.x - anchor.x;
                            float sdy = civ_pos.y - anchor.y;
                            float sdz = civ_pos.z - anchor.z;
                            float scene_dist = sqrtf(sdx * sdx + sdy * sdy + sdz * sdz);
                            if (scene_dist < dispatch_timing::CIVILIAN_AVOIDANCE_WEAKEN_SCENE_M) {
                                scene_weaken = dispatch_timing::CIVILIAN_AVOIDANCE_WEAKEN_FACTOR;
                                break;
                            }
                        }
                    }
                    
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
                                float intensity = ((22.0f - dot_forward) / 22.0f) * scene_weaken;
                                
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
                                g_civilian_panic_timers[veh] =
                                    now + dispatch_timing::CIVILIAN_PANIC_COOLDOWN_MS;
                                break; // 响应最近的一个避让源即可
                            }
                        }
                    }
                }
            }
        }
    }
}
