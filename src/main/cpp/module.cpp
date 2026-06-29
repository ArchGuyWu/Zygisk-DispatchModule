/**
 * GTA SA DE 警力派发 Zygisk 模块
 *
 * 入口：module.cpp（全局符号、辅助函数、Zygisk）
 * Hook 安装：hook_install.cpp | 派发：dispatch_*.cpp | 稳定性：hooks_stability.cpp
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

void* g_FireManager = nullptr;
fn_FindNearestFire_t g_FindNearestFire = nullptr;

fn_CEventGunShot_ctor_t g_CEventGunShot_ctor = nullptr;
fn_CEventGunShot_dtor_t g_CEventGunShot_dtor = nullptr;
fn_CEventGroup_Add_t    g_CEventGroup_Add = nullptr;

FMalloc** g_p_GMalloc = nullptr;

fn_RegisterKill_t             g_orig_register_kill = nullptr;
fn_SetWantedLevel_orig_t      g_orig_set_wanted = nullptr;
fn_GetWeaponLockOnTarget_t    g_GetWeaponLockOnTarget = nullptr;
fn_IsAlive_t                  g_IsAlive = nullptr;
fn_VehicleInflictDamage_t     g_VehicleInflictDamage = nullptr;
fn_GetPoolPed_t               g_GetPoolPed = nullptr;
void*                         g_ms_pPedPool = nullptr;
void**                        g_CSequenceManager_ms_instance = nullptr;
void*                         g_ms_pVehiclePool = nullptr;

void*                         g_TheCamera = nullptr;
fn_GetGameCamPosition_t       g_GetGameCamPosition = nullptr;
fn_GetLookDirection_t         g_GetLookDirection = nullptr;

fn_IsDriver_t                g_IsDriver = nullptr;
fn_IsPassenger_t             g_IsPassenger = nullptr;
fn_TellOccupantsToLeaveCar_t g_TellOccupantsToLeaveCar = nullptr;
fn_GetPoolVehicle_t          g_GetPoolVehicle = nullptr;
fn_GetCarToGoToCoors_t        g_GetCarToGoToCoors = nullptr;
fn_SwitchRoadsOffInArea_t     g_SwitchRoadsOffInArea = nullptr;
void*                         g_ThePaths = nullptr;

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
fn_AddTaskPrimaryMaybeInGroup_t g_AddTaskPrimaryMaybeInGroup = nullptr;
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
fn_TheScriptsProcess_t g_orig_the_scripts_process = nullptr;

void* g_stub_event_damage_ctor_c1 = nullptr;
void* g_stub_event_damage_ctor_c2 = nullptr;
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
    // Tier 2 VMA (or Tier 3 pipe fallback) then direct read — no double pipe per field.
    if (!is_pointer_readable(task)) return false;
    void* vtable = *reinterpret_cast<void**>(task);
    if (!vtable) return false;
    if (!vma_is_readable(vtable) && !pipe_probe_readable(vtable)) return false;

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

bool is_firearm(eCrimeType crime) {
    return crime == CRIME_FIRE_WEAPON
        || crime == CRIME_KILL_PED_WITH_GUN
        || crime == CRIME_KILL_COP;
}

bool is_gang_or_criminal(int ped_type) {
    return (ped_type >= PED_TYPE_GANG1 && ped_type <= PED_TYPE_GANG8)
        || ped_type == PED_TYPE_DEALER
        || ped_type == PED_TYPE_CRIMINAL;
}

// 火源 3D 坐标安全提取函数（双偏移兼容 + 异常坐标边界过滤）
bool get_fire_position(void* fire, CVector& out_pos) {
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

unsigned short get_entity_model_index(void* entity) {
    if (!entity) return 0;
    return *reinterpret_cast<unsigned short*>(reinterpret_cast<char*>(entity) + 0x3A);
}

void stabilize_motorcycle(void* veh) {
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

void set_entity_pos(void* entity, CVector pos) {
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
bool is_pos_visible_to_player_camera(CVector pos) {
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
int count_criminals_near(CVector pos, float radius) {
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
CPed* find_best_criminal_target_for_cop(CPed* cop, CVector crime_pos, float radius) {
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
