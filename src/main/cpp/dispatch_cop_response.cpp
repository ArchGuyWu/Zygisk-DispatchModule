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

// 指派 CTaskComplexEnterCar 任务，让其上车
void make_cop_enter_vehicle(CPed* cop, void* vehicle, bool as_driver) {
    if (!cop || !is_ped_pointer_valid_safe(cop) || !vehicle || !is_vehicle_pointer_valid(vehicle) || !g_TaskNew || !g_TaskEnterCar_ctor || !g_SetTask) return;
    if (g_IsAlive && !g_IsAlive(cop)) return;

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
static void command_vehicle_ai(void* vehicle, const CVector& target_loc, float dist_to_target) {
    if (!g_GetCarToGoToCoors || !vehicle) return;

    // 当距离目标非常近（例如 < 32 米）时：
    // 我们将驾驶目的地重置为车辆当前的 3D 坐标（原地诱骗急刹），防止其开到人行道或越野冲撞，
    // 调用 TellOccupantsToLeaveCar 让警员提前下车
    if (dist_to_target < 16.0f) {
        CVector veh_pos = get_entity_pos(vehicle);
        g_GetCarToGoToCoors(vehicle, &veh_pos, 4, false); // Mode 4 (DF_STOP_CAR) 瞬间手刹锁死
        if (g_TellOccupantsToLeaveCar) {
            g_TellOccupantsToLeaveCar(vehicle);
        }
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

static bool is_cop_currently_exiting(CPed* cop) {
    std::lock_guard<std::mutex> lock(g_exits_mutex);
    int64_t cur_time = now_ms();
    for (auto it = g_cop_exits.begin(); it != g_cop_exits.end(); ) {
        if (!is_ped_pointer_valid_safe(it->cop)) {
            it = g_cop_exits.erase(it);
        } else {
            if (it->cop == cop) {
                if (cur_time - it->exit_time < 3500) {
                    return true;
                }
            }
            ++it;
        }
    }
    return false;
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

void make_cops_attack_criminal(CPed* criminal) {
    if (!criminal || !is_ped_pointer_valid_safe(criminal)) return;
    if (!g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return;

    std::shared_ptr<CrimeEvent> crime_case = find_crime_containing_criminal(criminal);

    // 若存在其他活跃枪击案且当前嫌犯不在那些案件中，暂缓低优先级调度。
    if (any_active_firearm_case_blocking(criminal)) {
        return;
    }

    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return;

    CVector crime_pos = get_entity_pos(criminal);

    // =====================================================================
    // thread_local 快照向量，减少每帧堆分配
    // =====================================================================
    thread_local static std::vector<std::pair<std::pair<CPed*, CPed*>, int64_t>> cop_attack_assign_time_snapshot;
    thread_local static std::vector<std::pair<CPed*, int64_t>> armed_cops_time_snapshot;
    thread_local static std::vector<std::pair<CPed*, eWeaponType>> cop_assigned_weapon_snapshot;
    thread_local static std::vector<std::pair<void*, int64_t>> dispatched_vehicles_time_snapshot;
    thread_local static std::vector<std::pair<void*, StuckTracker>> stuck_vehicles_snapshot;
    
    thread_local static std::vector<void*> vehicles_emptied_snapshot;
    thread_local static std::vector<void*> vehicles_ordered_to_scene_snapshot;
    thread_local static std::vector<void*> vehicles_siren_awakened_snapshot;

    // 延迟批量提交数据容器
    thread_local static std::vector<std::pair<CPed*, int64_t>> pending_armed_cops_time;
    thread_local static std::vector<std::pair<CPed*, eWeaponType>> pending_cop_assigned_weapon;
    thread_local static std::vector<std::pair<void*, int64_t>> pending_dispatched_vehicles_time;
    thread_local static std::vector<void*> pending_vehicles_emptied;
    thread_local static std::vector<void*> pending_vehicles_ordered_to_scene;
    thread_local static std::vector<void*> pending_vehicles_siren_awakened;
    thread_local static std::vector<std::pair<void*, StuckTracker>> pending_stuck_vehicles;
    thread_local static std::vector<std::pair<std::pair<CPed*, CPed*>, int64_t>> pending_cop_attack_assign_time;
    thread_local static std::vector<TemporaryRoadClosure> pending_temp_closures;

    thread_local static std::vector<void*> counted_vehicles;

    thread_local static bool initialized = false;
    if (!initialized) {
        cop_attack_assign_time_snapshot.reserve(128);
        armed_cops_time_snapshot.reserve(128);
        cop_assigned_weapon_snapshot.reserve(128);
        dispatched_vehicles_time_snapshot.reserve(128);
        stuck_vehicles_snapshot.reserve(128);
        vehicles_emptied_snapshot.reserve(128);
        vehicles_ordered_to_scene_snapshot.reserve(128);
        vehicles_siren_awakened_snapshot.reserve(128);

        pending_armed_cops_time.reserve(128);
        pending_cop_assigned_weapon.reserve(128);
        pending_dispatched_vehicles_time.reserve(128);
        pending_vehicles_emptied.reserve(128);
        pending_vehicles_ordered_to_scene.reserve(128);
        pending_vehicles_siren_awakened.reserve(128);
        pending_stuck_vehicles.reserve(128);
        pending_cop_attack_assign_time.reserve(128);
        pending_temp_closures.reserve(128);

        counted_vehicles.reserve(128);
        initialized = true;
    }

    cop_attack_assign_time_snapshot.clear();
    armed_cops_time_snapshot.clear();
    cop_assigned_weapon_snapshot.clear();
    dispatched_vehicles_time_snapshot.clear();
    stuck_vehicles_snapshot.clear();
    vehicles_emptied_snapshot.clear();
    vehicles_ordered_to_scene_snapshot.clear();
    vehicles_siren_awakened_snapshot.clear();

    pending_armed_cops_time.clear();
    pending_cop_assigned_weapon.clear();
    pending_dispatched_vehicles_time.clear();
    pending_vehicles_emptied.clear();
    pending_vehicles_ordered_to_scene.clear();
    pending_vehicles_siren_awakened.clear();
    pending_stuck_vehicles.clear();
    pending_cop_attack_assign_time.clear();
    pending_temp_closures.clear();

    counted_vehicles.clear();

    auto vector_contains = [](const std::vector<void*>& vec, void* val) {
        for (void* item : vec) {
            if (item == val) return true;
        }
        return false;
    };

    // 一站式极速拷贝多线程全局状态
    {
        std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
        cop_attack_assign_time_snapshot.assign(g_cop_attack_assign_time.begin(), g_cop_attack_assign_time.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
        armed_cops_time_snapshot.assign(g_armed_cops_time.begin(), g_armed_cops_time.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
        cop_assigned_weapon_snapshot.assign(g_cop_assigned_weapon.begin(), g_cop_assigned_weapon.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
        dispatched_vehicles_time_snapshot.assign(g_dispatched_vehicles_time.begin(), g_dispatched_vehicles_time.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
        stuck_vehicles_snapshot.assign(g_stuck_vehicles.begin(), g_stuck_vehicles.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
        vehicles_emptied_snapshot.assign(g_vehicles_emptied.begin(), g_vehicles_emptied.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_mutex);
        vehicles_ordered_to_scene_snapshot.assign(g_vehicles_ordered_to_scene.begin(), g_vehicles_ordered_to_scene.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
        vehicles_siren_awakened_snapshot.assign(g_vehicles_siren_awakened.begin(), g_vehicles_siren_awakened.end());
    }

    // 动态提取枪械案件状态
    bool is_active_firearm = false;
    if (crime_case && !crime_case->cancelled) {
        if (crime_case->is_firearm) {
            is_active_firearm = true;
        } else {
            auto& list = crime_case->consolidated_criminals;
            auto& is_fire_list = crime_case->criminal_is_firearm;
            for (size_t idx = 0; idx < list.size(); ++idx) {
                if (list[idx] == criminal && idx < is_fire_list.size() && is_fire_list[idx]) {
                    is_active_firearm = true;
                    break;
                }
            }
        }
    }

    // =====================================================================
    // Pass 1: 扫描当前处于响应状态的已调度单位数量（免偏移量判定，不依赖结构体偏移）
    // =====================================================================
    int active_vehicles_count = 0;
    int active_foot_cops_count = 0;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            
            // 判定该 NPC 是否属于并案列表中的罪犯之一
            bool is_cop_not_criminal = true;
            if (crime_case && !crime_case->cancelled) {
                for (CPed* c : crime_case->consolidated_criminals) {
                    if (ped == c) {
                        is_cop_not_criminal = false;
                        break;
                    }
                }
            } else {
                if (ped == criminal) is_cop_not_criminal = false;
            }

            if (ped && is_ped_pointer_valid_safe(ped) && is_cop_not_criminal) {
                if (g_IsAlive && !g_IsAlive(ped)) {
                    continue;
                }
                int ped_type = g_GetPedType(ped);
                if (ped_type == PED_TYPE_COP) {
                    void* veh = find_vehicle_of_cop(ped);
                    if (veh && is_vehicle_pointer_valid(veh)) {
                        // 警员在载具中，且该载具已被调度
                        if (vector_contains(vehicles_ordered_to_scene_snapshot, veh) || 
                            vector_contains(vehicles_siren_awakened_snapshot, veh) || 
                            veh == crime_case->spawned_vehicle) {
                            if (!vector_contains(counted_vehicles, veh)) {
                                counted_vehicles.push_back(veh);
                                active_vehicles_count++;
                            }
                        }
                    } else {
                        // 地面散步/巡逻警员
                        int64_t last_assign = 0;
                        if (crime_case && !crime_case->cancelled) {
                            for (CPed* c : crime_case->consolidated_criminals) {
                                auto key = std::make_pair(ped, c);
                                for (const auto& item : cop_attack_assign_time_snapshot) {
                                    if (item.first == key) {
                                        last_assign = std::max(last_assign, item.second);
                                        break;
                                    }
                                }
                            }
                        } else {
                            auto key = std::make_pair(ped, criminal);
                            for (const auto& item : cop_attack_assign_time_snapshot) {
                                if (item.first == key) {
                                    last_assign = item.second;
                                    break;
                                }
                            }
                        }
                        
                        bool already_targeting = false;
                        if (g_GetWeaponLockOnTarget) {
                            CEntity* target = g_GetWeaponLockOnTarget(ped);
                            if (target) {
                                if (crime_case && !crime_case->cancelled) {
                                    for (CPed* c : crime_case->consolidated_criminals) {
                                        if (target == reinterpret_cast<CEntity*>(c)) {
                                            already_targeting = true;
                                            break;
                                        }
                                    }
                                } else {
                                    if (target == reinterpret_cast<CEntity*>(criminal)) {
                                        already_targeting = true;
                                    }
                                }
                            }
                        }

                        // 如果 15 秒内分派过任务，或者已经正在瞄准任何嫌疑人，则视为已被调度地面警员
                        if (already_targeting || (now_ms() - last_assign < 15000)) {
                            active_foot_cops_count++;
                        }
                    }
                }
            }
        }
    }

    // =====================================================================
    // Case merge and dispatch quotas
    // =====================================================================
    int max_vehicles = 2;
    int max_foot_cops = 2;

    if (crime_case && !crime_case->cancelled) {
        auto& list = crime_case->consolidated_criminals;
        auto& is_fire_list = crime_case->criminal_is_firearm;
        
        int total_criminals = list.size();
        int armed_criminals = 0;
        for (size_t idx = 0; idx < list.size(); ++idx) {
            if (idx < is_fire_list.size() && is_fire_list[idx]) {
                armed_criminals++;
            }
        }
        
        int cops_killed = crime_case->cops_killed;
        
        if (cops_killed > 0) {
            max_vehicles = 3;
            max_foot_cops = 4;
            LOGI("📊 [dispatchCenter - Quota] Escalated to Max quota (Vehicles=%d, FootCops=%d) due to cop casualties (%d)", 
                 max_vehicles, max_foot_cops, cops_killed);
        } else if (armed_criminals == 0) {
            if (total_criminals <= 1) {
                max_vehicles = 1;
                max_foot_cops = 1;
            } else {
                max_vehicles = 2;
                max_foot_cops = 2;
            }
            LOGI("📊 [dispatchCenter - Quota] Melee-only crime. Quota set to (Vehicles=%d, FootCops=%d) for %d criminals", 
                 max_vehicles, max_foot_cops, total_criminals);
        } else {
            float armed_ratio = (float)armed_criminals / (float)total_criminals;
            if (armed_ratio < 0.5f) {
                max_vehicles = 2;
                max_foot_cops = 2;
            } else {
                if (total_criminals <= 2) {
                    max_vehicles = 2;
                    max_foot_cops = 3;
                } else {
                    max_vehicles = 3;
                    max_foot_cops = 4;
                }
            }
            LOGI("📊 [dispatchCenter - Quota] Firearm crime (Armed: %d/%d, Ratio: %.1f%%). Quota set to (Vehicles=%d, FootCops=%d)", 
                 armed_criminals, total_criminals, armed_ratio * 100.0f, max_vehicles, max_foot_cops);
        }
    } else {
        max_vehicles = 2;
        max_foot_cops = 2;
    }

    // =====================================================================
    // Pass 2: 遍历、过滤并对合格单位进行精准追加调度
    // =====================================================================
    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            
            // 判定该 NPC 是否属于并案列表中的罪犯之一
            bool is_cop_not_criminal = true;
            if (crime_case && !crime_case->cancelled) {
                for (CPed* c : crime_case->consolidated_criminals) {
                    if (ped == c) {
                        is_cop_not_criminal = false;
                        break;
                    }
                }
            } else {
                if (ped == criminal) is_cop_not_criminal = false;
            }

            if (ped && is_ped_pointer_valid_safe(ped) && is_cop_not_criminal) {
                if (g_IsAlive && !g_IsAlive(ped)) {
                    continue;
                }
                int ped_type = g_GetPedType(ped);
                if (ped_type == PED_TYPE_COP) {
                    CVector cop_pos = get_entity_pos(ped);
                    CVector target_crime_pos = crime_pos;
                    CPed* target_criminal = criminal;

                    // 🚀 【黑科技 1】动态目标重定向：多犯罪NPC并案下，就近选择攻击对象，极速消灭威胁
                    if (crime_case && !crime_case->cancelled) {
                        float best_d = 999999.0f;
                        for (CPed* c : crime_case->consolidated_criminals) {
                            if (c && is_ped_pointer_valid_safe(c)) {
                                CVector c_pos = get_entity_pos(c);
                                float dx_c = cop_pos.x - c_pos.x;
                                float dy_c = cop_pos.y - c_pos.y;
                                float dz_c = cop_pos.z - c_pos.z;
                                float dist_c = dx_c * dx_c + dy_c * dy_c + dz_c * dz_c;
                                if (dist_c < best_d) {
                                    best_d = dist_c;
                                    target_crime_pos = c_pos;
                                    target_criminal = c;
                                }
                            }
                        }
                    }

                    float dx = cop_pos.x - target_crime_pos.x;
                    float dy = cop_pos.y - target_crime_pos.y;
                    float dz = cop_pos.z - target_crime_pos.z;
                    float dist_sq = dx * dx + dy * dy + dz * dz;

                    // 1. 如果警车驾驶中的警员距离还很远，先给他发合适武器（防手无寸铁），并开启警车自主避障驾驶
                    void* veh = find_vehicle_of_cop(ped);
                    if (veh) {
                        if (is_vehicle_pointer_valid(veh)) {
                            CVector veh_pos = get_entity_pos(veh);
                            float v_dx = veh_pos.x - target_crime_pos.x;
                            float v_dy = veh_pos.y - target_crime_pos.y;
                            float v_dz = veh_pos.z - target_crime_pos.z;
                            float v_dist = sqrtf(v_dx * v_dx + v_dy * v_dy + v_dz * v_dz);

                            int64_t last_armed = 0;
                            for (const auto& item : armed_cops_time_snapshot) {
                                if (item.first == ped) {
                                    last_armed = item.second;
                                    break;
                                }
                            }
                            if (now_ms() - last_armed > 5000) { // 每 5 秒限制最多执行一次
                                bool is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
                                eWeaponType target_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);
                                if (g_GiveWeapon && g_SetCurrentWeapon) {
                                    g_GiveWeapon(ped, target_weapon, 9999, true);
                                    g_SetCurrentWeapon(ped, target_weapon);
                                }
                                bool found = false;
                                for (auto& item : armed_cops_time_snapshot) {
                                    if (item.first == ped) {
                                        item.second = now_ms();
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    armed_cops_time_snapshot.push_back({ped, now_ms()});
                                }
                                pending_armed_cops_time.push_back({ped, now_ms()});

                                bool found_w = false;
                                for (auto& item : cop_assigned_weapon_snapshot) {
                                    if (item.first == ped) {
                                        item.second = target_weapon;
                                        found_w = true;
                                        break;
                                    }
                                }
                                if (!found_w) {
                                    cop_assigned_weapon_snapshot.push_back({ped, target_weapon});
                                }
                                pending_cop_assigned_weapon.push_back({ped, target_weapon});
                            }

                            int64_t first_seen = 0;
                            bool found_disp = false;
                            for (const auto& item : dispatched_vehicles_time_snapshot) {
                                if (item.first == veh) {
                                    first_seen = item.second;
                                    found_disp = true;
                                    break;
                                }
                            }
                            if (!found_disp) {
                                int64_t now_time = now_ms();
                                dispatched_vehicles_time_snapshot.push_back({veh, now_time});
                                pending_dispatched_vehicles_time.push_back({veh, now_time});
                                first_seen = now_time;
                            }
                            int64_t elapsed = now_ms() - first_seen;

                            bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                            float exit_dist = is_bike ? 12.0f : 16.0f;
                            // 复合下车判定：距离接近 (摩托车12米/轿车16米内)，或接近且可能卡死 (行驶超6秒且在60米内)，或超时过久 (行驶超12秒)
                            bool should_exit = (v_dist < exit_dist) || 
                                               (elapsed > 6000 && v_dist < 60.0f) || 
                                               (elapsed > 12000);

                            if (should_exit) {
                                if (!vector_contains(vehicles_emptied_snapshot, veh)) {
                                    // 1. 原地目标诱骗急刹：将 Autopilot 目标设为当前坐标，让其平稳减速刹停，绝不上人行道
                                    if (g_GetCarToGoToCoors) {
                                        g_GetCarToGoToCoors(veh, &veh_pos, 4, false); // Mode 4 (DF_STOP_CAR) 瞬间手刹锁死
                                    }
                                    // 2. 战术提前离车：拉开交火线包抄
                                    if (g_TellOccupantsToLeaveCar) {
                                        g_TellOccupantsToLeaveCar(veh);
                                    }
                                    if (g_VehicleInflictDamage) {
                                        g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, veh_pos);
                                    }
                                    vehicles_emptied_snapshot.push_back(veh); // 局部同步
                                    pending_vehicles_emptied.push_back(veh);

                                    if (veh == crime_case->spawned_vehicle) {
                                        if (crime_case) crime_case->occupants_ordered_out = true;
                                    }
                                    LOGI("Vehicle exit triggered (dist=%.1f, elapsed=%lld ms): Stopped Autopilot & Ordered cops to leave vehicle %p", 
                                         v_dist, (long long)elapsed, veh);
                                }
                            } else {
                                // 检查是否已被调度，若不是，则进入配额判定
                                bool already_dispatched = vector_contains(vehicles_ordered_to_scene_snapshot, veh) || 
                                                          vector_contains(vehicles_siren_awakened_snapshot, veh) || 
                                                          veh == crime_case->spawned_vehicle;

                                // =====================================================================
                                // 检测卡死的警车并尝试重新调度
                                // =====================================================================
                                if (already_dispatched) {
                                    CVector current_pos = get_entity_pos(veh);
                                    int64_t now_time = now_ms();
                                    bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                    if (is_bike && g_GetMatrix) {
                                        CMatrix* mat = g_GetMatrix(veh);
                                        if (mat && mat->at_z < 0.8f) { // Tilting > 36.8 degrees
                                            stabilize_motorcycle(veh);
                                        }
                                    }

                                    StuckTracker tracker;
                                    bool found_stuck = false;
                                    size_t stuck_idx = 0;
                                    for (size_t idx = 0; idx < stuck_vehicles_snapshot.size(); ++idx) {
                                        if (stuck_vehicles_snapshot[idx].first == veh) {
                                            tracker = stuck_vehicles_snapshot[idx].second;
                                            found_stuck = true;
                                            stuck_idx = idx;
                                            break;
                                        }
                                    }

                                    if (!found_stuck) {
                                        tracker.last_pos = current_pos;
                                        tracker.last_check_time = now_time;
                                        tracker.stuck_since = 0;
                                        tracker.last_intervention_time = 0;
                                        
                                        stuck_vehicles_snapshot.push_back({veh, tracker}); // 局部同步
                                        pending_stuck_vehicles.push_back({veh, tracker});
                                    } else {
                                        float time_diff = (now_time - tracker.last_check_time) / 1000.0f;
                                        if (time_diff >= 1.0f) { // 每秒检查一次
                                            float dx_s = current_pos.x - tracker.last_pos.x;
                                            float dy_s = current_pos.y - tracker.last_pos.y;
                                            float dz_s = current_pos.z - tracker.last_pos.z;
                                            float dist_moved = sqrtf(dx_s * dx_s + dy_s * dy_s + dz_s * dz_s);
                                            float speed = dist_moved / time_diff;

                                            // [Proactive Water Escape]: Hollywood cinematic shoreline rescue
                                            if (speed >= 3.0f && dist_moved > 0.1f) {
                                                float dir_x = dx_s / dist_moved;
                                                float dir_y = dy_s / dist_moved;
                                                float dir_z = dz_s / dist_moved;

                                                float lookahead = speed * 1.5f; // Lookahead 1.5 seconds
                                                CVector p_pos = {
                                                    current_pos.x + dir_x * lookahead,
                                                    current_pos.y + dir_y * lookahead,
                                                    current_pos.z + dir_z * lookahead
                                                };

                                                // If predicted to hit low elevation (water level), but perp is on land and veh is currently safe
                                                bool will_fall = (p_pos.z < 1.0f && current_pos.z >= 2.0f && target_crime_pos.z >= 2.5f);
                                                if (will_fall && !vector_contains(vehicles_emptied_snapshot, veh)) {
                                                    if (g_VehicleInflictDamage) {
                                                        g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, current_pos);
                                                    }
                                                    vehicles_emptied_snapshot.push_back(veh);
                                                    pending_vehicles_emptied.push_back(veh);
                                                    if (veh == crime_case->spawned_vehicle) {
                                                        if (crime_case) crime_case->occupants_ordered_out = true;
                                                    }
                                                    LOGW("[dispatchCenter - ProactiveWaterRescue] CINEMATIC RESCUE! Vehicle %p predicted to plunge into deep water. Safe bulk exit triggered!", veh);
                                                }
                                            }

                                            // [Anti-Spin Guard]: Detect circling behavior (crucial for motorcycles)
                                            float cur_dir_x = 0.0f;
                                            float cur_dir_y = 0.0f;
                                            if (dist_moved > 0.05f) {
                                                cur_dir_x = dx_s / dist_moved;
                                                cur_dir_y = dy_s / dist_moved;
                                            }

                                            if (dist_moved > 0.5f) {
                                                if (tracker.last_dir_x != 0.0f || tracker.last_dir_y != 0.0f) {
                                                    float dir_dot = cur_dir_x * tracker.last_dir_x + cur_dir_y * tracker.last_dir_y;
                                                    if (dir_dot < 0.85f) { // 宽限至 31.8度角，更容易灵敏捕获摩托画圈
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
                                                float dx_vc = target_crime_pos.x - current_pos.x;
                                                float dy_vc = target_crime_pos.y - current_pos.y;
                                                float dz_vc = target_crime_pos.z - current_pos.z;
                                                float dist_vc = sqrtf(dx_vc * dx_vc + dy_vc * dy_vc + dz_vc * dz_vc);

                                                if (dist_vc < 60.0f) {
                                                    // A. Close range: Force immediate emergency exit
                                                    if (!vector_contains(vehicles_emptied_snapshot, veh)) {
                                                        g_GetCarToGoToCoors(veh, &current_pos, 4, false); // Emergency handbrake stop
                                                        if (g_TellOccupantsToLeaveCar) {
                                                            g_TellOccupantsToLeaveCar(veh);
                                                        }
                                                        vehicles_emptied_snapshot.push_back(veh);
                                                        pending_vehicles_emptied.push_back(veh);
                                                        if (veh == crime_case->spawned_vehicle) {
                                                            if (crime_case) crime_case->occupants_ordered_out = true;
                                                        }
                                                        LOGW("🔄 [Anti-Spin Guard] Circle spinning detected in close range (%.1fm). Safe bulk exit triggered!", dist_vc);
                                                    }
                                                } else {
                                                     // B. Far range: Force reverse nudge and physical 120-degree yaw rotation to break physical circling loop
                                                     bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                     if (is_bike) {
                                                         float dx_aim = target_crime_pos.x - current_pos.x;
                                                         float dy_aim = target_crime_pos.y - current_pos.y;
                                                         float dist_aim_2d = sqrtf(dx_aim * dx_aim + dy_aim * dy_aim);
                                                         if (dist_aim_2d > 0.01f) {
                                                             dx_aim /= dist_aim_2d;
                                                             dy_aim /= dist_aim_2d;
                                                         } else {
                                                             dx_aim = 0.0f;
                                                             dy_aim = 1.0f;
                                                         }

                                                         if (g_GetMatrix) {
                                                             CMatrix* mat = g_GetMatrix(veh);
                                                             if (mat) {
                                                                 mat->up_x = dx_aim;
                                                                 mat->up_y = dy_aim;
                                                                 mat->up_z = 0.0f;

                                                                 mat->at_x = 0.0f;
                                                                 mat->at_y = 0.0f;
                                                                 mat->at_z = 1.0f;

                                                                 mat->right_x = dy_aim;
                                                                 mat->right_y = -dx_aim;
                                                                 mat->right_z = 0.0f;
                                                             }
                                                         }

                                                         CVector realigned_pos = {
                                                             current_pos.x + dx_aim * 5.0f,
                                                             current_pos.y + dy_aim * 5.0f,
                                                             current_pos.z + 0.15f
                                                         };
                                                         
                                                         set_entity_pos(veh, realigned_pos);
                                                         stabilize_motorcycle(veh);
                                                         
                                                         command_vehicle_ai(veh, target_crime_pos, dist_vc);
                                                         LOGW("🔄🏍️ [Anti-Spin Guard - BIKE] Realignment Orbit Break. Pointed directly to crime scene (%.1fm) and teleported forward 5m.", dist_vc);
                                                     } else {
                                                         bool is_seen = is_cop_visible_to_player(veh, current_pos.x, current_pos.y, current_pos.z);
                                                         float f_x = 0.0f, f_y = 0.0f, f_z = 0.0f;
                                                         if (g_GetMatrix) {
                                                             CMatrix* mat = g_GetMatrix(veh);
                                                             if (mat) {
                                                                 f_x = mat->up_x;
                                                                 f_y = mat->up_y;
                                                                 f_z = mat->up_z;

                                                                 if (!is_seen) {
                                                                     // 物理朝向旋转 120 度打破画圆惯性：cos = -0.5, sin = 0.866
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
                                                         }
                                                         float f_len = sqrtf(f_x * f_x + f_y * f_y + f_z * f_z);
                                                         if (f_len > 0.01f) {
                                                             f_x /= f_len; f_y /= f_len; f_z /= f_len;
                                                         } else {
                                                             f_x = 0.0f; f_y = 1.0f; f_z = 0.0f;
                                                         }
                                                         CVector reverse_pos;
                                                         if (is_seen) {
                                                             reverse_pos = {
                                                                 current_pos.x - f_x * 3.5f,
                                                                 current_pos.y - f_y * 3.5f,
                                                                 current_pos.z + 0.3f
                                                             };
                                                         } else {
                                                             reverse_pos = {
                                                                 current_pos.x - f_x * 4.5f,
                                                                 current_pos.y - f_y * 4.5f,
                                                                 current_pos.z + 0.5f
                                                             };
                                                         }
                                                         
                                                         set_entity_pos(veh, reverse_pos);
                                                         
                                                         command_vehicle_ai(veh, target_crime_pos, dist_vc);
                                                         LOGW("🔄 [Anti-Spin Guard] Circle spinning detected in far range (%.1fm, seen=%d). Forced reverse nudge & yaw break.", dist_vc, is_seen);
                                                     }}
                                                tracker.spin_count = 0;
                                                tracker.last_intervention_time = now_time;
                                            }

                                            // [ACC Unified Fleet Control - Upgraded to Full Fleet & Bypass Nudge]: Prevents front and side rear-ends of any vehicles (Unrestricted by movement)
                                            float dir_x = 0.0f;
                                            float dir_y = 0.0f;
                                            float dir_z = 0.0f;
                                            if (dist_moved > 0.05f) {
                                                dir_x = dx_s / dist_moved;
                                                dir_y = dy_s / dist_moved;
                                                dir_z = dz_s / dist_moved;
                                            } else {
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
                                            }

                                            // =====================================================================
                                            // 🚨 [dispatchCenter - ACC Criminal Avoidance] 罪犯高优先级物理避让，保障绝不撞击/怼罪犯
                                            // =====================================================================
                                            bool ped_blocked = false;
                                            if (target_criminal && is_ped_pointer_valid_safe(target_criminal)) {
                                                CVector other_pos = get_entity_pos(target_criminal);
                                                float ox = other_pos.x - current_pos.x;
                                                float oy = other_pos.y - current_pos.y;
                                                float oz = other_pos.z - current_pos.z;
                                                float cop_dist = sqrtf(ox * ox + oy * oy + oz * oz);

                                                if (cop_dist < 10.0f) {
                                                    float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                    if (dot_p > 0.0f && dot_p < 10.0f) {
                                                        float lat_dist_sq = (cop_dist * cop_dist) - (dot_p * dot_p);
                                                        if (lat_dist_sq < 4.84f) { // 2.2米内横向偏离（直接阻挡在行进轨迹上）
                                                            ped_blocked = true;
                                                            if ((speed < 1.2f || dist_moved < 1.2f) && cop_dist < 8.0f) {
                                                                float side_sign = (((uintptr_t)veh) & 1) ? 1.0f : -1.0f;
                                                                float lx = -dir_y * side_sign;
                                                                float ly = dir_x * side_sign;
                                                                bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                float side_nudge = is_bike ? 0.8f : 1.8f;
                                                                float height_lift = is_bike ? 0.05f : 0.0f;
                                                                CVector detour_pos = {
                                                                    current_pos.x + lx * side_nudge + dir_x * 0.8f,
                                                                    current_pos.y + ly * side_nudge + dir_y * 0.8f,
                                                                    current_pos.z + height_lift
                                                                };
                                                                set_entity_pos(veh, detour_pos);
                                                                if (is_bike) {
                                                                    stabilize_motorcycle(veh);
                                                                }LOGI("[dispatchCenter - ACC Bypass Ped] Vehicle %p blocked by TARGET CRIMINAL %p (dist=%.1f, speed=%.2f). Detoured (side=%.1f)", 
                                                                     veh, target_criminal, cop_dist, speed, side_sign);
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            if (!ped_blocked && g_ms_pVehiclePool && g_GetPoolVehicle) {
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
                                                                    float cop_dist = sqrtf(ox * ox + oy * oy + oz * oz);

                                                                    if (cop_dist < 10.0f) {
                                                                        float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                                        if (dot_p > 0.0f && dot_p < 10.0f) {
                                                                            float lat_dist_sq = (cop_dist * cop_dist) - (dot_p * dot_p);
                                                                            if (lat_dist_sq < 4.84f) { // Under 2.2m lateral deviation (directly in path)
                                                                                // Dynamic smart bypass detour nudge if extremely slow or stationary (stuck behind civilians)
                                                                                if ((speed < 1.2f || dist_moved < 1.2f) && cop_dist < 8.0f) {
                                                                                    float side_sign = (((uintptr_t)veh) & 1) ? 1.0f : -1.0f;
                                                                                    float lx = -dir_y * side_sign;
                                                                                    float ly = dir_x * side_sign;
                                                                                    bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                    float side_nudge = is_bike ? 0.8f : 1.8f;
                                                                                    float height_lift = is_bike ? 0.05f : 0.0f;
                                                                                    CVector detour_pos = {
                                                                                        current_pos.x + lx * side_nudge + dir_x * 0.8f,
                                                                                        current_pos.y + ly * side_nudge + dir_y * 0.8f,
                                                                                        current_pos.z + height_lift
                                                                                    };
                                                                                    
                                                                                    set_entity_pos(veh, detour_pos);
                                                                                    if (is_bike) {
                                                                                        stabilize_motorcycle(veh);
                                                                                    }
                                                                                    LOGI("[dispatchCenter - ACC Bypass] Vehicle %p blocked by %p (dist=%.1f, speed=%.2f). Smooth detour nudge (side=%.1f) by 1.8m side, 0.8m forward.", 
                                                                                         veh, other_veh, cop_dist, speed, side_sign);
                                                                                }
                                                                                break; // Handle one roadblock vehicle per tick to avoid jitter
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            // =====================================================================
                                            // 🔥 [dispatchCenter - Fire Source Detection & Avoidance] 火源自动检测与避让系统
                                            // =====================================================================
                                            if (g_FireManager && g_FindNearestFire) {
                                                void* nearest_fire = g_FindNearestFire(g_FireManager, current_pos, true, true);
                                                if (nearest_fire) {
                                                    CVector fire_pos;
                                                    if (get_fire_position(nearest_fire, fire_pos)) {
                                                        float fx = fire_pos.x - current_pos.x;
                                                        float fy = fire_pos.y - current_pos.y;
                                                        float fz = fire_pos.z - current_pos.z;
                                                        float fire_dist = sqrtf(fx * fx + fy * fy + fz * fz);

                                                        if (fire_dist < 15.0f) {
                                                            // A. 被火源包围/受困紧急逃生 (距离过近且处于低速/停滞状态)
                                                            if (fire_dist < 6.5f && speed < 2.0f) {
                                                                if (!vector_contains(vehicles_emptied_snapshot, veh)) {
                                                                    if (g_GetCarToGoToCoors) {
                                                                        g_GetCarToGoToCoors(veh, &current_pos, 4, false); // 瞬间急刹
                                                                    }
                                                                    if (g_TellOccupantsToLeaveCar) {
                                                                        g_TellOccupantsToLeaveCar(veh); // 强令离开，防烧死
                                                                    }
                                                                    if (g_VehicleInflictDamage) {
                                                                        g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, current_pos);
                                                                    }
                                                                    vehicles_emptied_snapshot.push_back(veh);
                                                                    pending_vehicles_emptied.push_back(veh);
                                                                    if (veh == crime_case->spawned_vehicle) {
                                                                        if (crime_case) crime_case->occupants_ordered_out = true;
                                                                    }
                                                                    LOGW("🔥 [dispatchCenter - FireEmergencyExit] TRAPPED BY FIRE! Vehicle %p is too close to fire (dist=%.1f, speed=%.2f). Safe bulk exit triggered!", veh, fire_dist, speed);
                                                                }
                                                            } 
                                                            // B. 正常行驶中火源主动避让 (火源处于行进路线上)
                                                            else {
                                                                float dot_p = fx * dir_x + fy * dir_y + fz * dir_z;
                                                                if (dot_p > 0.0f && dot_p < 15.0f) { // 火源在车头15米内
                                                                    float lat_dist_sq = (fire_dist * fire_dist) - (dot_p * dot_p);
                                                                    if (lat_dist_sq < 25.0f) { // 横向偏离在5.0米内 (直接物理阻挡)
                                                                        // 计算垂直向量决定往哪侧闪避
                                                                        float lx = -dir_y;
                                                                        float ly = dir_x;
                                                                        float side_dot = fx * lx + fy * ly;
                                                                        float side_sign = (side_dot > 0.0f) ? -1.0f : 1.0f; // 避开火源那一侧

                                                                        // 计算安全的避让偏离点并微调位置重设 autopilot
                                                                        bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                        float side_nudge = is_bike ? 2.5f : 5.5f;
                                                                        float height_lift = is_bike ? 0.05f : 0.0f;
                                                                        CVector detour_pos = {
                                                                            current_pos.x + lx * side_sign * side_nudge - dir_x * 1.5f,
                                                                            current_pos.y + ly * side_sign * side_nudge - dir_y * 1.5f,
                                                                            current_pos.z + height_lift
                                                                        };

                                                                        set_entity_pos(veh, detour_pos);
                                                                        if (is_bike) {
                                                                            stabilize_motorcycle(veh);
                                                                        }LOGW("🔥 [dispatchCenter - FireAvoidanceDetour] Vehicle %p blocked by fire (dist=%.1f) in front. Detouring to safe side (side_sign=%.1f).", veh, fire_dist, side_sign);
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            // Double-insurance stuck detection: low speed OR very low physical physical movement distance in 1 sec (crucial for narrow U-Turns)
                                            if (speed < 0.8f || dist_moved < 1.2f) {
                                                if (tracker.stuck_since == 0) {
                                                    tracker.stuck_since = now_time;
                                                }
                                            } else {
                                                tracker.stuck_since = 0; // Moving normally, reset stuck timer
                                            }

                                            tracker.last_pos = current_pos;
                                            tracker.last_check_time = now_time;
                                            
                                            stuck_vehicles_snapshot[stuck_idx].second = tracker; // Local sync
                                            pending_stuck_vehicles.push_back({veh, tracker});
                                        }
                                    }

                                    // 卡死 3.5 秒以上，且距离上一次干预过去 6 秒以上，触发干预
                                    // =====================================================================
                                    // [Multi-Stage Unstucking System]: Water escape, stage 1 nudge, stage 2 warp.
                                    // =====================================================================
                                    float dx_v = target_crime_pos.x - current_pos.x;
                                    float dy_v = target_crime_pos.y - current_pos.y;
                                    float dz_v = target_crime_pos.z - current_pos.z;
                                    float dist_v = sqrtf(dx_v * dx_v + dy_v * dy_v + dz_v * dz_v);

                                    int64_t stuck_duration = (tracker.stuck_since > 0) ? (now_time - tracker.stuck_since) : 0;

                                    // A. Predictive/Active Water Rescue
                                    bool is_water_stuck = (current_pos.z < 1.0f && dist_v > 15.0f); 
                                    if (is_water_stuck && !vector_contains(vehicles_emptied_snapshot, veh)) {
                                        if (g_VehicleInflictDamage) {
                                            g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, current_pos);
                                        }
                                        vehicles_emptied_snapshot.push_back(veh);
                                        pending_vehicles_emptied.push_back(veh);
                                        if (veh == crime_case->spawned_vehicle) {
                                            if (crime_case) crime_case->occupants_ordered_out = true;
                                        }
                                        LOGW("[dispatchCenter - WaterAvoidance] Vehicle %p at extreme low sea level (Z=%.2f). Emergency bulk exit!", veh, current_pos.z);
                                    }

                                    // B. Multi-Stage Intervention Trigger Decision
                                    bool trigger_stage1 = false;
                                    bool trigger_stage2 = false;

                                    bool cop_visible = is_cop_visible_to_player(veh, current_pos.x, current_pos.y, current_pos.z);

                                    if (stuck_duration >= 7000 && !cop_visible && dist_v > 40.0f) {
                                        trigger_stage2 = true;
                                    } else if (stuck_duration > 3500) {
                                        if (now_time - tracker.last_intervention_time > 6000) {
                                            trigger_stage1 = true;
                                        }
                                    }

                                    if (trigger_stage2) {
                                        tracker.last_intervention_time = now_time;
                                        tracker.stuck_since = 0; // Reset stuck timer on successful teleport

                                        if (found_stuck) {
                                            stuck_vehicles_snapshot[stuck_idx].second = tracker;
                                        } else {
                                            for (auto& item : stuck_vehicles_snapshot) {
                                                if (item.first == veh) {
                                                    item.second = tracker;
                                                    break;
                                                }
                                            }
                                        }
                                        pending_stuck_vehicles.push_back({veh, tracker});

                                        float warp_factor = 25.0f / dist_v;
                                        if (warp_factor > 0.8f) warp_factor = 0.5f; 
                                        bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                        float height_offset = is_bike ? 0.15f : 0.8f;
                                        CVector warp_pos = {
                                            current_pos.x + dx_v * warp_factor,
                                            current_pos.y + dy_v * warp_factor,
                                            current_pos.z + dz_v * warp_factor + height_offset
                                        };
                                        
                                        set_entity_pos(veh, warp_pos);
                                        if (is_bike) {
                                            stabilize_motorcycle(veh);
                                        }
                                        command_vehicle_ai(veh, target_crime_pos, dist_v);
                                        LOGI("[dispatchCenter - Stage 2 Warp] Teleported vehicle %p forward 25m to break deadlock. (visible=%d, stuck_duration=%lld ms)", 
                                             veh, cop_visible, (long long)stuck_duration);
                                    }
                                    else if (trigger_stage1) {
                                        tracker.last_intervention_time = now_time;

                                        if (found_stuck) {
                                            stuck_vehicles_snapshot[stuck_idx].second = tracker;
                                        } else {
                                            for (auto& item : stuck_vehicles_snapshot) {
                                                if (item.first == veh) {
                                                    item.second = tracker;
                                                    break;
                                                }
                                            }
                                        }
                                        pending_stuck_vehicles.push_back({veh, tracker});

                                        LOGI("[dispatchCenter - Stage 1 Nudge] Stuck rescue (Stage 1) initiated for %p (stuck_duration=%lld ms)...", veh, (long long)stuck_duration);

                                        // 1. Temporarily disable stuck route section within 20m
                                        if (g_ThePaths && g_SwitchRoadsOffInArea) {
                                            g_SwitchRoadsOffInArea(
                                                g_ThePaths,
                                                current_pos.x - 20.0f, current_pos.y - 20.0f, current_pos.z - 8.0f,
                                                current_pos.x + 20.0f, current_pos.y + 20.0f, current_pos.z + 8.0f,
                                                true, true, false
                                            );
                                            pending_temp_closures.push_back({current_pos, 20.0f, now_time + 15000});
                                        }

                                        // 2. Clear traffic obstacles within 15m
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
                                                                bool is_other_cop = vector_contains(vehicles_ordered_to_scene_snapshot, other_veh) || 
                                                                                    vector_contains(vehicles_siren_awakened_snapshot, other_veh) || 
                                                                                    other_veh == crime_case->spawned_vehicle;
                                                                if (!is_other_cop) {
                                                                    CVector other_pos = get_entity_pos(other_veh);
                                                                    float ov_dx = other_pos.x - current_pos.x;
                                                                    float ov_dy = other_pos.y - current_pos.y;
                                                                    float ov_dz = other_pos.z - current_pos.z;
                                                                    float ov_dist = sqrtf(ov_dx * ov_dx + ov_dy * ov_dy + ov_dz * ov_dz);

                                                                    if (ov_dist < 15.0f) {
                                                                        bool is_visible = is_pos_visible_to_player_camera(other_pos);
                                                                        if (!is_visible) {
                                                                            CVector far_away = {other_pos.x, other_pos.y, other_pos.z - 50.0f};
                                                                            set_entity_pos(other_veh, far_away);
                                                                            LOGI("   +- Traffic Blockage Cleared (Unseen): Teleported vehicle %p underground", other_veh);
                                                                        } else {
                                                                            LOGI("   +- Traffic Blockage Skipped (Seen): Avoid visible popping, waiting for player to look away");
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // 3. Reverse backing nudge if visible to pull away from obstacles, or forward warp if unseen
                                        if (dist_v > 5.0f) {
                                            CVector nudged_pos;
                                                                                         if (cop_visible) {
                                                                                             bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                             // Pull away backwards! Calculate current forward vector from matrix
                                                                                             float f_x = 0.0f, f_y = 0.0f, f_z = 0.0f;
                                                                                             if (g_GetMatrix) {
                                                                                                 CMatrix* mat = g_GetMatrix(veh);
                                                                                                 if (mat) {
                                                                                                     f_x = mat->up_x;
                                                                                                     f_y = mat->up_y;
                                                                                                     f_z = mat->up_z;
                                                                                                 }
                                                                                             }
                                                                                             float f_len = sqrtf(f_x * f_x + f_y * f_y + f_z * f_z);
                                                                                             if (f_len > 0.01f) {
                                                                                                 f_x /= f_len; f_y /= f_len; f_z /= f_len;
                                                                                             } else {
                                                                                                 f_x = 0.0f; f_y = 1.0f; f_z = 0.0f;
                                                                                             }
                                                                                             // Nudge backwards by 3.5 meters, lift by 0.60m or 0.12m for bike
                                                                                             float height_lift = is_bike ? 0.12f : 0.60f;
                                                                                             nudged_pos.x = current_pos.x - f_x * 3.5f;
                                                                                             nudged_pos.y = current_pos.y - f_y * 3.5f;
                                                                                             nudged_pos.z = current_pos.z + height_lift;
                                                                                             LOGI("   +- Nudged vehicle %p BACKWARDS 3.5m and elevated %.2fm to pull away from obstacles (visible).", veh, height_lift);
                                                                                         } else {
                                                                                             bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                             // Unseen: Nudge towards destination forward by 12.0m to cross walls quickly
                                                                                             float nx = dx_v / dist_v;
                                                                                             float ny = dy_v / dist_v;
                                                                                             float nz = dz_v / dist_v;
                                                                                             float height_lift = is_bike ? 0.15f : 0.75f;
                                                                                             nudged_pos.x = current_pos.x + nx * 12.0f;
                                                                                             nudged_pos.y = current_pos.y + ny * 12.0f;
                                                                                             nudged_pos.z = current_pos.z + nz * 0.1f + height_lift;
                                                                                             LOGI("   +- Nudged vehicle %p FORWARD 12.0m (unseen warp) and elevated %.2fm to bypass obstacles.", veh, height_lift);
                                                                                         }

                                                                                         bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                         
                                                                                         set_entity_pos(veh, nudged_pos);
                                                                                         if (is_bike) {
                                                                                             stabilize_motorcycle(veh);
                                                                                         }
                                                                                         
                                                                                         command_vehicle_ai(veh, target_crime_pos, dist_v);}
                                    }
                                }

                                if (!already_dispatched && active_vehicles_count >= max_vehicles) {
                                    continue;
                                }

                                // 1. 命令车辆驶向现场（仅触发一次）
                                if (!vector_contains(vehicles_emptied_snapshot, veh) && !vector_contains(vehicles_ordered_to_scene_snapshot, veh) && is_vehicle_occupied_by_driver(veh)) {
                                    command_vehicle_ai(veh, target_crime_pos, v_dist);
                                    vehicles_ordered_to_scene_snapshot.push_back(veh); // 局部同步
                                    pending_vehicles_ordered_to_scene.push_back(veh);

                                    if (!already_dispatched) {
                                        counted_vehicles.push_back(veh);
                                        active_vehicles_count++;
                                        already_dispatched = true;
                                    }
                                    LOGI("Vehicle order sent (dist=%.1f): Commanded vehicle %p to drive to scene (active_vehicles=%d/%d)", 
                                         v_dist, veh, active_vehicles_count, max_vehicles);
                                }

                                // 2. 动态警笛唤醒
                                if (!vector_contains(vehicles_emptied_snapshot, veh) && v_dist < 90.0f && !vector_contains(vehicles_siren_awakened_snapshot, veh)) {
                                    if (g_GetCarToGoToCoors) {
                                        g_GetCarToGoToCoors(veh, &target_crime_pos, 0, false);
                                    }
                                    if (g_VehicleInflictDamage) {
                                        g_VehicleInflictDamage(veh, reinterpret_cast<CEntity*>(target_criminal), WEAPON_UNARMED, 0.0f, veh_pos);
                                    }
                                    vehicles_siren_awakened_snapshot.push_back(veh); // 局部同步
                                    pending_vehicles_siren_awakened.push_back(veh);

                                    if (!already_dispatched) {
                                        counted_vehicles.push_back(veh);
                                        active_vehicles_count++;
                                        already_dispatched = true;
                                    }
                                    LOGI("Vehicle siren awakened (dist=%.1f): Reset autopilot mission & Inflicted physical 0-damage (active_vehicles=%d/%d)", 
                                         v_dist, active_vehicles_count, max_vehicles);
                                }
                            }
                        }
                        continue; // 车辆内的人跳过任务指派，等待其下车
                    }

                    // 地面警员只在 70m 内响应（保持最高效追杀半径）
                    if (dist_sq > 70.0f * 70.0f) {
                        continue;
                    }

                    // =====================================================================
                    // 地面警员的事件驱动型分派调度与高频限流控制 (3 秒限频 & 15 秒存留状态查询)
                    // =====================================================================
                    int64_t last_assign = 0;
                    auto key_assign = std::make_pair(ped, target_criminal);
                    for (const auto& item : cop_attack_assign_time_snapshot) {
                        if (item.first == key_assign) {
                            last_assign = item.second;
                            break;
                        }
                    }

                    bool already_targeting = false;
                    if (g_GetWeaponLockOnTarget && g_GetWeaponLockOnTarget(ped) == reinterpret_cast<CEntity*>(target_criminal)) {
                        already_targeting = true;
                    }

                    // 提前确定分配的目标武器，判断是否是近战武器，用于高频打断防护
                    bool is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
                    eWeaponType target_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);
                    bool is_melee = (target_weapon == WEAPON_NIGHTSTICK || target_weapon == WEAPON_UNARMED || target_weapon < 22);

                    // =====================================================================
                    // 🚀 【核心修复】动态战术武器切换逻辑（针对已锁定目标/已处于响应的警员）
                    // =====================================================================
                    bool is_assigned_or_targeting = already_targeting || (now_ms() - last_assign < 15000);
                    if (is_assigned_or_targeting) {
                        int64_t last_armed = 0;
                        for (const auto& item : armed_cops_time_snapshot) {
                            if (item.first == ped) {
                                last_armed = item.second;
                                break;
                            }
                        }

                        eWeaponType last_assigned_weapon = WEAPON_UNARMED;
                        for (const auto& item : cop_assigned_weapon_snapshot) {
                            if (item.first == ped) {
                                last_assigned_weapon = item.second;
                                break;
                            }
                        }

                        // 如果是要切成手枪，而当前又是警棍/无武器，则说明是紧急枪械升级，必须强行无视 1.5 秒冷却，确保瞬间切枪
                        bool is_upgrading_to_firearm = (target_weapon == WEAPON_PISTOL && last_assigned_weapon != WEAPON_PISTOL);
                        if (now_ms() - last_armed > 1500 || is_upgrading_to_firearm) {
                            if (target_weapon != last_assigned_weapon) {
                                if (g_GiveWeapon && g_SetCurrentWeapon) {
                                    g_GiveWeapon(ped, target_weapon, 9999, true);
                                    g_SetCurrentWeapon(ped, target_weapon);
                                }
                                LOGI("🔄 [Weapon Dynamic Switch] Ground cop %p switched weapon from %d to %d (dist=%.1f, bypass_cooldown=%d)", 
                                     ped, last_assigned_weapon, target_weapon, sqrtf(dist_sq), is_upgrading_to_firearm);

                                // 局部同步更新
                                bool found_w = false;
                                for (auto& item : cop_assigned_weapon_snapshot) {
                                    if (item.first == ped) {
                                        item.second = target_weapon;
                                        found_w = true;
                                        break;
                                    }
                                }
                                if (!found_w) {
                                    cop_assigned_weapon_snapshot.push_back({ped, target_weapon});
                                }
                                pending_cop_assigned_weapon.push_back({ped, target_weapon});

                                // 更新上次武装时间
                                bool found_t = false;
                                for (auto& item : armed_cops_time_snapshot) {
                                    if (item.first == ped) {
                                        item.second = now_ms();
                                        found_t = true;
                                        break;
                                    }
                                }
                                if (!found_t) {
                                    armed_cops_time_snapshot.push_back({ped, now_ms()});
                                }
                                pending_armed_cops_time.push_back({ped, now_ms()});
                            }
                        }
                    }

                    // 【核心近战防抖限流控制】：由于近战没有 LockOnTarget 瞄准状态，already_targeting 始终为 false。
                    // 因而，对近战警员采用 15 秒（在场存留时限）长限流拦截；枪械警员继续保持 3 秒心跳唤醒。这可确保近战挥舞不被 0 伤物理受击打断！
                    if (already_targeting || (now_ms() - last_assign < (is_melee ? 15000 : 3000))) {
                        continue; // 3 秒（枪械）或 15 秒（近战）内已指派过，或者已在瞄准，跳过
                    }

                    // 40m 内的野生巡警纳入响应
                    bool is_extremely_nearby = false;
                    {
                        float p_dist = 99999.0f;
                        if (g_FindPlayerCoors) {
                            CVector player_pos = g_FindPlayerCoors(0);
                            float p_dx = cop_pos.x - player_pos.x;
                            float p_dy = cop_pos.y - player_pos.y;
                            float p_dz = cop_pos.z - player_pos.z;
                            p_dist = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
                        }
                        float c_dist = sqrtf(dist_sq);
                        if (c_dist < 40.0f || p_dist < 40.0f) {
                            is_extremely_nearby = true;
                        }
                    }

                    // 判定是否是已经响应过的地面警员（使用 15 秒内分派或正在瞄准）
                    bool already_assigned_foot_cop = already_targeting || (now_ms() - last_assign < 15000);
                    if (!already_assigned_foot_cop && active_foot_cops_count >= max_foot_cops) {
                        // 超过地面警员配额上限且在现场目击范围 (30m) 之外，直接跳过，使其优雅走过
                        continue;
                    }

                    // 避免穿帮音效：如果当前活跃案件是枪击案 (is_firearm == true)，听觉自然唤醒。但极近距离强行唤醒。
                    if (!is_extremely_nearby && g_crime_active.load() && crime_case->is_firearm && !crime_case->cancelled) {
                        if (g_FindPlayerCoors) {
                            CVector player_pos = g_FindPlayerCoors(0);
                            float p_dx = cop_pos.x - player_pos.x;
                            float p_dy = cop_pos.y - player_pos.y;
                            float p_dz = cop_pos.z - player_pos.z;
                            float p_dist = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
                            if (p_dist < 45.0f) {
                                continue;
                            }
                        }
                    }

                    // =====================================================================
                    // 智能双保险唤醒方案（假枪声为主，0 伤害物理注入为兜底备用）
                    // =====================================================================
                    bool dispatched_via_noise = false;
                    is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
                    eWeaponType chosen_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);

                    // 一律使用虚拟枪声事件进行静默、无穿帮唤醒（已集成 GMalloc，完全消除闪退风险）
                    if (g_CEventGunShot_ctor && g_CEventGunShot_dtor && g_CEventGroup_Add) {
                        void* event_group = get_ped_event_group(ped);
                        if (event_group) {
                            alignas(16) char event_buf[256];
                            memset(event_buf, 0, sizeof(event_buf));
                            CVector start_pos = cop_pos; // 惊雷在耳：声源直接设在警员耳边
                            CVector target_pos(cop_pos.x, cop_pos.y, cop_pos.z + 1.0f);
                            
                            g_CEventGunShot_ctor(event_buf, reinterpret_cast<CEntity*>(target_criminal), start_pos, target_pos, false);
                            g_CEventGroup_Add(event_group, event_buf, false);
                            g_CEventGunShot_dtor(event_buf);
                            
                            // 动态赋予最优战术武器
                            if (g_GiveWeapon && g_SetCurrentWeapon) {
                                g_GiveWeapon(ped, chosen_weapon, 9999, true);
                                g_SetCurrentWeapon(ped, chosen_weapon);
                            }

                            // Register ground cop to ECS
                            {
                                auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
                                if (!cop_comp) {
                                    cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(ped, ped);
                                }
                                auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
                                if (!combat_comp) {
                                    combat_comp = ecs::EntityManager::get().add_component<ecs::CombatComponent>(ped);
                                }
                                if (combat_comp) {
                                    combat_comp->target_entity = target_criminal;
                                }
                            }
                            
                            bool found_assign = false;
                            for (auto& item : cop_attack_assign_time_snapshot) {
                                if (item.first == key_assign) {
                                    item.second = now_ms();
                                    found_assign = true;
                                    break;
                                }
                            }
                            if (!found_assign) {
                                cop_attack_assign_time_snapshot.push_back({key_assign, now_ms()});
                            }
                            pending_cop_attack_assign_time.push_back({key_assign, now_ms()});

                            // 同时记录到分配武器
                            bool found_w = false;
                            for (auto& item : cop_assigned_weapon_snapshot) {
                                if (item.first == ped) {
                                    item.second = chosen_weapon;
                                    found_w = true;
                                    break;
                                }
                            }
                            if (!found_w) {
                                cop_assigned_weapon_snapshot.push_back({ped, chosen_weapon});
                            }
                            pending_cop_assigned_weapon.push_back({ped, chosen_weapon});

                            // 更新上次武装时间
                            bool found_t = false;
                            for (auto& item : armed_cops_time_snapshot) {
                                if (item.first == ped) {
                                    item.second = now_ms();
                                    found_t = true;
                                    break;
                                }
                            }
                            if (!found_t) {
                                armed_cops_time_snapshot.push_back({ped, now_ms()});
                            }
                            pending_armed_cops_time.push_back({ped, now_ms()});

                            if (!already_assigned_foot_cop) {
                                active_foot_cops_count++;
                                already_assigned_foot_cop = true;
                            }
                            LOGI("🎯 [Virtual Sound Event] Dispatched logic sound event (Weapon=%d) to cop %p towards criminal %p (active_foot_cops=%d/%d)", 
                                 chosen_weapon, ped, target_criminal, active_foot_cops_count, max_foot_cops);
                            dispatched_via_noise = true;
                        }
                    }

                    // 平滑降级
                    if (!dispatched_via_noise && g_orig_generate_damage_event) {
                        if (g_GiveWeapon && g_SetCurrentWeapon) {
                            g_GiveWeapon(ped, chosen_weapon, 9999, true);
                            g_SetCurrentWeapon(ped, chosen_weapon);
                        }
                        g_orig_generate_damage_event(ped, reinterpret_cast<CEntity*>(target_criminal), chosen_weapon, 0, 3, 0);

                        // Register ground cop to ECS
                        {
                            auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
                            if (!cop_comp) {
                                cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(ped, ped);
                            }
                            auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
                            if (!combat_comp) {
                                combat_comp = ecs::EntityManager::get().add_component<ecs::CombatComponent>(ped);
                            }
                            if (combat_comp) {
                                combat_comp->target_entity = target_criminal;
                            }
                        }
                        
                        bool found_assign = false;
                        for (auto& item : cop_attack_assign_time_snapshot) {
                            if (item.first == key_assign) {
                                item.second = now_ms();
                                found_assign = true;
                                break;
                            }
                        }
                        if (!found_assign) {
                            cop_attack_assign_time_snapshot.push_back({key_assign, now_ms()});
                        }
                        pending_cop_attack_assign_time.push_back({key_assign, now_ms()});

                        // 同时记录到分配武器
                        bool found_w = false;
                        for (auto& item : cop_assigned_weapon_snapshot) {
                            if (item.first == ped) {
                                item.second = chosen_weapon;
                                found_w = true;
                                break;
                            }
                        }
                        if (!found_w) {
                            cop_assigned_weapon_snapshot.push_back({ped, chosen_weapon});
                        }
                        pending_cop_assigned_weapon.push_back({ped, chosen_weapon});

                        // 更新上次武装时间
                        bool found_t = false;
                        for (auto& item : armed_cops_time_snapshot) {
                            if (item.first == ped) {
                                item.second = now_ms();
                                found_t = true;
                                break;
                            }
                        }
                        if (!found_t) {
                            armed_cops_time_snapshot.push_back({ped, now_ms()});
                        }
                        pending_armed_cops_time.push_back({ped, now_ms()});

                        if (!already_assigned_foot_cop) {
                            active_foot_cops_count++;
                            already_assigned_foot_cop = true;
                        }
                        LOGI("⚠️ [Fallback 0-Damage] Inflicted 0 damage (Weapon=%d) to cop %p by criminal %p (active_foot_cops=%d/%d)", 
                             chosen_weapon, ped, target_criminal, active_foot_cops_count, max_foot_cops);
                    }
                }
            }
        }
    }

    // =====================================================================
    // 批量提交状态后释放锁
    // =====================================================================
    if (!pending_armed_cops_time.empty()) {
        std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
        for (const auto& item : pending_armed_cops_time) {
            g_armed_cops_time[item.first] = item.second;
        }
    }
    if (!pending_cop_assigned_weapon.empty()) {
        std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
        for (const auto& item : pending_cop_assigned_weapon) {
            g_cop_assigned_weapon[item.first] = item.second;
        }
    }
    if (!pending_dispatched_vehicles_time.empty()) {
        std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
        for (const auto& item : pending_dispatched_vehicles_time) {
            g_dispatched_vehicles_time[item.first] = item.second;
        }
    }
    if (!pending_vehicles_emptied.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
        for (void* veh : pending_vehicles_emptied) {
            g_vehicles_emptied.insert(veh);
        }
    }
    if (!pending_vehicles_ordered_to_scene.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_mutex);
        for (void* veh : pending_vehicles_ordered_to_scene) {
            g_vehicles_ordered_to_scene.insert(veh);
        }
    }
    if (!pending_vehicles_siren_awakened.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
        for (void* veh : pending_vehicles_siren_awakened) {
            g_vehicles_siren_awakened.insert(veh);
        }
    }
    if (!pending_stuck_vehicles.empty()) {
        std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
        for (const auto& item : pending_stuck_vehicles) {
            g_stuck_vehicles[item.first] = item.second;
        }
    }
    if (!pending_cop_attack_assign_time.empty()) {
        std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
        for (const auto& item : pending_cop_attack_assign_time) {
            g_cop_attack_assign_time[item.first] = item.second;
        }
    }
    if (!pending_temp_closures.empty()) {
        std::lock_guard<std::mutex> lock(g_temp_closures_mutex);
        for (const auto& item : pending_temp_closures) {
            g_temp_road_closures.push_back(item);
        }
    }
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

void make_single_cop_attack_criminal(CPed* cop, CPed* criminal, bool force_weapon_update) {
    if (!cop || !is_ped_pointer_valid_safe(cop) || !criminal || !is_ped_pointer_valid_safe(criminal)) return;
    if (g_IsAlive && !g_IsAlive(cop)) return;
    if (g_GetPedType && g_GetPedType(cop) != PED_TYPE_COP) return;

    // Register cop to ECS and set target in CombatComponent
    {
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
        LOGI("🎯 [Event-Driven Single Cop Weapon Switch] Cop %p switched weapon to %d (perp=%p, force=%d)", cop, (int)target_weapon, criminal, force_weapon_update);
        
        {
            std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
            g_cop_assigned_weapon[cop] = target_weapon;
        }
        {
            std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
            g_armed_cops_time[cop] = now_ms();
        }
    }

    bool sound_sent = false;
    if (g_CEventGunShot_ctor && g_CEventGunShot_dtor && g_CEventGroup_Add) {
        void* event_group = get_ped_event_group(cop);
        if (event_group) {
            alignas(16) char event_buf[256];
            memset(event_buf, 0, sizeof(event_buf));
            CVector cop_pos = get_entity_pos(cop);
            CVector start_pos = cop_pos;
            CVector target_pos(cop_pos.x, cop_pos.y, cop_pos.z + 1.0f);
            
            g_CEventGunShot_ctor(event_buf, reinterpret_cast<CEntity*>(criminal), start_pos, target_pos, false);
            g_CEventGroup_Add(event_group, event_buf, false);
            g_CEventGunShot_dtor(event_buf);
            sound_sent = true;
            LOGI("🎯 [Event-Driven Single Cop Sound Dispatch] Sent gunshot sound to cop %p towards criminal %p", cop, criminal);
        }
    }

    if (!sound_sent && g_orig_generate_damage_event) {
        g_orig_generate_damage_event(cop, reinterpret_cast<CEntity*>(criminal), target_weapon, 0, 3, 0);
        LOGI("🎯 [Event-Driven Single Cop Fallback] Inflicted 0-damage to cop %p by criminal %p", cop, criminal);
    }

    {
        std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
        g_cop_attack_assign_time[{cop, criminal}] = now_ms();
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


