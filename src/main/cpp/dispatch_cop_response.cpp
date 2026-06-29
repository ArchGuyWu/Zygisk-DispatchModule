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
#include "dispatch_timing.hpp"

// =====================================================================
// 获取 Ped 的 CTaskManager
void* get_ped_intelligence(CPed* ped) {
    if (!ped) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5E8);
}

// 判定警车对于玩家是否可见或处于近场 (避免可见瞬移/强制掉头)
bool is_cop_visible_to_player(void* veh, float current_x, float current_y, float current_z) {
    if (!veh) return false;
    
    CVector pos = {current_x, current_y, current_z};
    if (is_pos_visible_to_player_camera(pos)) {
        return true;
    }

    if (g_FindPlayerPed) {
        void* player = g_FindPlayerPed(0);
        if (player && is_ped_pointer_valid_safe(player)) {
            CVector player_pos = get_entity_pos(reinterpret_cast<CPed*>(player));
            float dx = player_pos.x - current_x;
            float dy = player_pos.y - current_y;
            float dz = player_pos.z - current_z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < 80.0f) {
                return true;
            }
        }
    }
    return false;
}

// 前向声明，解决编译循环依赖问题
bool is_vehicle_occupied_by_driver(void* veh);

void dispatch_tell_occupants_to_leave_car(void* vehicle) {
    if (!vehicle || !g_orig_tell_occupants_leave_car) return;
    bind_vehicle_occupants(vehicle);
    record_exit_start_for_occupants(vehicle);
    g_orig_tell_occupants_leave_car(vehicle);
}

bool is_cop_currently_exiting(CPed* cop) {
    std::lock_guard<std::mutex> lock(g_exits_mutex);
    int64_t cur_time = now_ms();
    for (auto it = g_cop_exits.begin(); it != g_cop_exits.end(); ) {
        if (!is_ped_pointer_valid_safe(it->cop)) {
            it = g_cop_exits.erase(it);
        } else {
            if (it->cop == cop) {
                if (cur_time - it->exit_time < 8000) {
                    return true;
                }
            }
            ++it;
        }
    }
    return false;
}

bool should_block_cop_reenter_vehicle(CPed* cop) {
    if (!cop || !is_ped_pointer_valid_safe(cop)) return true;
    if (is_cop_currently_exiting(cop)) return true;

    auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
    if (cop_comp && cop_comp->has_exited_vehicle && g_crime_active.load()) {
        return true;
    }

    if (!g_crime_active.load()) return false;

    CVector cop_pos = get_entity_pos(cop);
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (const auto& crime : g_active_crimes) {
        if (!crime || crime->cancelled) continue;
        for (CPed* criminal : crime->consolidated_criminals) {
            if (!criminal || !is_ped_pointer_valid_safe(criminal)) continue;
            if (g_IsAlive && !g_IsAlive(criminal)) continue;
            CVector crim_pos = get_entity_pos(criminal);
            float dx = cop_pos.x - crim_pos.x;
            float dy = cop_pos.y - crim_pos.y;
            float dz = cop_pos.z - crim_pos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < 70.0f) {
                return true;
            }
        }
    }
    return false;
}

// 指派 CTaskComplexEnterCar 任务，让其上车
void make_cop_enter_vehicle(CPed* cop, void* vehicle, bool as_driver) {
    if (!cop || !is_ped_pointer_valid_safe(cop) || !vehicle || !is_vehicle_pointer_valid(vehicle) || !g_TaskNew || !g_TaskEnterCar_ctor || !g_SetTask) return;
    if (g_IsAlive && !g_IsAlive(cop)) return;
    if (should_block_cop_reenter_vehicle(cop)) {
        LOGI("👮 [make_cop_enter_vehicle] Blocked re-enter for cop %p (exiting or active combat nearby)", cop);
        return;
    }

    auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
    if (cop_comp) {
        int64_t cur_time = now_ms();
        if (cur_time - cop_comp->last_enter_vehicle_command_time_ms < 10000) {
            return;
        }
        cop_comp->last_enter_vehicle_command_time_ms = cur_time;
    }

    void* intelligence = get_ped_intelligence(cop);
    if (!intelligence) return;

    void* task_manager = reinterpret_cast<void*>(reinterpret_cast<char*>(intelligence) + 8);
    
    // 司乘关系/抢驾驶位冲突保护 (Seat Conflict Guard)
    // 如果我们想让警员作为司机上车，但该警车上已经有了现役司机（另一个警员），
    // 为了防止警员强行将同僚拽下车开走，我们强制将其改为作为乘客上车！
    if (as_driver && is_vehicle_occupied_by_driver(vehicle)) {
        as_driver = false;
        LOGW("👮 [make_cop_enter_vehicle - SeatConflictGuard] Vehicle %p already has a driver. Forcing cop %p to enter as passenger to maintain good driver-passenger relationship!", vehicle, cop);
    }

    void* task = g_TaskNew(512);
    if (task) {
        g_TaskEnterCar_ctor(task, reinterpret_cast<CVehicle*>(vehicle), as_driver, false, false, false);
        g_SetTask(task_manager, reinterpret_cast<CTask*>(task), 1, false);
        LOGI("👮 [make_cop_enter_vehicle] Commanded cop %p to enter vehicle %p as_driver: %d", cop, vehicle, as_driver);
    }
}

// Event Group Fix
// 偏移量确定为 0xC8 (200 字节)。之前使用的 0x30~0xC0 范围扫描在 0x60 处有假阳性指针，
// 传入 Add 会导致严重的 SIGSEGV 崩溃。此处改为直接安全偏移，避免该路径上的崩溃。
void* get_ped_event_group(CPed* ped) {
    void* intelligence = get_ped_intelligence(ped);
    if (!intelligence) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<char*>(intelligence) + 0xC8);
}

// 在载具池中查找最靠近指定坐标的载具（支持排除指定载具）
void* find_closest_vehicle_to(CVector pos, float max_dist, void* ignore_veh) {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle) return nullptr;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool) return nullptr;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return nullptr;

    void* closest = nullptr;
    float closest_dist = max_dist;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) { // 占用的插槽 (最高位 0x80 为 0)
            int handle = (i << 8) | flag;
            void* veh = g_GetPoolVehicle(handle);
            if (veh && veh != ignore_veh) {
                CVector veh_pos = get_entity_pos(veh);
                float dx = veh_pos.x - pos.x;
                float dy = veh_pos.y - pos.y;
                float dz = veh_pos.z - pos.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dist < closest_dist) {
                    closest_dist = dist;
                    closest = veh;
                }
            }
        }
    }
    return closest;
}

// 查找并返回该警员所在的载具指针
void* find_vehicle_of_cop(CPed* cop) {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle || !g_IsDriver || !g_IsPassenger) return nullptr;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool) return nullptr;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return nullptr;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            void* veh = g_GetPoolVehicle(handle);
            if (veh) {
                if (g_IsDriver(veh, cop) || g_IsPassenger(veh, cop)) {
                    return veh;
                }
            }
        }
    }
    return nullptr;
}

// 检查该载具中是否已经有了司机（防止无人巡警车行驶）
bool is_vehicle_occupied_by_driver(void* veh) {
    if (!veh || !g_ms_pPedPool || !g_GetPoolPed || !g_IsDriver) return false;
    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return false;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return false;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            if (ped) {
                if (g_IsDriver(veh, ped)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// 智能驾驶 AI 控制逻辑：
// 1. 尽量限制在正常路网上，防止开上人行道
// 2. 距离目标较近时温柔减速，防止直接蛮力撞击犯罪 NPC / 玩家
void command_vehicle_ai(void* vehicle, const CVector& target_loc, float dist_to_target) {
    if (!g_GetCarToGoToCoors || !vehicle) return;

    // 当距离目标非常近（例如 < 32 米）时：
    // 我们将驾驶目的地重置为车辆当前的 3D 坐标（原地诱骗急刹），防止其开到人行道或越野冲撞，
    // 调用 TellOccupantsToLeaveCar 让警员提前下车
    if (dist_to_target < 16.0f) {
        CVector veh_pos = get_entity_pos(vehicle);
        g_GetCarToGoToCoors(vehicle, &veh_pos, 4, false); // Mode 4 (DF_STOP_CAR) 瞬间手刹锁死
        dispatch_tell_occupants_to_leave_car(vehicle);
        return;
    }

    // 默认使用温柔且合规的紧急响应模式：
    // 模式 2 (DF_FAST/Emergency) 会开启警笛并超速/闯红灯，但被路网硬约束在车行道上，绝对不会越界开上人行道 (Avoid Sidewalks)
    int mode = 2;
    bool bAvoidPeds = true;

    g_GetCarToGoToCoors(vehicle, const_cast<CVector*>(&target_loc), mode, bAvoidPeds);
}

// 调度行驶的统一入口：有司机才让车开过去，没司机尝试再加人，若依旧没司机则拦截，防止幽灵车行驶
void command_cop_vehicle_to_scene(void* vehicle, const CVector& target_loc) {
    if (!vehicle) return;

    if (!is_vehicle_occupied_by_driver(vehicle)) {
        LOGW("⚠️ Dispatched vehicle %p has no driver yet. Re-running AddPoliceCarOccupants", vehicle);
        if (g_AddPoliceOccupants) {
            g_AddPoliceOccupants(reinterpret_cast<CVehicle*>(vehicle), true);
        }
    }

    if (is_vehicle_occupied_by_driver(vehicle)) {
        CVector veh_pos = get_entity_pos(vehicle);
        float dx = veh_pos.x - target_loc.x;
        float dy = veh_pos.y - target_loc.y;
        float dz = veh_pos.z - target_loc.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        
        command_vehicle_ai(vehicle, target_loc, dist);
        LOGI("✅ Commanded vehicle %p to scene with active driver (dist=%.1f)", vehicle, dist);
    } else {
        LOGE("❌ Dispatched vehicle %p STILL has no driver! Blocked GetCarToGoToCoors to prevent ghost vehicle", vehicle);
    }
}

// Commanded cop vector removed to use FindTaskByType(1105) dynamically

std::vector<CopVehicleBinding> g_cop_vehicle_bindings;
std::mutex g_bindings_mutex;

// 查找警员先前绑定的警车
void* find_bound_vehicle_of_cop(CPed* cop, bool& out_is_driver) {
    std::lock_guard<std::mutex> lock(g_bindings_mutex);
    for (const auto& binding : g_cop_vehicle_bindings) {
        if (binding.cop == cop) {
            out_is_driver = binding.as_driver;
            return binding.vehicle;
        }
    }
    return nullptr;
}

// 验证该警车原先绑定的驾驶员是否依然存活
bool is_alive_bound_driver_exists(void* vehicle) {
    std::lock_guard<std::mutex> lock(g_bindings_mutex);
    for (const auto& binding : g_cop_vehicle_bindings) {
        if (binding.vehicle == vehicle && binding.as_driver) {
            if (binding.cop && is_ped_pointer_valid_safe(binding.cop)) {
                if (g_IsAlive && g_IsAlive(binding.cop)) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<void*> g_spawned_cop_vehicles;
std::mutex g_spawned_cop_vehicles_mutex;

std::vector<CopExitRecord> g_cop_exits;
std::mutex g_exits_mutex;

// Missing global variables and mutexes for dispatched vehicle & cop state tracking
std::set<void*> g_vehicles_emptied;
std::mutex g_vehicles_emptied_mutex;

std::map<void*, int64_t> g_dispatched_vehicles_time;
std::mutex g_dispatched_vehicles_time_mutex;

std::map<std::pair<CPed*, CPed*>, int64_t> g_cop_attack_assign_time;
std::mutex g_cop_attack_assign_mutex;

std::map<CPed*, int64_t> g_armed_cops_time;
std::mutex g_armed_cops_mutex;

std::map<CPed*, eWeaponType> g_cop_assigned_weapon;
std::mutex g_cop_assigned_weapon_mutex;

std::set<void*> g_vehicles_ordered_to_scene;
std::mutex g_vehicles_mutex; // as used in line 2981 and 2447

std::set<void*> g_vehicles_siren_awakened;
std::mutex g_vehicles_siren_awakened_mutex;

// Helper functions for vehicle status queries and modifications
bool is_vehicle_emptied(void* vehicle) {
    if (!vehicle) return false;
    std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
    return g_vehicles_emptied.count(vehicle) > 0;
}

void add_vehicle_emptied(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
    g_vehicles_emptied.insert(vehicle);
}

bool is_vehicle_ordered_to_scene(void* vehicle) {
    if (!vehicle) return false;
    std::lock_guard<std::mutex> lock(g_vehicles_mutex);
    return g_vehicles_ordered_to_scene.count(vehicle) > 0;
}

void add_vehicle_ordered_to_scene(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_vehicles_mutex);
    g_vehicles_ordered_to_scene.insert(vehicle);
}

bool is_vehicle_siren_awakened(void* vehicle) {
    if (!vehicle) return false;
    std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
    return g_vehicles_siren_awakened.count(vehicle) > 0;
}

void add_vehicle_siren_awakened(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
    g_vehicles_siren_awakened.insert(vehicle);
}

std::map<void*, StuckTracker> g_stuck_vehicles;
std::mutex g_stuck_vehicles_mutex;

std::set<void*> g_spawned_swats;
std::mutex g_spawned_swats_mutex;

void register_spawned_swat(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_spawned_swats_mutex);
    g_spawned_swats.insert(vehicle);
    LOGI("Registered spawned SWAT vehicle %p", vehicle);
}

bool is_swat_van_nearby(CVector pos, float radius) {
    std::lock_guard<std::mutex> lock(g_spawned_swats_mutex);
    for (auto it = g_spawned_swats.begin(); it != g_spawned_swats.end(); ) {
        void* veh = *it;
        if (!is_vehicle_pointer_valid(veh)) {
            it = g_spawned_swats.erase(it);
        } else {
            CVector veh_pos = get_entity_pos(veh);
            float dx = veh_pos.x - pos.x;
            float dy = veh_pos.y - pos.y;
            float dz = veh_pos.z - pos.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < radius) {
                return true;
            }
            ++it;
        }
    }
    return false;
}

void bind_vehicle_occupants(void* vehicle) {
    if (!vehicle || !g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType || !g_IsDriver || !g_IsPassenger) return;
    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return;

    std::lock_guard<std::mutex> lock(g_bindings_mutex);
    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            if (ped) {
                int ped_type = g_GetPedType(ped);
                if (ped_type == PED_TYPE_COP) {
                    bool is_driver = g_IsDriver(vehicle, ped);
                    bool is_passenger = g_IsPassenger(vehicle, ped);
                    if (is_driver || is_passenger) {
                        auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
                        if (!cop_comp) {
                            cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(ped, ped);
                        }
                        if (cop_comp) {
                            cop_comp->is_in_vehicle = true;
                            cop_comp->has_exited_vehicle = false;
                        }

                        bool found = false;
                        for (auto& binding : g_cop_vehicle_bindings) {
                            if (binding.cop == ped) {
                                binding.vehicle = vehicle;
                                binding.as_driver = is_driver;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            g_cop_vehicle_bindings.push_back({ped, vehicle, is_driver});
                            LOGI("Bound cop %p to vehicle %p (driver: %d)", ped, vehicle, is_driver);
                        }
                    }
                }
            }
        }
    }
}

void record_exit_start_for_occupants(void* vehicle) {
    if (!vehicle || !g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType || !g_IsDriver || !g_IsPassenger) return;
    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return;

    std::lock_guard<std::mutex> lock(g_exits_mutex);
    int64_t cur_time = now_ms();
    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            if (ped) {
                int ped_type = g_GetPedType(ped);
                if (ped_type == PED_TYPE_COP) {
                    if (g_IsDriver(vehicle, ped) || g_IsPassenger(vehicle, ped)) {
                        auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
                        if (!cop_comp) {
                            cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(ped, ped);
                        }
                        if (cop_comp) {
                            cop_comp->is_in_vehicle = false;
                            cop_comp->has_exited_vehicle = true;
                        }

                        bool found = false;
                        for (auto& rec : g_cop_exits) {
                            if (rec.cop == ped) {
                                rec.exit_time = cur_time;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            g_cop_exits.push_back({ped, cur_time});
                            LOGI("Recorded exit start for cop %p", ped);
                        }
                    }
                }
            }
        }
    }
}

void setup_dispatched_cops(void* vehicle, CPed* criminal) {
    if (!vehicle) return;
    
    {
        std::lock_guard<std::mutex> lock(g_spawned_cop_vehicles_mutex);
        if (std::find(g_spawned_cop_vehicles.begin(), g_spawned_cop_vehicles.end(), vehicle) == g_spawned_cop_vehicles.end()) {
            g_spawned_cop_vehicles.push_back(vehicle);
            LOGI("setup_dispatched_cops: Tracking our custom spawned vehicle %p", vehicle);
        }
    }
    
    // 1. 绑定车内人员
    bind_vehicle_occupants(vehicle);
    
    // 2. 分配警员与司乘关系（检测到没司机时自动为其分配普通警员乘员，确保绝无空车幽灵车）
    if (!is_vehicle_occupied_by_driver(vehicle)) {
        if (g_AddPoliceOccupants) {
            LOGI("setup_dispatched_cops: Adding occupants to vehicle %p natively", vehicle);
            g_AddPoliceOccupants(reinterpret_cast<CVehicle*>(vehicle), true);
            // 重新刷新乘员绑定
            bind_vehicle_occupants(vehicle);
        }
    }
    
    // 3. 对载具施加 0.0f 拳头伤害瞬间唤醒！指定来源为 criminal。
    // 此时载具内自然持枪的警员会受到受击冲击，自动开启警灯与警笛，司机将自动一脚油门驶向该伤害来源处（即 criminal 的位置）！
    if (criminal && is_ped_pointer_valid_safe(criminal)) {
        if (g_VehicleInflictDamage) {
            CVector veh_pos = get_entity_pos(vehicle);
            g_VehicleInflictDamage(vehicle, reinterpret_cast<CEntity*>(criminal), WEAPON_UNARMED, 0.0f, veh_pos);
            LOGI("setup_dispatched_cops: Inflicted 0.0f punch damage to vehicle %p to wake it up towards criminal %p", vehicle, criminal);
        }
    }
}

eWeaponType determine_weapon_for_cop(CPed* cop, CPed* criminal, bool is_firearm_crime) {
    if (!cop || !criminal) return WEAPON_PISTOL;
    if (is_firearm_crime) return WEAPON_PISTOL;
    return WEAPON_NIGHTSTICK; // 非枪击案，全程手持警棍规范执法
}

int compute_nearby_cop_quota_for_crime(const std::shared_ptr<CrimeEvent>& crime) {
    if (!crime) return 1;
    int density = count_criminals_near(crime->location, 40.0f);
    if (density >= 6) return 4;
    if (density >= 3) return 2;
    return 1;
}

float compute_nearby_cop_search_radius(const std::shared_ptr<CrimeEvent>& crime) {
    if (!crime) return dispatch_timing::NEARBY_SEARCH_FIREARM_M;
    return crime->is_firearm ? dispatch_timing::NEARBY_SEARCH_FIREARM_M
                             : dispatch_timing::NEARBY_SEARCH_MELEE_M;
}

int dispatch_nearby_available_cops_to_crime(
    const std::shared_ptr<CrimeEvent>& crime,
    int max_cops,
    float search_radius) {
    if (!crime || crime->cancelled || max_cops <= 0) return 0;

    CPed* criminal = crime->criminal;
    if (!criminal || !is_ped_pointer_valid_safe(criminal)) return 0;
    if (g_IsAlive && !g_IsAlive(criminal)) return 0;
    if (!g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return 0;

    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return 0;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return 0;

    CVector crime_pos = crime->location;
    float radius_sq = search_radius * search_radius;
    int64_t cur_time = now_ms();

    struct NearbyCopCandidate {
        CPed* cop = nullptr;
        float dist_sq = 0.0f;
        void* vehicle = nullptr;
    };
    std::vector<NearbyCopCandidate> candidates;
    candidates.reserve(32);

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag < 0) continue;

        int handle = (i << 8) | flag;
        CPed* ped = g_GetPoolPed(handle);
        if (!ped || !is_ped_pointer_valid_safe(ped)) continue;
        if (g_IsAlive && !g_IsAlive(ped)) continue;
        if (g_GetPedType(ped) != PED_TYPE_COP) continue;

        bool is_case_criminal = false;
        for (CPed* c : crime->consolidated_criminals) {
            if (c == ped) {
                is_case_criminal = true;
                break;
            }
        }
        if (is_case_criminal) continue;

        CVector cop_pos = get_entity_pos(ped);
        float dx = cop_pos.x - crime_pos.x;
        float dy = cop_pos.y - crime_pos.y;
        float dz = cop_pos.z - crime_pos.z;
        float dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq > radius_sq) continue;

        if (g_GetWeaponLockOnTarget) {
            CEntity* lock_target = g_GetWeaponLockOnTarget(ped);
            if (lock_target == reinterpret_cast<CEntity*>(criminal)) {
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
            auto it = g_cop_attack_assign_time.find({ped, criminal});
            if (it != g_cop_attack_assign_time.end() &&
                (cur_time - it->second < dispatch_timing::NEARBY_ASSIGN_DEDUP_MS)) {
                continue;
            }
        }

        candidates.push_back({ped, dist_sq, find_vehicle_of_cop(ped)});
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const NearbyCopCandidate& a, const NearbyCopCandidate& b) {
            return a.dist_sq < b.dist_sq;
        });

    std::set<CPed*> dispatched_cops;
    std::set<void*> dispatched_vehicles;
    int mobilized = 0;

    for (const auto& candidate : candidates) {
        if (mobilized >= max_cops) break;
        if (dispatched_cops.count(candidate.cop)) continue;

        if (candidate.vehicle && is_vehicle_pointer_valid(candidate.vehicle)) {
            if (dispatched_vehicles.count(candidate.vehicle)) continue;
            if (!is_vehicle_occupied_by_driver(candidate.vehicle)) continue;

            command_cop_vehicle_to_scene(candidate.vehicle, crime_pos);
            setup_dispatched_cops(candidate.vehicle, criminal);
            add_vehicle_ordered_to_scene(candidate.vehicle);
            bind_vehicle_occupants(candidate.vehicle);

            if (std::find(crime->case_vehicles.begin(), crime->case_vehicles.end(), candidate.vehicle) ==
                crime->case_vehicles.end()) {
                crime->case_vehicles.push_back(candidate.vehicle);
            }
            if (!crime->spawned_vehicle) {
                crime->spawned_vehicle = candidate.vehicle;
            }

            dispatched_vehicles.insert(candidate.vehicle);
            dispatched_cops.insert(candidate.cop);
            mobilized++;
            LOGI("🚓 [NearbyCopDispatch] Routed patrol vehicle %p (cop %p) to case %llu (dist=%.1fm)",
                 candidate.vehicle, candidate.cop, (unsigned long long)crime->case_id, sqrtf(candidate.dist_sq));
        } else {
            make_single_cop_attack_criminal(candidate.cop, criminal, true);
            dispatched_cops.insert(candidate.cop);
            mobilized++;
            LOGI("🚓 [NearbyCopDispatch] Dispatched foot cop %p to case %llu (dist=%.1fm)",
                 candidate.cop, (unsigned long long)crime->case_id, sqrtf(candidate.dist_sq));
        }
    }

    if (mobilized > 0) {
        LOGI("🚓 [NearbyCopDispatch] Case %llu mobilized %d/%d nearby cops within %.0fm",
             (unsigned long long)crime->case_id, mobilized, max_cops, search_radius);
    }
    return mobilized;
}

int dispatch_nearby_available_cops_for_crime_auto(const std::shared_ptr<CrimeEvent>& crime) {
    if (!crime) return 0;
    return dispatch_nearby_available_cops_to_crime(
        crime,
        compute_nearby_cop_quota_for_crime(crime),
        compute_nearby_cop_search_radius(crime));
}

void make_cops_attack_criminal_immediate(CPed* criminal) {
    make_cops_attack_criminal(criminal);
}

bool is_specific_criminal_armed_with_firearm(CPed* target_criminal) {
    if (!target_criminal) return false;
    auto crime = find_crime_containing_criminal(target_criminal);
    if (!crime || crime->cancelled) return false;
    auto& list = crime->consolidated_criminals;
    auto& is_fire_list = crime->criminal_is_firearm;
    for (size_t idx = 0; idx < list.size(); ++idx) {
        if (list[idx] == target_criminal) {
            if (idx < is_fire_list.size()) {
                return is_fire_list[idx];
            }
        }
    }
    if (crime->criminal == target_criminal) {
        return crime->is_firearm;
    }
    return false;
}

enum class CopCombatDispatchMethod {
    NONE = 0,
    ALREADY_ACTIVE,
    ADD_CRIMINAL_TO_KILL,
    KILL_CRIMINAL_TASK,
    EVENT_GUNSHOT,
    EVENT_ZERO_DAMAGE
};

static bool cop_has_lock_on_target(CPed* cop, CPed* criminal) {
    if (!g_GetWeaponLockOnTarget || !cop || !criminal) return false;
    CEntity* target = g_GetWeaponLockOnTarget(cop);
    return target == reinterpret_cast<CEntity*>(criminal);
}

static bool cop_has_kill_criminal_task(CPed* cop) {
    if (!g_FindTaskByType || !cop) return false;
    void* intelligence = get_ped_intelligence(cop);
    if (!intelligence) return false;
    return g_FindTaskByType(intelligence, TASK_COMPLEX_KILL_CRIMINAL) != nullptr;
}

static bool cop_is_already_pursuing(CPed* cop, CPed* criminal) {
    return cop_has_lock_on_target(cop, criminal);
}

static bool try_dispatch_via_add_criminal(CPed* cop, CPed* criminal) {
    if (!g_AddCriminalToKill || !cop || !criminal) return false;
    g_AddCriminalToKill(cop, criminal);
    LOGI("🎯 [Native Dispatch] AddCriminalToKill cop %p -> criminal %p", cop, criminal);
    return true;
}

static bool try_dispatch_via_kill_task(CPed* cop, CPed* criminal) {
    if (!g_TaskNew || !g_TaskKillCriminal_ctor || !cop || !criminal) return false;

    void* intelligence = get_ped_intelligence(cop);
    if (!intelligence) return false;

    void* task = g_TaskNew(512);
    if (!task) return false;

    g_TaskKillCriminal_ctor(task, criminal, false);

    if (g_AddTaskPrimaryMaybeInGroup) {
        g_AddTaskPrimaryMaybeInGroup(intelligence, reinterpret_cast<CTask*>(task), false);
        LOGI("🎯 [Native Dispatch] AddTaskPrimaryMaybeInGroup KillCriminal cop %p -> criminal %p", cop, criminal);
        return true;
    }

    if (!g_SetTask) return false;
    void* task_manager = reinterpret_cast<void*>(reinterpret_cast<char*>(intelligence) + 8);
    if (!task_manager) return false;

    g_SetTask(task_manager, reinterpret_cast<CTask*>(task), 3, false);
    LOGI("🎯 [Native Dispatch] SetTask KillCriminal cop %p -> criminal %p", cop, criminal);
    return true;
}

static CopCombatDispatchMethod try_dispatch_via_events(CPed* cop, CPed* criminal, eWeaponType weapon) {
    if (g_CEventGunShot_ctor && g_CEventGunShot_dtor && g_CEventGroup_Add) {
        void* event_group = get_ped_event_group(cop);
        if (event_group) {
            alignas(16) char event_buf[256];
            memset(event_buf, 0, sizeof(event_buf));
            CVector cop_pos = get_entity_pos(cop);
            CVector target_pos(cop_pos.x, cop_pos.y, cop_pos.z + 1.0f);

            g_CEventGunShot_ctor(event_buf, reinterpret_cast<CEntity*>(criminal), cop_pos, target_pos, false);
            g_CEventGroup_Add(event_group, event_buf, false);
            g_CEventGunShot_dtor(event_buf);
            LOGI("🎯 [Event Fallback] GunShot event cop %p -> criminal %p", cop, criminal);
            return CopCombatDispatchMethod::EVENT_GUNSHOT;
        }
    }

    if (g_orig_generate_damage_event) {
        g_orig_generate_damage_event(cop, reinterpret_cast<CEntity*>(criminal), weapon, 0, 3, 0);
        LOGI("🎯 [Event Fallback] 0-damage cop %p by criminal %p", cop, criminal);
        return CopCombatDispatchMethod::EVENT_ZERO_DAMAGE;
    }

    return CopCombatDispatchMethod::NONE;
}

static CopCombatDispatchMethod dispatch_cop_combat_ladder(
    CPed* cop,
    CPed* criminal,
    eWeaponType weapon,
    bool force_redispatch) {
    if (!force_redispatch && cop_is_already_pursuing(cop, criminal)) {
        LOGI("🎯 [Native Dispatch] Cop %p already pursuing criminal %p — skip combat inject", cop, criminal);
        return CopCombatDispatchMethod::ALREADY_ACTIVE;
    }

    bool in_vehicle = find_vehicle_of_cop(cop) != nullptr;
    bool used_kill_task = false;
    bool used_add_criminal = false;

    if (!in_vehicle) {
        // 地面警：原生 API 对普通巡逻警常无效，先尝试但不以其返回值作为终止条件
        used_kill_task = try_dispatch_via_kill_task(cop, criminal);
        used_add_criminal = try_dispatch_via_add_criminal(cop, criminal);

        // 事件唤醒是地面警最可靠路径（358aecc 之前即如此）；无 LockOn 时始终补发
        if (!cop_has_lock_on_target(cop, criminal)) {
            CopCombatDispatchMethod event_method = try_dispatch_via_events(cop, criminal, weapon);
            if (event_method != CopCombatDispatchMethod::NONE) {
                LOGI("🎯 [Foot Dispatch] Event wake supplement for cop %p -> criminal %p (native_kill=%d, native_add=%d)",
                     cop, criminal, used_kill_task ? 1 : 0, used_add_criminal ? 1 : 0);
                return event_method;
            }
        }

        if (used_kill_task) return CopCombatDispatchMethod::KILL_CRIMINAL_TASK;
        if (used_add_criminal) return CopCombatDispatchMethod::ADD_CRIMINAL_TO_KILL;
        return CopCombatDispatchMethod::NONE;
    }

    // 载具内：原生优先，事件仅兜底
    if (try_dispatch_via_add_criminal(cop, criminal)) {
        return CopCombatDispatchMethod::ADD_CRIMINAL_TO_KILL;
    }

    return try_dispatch_via_events(cop, criminal, weapon);
}

static void register_cop_combat_ecs(CPed* cop, CPed* criminal) {
    auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
    if (!cop_comp) {
        cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(cop, cop);
    }
    auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (!combat_comp) {
        combat_comp = ecs::EntityManager::get().add_component<ecs::CombatComponent>(cop);
    }
    if (combat_comp) {
        combat_comp->target_entity = criminal;
    }
}

void make_single_cop_attack_criminal(CPed* cop, CPed* criminal, bool force_weapon_update) {
    if (!cop || !is_ped_pointer_valid_safe(cop) || !criminal || !is_ped_pointer_valid_safe(criminal)) return;
    if (g_IsAlive && !g_IsAlive(cop)) return;
    if (g_GetPedType && g_GetPedType(cop) != PED_TYPE_COP) return;

    register_cop_combat_ecs(cop, criminal);

    // Determine weapon based on specific threat
    bool is_firearm = is_specific_criminal_armed_with_firearm(criminal);
    eWeaponType target_weapon = determine_weapon_for_cop(cop, criminal, is_firearm);

    // Get last assigned weapon
    eWeaponType last_assigned_weapon = WEAPON_UNARMED;
    {
        std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
        auto it = g_cop_assigned_weapon.find(cop);
        if (it != g_cop_assigned_weapon.end()) {
            last_assigned_weapon = it->second;
        }
    }

    int64_t last_armed = 0;
    {
        std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
        auto it = g_armed_cops_time.find(cop);
        if (it != g_armed_cops_time.end()) {
            last_armed = it->second;
        }
    }

    bool is_upgrading = (target_weapon == WEAPON_PISTOL && last_assigned_weapon != WEAPON_PISTOL);
    bool should_switch = force_weapon_update || is_upgrading || (target_weapon != last_assigned_weapon && (now_ms() - last_armed > 1500));

    if (should_switch) {
        if (g_GiveWeapon && g_SetCurrentWeapon) {
            g_GiveWeapon(cop, target_weapon, 9999, true);
            g_SetCurrentWeapon(cop, target_weapon);
        }
        LOGI("🎯 [Cop Weapon Switch] Cop %p switched weapon to %d (perp=%p, force=%d)", cop, (int)target_weapon, criminal, force_weapon_update);

        {
            std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
            g_cop_assigned_weapon[cop] = target_weapon;
        }
        {
            std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
            g_armed_cops_time[cop] = now_ms();
        }
    }

    bool force_redispatch = force_weapon_update;
    bool need_combat_dispatch = force_redispatch || !cop_is_already_pursuing(cop, criminal);
    if (need_combat_dispatch) {
        CopCombatDispatchMethod method = dispatch_cop_combat_ladder(cop, criminal, target_weapon, force_redispatch);
        if (method != CopCombatDispatchMethod::NONE) {
            std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
            g_cop_attack_assign_time[{cop, criminal}] = now_ms();
        }
    }
}

void update_cops_targeting_criminal_event_driven(CPed* criminal) {
    if (!criminal || !is_ped_pointer_valid_safe(criminal)) return;

    std::vector<CPed*> cops_to_update;
    int64_t cur_time = now_ms();

    {
        std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
        for (const auto& item : g_cop_attack_assign_time) {
            if (item.first.second == criminal) {
                if (cur_time - item.second < 20000) {
                    cops_to_update.push_back(item.first.first);
                }
            }
        }
    }

    for (CPed* cop : cops_to_update) {
        make_single_cop_attack_criminal(cop, criminal, true);
    }
}

static void update_primary_criminal_for_case(CrimeEvent& crime) {
    if (crime.cancelled) return;

    CPed* best_criminal = nullptr;
    ecs::CriminalThreatLevel highest_threat = ecs::CriminalThreatLevel::UNARMED_INACTIVE;

    for (CPed* ped : crime.consolidated_criminals) {
        if (!ped || !is_ped_pointer_valid_safe(ped)) {
            continue;
        }
        if (g_IsAlive && !g_IsAlive(ped)) {
            continue;
        }

        auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
        ecs::CriminalThreatLevel threat = ecs::CriminalThreatLevel::UNARMED_INACTIVE;
        if (crim_comp) {
            threat = crim_comp->get_threat_level();
        }

        if (!best_criminal || (int)threat > (int)highest_threat) {
            best_criminal = ped;
            highest_threat = threat;
        } else if ((int)threat == (int)highest_threat) {
            if (crime.criminal == ped) {
                best_criminal = ped;
            }
        }
    }

    if (best_criminal && best_criminal != crime.criminal) {
        CPed* old_criminal = crime.criminal;
        crime.criminal = best_criminal;
        g_tracked_criminal.store(best_criminal);

        LOGI("⚡️ [ECS ThreatPrioritizer] Case %llu primary target switched: %p -> %p (Threat: %d)",
             (unsigned long long)crime.case_id, old_criminal, best_criminal, (int)highest_threat);

        std::vector<CPed*> cops_to_redirect;
        {
            std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
            int64_t cur_time = now_ms();
            for (const auto& item : g_cop_attack_assign_time) {
                if (item.first.second == old_criminal) {
                    if (cur_time - item.second < 20000) {
                        cops_to_redirect.push_back(item.first.first);
                    }
                }
            }
        }

        for (CPed* cop : cops_to_redirect) {
            make_single_cop_attack_criminal(cop, best_criminal, true);
        }
    }
}

void update_primary_criminal_by_threat() {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            update_primary_criminal_for_case(*crime);
        }
    }
}


