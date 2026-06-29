/**
 * GTA SA DE 警力派发 Zygisk 模块
 *
 * 入口：module.cpp（全局符号、辅助函数、Hook 安装、Zygisk）
 * 派发：dispatch_logic.cpp | 稳定性 Hook：hooks_stability.cpp | ECS：ecs_systems.cpp
 *
 * 符号经 nm -D libUE4.so 核对；运行时通过 xdl_sym 解析。
 */

#include <jni.h>
#include <android/log.h>
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

#include "zygisk/zygisk.hpp"
#include "shadowhook.h"
#include "third_party/xdl/xdl.h"
#include "sh_nothing_bin.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <random>
#include <functional>

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "ecs_engine.hpp"
#include "mod_shared.hpp"

// =====================================================================
// 全局函数指针（运行时通过 dlsym 解析）
// =====================================================================
fn_FindPlayerPed_t            g_FindPlayerPed = nullptr;
fn_FindPlayerCoors_t          g_FindPlayerCoors = nullptr;
fn_GetPedType_t               g_GetPedType = nullptr;
fn_GetMatrix_t                g_GetMatrix = nullptr;
fn_FindDistToNearestPedOfType_t g_FindDistToNearestCop = nullptr;
fn_ScriptGenEmergencyCar_t    g_ScriptGenEmergencyCar = nullptr;
fn_GenOneEmergencyCar_t       g_GenOneEmergencyCar = nullptr;
fn_AddPoliceOccupants_t       g_AddPoliceOccupants = nullptr;
fn_AddCriminalToKill_t        g_AddCriminalToKill = nullptr;
fn_GiveWeapon_t               g_GiveWeapon = nullptr;
fn_SetCurrentWeapon_t         g_SetCurrentWeapon = nullptr;
fn_GiveWeaponAtStartOfFight_t g_GiveWeaponAtStartOfFight = nullptr;

// 火源检测与避让系统全局变量与类型定义
void* g_FireManager = nullptr;
typedef void* (*fn_FindNearestFire_t)(void* fire_manager_this, const CVector& pos, bool bCheckScriptFires, bool bCheckNormalFires);
fn_FindNearestFire_t g_FindNearestFire = nullptr;

// 假枪声所需函数指针与类型定义
typedef void (*fn_CEventGunShot_ctor_t)(void*, CEntity*, CVector, CVector, bool);
typedef void (*fn_CEventGunShot_dtor_t)(void*);
typedef void (*fn_CEventGroup_Add_t)(void*, void*, bool);

fn_CEventGunShot_ctor_t g_CEventGunShot_ctor = nullptr;
fn_CEventGunShot_dtor_t g_CEventGunShot_dtor = nullptr;
fn_CEventGroup_Add_t    g_CEventGroup_Add = nullptr;

FMalloc** g_p_GMalloc = nullptr;

fn_RegisterKill_t             g_RegisterKill = nullptr;
fn_GetWeaponLockOnTarget_t    g_GetWeaponLockOnTarget = nullptr;
fn_IsAlive_t                  g_IsAlive = nullptr;
fn_VehicleInflictDamage_t     g_VehicleInflictDamage = nullptr;
fn_GetPoolPed_t               g_GetPoolPed = nullptr;
void*                         g_ms_pPedPool = nullptr;
void**                        g_CSequenceManager_ms_instance = nullptr;
void*                         g_ms_pVehiclePool = nullptr;

// 摄像机/视野判定相关符号
void*                         g_TheCamera = nullptr;
typedef CVector (*fn_GetGameCamPosition_t)(void*);
typedef CVector (*fn_GetLookDirection_t)(void*);

fn_GetGameCamPosition_t       g_GetGameCamPosition = nullptr;
fn_GetLookDirection_t         g_GetLookDirection = nullptr;

// 任务与载具交互相关符号
typedef bool (*fn_IsDriver_t)(const void* vehicle_this, const CPed* ped);
typedef bool (*fn_IsPassenger_t)(const void* vehicle_this, const CPed* ped);
typedef void (*fn_TellOccupantsToLeaveCar_t)(void* vehicle);
typedef void* (*fn_GetPoolVehicle_t)(int);

fn_IsDriver_t                g_IsDriver = nullptr;
fn_IsPassenger_t             g_IsPassenger = nullptr;
fn_TellOccupantsToLeaveCar_t g_TellOccupantsToLeaveCar = nullptr;
fn_GetPoolVehicle_t          g_GetPoolVehicle = nullptr;

typedef void (*fn_GetCarToGoToCoors_t)(void* vehicle, CVector* coors, int drivingMode, bool flag);
fn_GetCarToGoToCoors_t        g_GetCarToGoToCoors = nullptr;

typedef void (*fn_SwitchRoadsOffInArea_t)(void* instance, float minX, float minY, float minZ, float maxX, float maxY, float maxZ, bool bSwitchOff, bool bKeepVehicles, bool bAllowBoats);
fn_SwitchRoadsOffInArea_t     g_SwitchRoadsOffInArea = nullptr;
void*                         g_ThePaths = nullptr;

typedef void* (*fn_TaskNew_t)(unsigned long);
fn_TaskNew_t                  g_TaskNew = nullptr;
fn_TaskKillCriminal_ctor_t    g_TaskKillCriminal_ctor = nullptr;
fn_SetTask_t                  g_SetTask = nullptr;
fn_TaskEnterCar_ctor_t        g_TaskEnterCar_ctor = nullptr;
void*                         g_vtable_KillCriminal = nullptr;
void*                         g_vtable_EnterCar = nullptr;

void*                         g_vtable_CTask = nullptr;
void*                         g_vtable_CTaskSimple = nullptr;
void*                         g_vtable_CTaskComplex = nullptr;
void*                         g_vtable_CEvent = nullptr;

typedef void (*fn_AddTaskPrimaryMaybeInGroup_t)(void* ped_intel_this, CTask* task, bool writeToEventLog);
fn_AddTaskPrimaryMaybeInGroup_t g_AddTaskPrimaryMaybeInGroup = nullptr;

typedef void* (*fn_FindTaskByType_t)(const void* ped_intel_this, int task_type);
fn_FindTaskByType_t g_FindTaskByType = nullptr;


// =====================================================================
// Hook 存根
// =====================================================================
void* g_stub_report_crime = nullptr;
fn_ReportCrime_orig_t g_orig_report_crime = nullptr;
void* g_stub_register_kill = nullptr;
void* g_stub_set_wanted = nullptr;
void* g_stub_generate_damage_event = nullptr;
fn_GenerateDamageEvent_orig_t g_orig_generate_damage_event = nullptr;

void* g_stub_the_scripts_process = nullptr;
typedef void (*fn_TheScriptsProcess_t)();
fn_TheScriptsProcess_t g_orig_the_scripts_process = nullptr;

void* g_stub_event_damage_ctor_c1 = nullptr;
void* g_stub_event_damage_ctor_c2 = nullptr;
typedef void (*fn_EventDamage_ctor_t)(void* event_this, CEntity* damageSource, unsigned int startTime, eWeaponType weaponType, int pieceType, unsigned char damageSeverity, bool b1, bool b2);
fn_EventDamage_ctor_t g_orig_event_damage_ctor_c1 = nullptr;
fn_EventDamage_ctor_t g_orig_event_damage_ctor_c2 = nullptr;

void* g_stub_set_current_weapon = nullptr;
fn_SetCurrentWeapon_t g_orig_SetCurrentWeapon = nullptr;

std::atomic<int> g_player_wanted_level{0};

// =====================================================================
// 辅助函数
// =====================================================================

// 验证 Ped 指针在 Ped Pool 中是否依然有效，降低野指针风险 (前向声明所需)
bool is_ped_pointer_valid_safe(void* target_ped) {
    if (!target_ped) return false;

    // 优先通过 FindPlayerPed 判定玩家 Ped 的有效性，防止转场/淡入淡出期间玩家 Ped 临时不在 Pool 中而被误判
    if (g_FindPlayerPed) {
        if (target_ped == g_FindPlayerPed(-1) || target_ped == g_FindPlayerPed(0)) {
            return true;
        }
    }

    if (!g_ms_pPedPool || !is_pointer_readable(g_ms_pPedPool) || !g_GetPoolPed) return false;
    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool || !is_pointer_readable(pool)) return false;

    char** p_byte_map = reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    if (!is_pointer_readable(p_byte_map)) return false;
    char* byte_map = *p_byte_map;
    if (!byte_map || !is_pointer_readable(byte_map)) return false;

    int* p_size = reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!is_pointer_readable(p_size)) return false;
    int size = *p_size;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            if (ped == target_ped) {
                return true;
            }
        }
    }
    return false;
}

void* g_pure_virtual_target = nullptr;

bool is_task_vtable_safe(void* task) {
    if (!task) return true;
    if (!is_pointer_readable(task)) return false;
    void* vtable = *reinterpret_cast<void**>(task);
    if (!vtable) return false;
    if (!is_pointer_readable(vtable)) return false;

    // Verify that the vtable is within the address space of libUE4.so.
    // We use g_vtable_CTask as a reference point.
    if (g_vtable_CTask) {
        uintptr_t ref = reinterpret_cast<uintptr_t>(g_vtable_CTask);
        uintptr_t vt = reinterpret_cast<uintptr_t>(vtable);
        uintptr_t diff = (vt > ref) ? (vt - ref) : (ref - vt);
        if (diff > 250000000ULL) { // 250 MB
            return false; // Too far from libUE4.so data segment, likely garbage/freed memory
        }
    }

    // Check if slot 2 (Clone) or slot 4 (IsSimple) points to __cxa_pure_virtual
    if (g_pure_virtual_target) {
        void** vtable_slots = reinterpret_cast<void**>(vtable);
        if (is_pointer_readable(vtable_slots + 4) && vtable_slots[4] == g_pure_virtual_target) {
            return false;
        }
        if (is_pointer_readable(vtable_slots + 2) && vtable_slots[2] == g_pure_virtual_target) {
            return false;
        }
    }

    if (g_vtable_CTask && (vtable == g_vtable_CTask || vtable == reinterpret_cast<void*>(reinterpret_cast<char*>(g_vtable_CTask) + 16))) return false;
    if (g_vtable_CTaskSimple && (vtable == g_vtable_CTaskSimple || vtable == reinterpret_cast<void*>(reinterpret_cast<char*>(g_vtable_CTaskSimple) + 16))) return false;
    if (g_vtable_CTaskComplex && (vtable == g_vtable_CTaskComplex || vtable == reinterpret_cast<void*>(reinterpret_cast<char*>(g_vtable_CTaskComplex) + 16))) return false;
    return true;
}

static bool is_firearm(eCrimeType crime) {
    return crime == CRIME_FIRE_WEAPON
        || crime == CRIME_KILL_PED_WITH_GUN
        || crime == CRIME_KILL_COP;
}

static bool is_gang_or_criminal(int ped_type) {
    return (ped_type >= PED_TYPE_GANG1 && ped_type <= PED_TYPE_GANG8)
        || ped_type == PED_TYPE_DEALER
        || ped_type == PED_TYPE_CRIMINAL;
}

// 火源 3D 坐标安全提取函数（双偏移兼容 + 异常坐标边界过滤）
static bool get_fire_position(void* fire, CVector& out_pos) {
    if (!fire) return false;
    
    // 1. 尝试标准 32位 偏移 4 (GTA 经典引擎 CFire Layout: 4字节标志位后即为 CVector 坐标)
    CVector* pPos4 = reinterpret_cast<CVector*>(reinterpret_cast<char*>(fire) + 4);
    if (std::isfinite(pPos4->x) && std::isfinite(pPos4->y) && std::isfinite(pPos4->z) &&
        pPos4->x >= -3500.0f && pPos4->x <= 3500.0f &&
        pPos4->y >= -3500.0f && pPos4->y <= 3500.0f &&
        pPos4->z >= -50.0f && pPos4->z <= 1000.0f) {
        out_pos = *pPos4;
        return true;
    }
    
    // 2. 尝试 64位 对齐/填充 偏移 8 (部分移动端编译器 8 字节对齐安全回退)
    CVector* pPos8 = reinterpret_cast<CVector*>(reinterpret_cast<char*>(fire) + 8);
    if (std::isfinite(pPos8->x) && std::isfinite(pPos8->y) && std::isfinite(pPos8->z) &&
        pPos8->x >= -3500.0f && pPos8->x <= 3500.0f &&
        pPos8->y >= -3500.0f && pPos8->y <= 3500.0f &&
        pPos8->z >= -50.0f && pPos8->z <= 1000.0f) {
        out_pos = *pPos8;
        return true;
    }
    
    return false;
}

// 验证载具指针在 Vehicle Pool 中是否依然有效，降低野指针风险 (前向声明所需)
bool is_vehicle_pointer_valid(void* target_veh) {
    if (!target_veh || !g_ms_pVehiclePool || !is_pointer_readable(g_ms_pVehiclePool) || !g_GetPoolVehicle) return false;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool || !is_pointer_readable(pool)) return false;

    char** p_byte_map = reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    if (!is_pointer_readable(p_byte_map)) return false;
    char* byte_map = *p_byte_map;
    if (!byte_map || !is_pointer_readable(byte_map)) return false;

    int* p_size = reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!is_pointer_readable(p_size)) return false;
    int size = *p_size;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            void* veh = g_GetPoolVehicle(handle);
            if (veh == target_veh) {
                return true;
            }
        }
    }
    return false;
}

static void cleanup_invalid_vehicles(std::vector<void*>& vec, std::mutex& mtx) {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto it = vec.begin(); it != vec.end(); ) {
        if (!is_vehicle_pointer_valid(*it)) {
            it = vec.erase(it);
        } else {
            ++it;
        }
    }
}

#define MODEL_POLICE_BIKE 523

unsigned short get_entity_model_index(void* entity) {
    if (!entity) return 0;
    return *reinterpret_cast<unsigned short*>(reinterpret_cast<char*>(entity) + 0x3A);
}

static void stabilize_motorcycle(void* veh) {
    if (!veh || !g_GetMatrix) return;
    CMatrix* mat = g_GetMatrix(veh);
    if (!mat) return;

    float heading_x = mat->up_x;
    float heading_y = mat->up_y;
    float len = sqrtf(heading_x * heading_x + heading_y * heading_y);
    if (len < 0.001f) {
        heading_x = 0.0f;
        heading_y = 1.0f;
        len = 1.0f;
    }
    float norm_up_x = heading_x / len;
    float norm_up_y = heading_y / len;

    mat->up_x = norm_up_x;
    mat->up_y = norm_up_y;
    mat->up_z = 0.0f;

    mat->at_x = 0.0f;
    mat->at_y = 0.0f;
    mat->at_z = 1.0f;

    mat->right_x = norm_up_y;
    mat->right_y = -norm_up_x;
    mat->right_z = 0.0f;
}


// 统一验证实体 (CPed/CVehicle) 的有效性，保障野指针安全
bool is_entity_pointer_valid(void* entity) {
    if (!entity) return false;
    if (is_ped_pointer_valid_safe(entity)) {
        return true;
    }
    if (is_vehicle_pointer_valid(entity)) {
        return true;
    }
    return false;
}

CVector get_entity_pos(void* entity) {
    CVector pos = {0, 0, 0};
    if (is_entity_pointer_valid(entity) && g_GetMatrix) {
        CMatrix* mat = g_GetMatrix(entity);
        if (mat) {
            pos.x = mat->pos_x;
            pos.y = mat->pos_y;
            pos.z = mat->pos_z;
        }
    }
    return pos;
}

static void set_entity_pos(void* entity, CVector pos) {
    if (is_entity_pointer_valid(entity) && g_GetMatrix) {
        CMatrix* mat = g_GetMatrix(entity);
        if (mat) {
            mat->pos_x = pos.x;
            mat->pos_y = pos.y;
            mat->pos_z = pos.z;
        }
    }
}

// 防穿帮视野检测
static bool is_pos_visible_to_player_camera(CVector pos) {
    if (!g_TheCamera || !g_GetGameCamPosition || !g_GetLookDirection) {
        return true; // 备份退化：默认认为可见，以安全为主
    }
    CVector cam_pos = g_GetGameCamPosition(g_TheCamera);
    CVector cam_dir = g_GetLookDirection(g_TheCamera);
    float len = sqrtf(cam_dir.x * cam_dir.x + cam_dir.y * cam_dir.y + cam_dir.z * cam_dir.z);
    if (len < 0.1f) return true;
    cam_dir.x /= len;
    cam_dir.y /= len;
    cam_dir.z /= len;

    // 从相机指向目标点的 3D 向量
    float to_pos_x = pos.x - cam_pos.x;
    float to_pos_y = pos.y - cam_pos.y;
    float to_pos_z = pos.z - cam_pos.z;
    float pos_dist = sqrtf(to_pos_x * to_pos_x + to_pos_y * to_pos_y + to_pos_z * to_pos_z);
    if (pos_dist < 0.1f) return true;

    to_pos_x /= pos_dist;
    to_pos_y /= pos_dist;
    to_pos_z /= pos_dist;

    // 计算 3D 夹角点积
    float dot = to_pos_x * cam_dir.x + to_pos_y * cam_dir.y + to_pos_z * cam_dir.z;

    // 考虑 FOV，点积大于 0.40 (对应夹角约 66 度，FOV 约 132 度) 认为是相机正面可视锥体
    // 同时只有当距离小于一定范围时（比如 80 米内，超过 80 米在远景雾中或不明显）才认定可见
    if (dot > 0.40f && pos_dist < 80.0f) {
        return true; // 在玩家可视范围内
    }
    return false; // 在玩家视野外（后方或被挡）
}

CVector get_spawn_target(CVector crime_pos) {
    CVector spawn_target = crime_pos;
    float base_z = crime_pos.z;
    if (g_FindPlayerCoors) {
        base_z = g_FindPlayerCoors(0).z;
    }

    CVector cam_pos = crime_pos;
    CVector cam_dir = {0.0f, 1.0f, 0.0f}; // 默认朝向北方
    bool has_cam = false;

    if (g_TheCamera && g_GetGameCamPosition && g_GetLookDirection) {
        cam_pos = g_GetGameCamPosition(g_TheCamera);
        cam_dir = g_GetLookDirection(g_TheCamera);
        float len = sqrtf(cam_dir.x * cam_dir.x + cam_dir.y * cam_dir.y + cam_dir.z * cam_dir.z);
        if (len > 0.1f) {
            cam_dir.x /= len;
            cam_dir.y /= len;
            cam_dir.z /= len;
            has_cam = true;
        }
    }

    if (!has_cam && g_FindPlayerPed && g_GetMatrix) {
        void* player = g_FindPlayerPed(0);
        if (player) {
            CMatrix* mat = g_GetMatrix(player);
            if (mat) {
                cam_pos = { mat->pos_x, mat->pos_y, mat->pos_z };
                cam_dir = { mat->up_x, mat->up_y, mat->up_z };
                float len = sqrtf(cam_dir.x * cam_dir.x + cam_dir.y * cam_dir.y + cam_dir.z * cam_dir.z);
                if (len > 0.1f) {
                    cam_dir.x /= len;
                    cam_dir.y /= len;
                    cam_dir.z /= len;
                    has_cam = true;
                }
            }
        }
    }

    // 就近 + 权重采样形式：
    // 在以犯罪点为中心、半径为 30 米到 45 米的圆环内，按 15 度角采样 24 个候选生成点。
    // 计算每个候选点的得分，得分最高（权重最大）的当选。
    struct SpawnCandidate {
        CVector pos;
        float score;
    };
    std::vector<SpawnCandidate> candidates;

    // 两个较近半径档次采样 (30m 和 45m)，共 48 个候选点，极力缩短寻路距离，避免生成在平行街区
    float radii[] = { 30.0f, 45.0f };
    for (float r : radii) {
        for (int i = 0; i < 24; i++) {
            float angle = i * (2.0f * M_PI / 24.0f);
            CVector cand_pos;
            cand_pos.x = crime_pos.x + cosf(angle) * r;
            cand_pos.y = crime_pos.y + sinf(angle) * r;
            cand_pos.z = base_z;

            // [Workaround] 考量3：移动端加载视距安全修正。若生成点距离玩家大于 70 米，极易秒删。
            // 我们将其安全缩减比例到 60.0f 米内。
            if (g_FindPlayerCoors) {
                CVector player_pos = g_FindPlayerCoors(0);
                float p_dx = cand_pos.x - player_pos.x;
                float p_dy = cand_pos.y - player_pos.y;
                float p_dz = cand_pos.z - player_pos.z;
                float dist_to_player = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
                if (dist_to_player > 70.0f) {
                    float scale = 60.0f / dist_to_player;
                    cand_pos.x = player_pos.x + p_dx * scale;
                    cand_pos.y = player_pos.y + p_dy * scale;
                    cand_pos.z = player_pos.z + p_dz * scale;
                }
            }

            // 权重分初始化为 100
            float score = 100.0f;

            // 考量1：就近原则。距离越短，得分越高 (离 crime_pos 越近越好，减去与 crime_pos 的距离比例，加大权重)
            score -= r * 1.0f; 

            // 考量2：视野遮挡。判断是否处于相机正面大角度视野中。
            if (has_cam) {
                // 计算从相机位置指向候选点的向量
                float to_cand_x = cand_pos.x - cam_pos.x;
                float to_cand_y = cand_pos.y - cam_pos.y;
                float len = sqrtf(to_cand_x * to_cand_x + to_cand_y * to_cand_y);
                if (len > 0.1f) {
                    to_cand_x /= len;
                    to_cand_y /= len;
                    // 计算与相机方向的点积 (2D dot product)
                    float dot = to_cand_x * cam_dir.x + to_cand_y * cam_dir.y;
                    
                    if (dot > 0.6f) {
                        // 在相机前方主视线内，扣除 500 分，降低该点在玩家视野内刷出的概率
                        score -= 500.0f; 
                    } else if (dot > 0.25f) {
                        // 处于侧前方视野边缘，给予大额负权重
                        score -= 150.0f;
                    } else if (dot < -0.3f) {
                        // 处于后方视野，给予奖励正权重
                        score += 50.0f;
                    }
                }
            }

            candidates.push_back({cand_pos, score});
        }
    }

    // 寻找得分最高的一个点
    float max_score = -9999.0f;
    CVector best_pos = crime_pos;
    bool found_valid = false;

    for (const auto& cand : candidates) {
        if (cand.score > max_score) {
            max_score = cand.score;
            best_pos = cand.pos;
            found_valid = true;
        }
    }

    if (found_valid && max_score > -200.0f) {
        spawn_target = best_pos;
        LOGI("Selected best spawn target (score=%.1f): (%.1f, %.1f, %.1f)", 
             max_score, spawn_target.x, spawn_target.y, spawn_target.z);
    } else {
        // 退化备份：若无有效点或全在视线内，则在玩家侧后方（相反视角）强制偏移 32 米，以确保完全不在玩家视线内
        float fallback_angle = 0.0f;
        if (has_cam) {
            // 取相机方向的相反方向 (后方)
            fallback_angle = atan2f(-cam_dir.y, -cam_dir.x);
        }
        CVector base_pos = g_FindPlayerCoors ? g_FindPlayerCoors(0) : crime_pos;
        spawn_target.x = base_pos.x + cosf(fallback_angle) * 32.0f;
        spawn_target.y = base_pos.y + sinf(fallback_angle) * 32.0f;
        spawn_target.z = base_pos.z;

        // [Workaround] 移动端安全备份：确保退化备份也在 70m 视距范围内
        if (g_FindPlayerCoors) {
            CVector player_pos = g_FindPlayerCoors(0);
            float p_dx = spawn_target.x - player_pos.x;
            float p_dy = spawn_target.y - player_pos.y;
            float p_dz = spawn_target.z - player_pos.z;
            float dist_to_player = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
            if (dist_to_player > 70.0f) {
                float scale = 60.0f / dist_to_player;
                spawn_target.x = player_pos.x + p_dx * scale;
                spawn_target.y = player_pos.y + p_dy * scale;
                spawn_target.z = player_pos.z + p_dz * scale;
            }
        }
        LOGI("Selected fallback backward spawn target: (%.1f, %.1f, %.1f)", spawn_target.x, spawn_target.y, spawn_target.z);
    }

    return spawn_target;
}

// 统计特定范围内的犯罪分子密度，以作为警力配置的依据
static int count_criminals_near(CVector pos, float radius) {
    if (!g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return 0;
    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return 0;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return 0;

    int count = 0;
    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            if (ped) {
                int ped_type = g_GetPedType(ped);
                if (is_gang_or_criminal(ped_type)) {
                    CVector ped_pos = get_entity_pos(ped);
                    float dx = ped_pos.x - pos.x;
                    float dy = ped_pos.y - pos.y;
                    float dz = ped_pos.z - pos.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist < radius) {
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

// 寻找最靠近指定警员的、在犯罪地点附近的犯罪 NPC，实现多对多任务均衡分配
static CPed* find_best_criminal_target_for_cop(CPed* cop, CVector crime_pos, float radius) {
    if (!cop || !g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return nullptr;
    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return nullptr;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return nullptr;

    CVector cop_pos = get_entity_pos(cop);
    CPed* best_target = nullptr;
    float best_dist = radius; // 限制在指定搜索半径内

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            if (ped && ped != cop) {
                int ped_type = g_GetPedType(ped);
                if (is_gang_or_criminal(ped_type)) {
                    CVector ped_pos = get_entity_pos(ped);
                    // 确保该犯罪分子在冲突中心附近
                    float dx_c = ped_pos.x - crime_pos.x;
                    float dy_c = ped_pos.y - crime_pos.y;
                    float dz_c = ped_pos.z - crime_pos.z;
                    float dist_to_crime = sqrtf(dx_c * dx_c + dy_c * dy_c + dz_c * dz_c);
                    if (dist_to_crime < radius) {
                        // 寻找离当前警员最近的目标
                        float dx = ped_pos.x - cop_pos.x;
                        float dy = ped_pos.y - cop_pos.y;
                        float dz = ped_pos.z - cop_pos.z;
                        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (dist < best_dist) {
                            best_dist = dist;
                            best_target = ped;
                        }
                    }
                }
            }
        }
    }
    return best_target;
}








// =====================================================================
// Pure Virtual Function Safe Patching
// =====================================================================
extern "C" void* safe_pure_virtual_stub() {
    return nullptr;
}

void* find_pure_virtual_target(void* vtable_symbol, int num_slots) {
    if (!vtable_symbol) return nullptr;
    void** slots = reinterpret_cast<void**>(vtable_symbol);
    std::unordered_map<void*, int> counts;
    for (int i = 0; i < num_slots; ++i) {
        if (slots[i]) {
            counts[slots[i]]++;
        }
    }
    void* best_target = nullptr;
    int max_count = 1;
    for (const auto& pair : counts) {
        if (pair.second > max_count) {
            max_count = pair.second;
            best_target = pair.first;
        }
    }
    return best_target;
}

void patch_vtable_pure_virtuals(const char* name, void* vtable_symbol, int num_slots, void* pure_virtual_target, void* stub_func) {
    if (!vtable_symbol || !pure_virtual_target || !stub_func || num_slots <= 0) return;
    
    void** slots = reinterpret_cast<void**>(vtable_symbol);
    uintptr_t page_size = sysconf(_SC_PAGESIZE);
    
    // Calculate page range for num_slots
    uintptr_t start_addr = reinterpret_cast<uintptr_t>(slots);
    uintptr_t end_addr = reinterpret_cast<uintptr_t>(slots + num_slots);
    uintptr_t page_start = start_addr & ~(page_size - 1);
    uintptr_t page_end = (end_addr + page_size - 1) & ~(page_size - 1);
    size_t length = page_end - page_start;
    
    // Change permission of the entire range
    if (mprotect(reinterpret_cast<void*>(page_start), length, PROT_READ | PROT_WRITE) != 0) {
        LOGE("❌ mprotect failed for %s range [%p, %p]: %s", name, reinterpret_cast<void*>(page_start), reinterpret_cast<void*>(page_start + length), strerror(errno));
        return;
    }
    
    int patched_count = 0;
    for (int i = 0; i < num_slots; ++i) {
        if (slots[i] == pure_virtual_target) {
            slots[i] = stub_func;
            patched_count++;
        }
    }
    
    // Restore read-only permission
    mprotect(reinterpret_cast<void*>(page_start), length, PROT_READ);
    
    if (patched_count > 0) {
        LOGI("✅ Patched %d pure virtual slot(s) to safe_pure_virtual_stub in %s (%p)", patched_count, name, vtable_symbol);
    } else {
        LOGI("ℹ️ No pure virtual slot(s) found/patched in %s (%p)", name, vtable_symbol);
    }
}
// =====================================================================
// Hook 安装线程
// =====================================================================
void hook_thread_func() {
    LOGI("Hook thread started, waiting for %s...", TARGET_LIB);

    constexpr int MAX_WAIT_MS = 120000;
    constexpr int POLL_MS = 500;
    int waited = 0;
    void* lib = nullptr;

    // --- 使用 xdl_open 循环等待 libUE4.so 加载 (XDL 会直接在 Linker 链表中检索，不需要 proc/self/maps 权限) ---
    while (waited < MAX_WAIT_MS) {
        lib = xdl_open(TARGET_LIB, XDL_DEFAULT);
        if (lib) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        waited += POLL_MS;
    }

    if (!lib) {
        LOGE("%s not loaded after %d ms", TARGET_LIB, MAX_WAIT_MS);
        return;
    }
    LOGI("%s opened via xdl successfully", TARGET_LIB);

    LOGI("Resolving symbols...");
    RESOLVE_SYM(lib, g_vtable_CTask, "_ZTV5CTask", void*);
    RESOLVE_SYM(lib, g_vtable_CTaskSimple, "_ZTV11CTaskSimple", void*);
    RESOLVE_SYM(lib, g_vtable_CTaskComplex, "_ZTV12CTaskComplex", void*);
    RESOLVE_SYM(lib, g_vtable_CEvent, "_ZTV6CEvent", void*);
    RESOLVE_SYM(lib, g_FindPlayerPed, "_Z13FindPlayerPedi", fn_FindPlayerPed_t);
    RESOLVE_SYM(lib, g_FindPlayerCoors, "_Z15FindPlayerCoorsi", fn_FindPlayerCoors_t);
    RESOLVE_SYM(lib, g_GetPedType, "_ZNK4CPed10GetPedTypeEv", fn_GetPedType_t);
    RESOLVE_SYM(lib, g_GetMatrix, "_ZN10CPlaceable9GetMatrixEv", fn_GetMatrix_t);
    RESOLVE_SYM(lib, g_FindDistToNearestCop,
        "_ZN11CPopulation30FindDistanceToNearestPedOfTypeE8ePedType7CVector",
        fn_FindDistToNearestPedOfType_t);
    RESOLVE_SYM(lib, g_ScriptGenEmergencyCar,
        "_ZN8CCarCtrl37ScriptGenerateOneEmergencyServicesCarEj7CVector",
        fn_ScriptGenEmergencyCar_t);
    RESOLVE_SYM(lib, g_GenOneEmergencyCar,
        "_ZN8CCarCtrl31GenerateOneEmergencyServicesCarEj7CVector",
        fn_GenOneEmergencyCar_t);
    RESOLVE_SYM(lib, g_AddPoliceOccupants,
        "_ZN6CCarAI21AddPoliceCarOccupantsEP8CVehicleb",
        fn_AddPoliceOccupants_t);
    RESOLVE_SYM(lib, g_AddCriminalToKill,
        "_ZN7CCopPed17AddCriminalToKillEP4CPed",
        fn_AddCriminalToKill_t);
    RESOLVE_SYM(lib, g_GetWeaponLockOnTarget, "_ZNK4CPed21GetWeaponLockOnTargetEv", fn_GetWeaponLockOnTarget_t);
    RESOLVE_SYM(lib, g_IsAlive, "_ZNK4CPed7IsAliveEv", fn_IsAlive_t);
    RESOLVE_SYM(lib, g_VehicleInflictDamage, "_ZN8CVehicle13InflictDamageEP7CEntity11eWeaponTypef7CVector", fn_VehicleInflictDamage_t);
    RESOLVE_SYM(lib, g_GetPoolPed, "_Z10GetPoolPedi", fn_GetPoolPed_t);
    RESOLVE_SYM(lib, g_ms_pPedPool, "_ZN6CPools11ms_pPedPoolE", void*);
    RESOLVE_SYM(lib, g_ms_pVehiclePool, "_ZN6CPools15ms_pVehiclePoolE", void*);

    // 解析火源检测相关符号
    RESOLVE_SYM(lib, g_FireManager, "gFireManager", void*);
    RESOLVE_SYM(lib, g_FindNearestFire, "_ZN12CFireManager15FindNearestFireERK7CVectorbb", fn_FindNearestFire_t);

    // 解析摄像机/视野判定相关符号
    RESOLVE_SYM(lib, g_TheCamera, "TheCamera", void*);
    RESOLVE_SYM(lib, g_GetGameCamPosition, "_ZN7CCamera18GetGameCamPositionEv", fn_GetGameCamPosition_t);
    RESOLVE_SYM(lib, g_GetLookDirection, "_ZN7CCamera16GetLookDirectionEv", fn_GetLookDirection_t);

    // 解析任务与载具交互相关符号
    RESOLVE_SYM(lib, g_IsDriver, "_ZNK8CVehicle8IsDriverEPK4CPed", fn_IsDriver_t);
    RESOLVE_SYM(lib, g_IsPassenger, "_ZNK8CVehicle11IsPassengerEPK4CPed", fn_IsPassenger_t);
    RESOLVE_SYM(lib, g_TellOccupantsToLeaveCar, "_ZN6CCarAI23TellOccupantsToLeaveCarEP8CVehicle", fn_TellOccupantsToLeaveCar_t);
    RESOLVE_SYM(lib, g_GetPoolVehicle, "_Z14GetPoolVehiclei", fn_GetPoolVehicle_t);
    RESOLVE_SYM(lib, g_GetCarToGoToCoors, "_ZN6CCarAI17GetCarToGoToCoorsEP8CVehicleP7CVectorib", fn_GetCarToGoToCoors_t);
    RESOLVE_SYM(lib, g_ThePaths, "ThePaths", void*);
    RESOLVE_SYM(lib, g_SwitchRoadsOffInArea, "_ZN9CPathFind20SwitchRoadsOffInAreaEffffffbbb", fn_SwitchRoadsOffInArea_t);
    RESOLVE_SYM(lib, g_GiveWeapon, "_ZN4CPed10GiveWeaponE11eWeaponTypejb", fn_GiveWeapon_t);
    RESOLVE_SYM(lib, g_SetCurrentWeapon, "_ZN4CPed16SetCurrentWeaponE11eWeaponType", fn_SetCurrentWeapon_t);
    RESOLVE_SYM(lib, g_GiveWeaponAtStartOfFight, "_ZN4CPed24GiveWeaponAtStartOfFightEv", fn_GiveWeaponAtStartOfFight_t);

    RESOLVE_SYM(lib, g_TaskNew, "_ZN5CTasknwEm", fn_TaskNew_t);
    RESOLVE_SYM(lib, g_TaskKillCriminal_ctor, "_ZN24CTaskComplexKillCriminalC2EP4CPedb", fn_TaskKillCriminal_ctor_t);
    RESOLVE_SYM(lib, g_SetTask, "_ZN12CTaskManager7SetTaskEP5CTaskib", fn_SetTask_t);
    RESOLVE_SYM(lib, g_TaskEnterCar_ctor, "_ZN20CTaskComplexEnterCarC2EP8CVehiclebbbb", fn_TaskEnterCar_ctor_t);
    RESOLVE_SYM(lib, g_AddTaskPrimaryMaybeInGroup, "_ZN16CPedIntelligence26AddTaskPrimaryMaybeInGroupEP5CTaskb", fn_AddTaskPrimaryMaybeInGroup_t);
    RESOLVE_SYM(lib, g_FindTaskByType, "_ZNK16CPedIntelligence14FindTaskByTypeEi", fn_FindTaskByType_t);
    RESOLVE_SYM(lib, g_vtable_KillCriminal, "_ZTV24CTaskComplexKillCriminal", void*);
    RESOLVE_SYM(lib, g_vtable_EnterCar, "_ZTV20CTaskComplexEnterCar", void*);

    // 解析假枪声事件相关符号
    RESOLVE_SYM(lib, g_CEventGunShot_ctor, "_ZN13CEventGunShotC1EP7CEntity7CVectorS2_b", fn_CEventGunShot_ctor_t);
    RESOLVE_SYM(lib, g_CEventGunShot_dtor, "_ZN13CEventGunShotD1Ev", fn_CEventGunShot_dtor_t);
    RESOLVE_SYM(lib, g_CEventGroup_Add, "_ZN11CEventGroup3AddER6CEventb", fn_CEventGroup_Add_t);

    // 解析游戏引擎自带的 GMalloc 内存分配器指针，确保完全使用 native 分配以使用引擎原生分配器
    RESOLVE_SYM(lib, g_p_GMalloc, "GMalloc", FMalloc**);
    RESOLVE_SYM(lib, g_CSequenceManager_ms_instance, "_ZN16CSequenceManager11ms_instanceE", void**);

    // --- 安装 Hooks ---
    LOGI("Installing hooks...");

    // Hook CCrime::ReportCrime
    g_stub_report_crime = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6CCrime11ReportCrimeE10eCrimeTypeP7CEntityP4CPed",
        reinterpret_cast<void*>(proxy_report_crime),
        reinterpret_cast<void**>(&g_orig_report_crime));
    if (g_stub_report_crime) LOGI("✅ Hooked CCrime::ReportCrime");
    else LOGE("❌ Failed to hook CCrime::ReportCrime: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CEventHandler::RegisterKill
    g_stub_register_kill = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN13CEventHandler12RegisterKillEPK4CPedPK7CEntity11eWeaponTypeb",
        reinterpret_cast<void*>(proxy_register_kill),
        nullptr);
    if (g_stub_register_kill) LOGI("✅ Hooked CEventHandler::RegisterKill");
    else LOGE("❌ Failed to hook RegisterKill: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CWanted::SetWantedLevel
    g_stub_set_wanted = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN7CWanted14SetWantedLevelEi",
        reinterpret_cast<void*>(proxy_set_wanted_level),
        nullptr);
    if (g_stub_set_wanted) LOGI("✅ Hooked CWanted::SetWantedLevel");
    else LOGE("❌ Failed to hook SetWantedLevel: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CWeapon::GenerateDamageEvent
    g_stub_generate_damage_event = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN7CWeapon19GenerateDamageEventEP4CPedP7CEntity11eWeaponTypei14ePedPieceTypesi",
        reinterpret_cast<void*>(proxy_generate_damage_event),
        (void**)&g_orig_generate_damage_event);
    if (g_stub_generate_damage_event) LOGI("✅ Hooked CWeapon::GenerateDamageEvent");
    else LOGE("❌ Failed to hook CWeapon::GenerateDamageEvent: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CEventDamage constructors
    g_stub_event_damage_ctor_c1 = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN12CEventDamageC1EP7CEntityj11eWeaponType14ePedPieceTypeshbb",
        reinterpret_cast<void*>(proxy_event_damage_ctor_c1),
        reinterpret_cast<void**>(&g_orig_event_damage_ctor_c1));
    if (g_stub_event_damage_ctor_c1) LOGI("✅ Hooked CEventDamage::CEventDamage (C1)");
    else LOGE("❌ Failed to hook CEventDamage::CEventDamage (C1): %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_event_damage_ctor_c2 = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN12CEventDamageC2EP7CEntityj11eWeaponType14ePedPieceTypeshbb",
        reinterpret_cast<void*>(proxy_event_damage_ctor_c2),
        reinterpret_cast<void**>(&g_orig_event_damage_ctor_c2));
    if (g_stub_event_damage_ctor_c2) LOGI("✅ Hooked CEventDamage::CEventDamage (C2)");
    else LOGE("❌ Failed to hook CEventDamage::CEventDamage (C2): %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CPed::SetCurrentWeapon
    g_stub_set_current_weapon = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN4CPed16SetCurrentWeaponE11eWeaponType",
        reinterpret_cast<void*>(proxy_SetCurrentWeapon),
        reinterpret_cast<void**>(&g_orig_SetCurrentWeapon));
    if (g_stub_set_current_weapon) LOGI("✅ Hooked CPed::SetCurrentWeapon");
    else LOGE("❌ Failed to hook CPed::SetCurrentWeapon: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTheScripts::Process
    g_stub_the_scripts_process = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN11CTheScripts7ProcessEv",
        reinterpret_cast<void*>(proxy_the_scripts_process),
        reinterpret_cast<void**>(&g_orig_the_scripts_process));
    if (g_stub_the_scripts_process) LOGI("✅ Hooked CTheScripts::Process");
    else LOGE("❌ Failed to hook CTheScripts::Process: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarAI::AddPoliceCarOccupants
    g_stub_add_police_occupants = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6CCarAI21AddPoliceCarOccupantsEP8CVehicleb",
        reinterpret_cast<void*>(proxy_add_police_occupants),
        reinterpret_cast<void**>(&g_orig_add_police_occupants));
    if (g_stub_add_police_occupants) LOGI("✅ Hooked CCarAI::AddPoliceCarOccupants");
    else LOGE("❌ Failed to hook CCarAI::AddPoliceCarOccupants: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarAI::TellOccupantsToLeaveCar
    g_stub_tell_occupants_leave_car = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6CCarAI23TellOccupantsToLeaveCarEP8CVehicle",
        reinterpret_cast<void*>(proxy_tell_occupants_leave_car),
        reinterpret_cast<void**>(&g_orig_tell_occupants_leave_car));
    if (g_stub_tell_occupants_leave_car) LOGI("✅ Hooked CCarAI::TellOccupantsToLeaveCar");
    else LOGE("❌ Failed to hook CCarAI::TellOccupantsToLeaveCar: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarCtrl::GenerateOneEmergencyServicesCar (救护车视距 Bug 修复 Workaround)
    g_stub_generate_one_emergency_car = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN8CCarCtrl31GenerateOneEmergencyServicesCarEj7CVector",
        reinterpret_cast<void*>(proxy_generate_one_emergency_car),
        reinterpret_cast<void**>(&g_orig_generate_one_emergency_car));
    if (g_stub_generate_one_emergency_car) LOGI("✅ Hooked CCarCtrl::GenerateOneEmergencyServicesCar (Ambulance Workaround)");
    else LOGE("❌ Failed to hook CCarCtrl::GenerateOneEmergencyServicesCar: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarCtrl::ScriptGenerateOneEmergencyServicesCar (救护车视距 Bug 修复 Workaround)
    g_stub_script_generate_one_emergency_car = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN8CCarCtrl37ScriptGenerateOneEmergencyServicesCarEj7CVector",
        reinterpret_cast<void*>(proxy_script_generate_one_emergency_car),
        reinterpret_cast<void**>(&g_orig_script_generate_one_emergency_car));
    if (g_stub_script_generate_one_emergency_car) LOGI("✅ Hooked CCarCtrl::ScriptGenerateOneEmergencyServicesCar (Ambulance Script Workaround)");
    else LOGE("❌ Failed to hook CCarCtrl::ScriptGenerateOneEmergencyServicesCar: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));



    // 4b. CTaskSimpleHoldEntity::SetPedPosition
    g_stub_set_ped_pos = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CTaskSimpleHoldEntity14SetPedPositionEP4CPed",
        reinterpret_cast<void*>(proxy_set_ped_pos),
        reinterpret_cast<void**>(&g_orig_set_ped_pos));
    if (g_stub_set_ped_pos) LOGI("✅ Hooked CTaskSimpleHoldEntity::SetPedPosition");
    else LOGE("❌ Failed to hook CTaskSimpleHoldEntity::SetPedPosition: %s", shadowhook_to_errmsg(shadowhook_get_errno()));



    // Hook CTaskManager::ManageTasks (防止各种任务生命周期、清理或零值野指针导致的任务管理闪退)
    g_stub_manage_tasks = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN12CTaskManager11ManageTasksEv",
        reinterpret_cast<void*>(proxy_manage_tasks),
        reinterpret_cast<void**>(&g_orig_manage_tasks));
    if (g_stub_manage_tasks) LOGI("✅ Hooked CTaskManager::ManageTasks");
    else LOGE("❌ Failed to hook CTaskManager::ManageTasks: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CAttractorScanner::ScanForAttractorsInRange (防止行人周期性查找吸引子时任务析构纯虚函数闪退)
    g_stub_scan_for_attractors_in_range = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN17CAttractorScanner24ScanForAttractorsInRangeERK4CPed",
        reinterpret_cast<void*>(proxy_scan_for_attractors_in_range),
        reinterpret_cast<void**>(&g_orig_scan_for_attractors_in_range));
    if (g_stub_scan_for_attractors_in_range) LOGI("✅ Hooked CAttractorScanner::ScanForAttractorsInRange");
    else LOGE("❌ Failed to hook CAttractorScanner::ScanForAttractorsInRange: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexGangFollower::ControlSubTask (防止帮派跟从者任务中 leader/目标 ped 为空时解引用闪退)
    g_stub_ccgf_control = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexGangFollower14ControlSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_ccgf_control),
        reinterpret_cast<void**>(&g_orig_ccgf_control));
    if (g_stub_ccgf_control) LOGI("✅ Hooked CTaskComplexGangFollower::ControlSubTask");
    else LOGE("❌ Failed to hook CTaskComplexGangFollower::ControlSubTask: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));


    // Hook CTaskComplexUsePairedAttractor::CreateNextSubTask (防止找不到吸引子任务时解引用崩溃)
    g_stub_paired_attractor_create_next_sub_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN30CTaskComplexUsePairedAttractor17CreateNextSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_paired_attractor_create_next_sub_task),
        reinterpret_cast<void**>(&g_orig_paired_attractor_create_next_sub_task));
    if (g_stub_paired_attractor_create_next_sub_task) LOGI("✅ Hooked CTaskComplexUsePairedAttractor::CreateNextSubTask");
    else LOGE("❌ Failed to hook CTaskComplexUsePairedAttractor::CreateNextSubTask: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexFacial::Destructor (防止析构时子任务被销毁/空指针的竞态双重释放闪退)
    g_stub_facial_dtor = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN18CTaskComplexFacialD0Ev",
        reinterpret_cast<void*>(proxy_facial_dtor),
        reinterpret_cast<void**>(&g_orig_facial_dtor));
    if (g_stub_facial_dtor) LOGI("✅ Hooked CTaskComplexFacial::~CTaskComplexFacial");
    else LOGE("❌ Failed to hook CTaskComplexFacial::~CTaskComplexFacial: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskManager::FindActiveTaskByType (直接杜绝该函数内对零填充辅助任务/主任务虚表解引用闪退)
    g_stub_find_active_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK12CTaskManager20FindActiveTaskByTypeEi",
        reinterpret_cast<void*>(proxy_find_active_task),
        reinterpret_cast<void**>(&g_orig_find_active_task));
    if (g_stub_find_active_task) LOGI("✅ Hooked CTaskManager::FindActiveTaskByType");
    else LOGE("❌ Failed to hook CTaskManager::FindActiveTaskByType: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));


    // Hook CTaskManager Destructor (析构时强制净化，防止删除零填充任务虚表解引用闪退)
    g_stub_task_manager_destructor = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN12CTaskManagerD1Ev",
        reinterpret_cast<void*>(proxy_task_manager_destructor),
        reinterpret_cast<void**>(&g_orig_task_manager_destructor));
    if (g_stub_task_manager_destructor) LOGI("✅ Hooked CTaskManager Destructor");
    else LOGE("❌ Failed to hook CTaskManager Destructor: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexPartnerGreet::GetPartnerSequence (防止在伙伴问候任务中读取已销毁或零填充任务虚表闪退)
    g_stub_partner_greet_get_sequence = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexPartnerGreet18GetPartnerSequenceEv",
        reinterpret_cast<void*>(proxy_partner_greet_get_sequence),
        reinterpret_cast<void**>(&g_orig_partner_greet_get_sequence));
    if (g_stub_partner_greet_get_sequence) LOGI("✅ Hooked CTaskComplexPartnerGreet::GetPartnerSequence");
    else LOGE("❌ Failed to hook CTaskComplexPartnerGreet::GetPartnerSequence: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CAEPedSpeechAudioEntity::PlayLoadedSound (防止语音实体播放时对 null 语音管理器指针写操作闪退)
    g_stub_play_loaded_sound = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN23CAEPedSpeechAudioEntity15PlayLoadedSoundEv",
        reinterpret_cast<void*>(proxy_play_loaded_sound),
        reinterpret_cast<void**>(&g_orig_play_loaded_sound));
    if (g_stub_play_loaded_sound) LOGI("✅ Hooked CAEPedSpeechAudioEntity::PlayLoadedSound");
    else LOGE("❌ Failed to hook CAEPedSpeechAudioEntity::PlayLoadedSound: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarGenerator::CheckIfWithinRangeOfAnyPlayers (防止刷车器运行时检测玩家距离时，由于玩家 Ped 临时析构置空导致的读操作闪退)
    g_stub_check_if_within_range = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN13CCarGenerator30CheckIfWithinRangeOfAnyPlayersEv",
        reinterpret_cast<void*>(proxy_check_if_within_range),
        reinterpret_cast<void**>(&g_orig_check_if_within_range));
    if (g_stub_check_if_within_range) {
        LOGI("✅ Hooked CCarGenerator::CheckIfWithinRangeOfAnyPlayers");
        // Resolve CPools::ms_pPedPool
        RESOLVE_SYM(lib, g_p_ms_pPedPool, "_ZN6CPools11ms_pPedPoolE", void***);
    }
    else LOGE("❌ Failed to hook CCarGenerator::CheckIfWithinRangeOfAnyPlayers: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexAvoidOtherPedWhileWandering::ControlSubTask (防止在避让行人任务决策中读取他人悬挂野指针任务闪退)
    g_stub_avoid_ped_control = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN39CTaskComplexAvoidOtherPedWhileWandering14ControlSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_avoid_ped_control),
        reinterpret_cast<void**>(&g_orig_avoid_ped_control));
    if (g_stub_avoid_ped_control) LOGI("✅ Hooked CTaskComplexAvoidOtherPedWhileWandering::ControlSubTask");
    else LOGE("❌ Failed to hook CTaskComplexAvoidOtherPedWhileWandering::ControlSubTask: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));




    // Hook CPed::PlayFootSteps (防止转场期间玩家 Clump 临时脱离导致空指针解引用闪退)
    g_stub_play_footsteps = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN4CPed13PlayFootStepsEv",
        reinterpret_cast<void*>(proxy_play_footsteps),
        reinterpret_cast<void**>(&g_orig_play_footsteps));
    if (g_stub_play_footsteps) LOGI("✅ Hooked CPed::PlayFootSteps");
    else LOGE("❌ Failed to hook CPed::PlayFootSteps: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));




    // Hook CPed::ProcessBuoyancy (防止任务槽被置空/野指针导致 ProcessBuoyancy 虚表解引用闪退)
    g_stub_process_buoyancy = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN4CPed15ProcessBuoyancyEv",
        reinterpret_cast<void*>(proxy_process_buoyancy),
        reinterpret_cast<void**>(&g_orig_process_buoyancy));
    if (g_stub_process_buoyancy) LOGI("✅ Hooked CPed::ProcessBuoyancy");
    else LOGE("❌ Failed to hook CPed::ProcessBuoyancy: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CPedIntelligence::ProcessStaticCounter (防止更新静态计数器时对零填充任务解引用闪退)
    g_stub_process_static_counter = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN16CPedIntelligence20ProcessStaticCounterEv",
        reinterpret_cast<void*>(proxy_process_static_counter),
        reinterpret_cast<void**>(&g_orig_process_static_counter));
    if (g_stub_process_static_counter) LOGI("✅ Hooked CPedIntelligence::ProcessStaticCounter");
    else LOGE("❌ Failed to hook CPedIntelligence::ProcessStaticCounter: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook cBuoyancy::ProcessBuoyancy (解决物理tick途中任务被销毁/空指针的竞态问题)
    g_stub_cbuoyancy_process_buoyancy = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN9cBuoyancy15ProcessBuoyancyEP9CPhysicalfP7CVectorS3_",
        reinterpret_cast<void*>(proxy_cbuoyancy_process_buoyancy),
        reinterpret_cast<void**>(&g_orig_cbuoyancy_process_buoyancy));
    if (g_stub_cbuoyancy_process_buoyancy) LOGI("✅ Hooked cBuoyancy::ProcessBuoyancy");
    else LOGE("❌ Failed to hook cBuoyancy::ProcessBuoyancy: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));







    // Hook u_strlen_64 (防止 ICU 字符串长度计算传入野指针崩溃)
    g_stub_u_strlen = shadowhook_hook_sym_name(
        TARGET_LIB,
        "u_strlen_64",
        reinterpret_cast<void*>(proxy_u_strlen),
        reinterpret_cast<void**>(&g_orig_u_strlen));
    if (g_stub_u_strlen) LOGI("✅ Hooked u_strlen_64");
    else LOGE("❌ Failed to hook u_strlen_64: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexSequence::Flush (防止删除/清空序列时因序列中残留零填充任务导致闪退)
    g_stub_sequence_flush = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN20CTaskComplexSequence5FlushEv",
        reinterpret_cast<void*>(proxy_sequence_flush),
        reinterpret_cast<void**>(&g_orig_sequence_flush));
    if (g_stub_sequence_flush) LOGI("✅ Hooked CTaskComplexSequence::Flush");
    else LOGE("❌ Failed to hook CTaskComplexSequence::Flush: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskSimpleEvasiveStep::FinishAnimEvasiveStepCB (解决躲避动作结束回调时任务已被零填充析构的崩溃问题)
    g_stub_finish_anim_evasive_step_cb = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN22CTaskSimpleEvasiveStep23FinishAnimEvasiveStepCBEP21CAnimBlendAssociationPv",
        reinterpret_cast<void*>(proxy_finish_anim_evasive_step_cb),
        reinterpret_cast<void**>(&g_orig_finish_anim_evasive_step_cb));
    if (g_stub_finish_anim_evasive_step_cb) LOGI("✅ Hooked CTaskSimpleEvasiveStep::FinishAnimEvasiveStepCB");
    else LOGE("❌ Failed to hook CTaskSimpleEvasiveStep::FinishAnimEvasiveStepCB: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexBeInGroup::ControlSubTask (防止在组任务被零填充时调用子任务控制导致闪退)
    g_stub_be_in_group_control_sub_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CTaskComplexBeInGroup14ControlSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_be_in_group_control_sub_task),
        reinterpret_cast<void**>(&g_orig_be_in_group_control_sub_task));
    if (g_stub_be_in_group_control_sub_task) LOGI("✅ Hooked CTaskComplexBeInGroup::ControlSubTask");
    else LOGE("❌ Failed to hook CTaskComplexBeInGroup::ControlSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook IKChainManager_c::Update
    g_stub_ik_chain_update = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN16IKChainManager_c6UpdateEf",
        reinterpret_cast<void*>(proxy_ik_chain_update),
        reinterpret_cast<void**>(&g_orig_ik_chain_update));
    if (g_stub_ik_chain_update) LOGI("✅ Hooked IKChainManager_c::Update");
    else LOGE("❌ Failed to hook IKChainManager_c::Update: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCam::Process_FollowPed_SA
    g_stub_process_follow_ped_sa = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN4CCam20Process_FollowPed_SAERK7CVectorfffb",
        reinterpret_cast<void*>(proxy_process_follow_ped_sa),
        reinterpret_cast<void**>(&g_orig_process_follow_ped_sa));
    if (g_stub_process_follow_ped_sa) LOGI("✅ Hooked CCam::Process_FollowPed_SA");
    else LOGE("❌ Failed to hook CCam::Process_FollowPed_SA: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexLeaveCar::MakeAbortable
    g_stub_leave_car_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN20CTaskComplexLeaveCar13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_leave_car_make_abortable),
        reinterpret_cast<void**>(&g_orig_leave_car_make_abortable));
    if (g_stub_leave_car_make_abortable) LOGI("✅ Hooked CTaskComplexLeaveCar::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexLeaveCar::MakeAbortable: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarAI::UpdateCarAI
    g_stub_update_car_ai = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6CCarAI11UpdateCarAIEP8CVehicle",
        reinterpret_cast<void*>(proxy_update_car_ai),
        reinterpret_cast<void**>(&g_orig_update_car_ai));
    if (g_stub_update_car_ai) LOGI("✅ Hooked CCarAI::UpdateCarAI");
    else LOGE("❌ Failed to hook CCarAI::UpdateCarAI: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexFacial::ControlSubTask
    g_stub_facial_control_sub_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN18CTaskComplexFacial14ControlSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_facial_control_sub_task),
        reinterpret_cast<void**>(&g_orig_facial_control_sub_task));
    if (g_stub_facial_control_sub_task) LOGI("✅ Hooked CTaskComplexFacial::ControlSubTask");
    else LOGE("❌ Failed to hook CTaskComplexFacial::ControlSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CWanted::Update (通缉系统更新)
    g_stub_wanted_update = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN7CWanted6UpdateEv",
        reinterpret_cast<void*>(proxy_wanted_update),
        reinterpret_cast<void**>(&g_orig_wanted_update));
    if (g_stub_wanted_update) LOGI("✅ Hooked CWanted::Update");
    else LOGE("❌ Failed to hook CWanted::Update: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CPopulation::AddPed (行人生成)
    g_stub_add_ped = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN11CPopulation6AddPedE8ePedTypejRK7CVectorb",
        reinterpret_cast<void*>(proxy_add_ped),
        reinterpret_cast<void**>(&g_orig_add_ped));
    if (g_stub_add_ped) LOGI("✅ Hooked CPopulation::AddPed");
    else LOGE("❌ Failed to hook CPopulation::AddPed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Patch base class pure virtual slots to neutral stubs
    void* pure_virtual_target = nullptr;
    if (g_vtable_CTask) {
        void** slots = reinterpret_cast<void**>(g_vtable_CTask);
        // Slot 4 is the first pure virtual function in _ZTV5CTask
        pure_virtual_target = slots[4];
        if (pure_virtual_target) {
            LOGI("🎯 Dynamically extracted __cxa_pure_virtual target from g_vtable_CTask[4]: %p", pure_virtual_target);
        }
    }
    if (!pure_virtual_target || pure_virtual_target == reinterpret_cast<void*>(0)) {
        if (g_vtable_CTask) {
            pure_virtual_target = find_pure_virtual_target(g_vtable_CTask, 11);
            if (pure_virtual_target) {
                LOGI("🎯 Dynamically extracted __cxa_pure_virtual target via consensus: %p", pure_virtual_target);
            }
        }
    }
    if (!pure_virtual_target) {
        pure_virtual_target = dlsym(RTLD_DEFAULT, "__cxa_pure_virtual");
        if (pure_virtual_target) {
            LOGI("🎯 Resolved __cxa_pure_virtual via dlsym: %p", pure_virtual_target);
        }
    }
    if (!pure_virtual_target) {
        pure_virtual_target = xdl_sym(lib, "__cxa_pure_virtual", nullptr);
        if (pure_virtual_target) {
            LOGI("🎯 Resolved __cxa_pure_virtual via xdl_sym: %p", pure_virtual_target);
        }
    }
    if (pure_virtual_target) {
        g_pure_virtual_target = pure_virtual_target;
        LOGI("Found __cxa_pure_virtual at %p, scanning and patching base vtables...", pure_virtual_target);
        patch_vtable_pure_virtuals("_ZTV5CTask", g_vtable_CTask, 11, pure_virtual_target, reinterpret_cast<void*>(safe_pure_virtual_stub));
        patch_vtable_pure_virtuals("_ZTV11CTaskSimple", g_vtable_CTaskSimple, 13, pure_virtual_target, reinterpret_cast<void*>(safe_pure_virtual_stub));
        patch_vtable_pure_virtuals("_ZTV12CTaskComplex", g_vtable_CTaskComplex, 15, pure_virtual_target, reinterpret_cast<void*>(safe_pure_virtual_stub));
        patch_vtable_pure_virtuals("_ZTV6CEvent", g_vtable_CEvent, 19, pure_virtual_target, reinterpret_cast<void*>(safe_pure_virtual_stub));
    } else {
        LOGW("⚠️ __cxa_pure_virtual symbol not found! Vtable safety patch could not be applied.");
    }

    if (lib) {
        xdl_close(lib);
    }

    LOGI("=== All hooks installed ===");
    LOGI("=== Police Intervention System ACTIVE ===");
}

// =====================================================================
// 伴生库写入逻辑
// =====================================================================
void write_nothing_so(const std::string& path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd >= 0) {
        ssize_t written = write(fd, sh_nothing_so, sh_nothing_so_len);
        close(fd);
        if (written == static_cast<ssize_t>(sh_nothing_so_len)) {
            LOGI("Successfully extracted libshadowhook_nothing.so to %s", path.c_str());
        } else {
            LOGE("Failed to write full size of libshadowhook_nothing.so to %s: %d", path.c_str(), errno);
        }
    } else {
        LOGW("Failed to open %s for writing: %d", path.c_str(), errno);
    }
}

void extract_nothing_so(const char* process_name) {
    // 提取基础包名（防止子进程包含冒号导致路径失效）
    std::string pkg_name(process_name);
    size_t colon_pos = pkg_name.find(':');
    if (colon_pos != std::string::npos) {
        pkg_name = pkg_name.substr(0, colon_pos);
    }

    // 仅写入应用 code_cache 目录
    // 同时优先且仅使用 /data/user/0/ 的标准规范路径，规避 Android 10+ 下对 /data/data/ 软链接路径的 SELinux 限制
    std::string base_dir = std::string("/data/user/0/") + pkg_name;
    std::string cache_dir = base_dir + "/code_cache";
    
    mkdir(cache_dir.c_str(), 0700);
    
    std::string path = cache_dir + "/libshadowhook_nothing.so";
    write_nothing_so(path);
}

void cleanup_nothing_so(const char* process_name) {
    std::string pkg_name(process_name);
    size_t colon_pos = pkg_name.find(':');
    if (colon_pos != std::string::npos) {
        pkg_name = pkg_name.substr(0, colon_pos);
    }

    std::string path = std::string("/data/user/0/") + pkg_name + "/code_cache/libshadowhook_nothing.so";
    unlink(path.c_str());
    LOGI("Cleaned up libshadowhook_nothing.so from disk");
}

// =====================================================================
// Zygisk 模块
// =====================================================================
class PoliceModModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api_ = api;
        this->env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        if (!args || !args->nice_name) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const char* process = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (!process) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (strstr(process, TARGET_PACKAGE) != nullptr ||
            strstr(process, TARGET_PACKAGE_ALT) != nullptr) {
            LOGI("Target matched: %s", process);
            is_target_ = true;
        } else {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }

        env_->ReleaseStringUTFChars(args->nice_name, process);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!is_target_) return;

        // 获取包名，以提取伴生库到应用私有目录中（此时运行在应用 UID 下，拥有写权限）
        const char* process = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (process) {
            extract_nothing_so(process);

            // 初始化 ShadowHook
            int ret = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
            if (ret != 0) {
                LOGE("ShadowHook init failed in postAppSpecialize: %d", ret);
            } else {
                LOGI("ShadowHook initialized successfully in postAppSpecialize");
            }

            // 初始化完成后，立刻清理磁盘上的 so 文件，写入后删除伴生库文件
            cleanup_nothing_so(process);

            env_->ReleaseStringUTFChars(args->nice_name, process);
        } else {
            LOGE("Failed to get process name in postAppSpecialize");
        }

        // 启动 Hook 线程
        std::thread t(hook_thread_func);
        t.detach();
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool is_target_ = false;
};

REGISTER_ZYGISK_MODULE(PoliceModModule)
