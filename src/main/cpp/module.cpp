/**
 * Zygisk + ShadowHook: GTA SA DE 警方干预系统 Mod
 *
 * 完整功能列表：
 *   1. 检测 NPC 犯罪行为，区分冷兵器/枪械
 *   2. 非枪械：初始仅 1 单位响应；枪械或有人牺牲：附近全体响应
 *   3. 附近无警力时，冷兵器 30 秒 / 枪械 15 秒 后调度接警车
 *   4. 若犯罪 NPC 被提前消灭或自然刷出警车则取消调度
 *   5. 接警车鸣笛抵达犯罪坐标附近，副驾驶下车停留、驾驶员留在车内
 *   6. 10 秒无事后副驾驶上车离开
 *   7. 玩家协助警方击毙犯罪 NPC 且仅瞄准该 NPC 射击时免遭通缉
 *   8. 每有警察牺牲，分别延迟 10/8/6 秒增派一辆警车，最多 3 辆
 *
 * 技术栈：Zygisk API v4 + ShadowHook (UNIQUE mode)
 *
 * 所有使用的符号均通过 nm -D libUE4.so 验证为导出符号。
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

// =====================================================================
// 日志
// =====================================================================
#define LOG_TAG "dispatchCenter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// =====================================================================
// 目标应用
// =====================================================================
static constexpr const char* TARGET_PACKAGE     = "com.rockstargames.gtasa.de";
static constexpr const char* TARGET_PACKAGE_ALT = "com.netflix.NGP.GTASanAndreasDefinitiveEdition";
static constexpr const char* TARGET_LIB         = "libUE4.so";

// =====================================================================
// GTA SA 引擎前向声明（不透明指针）
// =====================================================================
struct CEntity;
struct CPed;
struct CPlayerPed;
struct CCopPed;
struct CVehicle;
struct CPlaceable;
struct CTaskManager;
struct CPedIntelligence;
struct CTask;
struct CEvent;

// CVector: GTA SA 标准 3D 向量
struct CVector {
    float x, y, z;
    CVector() : x(0), y(0), z(0) {}
    CVector(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct CVector2D {
    float x, y;
    CVector2D() : x(0), y(0) {}
    CVector2D(float _x, float _y) : x(_x), y(_y) {}
};

// CMatrix: GTA SA 4x4 矩阵（简化版，只需要位置分量）
struct CMatrix {
    float right_x, right_y, right_z, pad0;
    float up_x, up_y, up_z, pad1;
    float at_x, at_y, at_z, pad2;
    float pos_x, pos_y, pos_z, pad3;
};

#include "ecs_engine.hpp"

// 犯罪类型枚举（部分常用值）
enum eCrimeType : int {
    CRIME_NONE = 0,
    CRIME_FIRE_WEAPON = 1,
    CRIME_DAMAGED_PED = 3,
    CRIME_DAMAGED_COP = 4,
    CRIME_DAMAGED_CAR = 5,
    CRIME_DAMAGED_COP_CAR = 6,
    CRIME_KILL_PED_WITH_GUN = 12,
    CRIME_KILL_PED_NO_GUN = 13,
    CRIME_KILL_COP = 14,
};

// 武器类型枚举（简化）
enum eWeaponType : int {
    WEAPON_UNARMED = 0,
    WEAPON_BRASSKNUCKLE = 1,
    WEAPON_GOLFCLUB = 2,
    WEAPON_NIGHTSTICK = 3,
    WEAPON_PISTOL = 22,
    WEAPON_PISTOL_SILENCED = 23,
    WEAPON_DESERT_EAGLE = 24,
    WEAPON_SHOTGUN = 25,
    WEAPON_SAWNOFF = 26,
    WEAPON_SPAS12 = 27,
    WEAPON_MICRO_UZI = 28,
    WEAPON_MP5 = 29,
    WEAPON_AK47 = 30,
    WEAPON_M4 = 31,
    WEAPON_TEC9 = 32,
    WEAPON_RIFLE = 33,
    WEAPON_SNIPER = 34,
    WEAPON_MINIGUN = 38,
};

// PedType 常量
static constexpr int PED_TYPE_PLAYER    = 0;
static constexpr int PED_TYPE_COP       = 6;
static constexpr int PED_TYPE_CIVMALE   = 4;
static constexpr int PED_TYPE_CIVFEMALE = 5;
static constexpr int PED_TYPE_DEALER    = 17;
static constexpr int PED_TYPE_GANG1     = 7;   // Ballas
static constexpr int PED_TYPE_GANG8     = 14;  // Varrio Los Aztecas
static constexpr int PED_TYPE_CRIMINAL  = 20;

// 警车模型 ID
static constexpr unsigned int MODEL_POLICE_CAR = 596;  // LSPD 警车
static constexpr unsigned int MODEL_SWAT_VAN   = 427;  // SWAT Enforcer 警车

// =====================================================================
// 函数指针类型定义（全部从 nm -D 确认 of 导出符号）
// =====================================================================

// FindPlayerPed(int player_id) -> CPlayerPed*
typedef CPlayerPed* (*fn_FindPlayerPed_t)(int);

// FindPlayerCoors(int player_id) -> CVector
typedef CVector (*fn_FindPlayerCoors_t)(int);

// CPed::GetPedType() const -> int
typedef int (*fn_GetPedType_t)(const void*);

// CPlaceable::GetMatrix() -> CMatrix*
typedef CMatrix* (*fn_GetMatrix_t)(void*);

// CPopulation::FindDistanceToNearestPedOfType(ePedType, CVector) -> float
typedef float (*fn_FindDistToNearestPedOfType_t)(int, CVector);

// CCarCtrl::ScriptGenerateOneEmergencyServicesCar(unsigned int model, CVector pos) -> ?
// 返回值可能是 CVehicle* 或 void; 从符号签名推断返回 void
typedef void (*fn_ScriptGenEmergencyCar_t)(unsigned int, CVector);

// CCarCtrl::GenerateOneEmergencyServicesCar(unsigned int model, CVector pos)
typedef void (*fn_GenOneEmergencyCar_t)(unsigned int, CVector);

// CCarAI::AddPoliceCarOccupants(CVehicle*, bool) -> void
typedef void (*fn_AddPoliceOccupants_t)(CVehicle*, bool);

// CCopPed::AddCriminalToKill(CPed*) -> void
typedef void (*fn_AddCriminalToKill_t)(void*, CPed*);

// CEventHandler::RegisterKill(CPed const*, CEntity const*, eWeaponType, bool)
typedef void (*fn_RegisterKill_t)(const CPed*, const CEntity*, eWeaponType, bool);

// CRunningScript::IsPedDead(CPed*) -> bool-like
typedef int (*fn_IsPedDead_t)(void*, CPed*);

// CWorld::Add(CEntity*)
typedef void (*fn_WorldAdd_t)(CEntity*);

// CWorld::Remove(CEntity*)
typedef void (*fn_WorldRemove_t)(CEntity*);

// CTaskManager::SetTask(CTask*, int slot, bool) -> void
typedef void (*fn_SetTask_t)(void*, CTask*, int, bool);

// CTaskComplexLeaveCar::CTaskComplexLeaveCar(CVehicle*, int target_door, int delay, bool, bool)
typedef void (*fn_TaskLeaveCar_ctor_t)(void*, CVehicle*, int, int, bool, bool);

// CTaskComplexEnterCar::CTaskComplexEnterCar(CVehicle*, bool as_driver, bool, bool, bool)
typedef void (*fn_TaskEnterCar_ctor_t)(void*, CVehicle*, bool, bool, bool, bool);

// CTaskComplexKillCriminal::CTaskComplexKillCriminal(CPed*, bool)
typedef void (*fn_TaskKillCriminal_ctor_t)(void*, CPed*, bool);

// CCrime::ReportCrime(eCrimeType, CEntity* victim, CPed* perpetrator) [STATIC]
typedef void (*fn_ReportCrime_orig_t)(eCrimeType, CEntity*, CPed*);

// CWanted::SetWantedLevel(int)
typedef void (*fn_SetWantedLevel_orig_t)(void*, int);

// CPed::GetWeaponLockOnTarget() const -> CEntity*
typedef CEntity* (*fn_GetWeaponLockOnTarget_t)(const void*);

// CPed::IsAlive() const -> bool
typedef bool (*fn_IsAlive_t)(const void*);

// CWeapon::GenerateDamageEvent(CPed*, CEntity*, eWeaponType, int, ePedPieceTypes, int) -> bool (STATIC)
typedef bool (*fn_GenerateDamageEvent_orig_t)(CPed*, CEntity*, eWeaponType, int, int, int);

// CVehicle::InflictDamage(CEntity*, eWeaponType, float, CVector)
typedef void (*fn_VehicleInflictDamage_t)(void*, CEntity*, eWeaponType, float, CVector);


// GetPoolPed(int) -> CPed*
typedef CPed* (*fn_GetPoolPed_t)(int);

// CPed::GiveWeapon(eWeaponType, unsigned int, bool) -> void
typedef void (*fn_GiveWeapon_t)(void*, eWeaponType, unsigned int, bool);

// CPed::SetCurrentWeapon(eWeaponType) -> void
typedef void (*fn_SetCurrentWeapon_t)(void*, eWeaponType);

// CPed::GiveWeaponAtStartOfFight() -> void
typedef void (*fn_GiveWeaponAtStartOfFight_t)(void*);

// =====================================================================
// 全局函数指针（运行时通过 dlsym 解析）
// =====================================================================
static fn_FindPlayerPed_t            g_FindPlayerPed = nullptr;
static fn_FindPlayerCoors_t          g_FindPlayerCoors = nullptr;
static fn_GetPedType_t               g_GetPedType = nullptr;
static fn_GetMatrix_t                g_GetMatrix = nullptr;
static fn_FindDistToNearestPedOfType_t g_FindDistToNearestCop = nullptr;
static fn_ScriptGenEmergencyCar_t    g_ScriptGenEmergencyCar = nullptr;
static fn_GenOneEmergencyCar_t       g_GenOneEmergencyCar = nullptr;
static fn_AddPoliceOccupants_t       g_AddPoliceOccupants = nullptr;
static fn_AddCriminalToKill_t        g_AddCriminalToKill = nullptr;
static fn_GiveWeapon_t               g_GiveWeapon = nullptr;
static fn_SetCurrentWeapon_t         g_SetCurrentWeapon = nullptr;
static fn_GiveWeaponAtStartOfFight_t g_GiveWeaponAtStartOfFight = nullptr;

// 火源检测与避让系统全局变量与类型定义
static void* g_FireManager = nullptr;
typedef void* (*fn_FindNearestFire_t)(void* fire_manager_this, const CVector& pos, bool bCheckScriptFires, bool bCheckNormalFires);
static fn_FindNearestFire_t g_FindNearestFire = nullptr;

// 假枪声所需函数指针与类型定义
typedef void (*fn_CEventGunShot_ctor_t)(void*, CEntity*, CVector, CVector, bool);
typedef void (*fn_CEventGunShot_dtor_t)(void*);
typedef void (*fn_CEventGroup_Add_t)(void*, void*, bool);

static fn_CEventGunShot_ctor_t g_CEventGunShot_ctor = nullptr;
static fn_CEventGunShot_dtor_t g_CEventGunShot_dtor = nullptr;
static fn_CEventGroup_Add_t    g_CEventGroup_Add = nullptr;

// Unreal Engine 4 内存分配器接口与全局指针声明
class FMalloc {
public:
    virtual ~FMalloc() {}
    virtual void* Malloc(size_t Count, uint32_t Alignment = 0) = 0;
    virtual void* Realloc(void* Original, size_t Count, uint32_t Alignment = 0) = 0;
    virtual void Free(void* Original) = 0;
};
static FMalloc** g_p_GMalloc = nullptr;

static fn_RegisterKill_t             g_RegisterKill = nullptr;
static fn_GetWeaponLockOnTarget_t    g_GetWeaponLockOnTarget = nullptr;
static fn_IsAlive_t                  g_IsAlive = nullptr;
static fn_VehicleInflictDamage_t     g_VehicleInflictDamage = nullptr;
static fn_GetPoolPed_t               g_GetPoolPed = nullptr;
static void*                         g_ms_pPedPool = nullptr;
static void**                        g_CSequenceManager_ms_instance = nullptr;
static void*                         g_ms_pVehiclePool = nullptr;

// 摄像机/视野判定相关符号
static void*                         g_TheCamera = nullptr;
typedef CVector (*fn_GetGameCamPosition_t)(void*);
typedef CVector (*fn_GetLookDirection_t)(void*);

static fn_GetGameCamPosition_t       g_GetGameCamPosition = nullptr;
static fn_GetLookDirection_t         g_GetLookDirection = nullptr;

// 任务与载具交互相关符号
typedef bool (*fn_IsDriver_t)(const void* vehicle_this, const CPed* ped);
typedef bool (*fn_IsPassenger_t)(const void* vehicle_this, const CPed* ped);
typedef void (*fn_TellOccupantsToLeaveCar_t)(void* vehicle);
typedef void* (*fn_GetPoolVehicle_t)(int);

static fn_IsDriver_t                g_IsDriver = nullptr;
static fn_IsPassenger_t             g_IsPassenger = nullptr;
static fn_TellOccupantsToLeaveCar_t g_TellOccupantsToLeaveCar = nullptr;
static fn_GetPoolVehicle_t          g_GetPoolVehicle = nullptr;

typedef void (*fn_GetCarToGoToCoors_t)(void* vehicle, CVector* coors, int drivingMode, bool flag);
static fn_GetCarToGoToCoors_t        g_GetCarToGoToCoors = nullptr;

typedef void (*fn_SwitchRoadsOffInArea_t)(void* instance, float minX, float minY, float minZ, float maxX, float maxY, float maxZ, bool bSwitchOff, bool bKeepVehicles, bool bAllowBoats);
static fn_SwitchRoadsOffInArea_t     g_SwitchRoadsOffInArea = nullptr;
static void*                         g_ThePaths = nullptr;

typedef void* (*fn_TaskNew_t)(unsigned long);
static fn_TaskNew_t                  g_TaskNew = nullptr;
static fn_TaskKillCriminal_ctor_t    g_TaskKillCriminal_ctor = nullptr;
static fn_SetTask_t                  g_SetTask = nullptr;
static fn_TaskEnterCar_ctor_t        g_TaskEnterCar_ctor = nullptr;
static void*                         g_vtable_KillCriminal = nullptr;
static void*                         g_vtable_EnterCar = nullptr;

static void*                         g_vtable_CTask = nullptr;
static void*                         g_vtable_CTaskSimple = nullptr;
static void*                         g_vtable_CTaskComplex = nullptr;
static void*                         g_vtable_CEvent = nullptr;

typedef void (*fn_AddTaskPrimaryMaybeInGroup_t)(void* ped_intel_this, CTask* task, bool writeToEventLog);
static fn_AddTaskPrimaryMaybeInGroup_t g_AddTaskPrimaryMaybeInGroup = nullptr;

typedef void* (*fn_FindTaskByType_t)(const void* ped_intel_this, int task_type);
static fn_FindTaskByType_t g_FindTaskByType = nullptr;

static constexpr int TASK_COMPLEX_KILL_CRIMINAL = 1105;

// =====================================================================
// Hook 存根
// =====================================================================
static void* g_stub_report_crime = nullptr;
static fn_ReportCrime_orig_t g_orig_report_crime = nullptr;
static void* g_stub_register_kill = nullptr;
static void* g_stub_set_wanted = nullptr;
static void* g_stub_generate_damage_event = nullptr;
static fn_GenerateDamageEvent_orig_t g_orig_generate_damage_event = nullptr;

static void* g_stub_the_scripts_process = nullptr;
typedef void (*fn_TheScriptsProcess_t)();
static fn_TheScriptsProcess_t g_orig_the_scripts_process = nullptr;

static void* g_stub_event_damage_ctor_c1 = nullptr;
static void* g_stub_event_damage_ctor_c2 = nullptr;
typedef void (*fn_EventDamage_ctor_t)(void* event_this, CEntity* damageSource, unsigned int startTime, eWeaponType weaponType, int pieceType, unsigned char damageSeverity, bool b1, bool b2);
static fn_EventDamage_ctor_t g_orig_event_damage_ctor_c1 = nullptr;
static fn_EventDamage_ctor_t g_orig_event_damage_ctor_c2 = nullptr;

static void* g_stub_set_current_weapon = nullptr;
static fn_SetCurrentWeapon_t g_orig_SetCurrentWeapon = nullptr;

// =====================================================================
// 辅助函数
// =====================================================================
// 验证 Ped 指针在 Ped Pool 中是否依然有效，杜绝野指针崩溃 (前向声明所需)
static bool is_ped_pointer_valid_safe(void* target_ped) {
    if (!target_ped) return false;

    // 优先通过 FindPlayerPed 判定玩家 Ped 的有效性，防止转场/淡入淡出期间玩家 Ped 临时不在 Pool 中而被误判
    if (g_FindPlayerPed) {
        if (target_ped == g_FindPlayerPed(-1) || target_ped == g_FindPlayerPed(0)) {
            return true;
        }
    }

    if (!g_ms_pPedPool || !g_GetPoolPed) return false;
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
            if (ped == target_ped) {
                return true;
            }
        }
    }
    return false;
}

static bool is_vehicle_pointer_valid_safe(void* target_veh) {
    if (!target_veh || !g_ms_pVehiclePool || !g_GetPoolVehicle) return false;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool) return false;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return false;

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

struct ThreadLocalPipe {
    int fds[2] = {-1, -1};
    ThreadLocalPipe() {
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            fds[0] = -1;
            fds[1] = -1;
        }
    }
    ~ThreadLocalPipe() {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
};

static inline bool is_pointer_readable(const void* ptr) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000ULL || addr > 0x00007fffffffffffULL || (addr & 7) != 0) {
        return false;
    }
    thread_local static ThreadLocalPipe tl_pipe;
    if (tl_pipe.fds[1] < 0) {
        return false;
    }
    long ret = write(tl_pipe.fds[1], ptr, 1);
    if (ret >= 0) {
        char dummy;
        read(tl_pipe.fds[0], &dummy, 1);
        return true;
    }
    if (errno == EFAULT) {
        return false;
    }
    return false;
}

static void* g_pure_virtual_target = nullptr;

static inline bool is_task_vtable_safe(void* task) {
    if (!task) return true;
    if (!is_pointer_readable(task)) return false;
    void* vtable = *reinterpret_cast<void**>(task);
    if (!vtable) return false;
    if (!is_pointer_readable(vtable)) return false;

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

// 极其鲁棒的火源 3D 坐标安全提取函数（双偏移兼容 + 异常坐标边界过滤）
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

// 验证载具指针在 Vehicle Pool 中是否依然有效，杜绝野指针崩溃 (前向声明所需)
static bool is_vehicle_pointer_valid(void* target_veh) {
    if (!target_veh || !g_ms_pVehiclePool || !g_GetPoolVehicle) return false;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool) return false;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return false;

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

static unsigned short get_entity_model_index(void* entity) {
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
static bool is_entity_pointer_valid(void* entity) {
    if (!entity) return false;
    if (is_ped_pointer_valid_safe(entity)) {
        return true;
    }
    if (is_vehicle_pointer_valid(entity)) {
        return true;
    }
    return false;
}

static CVector get_entity_pos(void* entity) {
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

// 📡 [防穿帮视野检测]：判定指定 3D 坐标点是否在玩家当前的摄像机视野范围内
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

static CVector get_spawn_target(CVector crime_pos) {
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
                        // 在相机前方主视线内，强力扣除 500 分，彻底排除该点，确保绝对不在玩家视野中凭空出现
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
        // 退化备份：若无有效点或全在视线内，则在玩家侧后方强制偏移 32 米
        float fallback_angle = 0.0f;
        if (has_cam) {
            // 取相机方向的相反方向 (后方)
            fallback_angle = atan2f(-cam_dir.y, -cam_dir.x);
        }
        spawn_target.x = crime_pos.x + cosf(fallback_angle) * 32.0f;
        spawn_target.y = crime_pos.y + sinf(fallback_angle) * 32.0f;
        spawn_target.z = base_z;

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
// 犯罪事件追踪系统
// =====================================================================
struct CrimeEvent {
    CPed* criminal = nullptr;      // 犯罪 NPC
    CVector location;              // 犯罪坐标
    bool is_firearm = false;       // 是否使用枪械
    int64_t detect_time_ms = 0;    // 检测到的时间戳
    bool dispatch_sent = false;    // 是否已发送调度
    bool cancelled = false;        // 是否被取消
    int cops_dispatched = 0;       // 已出警的警察数量
    int cops_killed = 0;           // 牺牲的警察数量
    int reinforcements_sent = 0;   // 已增派次数 (最多3次)

    // 并案机制：同一区域内多个犯罪分子的并案追踪
    std::vector<CPed*> consolidated_criminals;
    std::vector<bool> criminal_is_firearm;

    struct CriminalState {
        int first_threat_category = 0;   // 0: 无武装, 1: 近战, 2: 枪械 (用于武器降级保护)
        int current_threat_category = 0; // 当前武器分类
        bool is_active = true;           // 是否活跃
        bool shooting_air = false;       // 是否对空气开枪 (次级活跃)
        bool fleeing = false;            // 是否在逃跑 (不活跃)
    };
    std::unordered_map<CPed*, CriminalState> criminal_states;

    // 接警车信息
    void* spawned_vehicle = nullptr;
    bool occupants_ordered_out = false;
    int64_t spawn_time_ms = 0;

    // 异步任务回调队列结构 (免除 std::thread 多线程开销和幽灵车 Bug)
    struct DelayedTask {
        int64_t execute_time_ms;
        std::function<void()> callback;
    };
    std::vector<DelayedTask> pending_tasks;

    // 警戒区路段禁行相关信息
    bool road_closure_active = false;
    CVector road_closure_center;

    // 多并发调度独立状态机状态变量
    int dispatch_state = 0; // STATE_IDLE
    int64_t timer_start = 0;
    int dispatch_delay_ms = 0;
    int last_cops_killed = 0;
    int64_t on_scene_start = 0;
    std::vector<void*> case_vehicles;
    uint64_t case_id = 0;
};

static std::recursive_mutex g_crime_mutex;
static std::vector<std::shared_ptr<CrimeEvent>> g_active_crimes;
static uint64_t g_next_case_id = 1;

static void cleanup_single_case_vehicles(std::shared_ptr<CrimeEvent> crime);

// 兼容层：原全局活跃标志兼容
struct CrimeActiveCompat {
    bool load() const {
        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
        for (const auto& crime : g_active_crimes) {
            if (crime && !crime->cancelled) return true;
        }
        return false;
    }
    void store(bool active) {
        // 留空以向下兼容原有的状态修改
    }
    operator bool() const {
        return load();
    }
};
static CrimeActiveCompat g_crime_active;

// 兼容层：dummy 案件
static std::shared_ptr<CrimeEvent> g_dummy_crime = []() {
    auto d = std::make_shared<CrimeEvent>();
    d->cancelled = true;
    return d;
}();

static std::shared_ptr<CrimeEvent> get_primary_active_crime() {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    if (g_active_crimes.empty()) {
        return g_dummy_crime;
    }
    CVector player_pos{0, 0, 0};
    if (g_FindPlayerCoors) {
        player_pos = g_FindPlayerCoors(0);
    }
    std::shared_ptr<CrimeEvent> best_crime = nullptr;
    float min_dist = 999999.0f;
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            float dx = crime->location.x - player_pos.x;
            float dy = crime->location.y - player_pos.y;
            float dz = crime->location.z - player_pos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < min_dist) {
                min_dist = dist;
                best_crime = crime;
            }
        }
    }
    if (best_crime) return best_crime;
    for (const auto& crime : g_active_crimes) {
        if (crime) return crime;
    }
    return g_dummy_crime;
}

#define g_active_crime (*get_primary_active_crime())

// 玩家协助追踪
static std::atomic<CPed*> g_tracked_criminal{nullptr};
static std::atomic<int64_t> g_last_assist_time_ms{0};
static std::atomic<bool>    g_is_generating_custom_dispatch{false};    // 是否正在生成自定义调度车辆

// 玩家在协助警察时造成的流弹/误伤优化追踪
static std::atomic<bool>    g_player_stray_bullet_flag{false};         // 是否误伤了市民
static std::atomic<int64_t> g_player_stray_bullet_time{0};             // 市民误伤时间戳
static std::atomic<int>     g_friendly_fire_cop_hits{0};               // 误伤警察次数计数
static std::atomic<int64_t> g_last_friendly_fire_cop_time{0};          // 最后一次误伤警察时间戳
static std::atomic<bool>    g_player_friendly_fire_blocked{false};     // 是否拦截本次误伤通缉

// 用于判定 NPC 是否为自卫
struct AttackedNPC {
    CPed* npc;
    int64_t attack_time;
};
static std::vector<AttackedNPC> g_player_attacked_npcs;
static std::mutex g_attacked_npcs_mutex;

// 暴动限流：限频命令同一犯罪 NPC
struct CommandedCriminal {
    CPed* criminal;
    int64_t command_time;
};
static std::vector<CommandedCriminal> g_commanded_criminals;
static std::mutex g_commanded_criminals_mutex;

// 📡 [临时寻路规避区]：用于记录中途意外（如警车卡死塞车等）临时关闭的路网，以供后续按时开启
struct TemporaryRoadClosure {
    CVector center;
    float radius;
    int64_t reopen_time_ms;
};
static std::vector<TemporaryRoadClosure> g_temp_road_closures;
static std::mutex                         g_temp_closures_mutex;

void make_cops_attack_criminal_immediate(CPed* criminal);
static bool is_specific_criminal_armed_with_firearm(CPed* target_criminal);
static void make_single_cop_attack_criminal(CPed* cop, CPed* criminal, bool force_weapon_update);
static void update_cops_targeting_criminal_event_driven(CPed* criminal);
static void update_primary_criminal_by_threat();

void init_ecs_systems();

// =====================================================================
// 时间工具
// =====================================================================
static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static int get_random_range(int min_val, int max_val) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> distr(min_val, max_val);
    return distr(gen);
}

// 📡 [Decision Engine Optimization]：智能多犯罪事件过滤与优先级决策
// 1. 多犯罪 NPC 响应只应该发生在原版中非枪械攻击（<= 35m）/枪械攻击（<= 75m）的视听范围内。
// 2. 仅当视听范围内不存在枪械攻击时，才允许激活（或劫持）一处远端范围的枪械攻击响应。
// 3. 远端范围的非枪械攻击响应，仅当视听范围内无任何活跃犯罪，且警力不足（周边 60m 无警员或有警员牺牲）时才激活一处。
static bool should_activate_or_hijack_crime(CVector crime_pos, bool firearm) {
    if (!g_FindPlayerCoors) return true; // 降级默认允许
    
    CVector player_pos = g_FindPlayerCoors(0);
    float p_dx = crime_pos.x - player_pos.x;
    float p_dy = crime_pos.y - player_pos.y;
    float p_dz = crime_pos.z - player_pos.z;
    float dist_to_player = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
    
    const float AV_RANGE_FIREARM = 75.0f;
    const float AV_RANGE_MELEE = 35.0f;
    
    // 1. 判定新发生的犯罪是否在其原本的视听范围内 (Local AV)
    bool in_local_av = false;
    if (firearm) {
        in_local_av = (dist_to_player <= AV_RANGE_FIREARM);
    } else {
        in_local_av = (dist_to_player <= AV_RANGE_MELEE);
    }
    
    // 规则 1：如果犯罪发生在其原本的视听范围内，始终允许激活
    if (in_local_av) {
        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
        int active_count = 0;
        for (const auto& crime : g_active_crimes) {
            if (crime && !crime->cancelled) {
                active_count++;
            }
        }
        if (active_count >= 4) {
            // 尝试找一个最远的或者威胁更低的非枪械远端案件取消，给这个本地案件腾位
            std::shared_ptr<CrimeEvent> worst_crime = nullptr;
            float max_dist = -1.0f;
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    float dx = crime->location.x - player_pos.x;
                    float dy = crime->location.y - player_pos.y;
                    float dz = crime->location.z - player_pos.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    float limit = crime->is_firearm ? AV_RANGE_FIREARM : AV_RANGE_MELEE;
                    if (dist > limit && dist > max_dist) {
                        max_dist = dist;
                        worst_crime = crime;
                    }
                }
            }
            if (worst_crime) {
                worst_crime->cancelled = true;
                LOGI("Cancelled distant case %llu (dist=%.1f) to make room for local crime scene", (unsigned long long)worst_crime->case_id, max_dist);
                return true;
            }
            LOGW("Blocked local crime scene because all 4 concurrent case slots are occupied by local active cases");
            return false;
        }
        LOGI("Crime within original AV range (Firearm=%d, Dist=%.1f) -> Normal trigger", firearm, dist_to_player);
        return true;
    }
    
    // 2. 远端犯罪过滤
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    int remote_firearm_count = 0;
    int remote_melee_count = 0;
    int local_count = 0;
    int total_active = 0;
    
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            total_active++;
            float dx = crime->location.x - player_pos.x;
            float dy = crime->location.y - player_pos.y;
            float dz = crime->location.z - player_pos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            float limit = crime->is_firearm ? AV_RANGE_FIREARM : AV_RANGE_MELEE;
            if (dist <= limit) {
                local_count++;
            } else {
                if (crime->is_firearm) {
                    remote_firearm_count++;
                } else {
                    remote_melee_count++;
                }
            }
        }
    }
    
    if (total_active >= 4) {
        LOGI("Blocked remote crime (Dist=%.1f) because 4 concurrent case slots are already occupied", dist_to_player);
        return false; // 已满，不接受远端犯罪
    }
    
    if (firearm) {
        // 规则 2：远端枪械犯罪（dist > 75.0m）
        // “仅当不存在远端枪械攻击时，才允许激活一处远端范围的枪械攻击响应”
        if (remote_firearm_count > 0) {
            LOGI("Blocked remote firearm crime (Dist=%.1f) because another remote firearm crime is already active", dist_to_player);
            return false;
        }
        
        // 如果远端有非枪械犯罪，我们可以给新枪械犯罪让路
        if (remote_melee_count > 0) {
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    float dx = crime->location.x - player_pos.x;
                    float dy = crime->location.y - player_pos.y;
                    float dz = crime->location.z - player_pos.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist > AV_RANGE_MELEE && !crime->is_firearm) {
                        crime->cancelled = true;
                        LOGI("Remote firearm crime (Dist=%.1f) hijacks remote melee crime %llu (Dist=%.1f)", dist_to_player, (unsigned long long)crime->case_id, dist);
                        return true;
                    }
                }
            }
        }
        
        LOGI("Activated remote firearm crime response (Dist=%.1f)", dist_to_player);
        return true;
    } else {
        // 规则 3：远端非枪械犯罪（dist > 35.0m）
        // “远端非枪械犯罪仅当无任何活跃犯罪（或总活跃很少且警力不足）才激活”
        if (total_active > 0) {
            LOGI("Blocked remote melee crime (Dist=%.1f) because there are already active cases", dist_to_player);
            return false;
        }
        
        // 警力不足急需支援
        bool police_insufficient = false;
        if (g_FindDistToNearestCop) {
            float dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, player_pos);
            if (dist_to_cop > 60.0f) {
                police_insufficient = true;
            }
        }
        
        if (police_insufficient) {
            LOGI("Activated remote melee crime response (Dist=%.1f) under low police situation", dist_to_player);
            return true;
        }
        
        LOGI("Blocked remote melee crime (Dist=%.1f) because local area is safe/police are abundant", dist_to_player);
        return false;
    }
}

// =====================================================================
// 📡 [并案机制 (Case Consolidation)]：合并同一区域的多人犯罪事件
// =====================================================================
static void make_cops_attack_criminal(CPed* criminal);

static bool try_consolidate_crime(CPed* perpetrator, CVector crime_pos, bool firearm) {
    if (!perpetrator || !is_ped_pointer_valid_safe(perpetrator)) return false;
    if (!g_FindPlayerCoors) return false;
    
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    
    // 寻找 40 米半径内最近的活跃案件
    std::shared_ptr<CrimeEvent> best_crime = nullptr;
    float min_dist = 999999.0f;
    for (auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            float dx = crime_pos.x - crime->location.x;
            float dy = crime_pos.y - crime->location.y;
            float dz = crime_pos.z - crime->location.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist <= 40.0f && dist < min_dist) {
                min_dist = dist;
                best_crime = crime;
            }
        }
    }
    
    if (best_crime) {
        bool found = false;
        for (size_t i = 0; i < best_crime->consolidated_criminals.size(); ++i) {
            if (best_crime->consolidated_criminals[i] == perpetrator) {
                found = true;
                if (firearm && !best_crime->criminal_is_firearm[i]) {
                    best_crime->criminal_is_firearm[i] = true;
                    LOGI("Consolidated criminal %p escalated to firearm weapon in same area!", perpetrator);
                    
                    auto it = best_crime->criminal_states.find(perpetrator);
                    if (it != best_crime->criminal_states.end()) {
                        it->second.current_threat_category = 2;
                        if (it->second.first_threat_category < 2) {
                            it->second.first_threat_category = 2; // 升级首次攻击类别，因为升级了
                        }
                    }
                }
                break;
            }
        }
        
        if (!found) {
            best_crime->consolidated_criminals.push_back(perpetrator);
            best_crime->criminal_is_firearm.push_back(firearm);
            
            // 初始化嫌疑人详细状态
            CrimeEvent::CriminalState c_state;
            c_state.first_threat_category = firearm ? 2 : 1;
            c_state.current_threat_category = c_state.first_threat_category;
            c_state.is_active = true;
            c_state.shooting_air = false;
            c_state.fleeing = false;
            best_crime->criminal_states[perpetrator] = c_state;
            
            LOGI("📡 [dispatchCenter - CaseMerge] NPC criminal %p merged into active case %llu in same area. Total criminals: %zu", 
                 perpetrator, (unsigned long long)best_crime->case_id, best_crime->consolidated_criminals.size());
        }
        
        // 若新罪犯使用了枪械，自动升级整个案件的枪械属性
        if (firearm && !best_crime->is_firearm) {
            best_crime->is_firearm = true;
            LOGI("📡 [dispatchCenter - CaseMerge] Case %llu escalated to FIREARM due to consolidated criminal %p", (unsigned long long)best_crime->case_id, perpetrator);
        }
        
        // 立即让周边警员也攻击这个新罪犯
        make_cops_attack_criminal(perpetrator);
        return true; // 成功并案
    }
    return false; // 未能并案
}

// =====================================================================
// Hook 1: CCrime::ReportCrime (检测犯罪事件)
//
// 符号: _ZN6CCrime11ReportCrimeE10eCrimeTypeP7CEntityP4CPed
// 签名: static void CCrime::ReportCrime(eCrimeType, CEntity* victim, CPed* perpetrator)
// =====================================================================
static void proxy_report_crime(eCrimeType crime_type, CEntity* victim, CPed* perpetrator) {
    SHADOWHOOK_STACK_SCOPE();

    if (perpetrator && g_GetPedType && g_FindPlayerPed && is_ped_pointer_valid_safe(perpetrator)) {
        int perp_type = g_GetPedType(perpetrator);
        CPlayerPed* player = g_FindPlayerPed(0);

        // --- 玩家免责逻辑 ---
        if (player && reinterpret_cast<CPed*>(player) == perpetrator) {
            CPed* tracked = g_tracked_criminal.load();
            if (tracked != nullptr && victim == reinterpret_cast<CEntity*>(tracked)) {
                LOGI("Player assisting police against tracked criminal -> CCrime::ReportCrime BLOCKED");
                return;  // 不报告犯罪，玩家免遭通缉
            }
        }

        // --- NPC 犯罪检测逻辑 (事件驱动) ---
        bool is_fire = (crime_type == CRIME_FIRE_WEAPON);
        if ((victim || is_fire) && perp_type != PED_TYPE_PLAYER && perp_type != PED_TYPE_COP) {
            bool firearm = is_firearm(crime_type);
            int weap_cat = 0;
            if (firearm) {
                weap_cat = 2; // FIREARM
            } else {
                if (crime_type == CRIME_KILL_PED_NO_GUN || crime_type == CRIME_DAMAGED_PED || crime_type == CRIME_DAMAGED_COP) {
                    weap_cat = 1; // MELEE
                } else {
                    weap_cat = 0; // UNARMED
                }
            }
            CVector crime_pos = get_entity_pos(perpetrator);

            ecs::EventDispatcher::get().dispatch(ecs::CrimeReportEvent(perpetrator, victim, crime_pos, firearm, weap_cat, now_ms()));
        }
    }

    SHADOWHOOK_CALL_PREV(proxy_report_crime, crime_type, victim, perpetrator);
}

// =====================================================================
// Hook 2: CEventHandler::RegisterKill (检测警察阵亡)
//
// 符号: _ZN13CEventHandler12RegisterKillEPK4CPedPK7CEntity11eWeaponTypeb
// 签名: void CEventHandler::RegisterKill(CPed const*, CEntity const*, eWeaponType, bool)
// =====================================================================
static void proxy_register_kill(const CPed* dead_ped,
                                 const CEntity* killer,
                                 eWeaponType weapon,
                                 bool unk) {
    SHADOWHOOK_STACK_SCOPE();

    if (dead_ped && g_GetPedType && is_ped_pointer_valid_safe(const_cast<CPed*>(dead_ped))) {
        int dead_type = g_GetPedType(dead_ped);

        // 如果玩家杀死了警察，彻底剥夺免责状态
        if (dead_type == PED_TYPE_COP && g_FindPlayerPed) {
            CPlayerPed* player = g_FindPlayerPed(0);
            if (player && killer == reinterpret_cast<const CEntity*>(player)) {
                g_player_friendly_fire_blocked.store(false);
                g_friendly_fire_cop_hits.store(999); // 设为极高，让免责失效
                LOGW("Player directly killed cop %p -> Exemption immediately revoked!", dead_ped);
            }
        }

        // 犯罪 NPC 被杀 → 并案逻辑处理与事件解决判定
        {
            std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    auto& list = crime->consolidated_criminals;
                    auto& is_fire_list = crime->criminal_is_firearm;
                    auto it = std::find(list.begin(), list.end(), const_cast<CPed*>(dead_ped));
                    if (it != list.end()) {
                        size_t idx = std::distance(list.begin(), it);
                        LOGI("📡 [dispatchCenter - CaseMerge] Consolidated criminal NPC %p killed in case %llu (Index: %zu)", dead_ped, (unsigned long long)crime->case_id, idx);
                        
                        // 从并案列表和详细状态中擦除
                        list.erase(it);
                        if (idx < is_fire_list.size()) {
                            is_fire_list.erase(is_fire_list.begin() + idx);
                        }
                        crime->criminal_states.erase(const_cast<CPed*>(dead_ped));

                        // 如果被杀的是当前 primary criminal
                        if (crime->criminal == dead_ped) {
                            if (!list.empty()) {
                                // 还有其他同伙，将 primary criminal 转移到下一个存活的罪犯上
                                crime->criminal = list.front();
                                if (g_tracked_criminal.load() == dead_ped) {
                                    g_tracked_criminal.store(list.front());
                                }
                                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Primary criminal %p killed. Shifted primary tracking to %p.", 
                                     (unsigned long long)crime->case_id, dead_ped, crime->criminal);
                            } else {
                                // 所有同伙都死光了 -> 事件解决并结案
                                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: All consolidated criminal NPCs killed -> crime event resolved", (unsigned long long)crime->case_id);
                                crime->cancelled = true;
                                if (g_tracked_criminal.load() == dead_ped) {
                                    g_tracked_criminal.store(nullptr);
                                }
                                cleanup_single_case_vehicles(crime);
                            }
                        } else {
                            if (list.empty()) {
                                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: All consolidated criminal NPCs killed -> crime event resolved", (unsigned long long)crime->case_id);
                                crime->cancelled = true;
                                if (g_tracked_criminal.load() == dead_ped) {
                                    g_tracked_criminal.store(nullptr);
                                }
                                cleanup_single_case_vehicles(crime);
                            }
                        }
                    }
                }
            }
        }

        // 警察被杀 → 针对 80 米内最近的活跃案件触发增援
        if (dead_type == PED_TYPE_COP) {
            std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
            CVector cop_pos = get_entity_pos(const_cast<CPed*>(dead_ped));
            
            std::shared_ptr<CrimeEvent> nearest_crime = nullptr;
            float min_dist_sq = 80.0f * 80.0f; // 仅计算 80 米内的案件
            
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    float dx = cop_pos.x - crime->location.x;
                    float dy = cop_pos.y - crime->location.y;
                    float dz = cop_pos.z - crime->location.z;
                    float dist_sq = dx * dx + dy * dy + dz * dz;
                    if (dist_sq < min_dist_sq) {
                        min_dist_sq = dist_sq;
                        nearest_crime = crime;
                    }
                }
            }
            
            if (nearest_crime) {
                nearest_crime->cops_killed++;
                int killed = nearest_crime->cops_killed;
                LOGI("📡 [dispatchCenter] Cop killed at scene of case %llu (total: %d, dist: %.1f) -> triggering reinforcement!", 
                     (unsigned long long)nearest_crime->case_id, killed, sqrtf(min_dist_sq));
            } else {
                LOGI("📡 [dispatchCenter] Cop killed far from any active case -> ignored");
            }
        }
    }

    if (dead_ped && is_ped_pointer_valid_safe(const_cast<CPed*>(dead_ped))) {
        ecs::EventDispatcher::get().dispatch(ecs::EntityCleanupEvent(const_cast<CPed*>(dead_ped), true));
    }

    SHADOWHOOK_CALL_PREV(proxy_register_kill, dead_ped, killer, weapon, unk);
}

// =====================================================================
// Hook 3: CWanted::SetWantedLevel (玩家通缉拦截)
//
// 符号: _ZN7CWanted14SetWantedLevelEi
// 签名: void CWanted::SetWantedLevel(int level)
// =====================================================================
static void proxy_set_wanted_level(void* this_wanted, int level) {
    SHADOWHOOK_STACK_SCOPE();

    bool block_wanted = false;
    bool assisting = g_crime_active.load() || (now_ms() - g_last_assist_time_ms.load() < 4000);

    if (assisting) {
        if (g_friendly_fire_cop_hits.load() > 3) {
            block_wanted = false;
        } else {
            // 1. 原生协助判定：如果玩家正锁死目标犯人，或最近对犯人造成了伤害
            bool assist = false;
            if (g_FindPlayerPed && g_GetWeaponLockOnTarget) {
                CPlayerPed* player = g_FindPlayerPed(0);
                if (player) {
                    CEntity* target = g_GetWeaponLockOnTarget(player);
                    CPed* criminal = g_tracked_criminal.load();
                    if (target && target == reinterpret_cast<CEntity*>(criminal)) {
                        assist = true;
                    }
                }
            }
            if (!assist && (now_ms() - g_last_assist_time_ms.load() < 4000)) {
                assist = true;
            }

            if (assist) {
                block_wanted = true;
            } else {
                // 2. 检查市民流弹保护：最近 3 秒内发生过市民误伤
                if (g_player_stray_bullet_flag.load() && (now_ms() - g_player_stray_bullet_time.load() < 3000)) {
                    block_wanted = true;
                    LOGI("SetWantedLevel: Civilian stray bullet protection triggered -> Block wanted level %d", level);
                }
                // 3. 检查警员轻微误伤保护：误伤处于拦截状态且最后误伤在 3 秒内
                else if (g_player_friendly_fire_blocked.load() && (now_ms() - g_last_friendly_fire_cop_time.load() < 3000)) {
                    block_wanted = true;
                    LOGI("SetWantedLevel: Cop friendly fire protection active -> Block wanted level %d", level);
                }
            }
        }
    }

    if (level >= 1 && block_wanted) {
        LOGI("Player wanted level %d BLOCKED (Assisting=%d, CivilianStray=%d, CopFriendlyFire=%d)",
             level, assisting ? 1 : 0, g_player_stray_bullet_flag.load() ? 1 : 0, g_player_friendly_fire_blocked.load() ? 1 : 0);
        return;
    }

    SHADOWHOOK_CALL_PREV(proxy_set_wanted_level, this_wanted, level);
}

// =====================================================================
// Hook 4: CWeapon::GenerateDamageEvent (核心：检测 NPC 犯罪与伤害输出)
// =====================================================================
static bool proxy_generate_damage_event(CPed* victim,
                                         CEntity* perpetrator,
                                         eWeaponType weaponType,
                                         int damage,
                                         int pedPiece,
                                         int direction) {
    SHADOWHOOK_STACK_SCOPE();

    if (perpetrator && victim && g_GetPedType && g_FindPlayerPed) {
        if (is_ped_pointer_valid_safe(perpetrator) && is_ped_pointer_valid_safe(victim)) {
            int perp_type = g_GetPedType(perpetrator);
            int victim_type = g_GetPedType(victim);
            CPlayerPed* player = g_FindPlayerPed(0);

            // 1. 如果是玩家攻击 NPC -> 记录协助状态，更新自卫判定，并处理流弹市民与友好火力警员的免责检测
            if (player && perpetrator == reinterpret_cast<CEntity*>(player) && victim != reinterpret_cast<CPed*>(player)) {
                CPed* tracked = g_tracked_criminal.load();
                if (tracked && victim == tracked) {
                    g_last_assist_time_ms.store(now_ms());
                } else {
                    // 玩家伤害了非追踪犯人 -> 检查当前是否在警方协助状态下，以此优化误伤体验
                    bool assisting = g_crime_active.load() || (now_ms() - g_last_assist_time_ms.load() < 4000);
                    if (assisting) {
                        if (victim_type == PED_TYPE_COP) {
                            // 误伤警员判定
                            int64_t cur_time = now_ms();
                            if (cur_time - g_last_friendly_fire_cop_time.load() > 5000) {
                                g_friendly_fire_cop_hits.store(1); // 超过5s未误击，重置计数
                            } else {
                                g_friendly_fire_cop_hits.fetch_add(1);
                            }
                            g_last_friendly_fire_cop_time.store(cur_time);

                            int hits = g_friendly_fire_cop_hits.load();
                            if (hits <= 3) {
                                g_player_friendly_fire_blocked.store(true);
                                LOGI("Player accidentally hit cop (hits=%d/3 within 5s) during assistance -> Flagged friendly fire exemption", hits);
                            } else {
                                g_player_friendly_fire_blocked.store(false);
                                LOGW("Player attacked cop repeatedly (hits=%d) -> friendly fire exemption REVOKED", hits);
                            }
                        } else {
                            // 误击普通市民 -> 标记流弹免责，有效期 3 秒
                            g_player_stray_bullet_flag.store(true);
                            g_player_stray_bullet_time.store(now_ms());
                            LOGI("Player stray bullet hit civilian %p (type=%d) during assistance -> Civilian protection active", victim, victim_type);
                        }
                    }
                }

                // 记录玩家对当前 NPC 的攻击，时效为 30 秒 (用于 NPC 自卫判定)
                std::lock_guard<std::mutex> lock(g_attacked_npcs_mutex);
                int64_t cur_time = now_ms();
                // 手动清理过期记录，并顺便清理掉已经无效的 NPC
                for (auto it = g_player_attacked_npcs.begin(); it != g_player_attacked_npcs.end();) {
                    if (cur_time - it->attack_time > 30000 || !is_ped_pointer_valid_safe(it->npc)) {
                        it = g_player_attacked_npcs.erase(it);
                    } else {
                        ++it;
                    }
                }
                bool found = false;
                for (auto& item : g_player_attacked_npcs) {
                    if (item.npc == victim) {
                        item.attack_time = cur_time;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    g_player_attacked_npcs.push_back({victim, cur_time});
                    LOGI("Recorded player attack on NPC %p for self-defense check", victim);
                }
            }

            // 1b. 警员受击自卫事件驱动：如果受害者是执勤警员，且攻击者是普通市民/黑帮NPC（非警察/非玩家），且伤害大于0
            if (victim_type == PED_TYPE_COP && perpetrator && perp_type != PED_TYPE_COP && perp_type != PED_TYPE_PLAYER && damage > 0) {
                ecs::EventDispatcher::get().dispatch(ecs::DamageEvent(victim, perpetrator, (int)weaponType, false, now_ms()));
            }

            // 2. 检测 NPC 犯罪行为 (非玩家且非警察攻击他人)
            if (player && perpetrator != reinterpret_cast<CEntity*>(player) && perp_type != PED_TYPE_COP && perpetrator != reinterpret_cast<CEntity*>(victim)) {
                // 自卫判定：如果受害人是玩家，且该 NPC 在最近 30 秒内被玩家先动手打过，判定为 NPC 正当防卫，不视作犯罪
                bool is_self_defense = false;
                if (victim == reinterpret_cast<CPed*>(player)) {
                    bool already_criminal = false;
                    {
                        std::lock_guard<std::recursive_mutex> lock_crime(g_crime_mutex);
                        if (g_crime_active.load() && g_active_crime.criminal == reinterpret_cast<CPed*>(perpetrator)) {
                            already_criminal = true;
                        }
                    }
                    if (!already_criminal) {
                        std::lock_guard<std::mutex> lock(g_attacked_npcs_mutex);
                        int64_t cur_time = now_ms();
                        for (const auto& item : g_player_attacked_npcs) {
                            if (item.npc == reinterpret_cast<CPed*>(perpetrator) && (cur_time - item.attack_time <= 30000)) {
                                is_self_defense = true;
                                break;
                            }
                        }
                    }
                }

                if (is_self_defense) {
                    LOGI("NPC %p attacks player in self-defense (player initiated attack first) -> BLOCKED from wanted/dispatch", perpetrator);
                } else {
                    int weap_cat = 0;
                    if (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN) {
                        weap_cat = 2; // FIREARM
                    } else if (weaponType == WEAPON_UNARMED) {
                        weap_cat = 0; // UNARMED
                    } else {
                        weap_cat = 1; // MELEE
                    }
                    bool firearm = (weap_cat == 2);
                    CVector crime_pos = get_entity_pos(perpetrator);

                    ecs::EventDispatcher::get().dispatch(ecs::CrimeReportEvent(reinterpret_cast<CPed*>(perpetrator), victim, crime_pos, firearm, weap_cat, now_ms()));
                }
            }
        }
    }

    return SHADOWHOOK_CALL_PREV(proxy_generate_damage_event, victim, perpetrator, weaponType, damage, pedPiece, direction);
}

static void handle_damage_event(CEntity* damageSource, eWeaponType weaponType) {
    if (!damageSource || !g_GetPedType || !g_FindPlayerPed) return;

    if (is_ped_pointer_valid_safe(damageSource)) {
        CPed* perpetrator = reinterpret_cast<CPed*>(damageSource);
        int perp_type = g_GetPedType(perpetrator);
        CPlayerPed* player = g_FindPlayerPed(0);

        if (player && perpetrator != reinterpret_cast<CPed*>(player) && perp_type != PED_TYPE_COP) {
            // 自卫判定：如果该 NPC 在最近 30 秒内被玩家打过，判定为自卫，不视作犯罪
            bool is_self_defense = false;
            {
                std::lock_guard<std::mutex> lock(g_attacked_npcs_mutex);
                int64_t cur_time = now_ms();
                for (const auto& item : g_player_attacked_npcs) {
                    if (item.npc == perpetrator && (cur_time - item.attack_time <= 30000)) {
                        is_self_defense = true;
                        break;
                    }
                }
            }

            if (is_self_defense) {
                LOGI("NPC %p attacks in self-defense -> BLOCKED from wanted/dispatch", perpetrator);
            } else {
                int weap_cat = 0;
                if (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN) {
                    weap_cat = 2; // FIREARM
                } else if (weaponType == WEAPON_UNARMED) {
                    weap_cat = 0; // UNARMED
                } else {
                    weap_cat = 1; // MELEE
                }
                bool firearm = (weap_cat == 2);
                CVector crime_pos = get_entity_pos(perpetrator);

                ecs::EventDispatcher::get().dispatch(ecs::CrimeReportEvent(perpetrator, nullptr, crime_pos, firearm, weap_cat, now_ms()));
            }
        }
    }
}

static void proxy_event_damage_ctor_c1(void* event_this,
                                        CEntity* damageSource,
                                        unsigned int startTime,
                                        eWeaponType weaponType,
                                        int pieceType,
                                        unsigned char damageSeverity,
                                        bool b1,
                                        bool b2) {
    SHADOWHOOK_STACK_SCOPE();
    handle_damage_event(damageSource, weaponType);
    SHADOWHOOK_CALL_PREV(proxy_event_damage_ctor_c1, event_this, damageSource, startTime, weaponType, pieceType, damageSeverity, b1, b2);
}

static void proxy_event_damage_ctor_c2(void* event_this,
                                        CEntity* damageSource,
                                        unsigned int startTime,
                                        eWeaponType weaponType,
                                        int pieceType,
                                        unsigned char damageSeverity,
                                        bool b1,
                                        bool b2) {
    SHADOWHOOK_STACK_SCOPE();
    handle_damage_event(damageSource, weaponType);
    SHADOWHOOK_CALL_PREV(proxy_event_damage_ctor_c2, event_this, damageSource, startTime, weaponType, pieceType, damageSeverity, b1, b2);
}

// =====================================================================
// Hook 2b: CPed::SetCurrentWeapon (事件驱动：监控犯罪嫌疑人切枪)
// =====================================================================
static void proxy_SetCurrentWeapon(CPed* ped, eWeaponType weaponType) {
    SHADOWHOOK_STACK_SCOPE();

    if (ped && is_ped_pointer_valid_safe(ped)) {
        int prev_weap = 0;
        auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
        if (combat) {
            prev_weap = combat->current_weapon_type;
        }
        ecs::EventDispatcher::get().dispatch(ecs::WeaponSwitchEvent(ped, prev_weap, (int)weaponType, now_ms()));
        std::shared_ptr<CrimeEvent> belonging_crime = nullptr;
        size_t criminal_idx = 0;
        bool is_our_criminal = false;
        
        {
            std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    for (size_t idx = 0; idx < crime->consolidated_criminals.size(); ++idx) {
                        if (crime->consolidated_criminals[idx] == ped) {
                            belonging_crime = crime;
                            criminal_idx = idx;
                            is_our_criminal = true;
                            break;
                        }
                    }
                    if (is_our_criminal) break;
                }
            }
            
            if (is_our_criminal && belonging_crime) {
                int current_threat = 0;
                if (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN) {
                    current_threat = 2; // FIREARM
                } else if (weaponType == WEAPON_UNARMED) {
                    current_threat = 0; // UNARMED
                } else {
                    current_threat = 1; // MELEE
                }
                
                auto it = belonging_crime->criminal_states.find(ped);
                if (it != belonging_crime->criminal_states.end()) {
                    int prev_first = it->second.first_threat_category;
                    it->second.current_threat_category = current_threat;
                    
                    if (current_threat > prev_first) {
                        // 武器升级：提高其优先级，并升级首要威胁
                        it->second.first_threat_category = current_threat;
                        it->second.is_active = true; // 升级后立即设为活跃
                        LOGI("⚡️ [Event-Driven] Criminal %p upgraded weapon. Threat level raised to %d (first_threat escalated).", ped, current_threat);
                        
                        if (current_threat == 2) {
                            belonging_crime->is_firearm = true;
                            if (criminal_idx < belonging_crime->criminal_is_firearm.size()) {
                                belonging_crime->criminal_is_firearm[criminal_idx] = true;
                            }
                        }
                    } else if (current_threat < prev_first) {
                        // 武器降级：保留首次攻击时的武器类别（武器降级保护），但归类为不活跃级别
                        it->second.is_active = false;
                        LOGI("⚡️ [Event-Driven] Criminal %p degraded weapon to %d (first_threat was %d). Classified as Inactive for tactical balance.", ped, current_threat, prev_first);
                    } else {
                        // 武器分类相同
                        if (current_threat == 2) {
                            it->second.is_active = true;
                        }
                    }
                }
            }
        }
        
        if (is_our_criminal && belonging_crime) {
            // 在锁外调用以规避死锁，对响应的地面巡警触发高优先级的攻击与即时武器更新
            update_cops_targeting_criminal_event_driven(ped);
            
            bool firearm = (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN);
            if (firearm) {
                make_cops_attack_criminal_immediate(ped);
            }
        }
    }

    SHADOWHOOK_CALL_PREV(proxy_SetCurrentWeapon, ped, weaponType);
}

// =====================================================================
// 获取 Ped 的 CTaskManager
static void* get_ped_intelligence(CPed* ped) {
    if (!ped) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5E8);
}

// 判定警车对于玩家是否可见或处于近场 (避免可见瞬移/强制掉头)
static bool is_cop_visible_to_player(void* veh, float current_x, float current_y, float current_z) {
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
static bool is_vehicle_occupied_by_driver(void* veh);

// 指派 CTaskComplexEnterCar 任务，让其上车
static void make_cop_enter_vehicle(CPed* cop, void* vehicle, bool as_driver) {
    if (!cop || !is_ped_pointer_valid_safe(cop) || !vehicle || !is_vehicle_pointer_valid_safe(vehicle) || !g_TaskNew || !g_TaskEnterCar_ctor || !g_SetTask) return;
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

// 🚧 [Event Group Fix]：在 64 位 Android (Definitive Edition) 下，CPedIntelligence 中的 CEventGroup 
// 偏移量确定为 0xC8 (200 字节)。之前使用的 0x30~0xC0 范围扫描在 0x60 处有假阳性指针，
// 传入 Add 会导致严重的 SIGSEGV 崩溃。此处改为直接安全偏移，100% 解决崩溃。
static void* get_ped_event_group(CPed* ped) {
    void* intelligence = get_ped_intelligence(ped);
    if (!intelligence) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<char*>(intelligence) + 0xC8);
}

// 在载具池中查找最靠近指定坐标的载具（支持排除指定载具）
static void* find_closest_vehicle_to(CVector pos, float max_dist, void* ignore_veh = nullptr) {
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
static void* find_vehicle_of_cop(CPed* cop) {
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
static bool is_vehicle_occupied_by_driver(void* veh) {
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
    // 并且我们调用 TellOccupantsToLeaveCar 让警员战术提前离车包抄，实现极其优雅、平稳的减速刹车
    if (dist_to_target < 32.0f) {
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
static void command_cop_vehicle_to_scene(void* vehicle, const CVector& target_loc) {
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

struct CopVehicleBinding {
    CPed* cop;
    void* vehicle;
    bool as_driver;
};
static std::vector<CopVehicleBinding> g_cop_vehicle_bindings;
static std::mutex g_bindings_mutex;

// 查找警员先前绑定的警车
static void* find_bound_vehicle_of_cop(CPed* cop, bool& out_is_driver) {
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
static bool is_alive_bound_driver_exists(void* vehicle) {
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

static std::vector<void*> g_spawned_cop_vehicles;
static std::mutex g_spawned_cop_vehicles_mutex;

struct CopExitRecord {
    CPed* cop;
    int64_t exit_time;
};
static std::vector<CopExitRecord> g_cop_exits;
static std::mutex g_exits_mutex;

// Missing global variables and mutexes for dispatched vehicle & cop state tracking
static std::set<void*> g_vehicles_emptied;
static std::mutex g_vehicles_emptied_mutex;

static std::map<void*, int64_t> g_dispatched_vehicles_time;
static std::mutex g_dispatched_vehicles_time_mutex;

static std::map<std::pair<CPed*, CPed*>, int64_t> g_cop_attack_assign_time;
static std::mutex g_cop_attack_assign_mutex;

static std::map<CPed*, int64_t> g_armed_cops_time;
static std::mutex g_armed_cops_mutex;

static std::map<CPed*, eWeaponType> g_cop_assigned_weapon;
static std::mutex g_cop_assigned_weapon_mutex;

static std::set<void*> g_vehicles_ordered_to_scene;
static std::mutex g_vehicles_mutex; // as used in line 2981 and 2447

static std::set<void*> g_vehicles_siren_awakened;
static std::mutex g_vehicles_siren_awakened_mutex;

// Helper functions for vehicle status queries and modifications
static bool is_vehicle_emptied(void* vehicle) {
    if (!vehicle) return false;
    std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
    return g_vehicles_emptied.count(vehicle) > 0;
}

static void add_vehicle_emptied(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
    g_vehicles_emptied.insert(vehicle);
}

static bool is_vehicle_ordered_to_scene(void* vehicle) {
    if (!vehicle) return false;
    std::lock_guard<std::mutex> lock(g_vehicles_mutex);
    return g_vehicles_ordered_to_scene.count(vehicle) > 0;
}

static void add_vehicle_ordered_to_scene(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_vehicles_mutex);
    g_vehicles_ordered_to_scene.insert(vehicle);
}

static bool is_vehicle_siren_awakened(void* vehicle) {
    if (!vehicle) return false;
    std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
    return g_vehicles_siren_awakened.count(vehicle) > 0;
}

static void add_vehicle_siren_awakened(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
    g_vehicles_siren_awakened.insert(vehicle);
}

struct StuckTracker {
    int64_t stuck_since = 0;
    int64_t last_check_time = 0;
    CVector last_pos = {0.0f, 0.0f, 0.0f};
    int64_t last_intervention_time = 0;
    float last_dir_x = 0.0f;
    float last_dir_y = 0.0f;
    int spin_count = 0;
};
static std::map<void*, StuckTracker> g_stuck_vehicles;
static std::mutex g_stuck_vehicles_mutex;

static std::set<void*> g_spawned_swats;
static std::mutex g_spawned_swats_mutex;

static void register_spawned_swat(void* vehicle) {
    if (!vehicle) return;
    std::lock_guard<std::mutex> lock(g_spawned_swats_mutex);
    g_spawned_swats.insert(vehicle);
    LOGI("Registered spawned SWAT vehicle %p", vehicle);
}

static bool is_swat_van_nearby(CVector pos, float radius) {
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

static void bind_vehicle_occupants(void* vehicle) {
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

static void record_exit_start_for_occupants(void* vehicle) {
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

static void setup_dispatched_cops(void* vehicle, CPed* criminal) {
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

static eWeaponType determine_weapon_for_cop(CPed* cop, CPed* criminal, bool is_firearm_crime) {
    if (!cop || !criminal) return WEAPON_PISTOL;
    if (is_firearm_crime) return WEAPON_PISTOL;
    return WEAPON_NIGHTSTICK; // 非枪击案，全程手持警棍规范执法
}

static void make_cops_attack_criminal(CPed* criminal) {
    if (!criminal || !is_ped_pointer_valid_safe(criminal)) return;
    if (!g_ms_pPedPool || !g_GetPoolPed || !g_GetPedType) return;

    // 优先解决枪击大案：若当前附近已经存在活跃的枪击大案，且当前准备调度的嫌犯并非该枪击犯（是个低优先级的普通嫌犯），
    // 则直接暂缓该低优先级嫌犯的警力调度，全力优先消灭持枪悍匪。这也免除了不必要的 0 伤害产生和 pool 轮询开销。
    if (g_crime_active.load() && !g_active_crime.cancelled) {
        if (g_active_crime.is_firearm && g_active_crime.criminal && g_active_crime.criminal != criminal) {
            return;
        }
    }

    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return;

    CVector crime_pos = get_entity_pos(criminal);

    // =====================================================================
    // ⚡️ [Snapshot System & Pool Reuse]: 线程局部静态向量，实现 100% 内存池复用和零分配
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
    if (g_crime_active.load() && !g_active_crime.cancelled) {
        if (g_active_crime.is_firearm) {
            is_active_firearm = true;
        } else {
            auto& list = g_active_crime.consolidated_criminals;
            auto& is_fire_list = g_active_crime.criminal_is_firearm;
            for (size_t idx = 0; idx < list.size(); ++idx) {
                if (list[idx] == criminal && idx < is_fire_list.size() && is_fire_list[idx]) {
                    is_active_firearm = true;
                    break;
                }
            }
        }
    }

    // =====================================================================
    // Pass 1: 扫描当前处于响应状态的已调度单位数量（免偏移量判定，100% 安全稳定）
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
            if (g_crime_active.load() && !g_active_crime.cancelled) {
                for (CPed* c : g_active_crime.consolidated_criminals) {
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
                            veh == g_active_crime.spawned_vehicle) {
                            if (!vector_contains(counted_vehicles, veh)) {
                                counted_vehicles.push_back(veh);
                                active_vehicles_count++;
                            }
                        }
                    } else {
                        // 地面散步/巡逻警员
                        int64_t last_assign = 0;
                        if (g_crime_active.load() && !g_active_crime.cancelled) {
                            for (CPed* c : g_active_crime.consolidated_criminals) {
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
                                if (g_crime_active.load() && !g_active_crime.cancelled) {
                                    for (CPed* c : g_active_crime.consolidated_criminals) {
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
    // 📡 [并案机制与智能动态配额 (Dispatch Quotas)]
    // =====================================================================
    int max_vehicles = 2;
    int max_foot_cops = 2;

    if (g_crime_active.load() && !g_active_crime.cancelled) {
        auto& list = g_active_crime.consolidated_criminals;
        auto& is_fire_list = g_active_crime.criminal_is_firearm;
        
        int total_criminals = list.size();
        int armed_criminals = 0;
        for (size_t idx = 0; idx < list.size(); ++idx) {
            if (idx < is_fire_list.size() && is_fire_list[idx]) {
                armed_criminals++;
            }
        }
        
        int cops_killed = g_active_crime.cops_killed;
        
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
            if (g_crime_active.load() && !g_active_crime.cancelled) {
                for (CPed* c : g_active_crime.consolidated_criminals) {
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
                    if (g_crime_active.load() && !g_active_crime.cancelled) {
                        float best_d = 999999.0f;
                        for (CPed* c : g_active_crime.consolidated_criminals) {
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
                            float exit_dist = is_bike ? 18.0f : 32.0f;
                            // 复合下车判定：距离接近 (摩托车18米/轿车32米内)，或接近且可能卡死 (行驶超6秒且在60米内)，或彻底超时 (行驶超12秒)
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

                                    if (veh == g_active_crime.spawned_vehicle) {
                                        g_active_crime.occupants_ordered_out = true;
                                    }
                                    LOGI("Vehicle exit triggered (dist=%.1f, elapsed=%lld ms): Stopped Autopilot & Ordered cops to leave vehicle %p", 
                                         v_dist, (long long)elapsed, veh);
                                }
                            } else {
                                // 检查是否已被调度，若不是，则进入配额判定
                                bool already_dispatched = vector_contains(vehicles_ordered_to_scene_snapshot, veh) || 
                                                          vector_contains(vehicles_siren_awakened_snapshot, veh) || 
                                                          veh == g_active_crime.spawned_vehicle;

                                // =====================================================================
                                // 📡 [智能调度中心干预机制]：自动检测中途被塞车/火灾/地形卡死的警车并进行干预
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
                                                    if (veh == g_active_crime.spawned_vehicle) {
                                                        g_active_crime.occupants_ordered_out = true;
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
                                                        if (veh == g_active_crime.spawned_vehicle) {
                                                            g_active_crime.occupants_ordered_out = true;
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

                                                if (cop_dist < 8.5f) {
                                                    float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                    if (dot_p > 0.0f && dot_p < 8.5f) {
                                                        float lat_dist_sq = (cop_dist * cop_dist) - (dot_p * dot_p);
                                                        if (lat_dist_sq < 4.84f) { // 2.2米内横向偏离（直接阻挡在行进轨迹上）
                                                            ped_blocked = true;
                                                            if ((speed < 1.2f || dist_moved < 1.2f) && cop_dist < 6.5f) {
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
                                                            } else {
                                                                float push_back_dist = 8.5f - cop_dist;
                                                                if (push_back_dist > 2.0f) push_back_dist = 2.0f;
                                                                CVector decel_pos = {
                                                                    current_pos.x - dir_x * push_back_dist,
                                                                    current_pos.y - dir_y * push_back_dist,
                                                                    current_pos.z
                                                                };
                                                                set_entity_pos(veh, decel_pos);
                                                                LOGI("[dispatchCenter - ACC Keep Ped] Fleet distance keep for %p behind TARGET CRIMINAL %p (dist=%.1f). Decelerated by %.2fm.", 
                                                                     veh, target_criminal, cop_dist, push_back_dist);
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

                                                                    if (cop_dist < 8.5f) {
                                                                        float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                                        if (dot_p > 0.0f && dot_p < 8.5f) {
                                                                            float lat_dist_sq = (cop_dist * cop_dist) - (dot_p * dot_p);
                                                                            if (lat_dist_sq < 4.84f) { // Under 2.2m lateral deviation (directly in path)
                                                                                // Dynamic smart bypass detour nudge if extremely slow or stationary (stuck behind civilians)
                                                                                if ((speed < 1.2f || dist_moved < 1.2f) && cop_dist < 6.5f) {
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
                                                                                } else {
                                                                                    // Default: decelerate/keep distance backward
                                                                                    float push_back_dist = 8.5f - cop_dist;
                                                                                    if (push_back_dist > 2.0f) push_back_dist = 2.0f;
                                                                                    CVector decel_pos = {
                                                                                        current_pos.x - dir_x * push_back_dist,
                                                                                        current_pos.y - dir_y * push_back_dist,
                                                                                        current_pos.z
                                                                                    };
                                                                                    
                                                                                    set_entity_pos(veh, decel_pos);
                                                                                    
                                                                                    LOGI("[dispatchCenter - ACC Keep] Fleet distance keep for %p behind %p (dist=%.1f). Decelerated backward by %.2fm.", 
                                                                                         veh, other_veh, cop_dist, push_back_dist);
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
                                                                    if (veh == g_active_crime.spawned_vehicle) {
                                                                        g_active_crime.occupants_ordered_out = true;
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
                                        if (veh == g_active_crime.spawned_vehicle) {
                                            g_active_crime.occupants_ordered_out = true;
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
                                                                                    other_veh == g_active_crime.spawned_vehicle;
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

                    // 🌟 【巡警强力劫持机制】: 如果野生巡警距离现场或玩家在 40 米内，视其为超级近距离，强行唤醒并参战！
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
                    if (!is_extremely_nearby && g_crime_active.load() && g_active_crime.is_firearm && !g_active_crime.cancelled) {
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
    // ⚡️ [Batch-Commit Phase]: 一站式批量写入多线程全局状态，极速释放锁
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

static bool is_specific_criminal_armed_with_firearm(CPed* target_criminal) {
    if (!target_criminal) return false;
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    if (g_crime_active.load() && !g_active_crime.cancelled) {
        auto& list = g_active_crime.consolidated_criminals;
        auto& is_fire_list = g_active_crime.criminal_is_firearm;
        for (size_t idx = 0; idx < list.size(); ++idx) {
            if (list[idx] == target_criminal) {
                if (idx < is_fire_list.size()) {
                    return is_fire_list[idx];
                }
            }
        }
        if (g_active_crime.criminal == target_criminal) {
            return g_active_crime.is_firearm;
        }
    }
    return false;
}

static void make_single_cop_attack_criminal(CPed* cop, CPed* criminal, bool force_weapon_update) {
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

static void update_cops_targeting_criminal_event_driven(CPed* criminal) {
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

static void update_primary_criminal_by_threat() {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    if (!g_crime_active.load() || g_active_crime.cancelled) {
        return;
    }

    CPed* best_criminal = nullptr;
    ecs::CriminalThreatLevel highest_threat = ecs::CriminalThreatLevel::UNARMED_INACTIVE;

    for (CPed* ped : g_active_crime.consolidated_criminals) {
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
            if (g_active_crime.criminal == ped) {
                best_criminal = ped;
            }
        }
    }

    if (best_criminal && best_criminal != g_active_crime.criminal) {
        CPed* old_criminal = g_active_crime.criminal;
        g_active_crime.criminal = best_criminal;
        g_tracked_criminal.store(best_criminal);

        LOGI("⚡️ [ECS ThreatPrioritizer] Primary target switched due to threat priority: %p -> %p (Threat: %d)",
             old_criminal, best_criminal, (int)highest_threat);

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



// =====================================================================
// 调度引擎 (安全运行在 CTheScripts::Process 主线程 Hook 中)
//
// 核心状态机：
//   IDLE → TIMING → ON_SCENE → CLEANUP
// =====================================================================
enum DispatchState {
    STATE_IDLE = 0,
    STATE_TIMING,        // 等待调度延迟
    STATE_DISPATCHED,    // 警车已生成，正在前往 (已弃用/仅作兼容占位)
    STATE_ON_SCENE,      // 警车已抵达现场
    STATE_CLEANUP,       // 事件结束，清理
};

// 调度状态变量
static DispatchState g_dispatch_state = STATE_IDLE;
static int64_t g_timer_start = 0;
static int g_dispatch_delay_ms = 0;
static int g_last_cops_killed = 0;
static int64_t g_on_scene_start = 0;
static int64_t g_last_tick_time_ms = 0;

static void cleanup_single_case_vehicles(std::shared_ptr<CrimeEvent> crime) {
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
// 📡 [trueDispatch Spawn Helper]：自定义调度车辆的生成包装器，保障正确的防拦截标识
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
// 🚑🚒 [Emergency Vehicle Escaper]：救护车与消防车的高级物理脱困、导航与避障机制
// =====================================================================
static std::map<void*, StuckTracker> g_emergency_stuck_vehicles;
static std::mutex g_emergency_stuck_vehicles_mutex;

static std::vector<void*> g_emergency_vehicles_emptied;
static std::mutex g_emergency_vehicles_emptied_mutex;

static constexpr int PED_TYPE_MEDIC     = 18;
static constexpr int PED_TYPE_FIREMAN   = 19;

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

                                                            if (dist_v < 8.5f) {
                                                                float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                                if (dot_p > 0.0f && dot_p < 8.5f) {
                                                                    float lat_dist_sq = (dist_v * dist_v) - (dot_p * dot_p);
                                                                    if (lat_dist_sq < 4.84f) { // 横向偏离小于 2.2米
                                                                        car_blocked = true;
                                                                        if ((speed < 1.2f || dist_moved < 1.2f) && dist_v < 6.5f) {
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
                                                                        } else {
                                                                            // 减速退避保持距离
                                                                            float push_back_dist = 8.5f - dist_v;
                                                                            if (push_back_dist > 2.0f) push_back_dist = 2.0f;
                                                                            CVector decel_pos = {
                                                                                current_pos.x - dir_x * push_back_dist,
                                                                                current_pos.y - dir_y * push_back_dist,
                                                                                current_pos.z
                                                                            };
                                                                            
                                                                            set_entity_pos(veh, decel_pos);
                                                                            
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
                                                                
                                                                LOGI("🔥 [Emergency Escaper - Ambulance Fire Detour] Detoured around fire source.");
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

                            // STAGE 2: 7秒以上且玩家看不见：高通过率跃迁 20m 终极打破物理死锁
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
// 🚗💨 [Civilian Avoidance Field]：给平民车辆施加“恐惧场”，让其智能避让紧急调度车辆
// =====================================================================
static bool is_emergency_vehicle_model(unsigned int model) {
    return (model == 416 || model == 407 || 
            model == 596 || model == 597 || model == 598 || model == 599 || 
            model == 601 || model == 528 || model == 490 || model == 523);
}

// 🚗💨 平民车辆避让时的惊慌按喇叭与急刹车冷却定时器
static std::unordered_map<void*, int64_t> g_civilian_panic_timers;
static int64_t g_last_cleanup_panic_timers = 0;

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

                        // 恐惧场作用范围：前方 25 米，横向 5 米
                        if (dist < 25.0f && dist > 0.1f) {
                            // 投影到应急车的前向向量和右向向量上
                            float dot_forward = dx * ev.dir.x + dy * ev.dir.y + dz * ev.dir.z;
                            float dot_right = dx * ev.right.x + dy * ev.right.y + dz * ev.right.z;

                            // 如果平民车处于应急车的前方（dot_forward > 0），且横向距离较近（|dot_right| < 5.0m）
                            if (dot_forward > 0.0f && dot_forward < 25.0f && fabsf(dot_right) < 5.0f) {
                                // 计算避让方向（横向避让：向左或向右分流）
                                float avoid_sign = (dot_right >= 0.0f) ? 1.0f : -1.0f;
                                
                                // 平民车越近，避让力度越强。使用渐进的平滑系数
                                float intensity = (25.0f - dot_forward) / 25.0f; // 0.0 -> 1.0
                                float side_shove = intensity * 0.45f; // 最大单次横移 0.45 米
                                
                                // 惊慌按喇叭与原生急刹车尾灯细节：
                                // [Note] 经过现场排查，高频高并发对全城平民车辆施加原生伤害事件 (g_VehicleInflictDamage)
                                // 会导致某些特定载具类型或处于特殊AI状态的车辆触发 'Pure virtual function called!' 闪退。
                                // 为保证系统 100% 绝对稳定性，此处不采用不安全的物理受击事件。

                                CMatrix* mat = g_GetMatrix(veh);
                                float civ_up_x = 0.0f;
                                float civ_up_y = 0.0f;
                                if (mat) {
                                    civ_up_x = mat->up_x;
                                    civ_up_y = mat->up_y;
                                }

                                // 刹车物理减速模拟：基于强度向车辆自身的后向向量（-mat->up）施加反向力
                                float brake_shove = intensity * 0.35f; // 最大单次物理急刹车拖拽 0.35 米

                                CVector new_civ_pos = civ_pos;
                                // 1. 施加横向避让位移
                                new_civ_pos.x += ev.right.x * avoid_sign * side_shove;
                                new_civ_pos.y += ev.right.y * avoid_sign * side_shove;
                                // 2. 施加物理急刹车拖拽（退后位置分量，降低实际前进速度）
                                new_civ_pos.x -= civ_up_x * brake_shove;
                                new_civ_pos.y -= civ_up_y * brake_shove;
                                
                                new_civ_pos.z += 0.05f; // 稍微抬升防止轮子陷入路面

                                // 施加微弱的 Yaw 角度旋转，让其车头指向避让方向，看起来非常像在“打方向盘避让”！
                                if (mat) {
                                    float rot_angle = avoid_sign * intensity * 0.08f; // 约 4.5 度偏角
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

                                if (dist < 8.0f) {
                                    LOGI("🚗💨 [Fear Field] Civilian vehicle %p panic-steered to avoid emergency vehicle %p (dist: %.1fm)", veh, ev.veh, dist);
                                }
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
    int64_t cur_time = now_ms();
    if (cur_time - g_last_tick_time_ms < 250) {
        return;
    }
    g_last_tick_time_ms = cur_time;

    static bool ecs_inited = false;
    if (!ecs_inited) {
        init_ecs_systems();
        ecs_inited = true;
    }
    ecs::EventDispatcher::get().dispatch(ecs::TickEvent(cur_time));

    if (!g_FindPlayerPed || !g_FindPlayerPed(0)) {
        return; // 不在游戏内
    }

    // 运行急救与消防车辆的自主脱困、避障与高级物理救援心跳
    emergency_vehicles_tick();

    // 运行平民车辆智能避让恐惧场
    apply_civilian_avoidance_field();

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

        // 轮询并执行过期的异步挂起任务 (串行，线程安全，且完美解决 lambda 回调内 push_back 导致的 vector 迭代器失效 crash 风险)
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
        g_crime_active.store(false);
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
        g_last_cops_killed = 0;
    }
}

static void proxy_the_scripts_process() {
    SHADOWHOOK_STACK_SCOPE();

    if (g_orig_the_scripts_process) {
        g_orig_the_scripts_process();
    }

    on_main_thread_tick();
}

// =====================================================================
// 🤝 [CTaskComplexPartner & derived Hooks]：防止各种伴随/打招呼序列空指针、野指针或零值状态导致的 Sequence Manager 空指针闪退
// =====================================================================
static inline bool is_sequence_manager_safe() {
    if (!g_CSequenceManager_ms_instance || !*g_CSequenceManager_ms_instance) {
        return false;
    }
    void* manager = *g_CSequenceManager_ms_instance;
    return is_pointer_readable(manager);
}

static inline bool is_partner_task_safe(void* self) {
    if (!self || !is_task_vtable_safe(self)) {
        return false;
    }
    return true;
}

static inline void sanitize_partner_task_pointers(void* task) {
    // 禁用伙伴任务“净化器”以防误伤非指针数据成员
}

static inline void sanitize_task_pointers(void* task, int max_size_bytes = 256) {
    // 【彻底修复说明】
    // 禁用盲目扫描内存的“净化器”。
    // 盲目扫描（按8字节步长解引用）会把非指针数据（如 float 数组、CVector 坐标、计时器等）误判为“已被析构清空的 C++ 对象”并置为 nullptr。
    // 例如：BoneNode_c::Limit(float) 在限制骨骼角度时会调用 BoneNode_c::GetLimits(int, float*, float*)。
    // 如果任务对象中包含指向此类限制值 float 数组的指针，且数组前两个 float 恰好为 0.0f（前8字节为0），
    // 盲扫就会将该指针强行置为 nullptr，导致 GetLimits 写入时发生空指针解引用闪退（fault addr 0x10）。
    //
    // 【后续彻底重构要求】
    // 若未来需要重新启用此净化器以防止其他未挂钩子处的虚函数闪退，必须通过逆向分析（如使用 IDA/r2）
    // 找出 CTask 各个子类（如 CTaskComplexPartner 等）中存放子任务或 Ped 指针的【精确偏移量】（Offsets），
    // 并仅针对这些特定偏移量进行安全校验与置空，严禁进行盲目全内存扫描。
}

// --- GetPartnerSequence Hooks ---
typedef void* (*fn_GetPartnerSequence_t)(void* self);

static void* g_stub_gps_deal = nullptr;
static fn_GetPartnerSequence_t g_orig_gps_deal = nullptr;
static void* proxy_gps_deal(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [GetPartnerSequenceDeal] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    if (!is_sequence_manager_safe()) {
        LOGW("⚠️ [GetPartnerSequenceDeal] Sequence manager unsafe! Returning nullptr.");
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_gps_deal, self);
}

static void* g_stub_gps_greet = nullptr;
static fn_GetPartnerSequence_t g_orig_gps_greet = nullptr;
static void* proxy_gps_greet(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [GetPartnerSequenceGreet] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    if (!is_sequence_manager_safe()) {
        LOGW("⚠️ [GetPartnerSequenceGreet] Sequence manager unsafe! Returning nullptr.");
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_gps_greet, self);
}

static void* g_stub_gps_shove = nullptr;
static fn_GetPartnerSequence_t g_orig_gps_shove = nullptr;
static void* proxy_gps_shove(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [GetPartnerSequenceShove] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    if (!is_sequence_manager_safe()) {
        LOGW("⚠️ [GetPartnerSequenceShove] Sequence manager unsafe! Returning nullptr.");
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_gps_shove, self);
}

static void* g_stub_gps_chat = nullptr;
static fn_GetPartnerSequence_t g_orig_gps_chat = nullptr;
static void* proxy_gps_chat(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [GetPartnerSequenceChat] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    if (!is_sequence_manager_safe()) {
        LOGW("⚠️ [GetPartnerSequenceChat] Sequence manager unsafe! Returning nullptr.");
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_gps_chat, self);
}


// --- CTaskSimpleHoldEntity::SetPedPosition Hooks ---
typedef void (*fn_SetPedPosition_t)(void* self, void* ped);

static void* g_stub_set_ped_pos = nullptr;
static fn_SetPedPosition_t g_orig_set_ped_pos = nullptr;
static void proxy_set_ped_pos(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) {
        LOGW("⚠️ [SetPedPosition] unsafe self! Skipping.");
        return;
    }
    if (!ped || !is_pointer_readable(ped)) {
        LOGW("⚠️ [SetPedPosition] null or unsafe ped! Skipping.");
        return;
    }
    // Check if the pointer at ped + 0x648 is null or unreadable
    char* ped_bytes = reinterpret_cast<char*>(ped);
    void** clump_slot = reinterpret_cast<void**>(ped_bytes + 0x648);
    if (!is_pointer_readable(clump_slot)) {
        LOGW("⚠️ [SetPedPosition] ped + 0x648 slot unreadable! Skipping to prevent crash.");
        return;
    }
    void* clump = *clump_slot;
    if (clump && !is_pointer_readable(clump)) {
        LOGW("⚠️ [SetPedPosition] ped->clump (%p) is invalid/unreadable! Skipping original to prevent crash.", clump);
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_set_ped_pos, self, ped);
}


// --- CreateFirstSubTask Hooks ---
typedef void* (*fn_CreateFirstSubTask_t)(void* self, void* ped);

static void* g_stub_cfst_base = nullptr;
static fn_CreateFirstSubTask_t g_orig_cfst_base = nullptr;
static void* proxy_cfst_base(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [CreateFirstSubTaskBase] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    return SHADOWHOOK_CALL_PREV(proxy_cfst_base, self, ped);
}

static void* g_stub_cfst_deal = nullptr;
static fn_CreateFirstSubTask_t g_orig_cfst_deal = nullptr;
static void* proxy_cfst_deal(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [CreateFirstSubTaskDeal] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    return SHADOWHOOK_CALL_PREV(proxy_cfst_deal, self, ped);
}

static void* g_stub_cfst_greet = nullptr;
static fn_CreateFirstSubTask_t g_orig_cfst_greet = nullptr;
static void* proxy_cfst_greet(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [CreateFirstSubTaskGreet] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    return SHADOWHOOK_CALL_PREV(proxy_cfst_greet, self, ped);
}


// --- ControlSubTask Hooks ---
typedef void* (*fn_ControlSubTask_t)(void* self, void* ped);

static void* g_stub_cst_base = nullptr;
static fn_ControlSubTask_t g_orig_cst_base = nullptr;
static void* proxy_cst_base(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_partner_task_safe(self)) {
        LOGW("⚠️ [ControlSubTaskBase] unsafe self! Returning nullptr.");
        return nullptr;
    }
    sanitize_partner_task_pointers(self);
    return SHADOWHOOK_CALL_PREV(proxy_cst_base, self, ped);
}

// --- CTaskComplexGoToPointAnyMeans::CreateSubTask Hook ---
typedef void* (*fn_GoToPointAnyMeans_CreateSubTask_t)(void* self, int subTaskId, CPed* ped);
static void* g_stub_gotopointanymeans_createsubtask = nullptr;
static fn_GoToPointAnyMeans_CreateSubTask_t g_orig_gotopointanymeans_createsubtask = nullptr;

static void* proxy_gotopointanymeans_createsubtask(void* self, int subTaskId, CPed* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_task_vtable_safe(self)) {
        LOGW("⚠️ [GoToPointAnyMeans::CreateSubTask] self %p is null or has unsafe vtable! Intercepting.", self);
        return nullptr;
    }
    if (ped && !is_ped_pointer_valid_safe(ped)) {
        LOGW("⚠️ [GoToPointAnyMeans::CreateSubTask] input ped %p is unsafe! Intercepting.", ped);
        return nullptr;
    }
    sanitize_task_pointers(self);
    return SHADOWHOOK_CALL_PREV(proxy_gotopointanymeans_createsubtask, self, subTaskId, ped);
}

// --- CTaskComplexTurnToFaceEntityOrCoord Hook ---
typedef void* (*fn_TurnToFaceEntity_CreateFirstSubTask_t)(void* self, void* ped);
static void* g_stub_turntofaceentity_createfirstsubtask = nullptr;
static fn_TurnToFaceEntity_CreateFirstSubTask_t g_orig_turntofaceentity_createfirstsubtask = nullptr;

typedef void* (*fn_TurnToFaceEntity_ControlSubTask_t)(void* self, void* ped);
static void* g_stub_turntofaceentity_controlsubtask = nullptr;
static fn_TurnToFaceEntity_ControlSubTask_t g_orig_turntofaceentity_controlsubtask = nullptr;

static void sanitize_turntofaceentity_target(void* self) {
    // 禁用面对实体任务的“净化器”以防误伤非指针数据成员（如 offset 0x18 处的 CVector 目标坐标）
}

static void* proxy_turntofaceentity_createfirstsubtask(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_task_vtable_safe(self)) {
        LOGW("⚠️ [TurnToFaceEntity::CreateFirstSubTask] self %p is null or has unsafe vtable! Intercepting.", self);
        return nullptr;
    }
    if (ped && !is_ped_pointer_valid_safe(ped)) {
        LOGW("⚠️ [TurnToFaceEntity::CreateFirstSubTask] input ped %p is unsafe! Intercepting.", ped);
        return nullptr;
    }
    sanitize_turntofaceentity_target(self);
    return SHADOWHOOK_CALL_PREV(proxy_turntofaceentity_createfirstsubtask, self, ped);
}

static void* proxy_turntofaceentity_controlsubtask(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_task_vtable_safe(self)) {
        LOGW("⚠️ [TurnToFaceEntity::ControlSubTask] self %p is null or has unsafe vtable! Intercepting.", self);
        return nullptr;
    }
    if (ped && !is_ped_pointer_valid_safe(ped)) {
        LOGW("⚠️ [TurnToFaceEntity::ControlSubTask] input ped %p is unsafe! Intercepting.", ped);
        return nullptr;
    }
    sanitize_turntofaceentity_target(self);
    return SHADOWHOOK_CALL_PREV(proxy_turntofaceentity_controlsubtask, self, ped);
}


// =====================================================================
// 🛠️ [CTaskManager & CAttractorScanner Safety Hooks]
// =====================================================================
typedef void (*fn_ManageTasks_t)(void* self);
static void* g_stub_manage_tasks = nullptr;
static fn_ManageTasks_t g_orig_manage_tasks = nullptr;

typedef bool (*fn_IsSimple_t)(void* self);

static inline bool is_task_simple(void* task) {
    if (!task || !is_pointer_readable(task)) return true;
    void** vtable = *reinterpret_cast<void***>(task);
    if (!is_pointer_readable(vtable)) return true;

    // IsSimple is at offset 0x20 (index 4 in 64-bit vtable)
    fn_IsSimple_t is_simple_fn = reinterpret_cast<fn_IsSimple_t>(vtable[4]);
    if (is_pointer_readable(reinterpret_cast<void*>(is_simple_fn))) {
        return is_simple_fn(task);
    }
    return true;
}

static void sanitize_task_chain(void* task, int depth = 0) {
    if (!task || depth > 10) return;
    if (!is_pointer_readable(task)) return;

    sanitize_task_pointers(task);

    // Only CTaskComplex (where is_task_simple returns false) has m_pSubTask at offset 16.
    // Wiping offset 16 of a CTaskSimple will corrupt its subclass member variables.
    if (!is_task_simple(task)) {
        void** parent_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 8);
        void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 16);

        if (is_pointer_readable(parent_slot)) {
            void* parent = *parent_slot;
            if (parent && !is_task_vtable_safe(parent)) {
                LOGW("⚠️ [Task Chain Sanitizer] Clearing unsafe parent task %p inside task %p", parent, task);
                *parent_slot = nullptr;
            }
        }

        if (is_pointer_readable(sub_slot)) {
            void* sub = *sub_slot;
            if (sub) {
                if (!is_task_vtable_safe(sub)) {
                    LOGW("⚠️ [Task Chain Sanitizer] Clearing unsafe subtask %p inside task %p", sub, task);
                    *sub_slot = nullptr;
                } else {
                    sanitize_task_chain(sub, depth + 1);
                }
            }
        }
    } else {
        // For CTaskSimple, we only sanitize its parent task pointer at offset 8.
        void** parent_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 8);
        if (is_pointer_readable(parent_slot)) {
            void* parent = *parent_slot;
            if (parent && !is_task_vtable_safe(parent)) {
                LOGW("⚠️ [Task Chain Sanitizer] Clearing unsafe parent task %p inside simple task %p", parent, task);
                *parent_slot = nullptr;
            }
        }
    }
}

static void proxy_manage_tasks(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self) {
        // CTaskManager contains primary tasks (5 slots) and secondary tasks (6 slots)
        // Total 11 slots of pointers starting at offset 0 of CTaskManager.
        for (int i = 0; i < 11; ++i) {
            void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
            void* task = *task_slot;
            if (task) {
                if (!is_task_vtable_safe(task)) {
                    LOGW("⚠️ [CTaskManager::ManageTasks] Found unsafe/zeroed task %p at slot %d inside CTaskManager %p. Clearing it to prevent crash.", task, i, self);
                    *task_slot = nullptr;
                } else {
                    sanitize_task_chain(task);
                }
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_manage_tasks, self);
}

typedef void (*fn_ScanForAttractorsInRange_t)(void* self, void* ped);
static void* g_stub_scan_for_attractors_in_range = nullptr;
static fn_ScanForAttractorsInRange_t g_orig_scan_for_attractors_in_range = nullptr;

static void proxy_scan_for_attractors_in_range(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!ped || !is_ped_pointer_valid_safe(ped)) return;
    void* intel = get_ped_intelligence(reinterpret_cast<CPed*>(ped));
    if (intel) {
        for (int offset = 0x8; offset <= 0x28; offset += 8) {
            void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + offset);
            void* task = *task_slot;
            if (task && !is_task_vtable_safe(task)) {
                LOGW("⚠️ [ScanForAttractorsInRange] Intercepted unsafe/destructing task %p at offset 0x%X in CPedIntelligence %p. Clearing it to prevent crash.", task, offset, intel);
                *task_slot = nullptr;
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_scan_for_attractors_in_range, self, ped);
}

// --- CTaskComplexGangFollower::ControlSubTask Hook ---
typedef void* (*fn_ControlSubTask_t)(void* self, void* ped);

static void* g_stub_ccgf_control = nullptr;
static fn_ControlSubTask_t g_orig_ccgf_control = nullptr;
static void* proxy_ccgf_control(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) {
        LOGW("⚠️ [GangFollower::ControlSubTask] unsafe self!");
        return nullptr;
    }
    if (!ped || !is_pointer_readable(ped)) {
        LOGW("⚠️ [GangFollower::ControlSubTask] unsafe ped!");
        return nullptr;
    }
    char* self_bytes = reinterpret_cast<char*>(self);
    void** leader_slot = reinterpret_cast<void**>(self_bytes + 0x18);
    if (!is_pointer_readable(leader_slot)) {
        LOGW("⚠️ [GangFollower::ControlSubTask] self + 0x18 slot unreadable!");
        return nullptr;
    }
    void* leader = *leader_slot;
    if (leader && !is_pointer_readable(leader)) {
        LOGW("⚠️ [GangFollower::ControlSubTask] leader (%p) is invalid/unreadable! Returning nullptr to prevent crash.", leader);
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_ccgf_control, self, ped);
}


// --- CTaskGangHassleVehicle::CalcTargetOffset Hook ---
typedef void (*fn_CalcTargetOffset_t)(void* self);
static void* g_stub_CalcTargetOffset = nullptr;
static fn_CalcTargetOffset_t g_orig_CalcTargetOffset = nullptr;

static void proxy_CalcTargetOffset(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self) return;
    
    // Check offset 0x18 (target vehicle)
    void** pVehicle = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
    if (is_pointer_readable(pVehicle)) {
        void* vehicle = *pVehicle;
        if (vehicle && !is_pointer_readable(vehicle)) {
            LOGW("⚠️ [CTaskGangHassleVehicle::CalcTargetOffset] Target vehicle is invalid (%p) at offset 0x18! Skipping calculation to prevent SIGSEGV.", vehicle);
            return;
        }
    }
    
    // Check offset 0x10 (target entity)
    void** pEntity = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (is_pointer_readable(pEntity)) {
        void* entity = *pEntity;
        if (entity && !is_pointer_readable(entity)) {
            LOGW("⚠️ [CTaskGangHassleVehicle::CalcTargetOffset] Target entity is invalid (%p) at offset 0x10! Skipping calculation to prevent SIGSEGV.", entity);
            return;
        }
    }
    
    SHADOWHOOK_CALL_PREV(proxy_CalcTargetOffset, self);
}

// --- CPed::DoFootLanded Hook ---
typedef void (*fn_DoFootLanded_t)(void* ped, bool left_foot, unsigned char surface_type);
static void* g_stub_do_foot_landed = nullptr;
static fn_DoFootLanded_t g_orig_do_foot_landed = nullptr;

static void proxy_do_foot_landed(void* ped, bool left_foot, unsigned char surface_type) {
    SHADOWHOOK_STACK_SCOPE();
    if (!ped || !is_ped_pointer_valid_safe(ped) || !*reinterpret_cast<void**>(ped)) {
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_do_foot_landed, ped, left_foot, surface_type);
}

static void* g_stub_add_police_occupants = nullptr;
static fn_AddPoliceOccupants_t g_orig_add_police_occupants = nullptr;

static void proxy_add_police_occupants(CVehicle* vehicle, bool bSirenOrAlarm) {
    SHADOWHOOK_STACK_SCOPE();
    SHADOWHOOK_CALL_PREV(proxy_add_police_occupants, vehicle, bSirenOrAlarm);
    bind_vehicle_occupants(vehicle);
}

// =====================================================================
// =====================================================================
// 🚑🚒 [Emergency Workaround]：移动端救护车与消防车因超长视距生成即秒删 Bug 的修复
// =====================================================================
static bool is_police_vehicle_model(unsigned int model) {
    return (model == 596 || model == 597 || model == 598 || model == 599 ||  // Cop Cars (LSPD, SFPD, LVPD, Ranger)
            model == 523 ||                                                 // Cop Bike
            model == 427 || model == 601 ||                                 // SWAT Enforcer, SWAT Water Cannon
            model == 490 || model == 528 ||                                 // FBI Rancher, FBI Truck
            model == 433 || model == 432);                                  // Barracks (Army Truck), Rhino (Tank)
}

static void* g_stub_generate_one_emergency_car = nullptr;
static fn_GenOneEmergencyCar_t g_orig_generate_one_emergency_car = nullptr;

static void proxy_generate_one_emergency_car(unsigned int model, CVector pos) {
    SHADOWHOOK_STACK_SCOPE();

    if (is_police_vehicle_model(model)) {
        if (!g_is_generating_custom_dispatch.load()) {
            LOGI("🚫 [trueDispatch] Intercepted native cheap cop spawn! Model: %u, Pos: (%.1f, %.1f, %.1f)", model, pos.x, pos.y, pos.z);
            return; // Intercept and quietly block native spawning
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

static void* g_stub_script_generate_one_emergency_car = nullptr;
static fn_ScriptGenEmergencyCar_t g_orig_script_generate_one_emergency_car = nullptr;

static void proxy_script_generate_one_emergency_car(unsigned int model, CVector pos) {
    SHADOWHOOK_STACK_SCOPE();

    if (is_police_vehicle_model(model)) {
        if (!g_is_generating_custom_dispatch.load()) {
            LOGI("🚫 [trueDispatch] Intercepted native cheap cop script spawn! Model: %u, Pos: (%.1f, %.1f, %.1f)", model, pos.x, pos.y, pos.z);
            return; // Intercept and quietly block native spawning
        }
    }

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

static void* g_stub_tell_occupants_leave_car = nullptr;
static fn_TellOccupantsToLeaveCar_t g_orig_tell_occupants_leave_car = nullptr;

static void proxy_tell_occupants_leave_car(void* vehicle) {
    SHADOWHOOK_STACK_SCOPE();
    bind_vehicle_occupants(vehicle); // Bind them here before they leave!
    record_exit_start_for_occupants(vehicle);
    SHADOWHOOK_CALL_PREV(proxy_tell_occupants_leave_car, vehicle);
}


// =====================================================================
// 获取 .so 基址
// =====================================================================
// =====================================================================
// 符号解析宏 (使用 xdl_sym 绕过 Linker Namespace 限制与 dlopen 失败问题)
// =====================================================================
#define RESOLVE_SYM(handle, var, mangled, type) do { \
    var = reinterpret_cast<type>(xdl_sym(handle, mangled, nullptr)); \
    if (var) LOGI("  ✅ " #var " -> %p", (void*)var); \
    else     LOGW("  ⚠️ " #var " not found (%s)", mangled); \
} while(0)

// =====================================================================
// 🛠️ [Pure Virtual Function Safe Patching]
// =====================================================================
extern "C" void* safe_pure_virtual_stub() {
    return nullptr;
}

static void* find_pure_virtual_target(void* vtable_symbol, int num_slots) {
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

static void patch_vtable_pure_virtuals(const char* name, void* vtable_symbol, int num_slots, void* pure_virtual_target, void* stub_func) {
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
// 🛡️ [CPed::PlayFootSteps Hook]：防止转场期间玩家 Clump 临时脱离导致空指针解引用闪退
// =====================================================================
static void* g_stub_play_footsteps = nullptr;
typedef void (*fn_PlayFootSteps_t)(void* self);
static fn_PlayFootSteps_t g_orig_play_footsteps = nullptr;

static void proxy_play_footsteps(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return;

    // 1. 检查 m_pRwObject (offset 0x20) 是否有效
    void** rw_obj_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x20);
    if (!is_pointer_readable(rw_obj_slot) || *rw_obj_slot == nullptr) {
        return;
    }
    void* rw_obj = *rw_obj_slot;

    // 2. 检查 rw_obj->field_308 (offset 0x308) 是否有效
    void** field_308_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(rw_obj) + 0x308);
    if (!is_pointer_readable(field_308_slot) || *field_308_slot == nullptr) {
        return;
    }
    void* field_308 = *field_308_slot;

    // 3. 检查 field_308->field_10 (offset 0x10) 是否非零
    int* field_10_ptr = reinterpret_cast<int*>(reinterpret_cast<char*>(field_308) + 0x10);
    if (!is_pointer_readable(field_10_ptr) || *field_10_ptr == 0) {
        return;
    }

    // 4. 检查 *field_308 是否有效
    void** field_308_deref_slot = reinterpret_cast<void**>(field_308);
    if (!is_pointer_readable(field_308_deref_slot) || *field_308_deref_slot == nullptr) {
        return;
    }

    SHADOWHOOK_CALL_PREV(proxy_play_footsteps, self);
}

// =====================================================================
// 🛡️ [CPed::ProcessBuoyancy Hook]：防止 ProcessBuoyancy 期间任务槽被置空/野指针导致虚表解引用闪退
// =====================================================================
static void* g_stub_process_buoyancy = nullptr;
typedef void (*fn_ProcessBuoyancy_t)(void* self);
static fn_ProcessBuoyancy_t g_orig_process_buoyancy = nullptr;

static void proxy_process_buoyancy(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && is_pointer_readable(self)) {
        void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x5e8);
        if (is_pointer_readable(intel_slot)) {
            void* intel = *intel_slot;
            if (intel && is_pointer_readable(intel)) {
                void* task_mgr = reinterpret_cast<char*>(intel) + 8;
                if (is_pointer_readable(task_mgr)) {
                    for (int i = 0; i < 11; ++i) {
                        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
                        if (is_pointer_readable(task_slot)) {
                            void* task = *task_slot;
                            if (task && !is_task_vtable_safe(task)) {
                                LOGW("⚠️ [ProcessBuoyancy Sanitizer] Clearing unsafe/zeroed task %p at slot %d inside CTaskManager %p", task, i, task_mgr);
                                *task_slot = nullptr;
                            }
                        }
                    }
                }
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_process_buoyancy, self);
}

// =====================================================================
// 🛡️ [CPedIntelligence::ProcessAfterPreRender Hook]：防止 ProcessAfterPreRender 期间已析构任务残留导致纯虚函数调用闪退
// =====================================================================
static void* g_stub_process_after_pre_render = nullptr;
typedef void (*fn_ProcessAfterPreRender_t)(void* self);
static fn_ProcessAfterPreRender_t g_orig_process_after_pre_render = nullptr;

static thread_local void* g_current_ped_intelligence = nullptr;

static void proxy_process_after_pre_render(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && is_pointer_readable(self)) {
        g_current_ped_intelligence = self;
        void* task_mgr = reinterpret_cast<char*>(self) + 8;
        if (is_pointer_readable(task_mgr)) {
            // 扫描 CPedIntelligence 中的前 20 个任务槽，净化已析构或无效的任务指针
            for (int i = 0; i < 20; ++i) {
                void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
                if (is_pointer_readable(task_slot)) {
                    void* task = *task_slot;
                    if (task && !is_task_vtable_safe(task)) {
                        LOGW("⚠️ [ProcessAfterPreRender Sanitizer] Clearing unsafe/zeroed/destructed task %p at slot %d inside CPedIntelligence %p", task, i, self);
                        *task_slot = nullptr;
                    }
                }
            }
        }
    }
    
    SHADOWHOOK_CALL_PREV(proxy_process_after_pre_render, self);
    g_current_ped_intelligence = nullptr;
}

// =====================================================================
// 🛡️ [CTask::Destructor Hook]：防止 ProcessAfterPreRender 期间任务中途被析构产生野指针导致闪退
// =====================================================================
static void* g_stub_task_destructor = nullptr;
typedef void (*fn_TaskDestructor_t)(void* self);
static fn_TaskDestructor_t g_orig_task_destructor = nullptr;

static void proxy_task_destructor(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && g_current_ped_intelligence && is_pointer_readable(g_current_ped_intelligence)) {
        void* task_mgr = reinterpret_cast<char*>(g_current_ped_intelligence) + 8;
        if (is_pointer_readable(task_mgr)) {
            for (int i = 0; i < 20; ++i) {
                void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
                if (is_pointer_readable(task_slot) && *task_slot == self) {
                    LOGW("⚠️ [Task Destructor Sanitizer] Task %p is being destructed during ProcessAfterPreRender of CPedIntelligence %p, clearing slot %d", self, g_current_ped_intelligence, i);
                    *task_slot = nullptr;
                }
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_task_destructor, self);
}

// =====================================================================
// 🛡️ [AssetPackManager_requestDownload Hook]：防止无谷歌服务环境下 Play Core 崩溃
// =====================================================================
static void* g_stub_asset_pack_manager_request_download = nullptr;
typedef int (*fn_AssetPackManager_requestDownload_t)(void* env, void* thiz, void* packNames);
static fn_AssetPackManager_requestDownload_t g_orig_asset_pack_manager_request_download = nullptr;

static int proxy_asset_pack_manager_request_download(void* env, void* thiz, void* packNames) {
    SHADOWHOOK_STACK_SCOPE();
    LOGW("⚠️ [AssetPackManager Hook] Bypassing AssetPackManager_requestDownload to prevent Play Core crash (returning -101)");
    return -101; // ASSET_PACK_API_NOT_AVAILABLE (-101)
}

// =====================================================================
// 🛡️ [HarfBuzz get_glyph_from_name Hook]：防止字体 post 表损坏导致的二分查找野指针闪退
// =====================================================================
static void* g_stub_hb_get_glyph_from_name = nullptr;
typedef bool (*fn_hb_get_glyph_from_name_t)(void* self, const char* name, int len, unsigned int* glyph);
static fn_hb_get_glyph_from_name_t g_orig_hb_get_glyph_from_name = nullptr;

// =====================================================================
// 🛡️ [u_strlen_64 Hook]：防止 ICU 计算 Unicode 字符串长度时传入野指针闪退
// =====================================================================
static void* g_stub_u_strlen = nullptr;
typedef int32_t (*fn_u_strlen_t)(const void* s);
static fn_u_strlen_t g_orig_u_strlen = nullptr;

static int32_t proxy_u_strlen(const void* s) {
    SHADOWHOOK_STACK_SCOPE();
    if (!s || !is_pointer_readable(s)) {
        LOGW("⚠️ [u_strlen_64 Hook] null/wild pointer detected! Returning 0.");
        return 0;
    }
    return SHADOWHOOK_CALL_PREV(proxy_u_strlen, s);
}

// =====================================================================
// 🛡️ [icu::DateTimePatternGenerator Hooks]：防止获取日期时间格式模板时空指针解引用闪退
// =====================================================================
static void* g_stub_dtpg_create_empty_instance = nullptr;
typedef void* (*fn_dtpg_create_empty_instance_t)(int* status);
static fn_dtpg_create_empty_instance_t g_orig_dtpg_create_empty_instance = nullptr;

static void* proxy_dtpg_create_empty_instance(int* status) {
    SHADOWHOOK_STACK_SCOPE();
    LOGW("⚠️ [DateTimePatternGenerator::createEmptyInstance Hook] Bypassing to prevent crash.");
    if (status) {
        *status = 16; // U_FILE_ACCESS_ERROR
    }
    return nullptr;
}

static void* g_stub_dtpg_create_instance_no_locale = nullptr;
typedef void* (*fn_dtpg_create_instance_no_locale_t)(int* status);
static fn_dtpg_create_instance_no_locale_t g_orig_dtpg_create_instance_no_locale = nullptr;

static void* proxy_dtpg_create_instance_no_locale(int* status) {
    SHADOWHOOK_STACK_SCOPE();
    LOGW("⚠️ [DateTimePatternGenerator::createInstance Hook] Bypassing to prevent crash.");
    if (status) {
        *status = 16; // U_FILE_ACCESS_ERROR
    }
    return nullptr;
}

static void* g_stub_dtpg_create_instance_with_locale = nullptr;
typedef void* (*fn_dtpg_create_instance_with_locale_t)(const void* locale, int* status);
static fn_dtpg_create_instance_with_locale_t g_orig_dtpg_create_instance_with_locale = nullptr;

static void* proxy_dtpg_create_instance_with_locale(const void* locale, int* status) {
    SHADOWHOOK_STACK_SCOPE();
    LOGW("⚠️ [DateTimePatternGenerator::createInstance(Locale) Hook] Bypassing to prevent crash.");
    if (status) {
        *status = 16; // U_FILE_ACCESS_ERROR
    }
    return nullptr;
}

static bool proxy_hb_get_glyph_from_name(void* self, const char* name, int len, unsigned int* glyph) {
    SHADOWHOOK_STACK_SCOPE();
    // 直接返回 false，绕过损坏的 post 表二分查找。HarfBuzz 会自动降级为 unicode cmap 查找，完全不影响渲染且绝对安全。
    return false;
}

// =====================================================================
// 🛡️ [ICU & FreeType Hooks]：防止本地化字符与字体渲染引擎空指针解引用闪退
// =====================================================================
static void* g_stub_timezone_get_display_name = nullptr;
typedef void* (*fn_TimeZone_getDisplayName_t)(void* self, bool daylight, int style, void* locale, void* result);
static fn_TimeZone_getDisplayName_t g_orig_timezone_get_display_name = nullptr;

static void* proxy_timezone_get_display_name(void* self, bool daylight, int style, void* locale, void* result) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) {
        LOGW("⚠️ [TimeZone::getDisplayName Hook] self is null or unreadable! Preventing crash.");
        return result;
    }
    return SHADOWHOOK_CALL_PREV(proxy_timezone_get_display_name, self, daylight, style, locale, result);
}

static void* g_stub_timezone_find_id = nullptr;
typedef int (*fn_TimeZone_findID_t)(const void* id);
static fn_TimeZone_findID_t g_orig_timezone_find_id = nullptr;

static int proxy_timezone_find_id(const void* id) {
    SHADOWHOOK_STACK_SCOPE();
    if (!id || !is_pointer_readable(id)) {
        LOGW("⚠️ [TimeZone::findID Hook] id is null or unreadable! Preventing crash.");
        return -1;
    }
    return SHADOWHOOK_CALL_PREV(proxy_timezone_find_id, id);
}

static void* g_stub_unicodeset_stringspan_span = nullptr;
typedef int (*fn_UnicodeSetStringSpan_span_t)(void* self, const void* s, int length, int spanCondition);
static fn_UnicodeSetStringSpan_span_t g_orig_unicodeset_stringspan_span = nullptr;

static int proxy_unicodeset_stringspan_span(void* self, const void* s, int length, int spanCondition) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self) || (length > 0 && (!s || !is_pointer_readable(s)))) {
        LOGW("⚠️ [UnicodeSetStringSpan::span Hook] self or s is null/unreadable! Preventing crash.");
        return 0;
    }
    return SHADOWHOOK_CALL_PREV(proxy_unicodeset_stringspan_span, self, s, length, spanCondition);
}

static void* g_stub_messageformat_findkeyword = nullptr;
typedef int (*fn_MessageFormat_findKeyword_t)(void* s, const void* list);
static fn_MessageFormat_findKeyword_t g_orig_messageformat_findkeyword = nullptr;

static int proxy_messageformat_findkeyword(void* s, const void* list) {
    SHADOWHOOK_STACK_SCOPE();
    if (!s || !is_pointer_readable(s) || !list || !is_pointer_readable(list)) {
        LOGW("⚠️ [MessageFormat::findKeyword Hook] s or list is null/unreadable! Preventing crash.");
        return -1;
    }
    return SHADOWHOOK_CALL_PREV(proxy_messageformat_findkeyword, s, list);
}

static void* g_stub_collationiterator_previous_codepoint = nullptr;
typedef int (*fn_CollationIterator_previousCodePoint_t)(void* self, int* status);
static fn_CollationIterator_previousCodePoint_t g_orig_collationiterator_previous_codepoint = nullptr;

static int proxy_collationiterator_previous_codepoint(void* self, int* status) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) {
        LOGW("⚠️ [CollationIterator::previousCodePoint Hook] self is null! Preventing crash.");
        if (status) *status = 1; // U_ILLEGAL_ARGUMENT_ERROR
        return -1;
    }
    return SHADOWHOOK_CALL_PREV(proxy_collationiterator_previous_codepoint, self, status);
}

static void* g_stub_tt_runins = nullptr;
typedef int (*fn_TT_RunIns_t)(void* exc);
static fn_TT_RunIns_t g_orig_tt_runins = nullptr;

static int proxy_tt_runins(void* exc) {
    SHADOWHOOK_STACK_SCOPE();
    if (!exc) {
        LOGW("⚠️ [TT_RunIns Hook] exc is null! Preventing crash.");
        return 0x14; // FT_Err_Invalid_Argument
    }
    return SHADOWHOOK_CALL_PREV(proxy_tt_runins, exc);
}

// =====================================================================
// 🛡️ [CScriptDecisionMakerModifications::Save Hook]：防止存档期间全局决策制造者未初始化或失效导致虚表解引用闪退
// =====================================================================
static void* g_stub_script_decision_maker_save = nullptr;
typedef void (*fn_ScriptDecisionMakerSave_t)(void* self);
static fn_ScriptDecisionMakerSave_t g_orig_script_decision_maker_save = nullptr;

static void proxy_script_decision_maker_save(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (g_orig_script_decision_maker_save) {
        xdl_info_t info;
        if (xdl_addr(reinterpret_cast<void*>(g_orig_script_decision_maker_save), &info, nullptr)) {
            uintptr_t lib_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
            if (lib_base) {
                // 1. 净化第一个全局决策制造者指针 (offset 0xa8b98f0)
                void** p_dm1 = reinterpret_cast<void**>(lib_base + 0xa8b98f0);
                if (is_pointer_readable(p_dm1)) {
                    void* dm1 = *p_dm1;
                    if (dm1) {
                        if (!is_pointer_readable(dm1)) {
                            *p_dm1 = nullptr;
                        } else {
                            void* vtable = *reinterpret_cast<void**>(dm1);
                            if (!vtable || !is_pointer_readable(vtable)) {
                                LOGW("⚠️ [Save Sanitizer] Clearing invalid global decision maker 1 %p", dm1);
                                *p_dm1 = nullptr;
                            }
                        }
                    }
                }
                
                // 2. 净化第二个全局决策制造者指针 (offset 0xa8afa48)
                void** p_dm2 = reinterpret_cast<void**>(lib_base + 0xa8afa48);
                if (is_pointer_readable(p_dm2)) {
                    void* dm2 = *p_dm2;
                    if (dm2) {
                        if (!is_pointer_readable(dm2)) {
                            *p_dm2 = nullptr;
                        } else {
                            void* vtable = *reinterpret_cast<void**>(dm2);
                            if (!vtable || !is_pointer_readable(vtable)) {
                                LOGW("⚠️ [Save Sanitizer] Clearing invalid global decision maker 2 %p", dm2);
                                *p_dm2 = nullptr;
                            }
                        }
                    }
                }
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_script_decision_maker_save, self);
}

// =====================================================================
// 🛡️ [CTaskComplexInWater::CreateFirstSubTask Hook]：防止水系统未初始化或数组为空导致在水中创建子任务时解引用闪退
// =====================================================================
static void* g_stub_task_complex_in_water_create_first_sub_task = nullptr;
typedef void* (*fn_TaskComplexInWaterCreateFirstSubTask_t)(void* self, void* ped);
static fn_TaskComplexInWaterCreateFirstSubTask_t g_orig_task_complex_in_water_create_first_sub_task = nullptr;

static void* proxy_task_complex_in_water_create_first_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (g_orig_task_complex_in_water_create_first_sub_task) {
        xdl_info_t info;
        if (xdl_addr(reinterpret_cast<void*>(g_orig_task_complex_in_water_create_first_sub_task), &info, nullptr)) {
            uintptr_t lib_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
            if (lib_base) {
                void** p_manager = reinterpret_cast<void**>(lib_base + 0xa8ba5b8);
                if (is_pointer_readable(p_manager)) {
                    void* manager = *p_manager;
                    if (!manager || !is_pointer_readable(manager)) {
                        LOGW("⚠️ [InWater Sanitizer] Null or unreadable water manager %p, skipping task creation", manager);
                        return nullptr;
                    }
                    void** p_array = reinterpret_cast<void**>(manager);
                    if (is_pointer_readable(p_array)) {
                        void* array = *p_array;
                        if (!array || !is_pointer_readable(array)) {
                            LOGW("⚠️ [InWater Sanitizer] Null or unreadable water array %p in manager %p, skipping task creation", array, manager);
                            return nullptr;
                        }
                    }
                }
            }
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_task_complex_in_water_create_first_sub_task, self, ped);
}

// =====================================================================
// 🛡️ [CPed::DoFootLanded Hook]：防止粒子特效系统未就绪或为空导致脚步落地逻辑解引用 +0x18 崩溃
// =====================================================================
static void* g_stub_ped_do_foot_landed = nullptr;
typedef void (*fn_PedDoFootLanded_t)(void* self, bool bLeftFoot, unsigned char uSurfaceType);
static fn_PedDoFootLanded_t g_orig_ped_do_foot_landed = nullptr;

static void proxy_ped_do_foot_landed(void* self, bool bLeftFoot, unsigned char uSurfaceType) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && is_pointer_readable(self)) {
        void** p_fx_system = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x90);
        if (is_pointer_readable(p_fx_system)) {
            void* fx_system = *p_fx_system;
            if (!fx_system || !is_pointer_readable(fx_system)) {
                LOGW("⚠️ [DoFootLanded Sanitizer] Null or unreadable FxSystem %p for ped %p, skipping DoFootLanded", fx_system, self);
                return;
            }
            // 进一步检查 FxSystem 内部结构 (防止 FxSystem::AddParticle 内部解引用 +0x18 崩溃)
            void** p_member_18 = reinterpret_cast<void**>(reinterpret_cast<char*>(fx_system) + 0x18);
            if (!is_pointer_readable(p_member_18) || *p_member_18 == nullptr) {
                LOGW("⚠️ [DoFootLanded Sanitizer] Invalid FxSystem member at +0x18 for ped %p, skipping DoFootLanded", self);
                return;
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_ped_do_foot_landed, self, bLeftFoot, uSurfaceType);
}

// =====================================================================
// Hook 安装线程
// =====================================================================
static void hook_thread_func() {
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

    // 解析游戏引擎自带的 GMalloc 内存分配器指针，确保完全使用 native 分配以彻底杜绝堆损坏
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

    // 🤝 [CTaskComplexPartner & derived Hooks Registration]

    // 1. GetPartnerSequence Deal
    g_stub_gps_deal = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN23CTaskComplexPartnerDeal18GetPartnerSequenceEv",
        reinterpret_cast<void*>(proxy_gps_deal),
        reinterpret_cast<void**>(&g_orig_gps_deal));
    if (g_stub_gps_deal) LOGI("✅ Hooked CTaskComplexPartnerDeal::GetPartnerSequence");
    else LOGE("❌ Failed to hook CTaskComplexPartnerDeal::GetPartnerSequence: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 2. GetPartnerSequence Greet
    g_stub_gps_greet = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexPartnerGreet18GetPartnerSequenceEv",
        reinterpret_cast<void*>(proxy_gps_greet),
        reinterpret_cast<void**>(&g_orig_gps_greet));
    if (g_stub_gps_greet) LOGI("✅ Hooked CTaskComplexPartnerGreet::GetPartnerSequence");
    else LOGE("❌ Failed to hook CTaskComplexPartnerGreet::GetPartnerSequence: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 3. GetPartnerSequence Shove
    g_stub_gps_shove = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexPartnerShove18GetPartnerSequenceEv",
        reinterpret_cast<void*>(proxy_gps_shove),
        reinterpret_cast<void**>(&g_orig_gps_shove));
    if (g_stub_gps_shove) LOGI("✅ Hooked CTaskComplexPartnerShove::GetPartnerSequence");
    else LOGE("❌ Failed to hook CTaskComplexPartnerShove::GetPartnerSequence: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 4. GetPartnerSequence Chat
    g_stub_gps_chat = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN23CTaskComplexPartnerChat18GetPartnerSequenceEv",
        reinterpret_cast<void*>(proxy_gps_chat),
        reinterpret_cast<void**>(&g_orig_gps_chat));
    if (g_stub_gps_chat) LOGI("✅ Hooked CTaskComplexPartnerChat::GetPartnerSequence");
    else LOGE("❌ Failed to hook CTaskComplexPartnerChat::GetPartnerSequence: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 4b. CTaskSimpleHoldEntity::SetPedPosition
    g_stub_set_ped_pos = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CTaskSimpleHoldEntity14SetPedPositionEP4CPed",
        reinterpret_cast<void*>(proxy_set_ped_pos),
        reinterpret_cast<void**>(&g_orig_set_ped_pos));
    if (g_stub_set_ped_pos) LOGI("✅ Hooked CTaskSimpleHoldEntity::SetPedPosition");
    else LOGE("❌ Failed to hook CTaskSimpleHoldEntity::SetPedPosition: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 5. CreateFirstSubTask Base
    g_stub_cfst_base = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN19CTaskComplexPartner18CreateFirstSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_cfst_base),
        reinterpret_cast<void**>(&g_orig_cfst_base));
    if (g_stub_cfst_base) LOGI("✅ Hooked CTaskComplexPartner::CreateFirstSubTask");
    else LOGE("❌ Failed to hook CTaskComplexPartner::CreateFirstSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 6. CreateFirstSubTask Deal
    g_stub_cfst_deal = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN23CTaskComplexPartnerDeal18CreateFirstSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_cfst_deal),
        reinterpret_cast<void**>(&g_orig_cfst_deal));
    if (g_stub_cfst_deal) LOGI("✅ Hooked CTaskComplexPartnerDeal::CreateFirstSubTask");
    else LOGE("❌ Failed to hook CTaskComplexPartnerDeal::CreateFirstSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 7. CreateFirstSubTask Greet
    g_stub_cfst_greet = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexPartnerGreet18CreateFirstSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_cfst_greet),
        reinterpret_cast<void**>(&g_orig_cfst_greet));
    if (g_stub_cfst_greet) LOGI("✅ Hooked CTaskComplexPartnerGreet::CreateFirstSubTask");
    else LOGE("❌ Failed to hook CTaskComplexPartnerGreet::CreateFirstSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 8. ControlSubTask Base
    g_stub_cst_base = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN19CTaskComplexPartner14ControlSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_cst_base),
        reinterpret_cast<void**>(&g_orig_cst_base));
    if (g_stub_cst_base) LOGI("✅ Hooked CTaskComplexPartner::ControlSubTask");
    else LOGE("❌ Failed to hook CTaskComplexPartner::ControlSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // 9. CTaskComplexGoToPointAnyMeans::CreateSubTask
    g_stub_gotopointanymeans_createsubtask = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK29CTaskComplexGoToPointAnyMeans13CreateSubTaskEiP4CPed",
        reinterpret_cast<void*>(proxy_gotopointanymeans_createsubtask),
        reinterpret_cast<void**>(&g_orig_gotopointanymeans_createsubtask));
    if (g_stub_gotopointanymeans_createsubtask) LOGI("✅ Hooked CTaskComplexGoToPointAnyMeans::CreateSubTask");
    else LOGE("❌ Failed to hook CTaskComplexGoToPointAnyMeans::CreateSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

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

    // Hook CTaskGangHassleVehicle::CalcTargetOffset (防止帮派骚扰载具任务中目标载具被销毁或为空时解引用闪退)
    g_stub_CalcTargetOffset = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN22CTaskGangHassleVehicle16CalcTargetOffsetEv",
        reinterpret_cast<void*>(proxy_CalcTargetOffset),
        reinterpret_cast<void**>(&g_orig_CalcTargetOffset));
    if (g_stub_CalcTargetOffset) LOGI("✅ Hooked CTaskGangHassleVehicle::CalcTargetOffset");
    else LOGE("❌ Failed to hook CTaskGangHassleVehicle::CalcTargetOffset: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexTurnToFaceEntityOrCoord::CreateFirstSubTask (防止面对实体/坐标任务中目标实体被销毁或为空时解引用闪退)
    g_stub_turntofaceentity_createfirstsubtask = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN35CTaskComplexTurnToFaceEntityOrCoord18CreateFirstSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_turntofaceentity_createfirstsubtask),
        reinterpret_cast<void**>(&g_orig_turntofaceentity_createfirstsubtask));
    if (g_stub_turntofaceentity_createfirstsubtask) LOGI("✅ Hooked CTaskComplexTurnToFaceEntityOrCoord::CreateFirstSubTask");
    else LOGE("❌ Failed to hook CTaskComplexTurnToFaceEntityOrCoord::CreateFirstSubTask: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexTurnToFaceEntityOrCoord::ControlSubTask (防止面对实体/坐标任务中目标实体被销毁或为空时解引用闪退)
    g_stub_turntofaceentity_controlsubtask = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN35CTaskComplexTurnToFaceEntityOrCoord14ControlSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_turntofaceentity_controlsubtask),
        reinterpret_cast<void**>(&g_orig_turntofaceentity_controlsubtask));
    if (g_stub_turntofaceentity_controlsubtask) LOGI("✅ Hooked CTaskComplexTurnToFaceEntityOrCoord::ControlSubTask");
    else LOGE("❌ Failed to hook CTaskComplexTurnToFaceEntityOrCoord::ControlSubTask: %s",
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

    // Hook CPed::DoFootLanded (防止粒子特效系统未就绪导致脚步落地逻辑解引用 +0x18 崩溃)
    g_stub_ped_do_foot_landed = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN4CPed12DoFootLandedEbh",
        reinterpret_cast<void*>(proxy_ped_do_foot_landed),
        reinterpret_cast<void**>(&g_orig_ped_do_foot_landed));
    if (g_stub_ped_do_foot_landed) LOGI("✅ Hooked CPed::DoFootLanded");
    else LOGE("❌ Failed to hook CPed::DoFootLanded: %s",
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

    // Hook CPedIntelligence::ProcessAfterPreRender (防止已析构任务残留导致 ProcessAfterPreRender 纯虚函数调用闪退)
    g_stub_process_after_pre_render = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN16CPedIntelligence21ProcessAfterPreRenderEv",
        reinterpret_cast<void*>(proxy_process_after_pre_render),
        reinterpret_cast<void**>(&g_orig_process_after_pre_render));
    if (g_stub_process_after_pre_render) LOGI("✅ Hooked CPedIntelligence::ProcessAfterPreRender");
    else LOGE("❌ Failed to hook CPedIntelligence::ProcessAfterPreRender: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTask::~CTask (防止在 ProcessAfterPreRender 期间任务中途被析构产生野指针导致闪退)
    g_stub_task_destructor = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN5CTaskD2Ev",
        reinterpret_cast<void*>(proxy_task_destructor),
        reinterpret_cast<void**>(&g_orig_task_destructor));
    if (g_stub_task_destructor) LOGI("✅ Hooked CTask::Destructor");
    else LOGE("❌ Failed to hook CTask::Destructor: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook AssetPackManager_requestDownload (防止无谷歌服务环境下 Play Core 崩溃)
    g_stub_asset_pack_manager_request_download = shadowhook_hook_sym_name(
        TARGET_LIB,
        "AssetPackManager_requestDownload",
        reinterpret_cast<void*>(proxy_asset_pack_manager_request_download),
        reinterpret_cast<void**>(&g_orig_asset_pack_manager_request_download));
    if (g_stub_asset_pack_manager_request_download) LOGI("✅ Hooked AssetPackManager_requestDownload");
    else LOGE("❌ Failed to hook AssetPackManager_requestDownload: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));



    // Hook TimeZone::findID (防止本地化时区查找空指针崩溃)
    g_stub_timezone_find_id = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6icu_648TimeZone6findIDERKNS_13UnicodeStringE",
        reinterpret_cast<void*>(proxy_timezone_find_id),
        reinterpret_cast<void**>(&g_orig_timezone_find_id));
    if (g_stub_timezone_find_id) LOGI("✅ Hooked TimeZone::findID");
    else LOGE("❌ Failed to hook TimeZone::findID: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook u_strlen_64 (防止 ICU 字符串长度计算传入野指针崩溃)
    g_stub_u_strlen = shadowhook_hook_sym_name(
        TARGET_LIB,
        "u_strlen_64",
        reinterpret_cast<void*>(proxy_u_strlen),
        reinterpret_cast<void**>(&g_orig_u_strlen));
    if (g_stub_u_strlen) LOGI("✅ Hooked u_strlen_64");
    else LOGE("❌ Failed to hook u_strlen_64: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook DateTimePatternGenerator::createEmptyInstance
    g_stub_dtpg_create_empty_instance = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6icu_6424DateTimePatternGenerator19createEmptyInstanceER10UErrorCode",
        reinterpret_cast<void*>(proxy_dtpg_create_empty_instance),
        reinterpret_cast<void**>(&g_orig_dtpg_create_empty_instance));
    if (g_stub_dtpg_create_empty_instance) LOGI("✅ Hooked DateTimePatternGenerator::createEmptyInstance");
    else LOGE("❌ Failed to hook DateTimePatternGenerator::createEmptyInstance: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook DateTimePatternGenerator::createInstance
    g_stub_dtpg_create_instance_no_locale = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6icu_6424DateTimePatternGenerator14createInstanceER10UErrorCode",
        reinterpret_cast<void*>(proxy_dtpg_create_instance_no_locale),
        reinterpret_cast<void**>(&g_orig_dtpg_create_instance_no_locale));
    if (g_stub_dtpg_create_instance_no_locale) LOGI("✅ Hooked DateTimePatternGenerator::createInstance");
    else LOGE("❌ Failed to hook DateTimePatternGenerator::createInstance: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook DateTimePatternGenerator::createInstance(Locale)
    g_stub_dtpg_create_instance_with_locale = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6icu_6424DateTimePatternGenerator14createInstanceERKNS_6LocaleER10UErrorCode",
        reinterpret_cast<void*>(proxy_dtpg_create_instance_with_locale),
        reinterpret_cast<void**>(&g_orig_dtpg_create_instance_with_locale));
    if (g_stub_dtpg_create_instance_with_locale) LOGI("✅ Hooked DateTimePatternGenerator::createInstance(Locale)");
    else LOGE("❌ Failed to hook DateTimePatternGenerator::createInstance(Locale): %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook HarfBuzz get_glyph_from_name (防止字体 post 表二分查找崩溃)
    g_stub_hb_get_glyph_from_name = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK2OT4post13accelerator_t19get_glyph_from_nameEPKciPj",
        reinterpret_cast<void*>(proxy_hb_get_glyph_from_name),
        reinterpret_cast<void**>(&g_orig_hb_get_glyph_from_name));
    if (g_stub_hb_get_glyph_from_name) LOGI("✅ Hooked HarfBuzz get_glyph_from_name");
    else LOGE("❌ Failed to hook HarfBuzz get_glyph_from_name: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook TimeZone::getDisplayName (防止本地化时区空指针崩溃)
    g_stub_timezone_get_display_name = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK6icu_648TimeZone14getDisplayNameEaNS0_12EDisplayTypeERKNS_6LocaleERNS_13UnicodeStringE",
        reinterpret_cast<void*>(proxy_timezone_get_display_name),
        reinterpret_cast<void**>(&g_orig_timezone_get_display_name));
    if (g_stub_timezone_get_display_name) LOGI("✅ Hooked TimeZone::getDisplayName");
    else LOGE("❌ Failed to hook TimeZone::getDisplayName: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook UnicodeSetStringSpan::span (防止Unicode字符解析空指针崩溃)
    g_stub_unicodeset_stringspan_span = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK6icu_6420UnicodeSetStringSpan4spanEPKDsi17USetSpanCondition",
        reinterpret_cast<void*>(proxy_unicodeset_stringspan_span),
        reinterpret_cast<void**>(&g_orig_unicodeset_stringspan_span));
    if (g_stub_unicodeset_stringspan_span) LOGI("✅ Hooked UnicodeSetStringSpan::span");
    else LOGE("❌ Failed to hook UnicodeSetStringSpan::span: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook MessageFormat::findKeyword (防止MessageFormat解析空指针崩溃)
    g_stub_messageformat_findkeyword = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6icu_6413MessageFormat11findKeywordERKNS_13UnicodeStringEPKPKDs",
        reinterpret_cast<void*>(proxy_messageformat_findkeyword),
        reinterpret_cast<void**>(&g_orig_messageformat_findkeyword));
    if (g_stub_messageformat_findkeyword) LOGI("✅ Hooked MessageFormat::findKeyword");
    else LOGE("❌ Failed to hook MessageFormat::findKeyword: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CollationIterator::previousCodePoint (防止文本排序迭代空指针崩溃)
    g_stub_collationiterator_previous_codepoint = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN6icu_6425FCDUTF16CollationIterator17previousCodePointER10UErrorCode",
        reinterpret_cast<void*>(proxy_collationiterator_previous_codepoint),
        reinterpret_cast<void**>(&g_orig_collationiterator_previous_codepoint));
    if (g_stub_collationiterator_previous_codepoint) LOGI("✅ Hooked CollationIterator::previousCodePoint");
    else LOGE("❌ Failed to hook CollationIterator::previousCodePoint: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook TT_RunIns (防止FreeType字体渲染解析崩溃)
    g_stub_tt_runins = shadowhook_hook_sym_name(
        TARGET_LIB,
        "TT_RunIns",
        reinterpret_cast<void*>(proxy_tt_runins),
        reinterpret_cast<void**>(&g_orig_tt_runins));
    if (g_stub_tt_runins) LOGI("✅ Hooked TT_RunIns");
    else LOGE("❌ Failed to hook TT_RunIns: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CScriptDecisionMakerModifications::Save (防止存档时全局决策制造者失效导致解引用闪退)
    g_stub_script_decision_maker_save = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN33CScriptDecisionMakerModifications4SaveEv",
        reinterpret_cast<void*>(proxy_script_decision_maker_save),
        reinterpret_cast<void**>(&g_orig_script_decision_maker_save));
    if (g_stub_script_decision_maker_save) LOGI("✅ Hooked CScriptDecisionMakerModifications::Save");
    else LOGE("❌ Failed to hook CScriptDecisionMakerModifications::Save: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexInWater::CreateFirstSubTask (防止在水中创建子任务时因水系统未初始化闪退)
    g_stub_task_complex_in_water_create_first_sub_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN19CTaskComplexInWater18CreateFirstSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_task_complex_in_water_create_first_sub_task),
        reinterpret_cast<void**>(&g_orig_task_complex_in_water_create_first_sub_task));
    if (g_stub_task_complex_in_water_create_first_sub_task) LOGI("✅ Hooked CTaskComplexInWater::CreateFirstSubTask");
    else LOGE("❌ Failed to hook CTaskComplexInWater::CreateFirstSubTask: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

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
static void write_nothing_so(const std::string& path) {
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

static void extract_nothing_so(const char* process_name) {
    // 提取基础包名（防止子进程包含冒号导致路径失效）
    std::string pkg_name(process_name);
    size_t colon_pos = pkg_name.find(':');
    if (colon_pos != std::string::npos) {
        pkg_name = pkg_name.substr(0, colon_pos);
    }

    // 彻底移除直接向应用根目录写入的尝试，规避权限和越权审计警报
    // 同时优先且仅使用 /data/user/0/ 的标准规范路径，规避 Android 10+ 下对 /data/data/ 软链接路径的 SELinux 限制
    std::string base_dir = std::string("/data/user/0/") + pkg_name;
    std::string cache_dir = base_dir + "/code_cache";
    
    mkdir(cache_dir.c_str(), 0700);
    
    std::string path = cache_dir + "/libshadowhook_nothing.so";
    write_nothing_so(path);
}

static void cleanup_nothing_so(const char* process_name) {
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
// ECS & 事件驱动系统初始化 (init_ecs_systems)
// =====================================================================
void init_ecs_systems() {
    LOGI("⚡️ [ECS Engine] Initializing ECS Systems...");

    // 1. CleanupSystem: 监听实体销毁事件
    ecs::EventDispatcher::get().subscribe<ecs::EntityCleanupEvent>("EntityCleanupEvent", [](const ecs::EntityCleanupEvent& ev) {
        if (ev.entity) {
            ecs::EntityManager::get().destroy_entity(ev.entity);
        }
    });

    // 2. CopDispatchSystem: 监听犯罪通报事件 & 伤害事件
    ecs::EventDispatcher::get().subscribe<ecs::CrimeReportEvent>("CrimeReportEvent", [](const ecs::CrimeReportEvent& ev) {
        auto* criminal = static_cast<CPed*>(ev.criminal);
        if (!criminal || !is_ped_pointer_valid_safe(criminal)) return;

        // 在 ECS 中注册并初始化/更新犯罪分子组件
        auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(criminal);
        if (!crim_comp) {
            crim_comp = ecs::EntityManager::get().add_component<ecs::CriminalComponent>(criminal, criminal);
            if (crim_comp) {
                crim_comp->first_detect_time_ms = ev.time_ms;
                crim_comp->initial_weapon_category = ev.weapon_category;
            }
        } else {
            // Weapon Downgrade Protection: Keep highest/first weapon category
            if (ev.weapon_category > crim_comp->initial_weapon_category) {
                crim_comp->initial_weapon_category = ev.weapon_category;
            }
        }

        if (crim_comp) {
            crim_comp->last_attack_time_ms = ev.time_ms;

            if (ev.victim) {
                crim_comp->is_active = true;
                crim_comp->is_air_shooter = false;
                crim_comp->is_fleeing = false;
                crim_comp->current_victim = ev.victim;
            } else {
                if (ev.weapon_category == 2) { // FIREARM
                    crim_comp->is_active = true; // Mark active for FIREARM_AIR_SHOOT
                    crim_comp->is_air_shooter = true;
                    crim_comp->is_fleeing = false;
                    crim_comp->current_victim = nullptr;
                } else {
                    crim_comp->is_active = false;
                    crim_comp->is_air_shooter = false;
                    crim_comp->is_fleeing = false;
                    crim_comp->current_victim = nullptr;
                }
            }
        }

        // 尝试并案
        bool merged = try_consolidate_crime(criminal, ev.location, ev.is_firearm);
        if (!merged) {
            // 如果未能并案，且符合激活条件，则激活为全新犯罪现场或顶替旧案
            if (should_activate_or_hijack_crime(ev.location, ev.is_firearm)) {
                std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);

                // 初始化并启动新案对象
                auto new_crime = std::make_shared<CrimeEvent>();
                new_crime->case_id = g_next_case_id++;
                new_crime->location = ev.location;
                new_crime->criminal = criminal;
                new_crime->is_firearm = ev.is_firearm;
                new_crime->consolidated_criminals.push_back(criminal);
                new_crime->criminal_is_firearm.push_back(ev.is_firearm);
                
                // 写入嫌疑人详细犯罪分级与状态
                CrimeEvent::CriminalState c_state;
                c_state.first_threat_category = ev.is_firearm ? 2 : (ev.weapon_category == 1 ? 1 : 0);
                c_state.current_threat_category = c_state.first_threat_category;
                c_state.is_active = crim_comp ? crim_comp->is_active : false;
                c_state.shooting_air = crim_comp ? crim_comp->is_air_shooter : false;
                c_state.fleeing = crim_comp ? crim_comp->is_fleeing : false;
                new_crime->criminal_states[criminal] = c_state;

                new_crime->dispatch_sent = false;
                new_crime->road_closure_active = false;
                new_crime->cops_killed = 0;
                new_crime->cancelled = false;
                new_crime->dispatch_state = STATE_IDLE; // STATE_IDLE

                g_active_crimes.push_back(new_crime);
                g_crime_active.store(true);
                g_tracked_criminal.store(criminal);

                LOGI("📡 [ECS CopDispatchSystem] Activated new crime event Case %llu! Perp: %p, Firearm: %d, Pos: (%.1f, %.1f, %.1f)",
                     (unsigned long long)new_crime->case_id, criminal, ev.is_firearm, ev.location.x, ev.location.y, ev.location.z);
            }
        }

        // 重新评估案件主犯与警力路由
        update_primary_criminal_by_threat();
    });

    ecs::EventDispatcher::get().subscribe<ecs::DamageEvent>("DamageEvent", [](const ecs::DamageEvent& ev) {
        auto* victim_cop = static_cast<CPed*>(ev.victim);
        auto* attacker_perp = static_cast<CPed*>(ev.attacker);
        if (victim_cop && is_ped_pointer_valid_safe(victim_cop) &&
            attacker_perp && is_ped_pointer_valid_safe(attacker_perp)) {

            // 注册警员与其战斗状态
            auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(victim_cop);
            if (!cop_comp) {
                cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(victim_cop, victim_cop);
            }
            auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(victim_cop);
            if (!combat_comp) {
                combat_comp = ecs::EntityManager::get().add_component<ecs::CombatComponent>(victim_cop);
            }
            if (combat_comp) {
                combat_comp->target_entity = attacker_perp;
            }

            // 警员受袭，触发即时自卫反击 (不强制更新武器模型，防止重置攻击动画)
            make_single_cop_attack_criminal(victim_cop, attacker_perp, false);
        }
    });

    // 3. CopWeaponSelectionSystem: 监听武器切换事件 & 周期性 Tick 事件
    ecs::EventDispatcher::get().subscribe<ecs::WeaponSwitchEvent>("WeaponSwitchEvent", [](const ecs::WeaponSwitchEvent& ev) {
        auto* ped = static_cast<CPed*>(ev.ped);
        if (!ped || !is_ped_pointer_valid_safe(ped)) return;

        // 如果是执勤警员切枪，更新其余下的 CombatComponent 记录
        int ped_type = g_GetPedType ? g_GetPedType(ped) : 0;
        if (ped_type == PED_TYPE_COP) {
            auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
            if (combat) {
                combat->current_weapon_type = ev.current_weapon;
                combat->last_weapon_switch_time_ms = ev.time_ms;
            }
            return;
        }

        // 如果是处于侦测中/并案列表中的犯罪分子切枪，则处理案件的即时升级与降级冻结
        if (g_crime_active.load() && !g_active_crime.cancelled) {
            bool is_our_criminal = false;
            size_t criminal_idx = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
                for (size_t idx = 0; idx < g_active_crime.consolidated_criminals.size(); ++idx) {
                    if (g_active_crime.consolidated_criminals[idx] == ped) {
                        is_our_criminal = true;
                        criminal_idx = idx;
                        break;
                    }
                }

                if (is_our_criminal) {
                    auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
                    if (crim_comp) {
                        int new_weap_cat = 0;
                        if (ev.current_weapon >= WEAPON_PISTOL && ev.current_weapon <= WEAPON_MINIGUN) {
                            new_weap_cat = 2; // FIREARM
                        } else if (ev.current_weapon == WEAPON_UNARMED) {
                            new_weap_cat = 0; // UNARMED
                        } else {
                            new_weap_cat = 1; // MELEE
                        }

                        if (new_weap_cat > crim_comp->initial_weapon_category) {
                            crim_comp->initial_weapon_category = new_weap_cat;
                            crim_comp->is_active = true;
                            crim_comp->is_fleeing = false;
                            crim_comp->is_air_shooter = false;
                            crim_comp->last_attack_time_ms = ev.time_ms; // 刷新其威胁时间，保证不因旧的时间戳被判为超时
                            LOGI("⚡️ [ECS WeaponSwitch] Criminal %p upgraded weapon category to %d! Escalated threat level to active.", ped, new_weap_cat);
                        } else if (new_weap_cat < crim_comp->initial_weapon_category) {
                            // Weapon Downgrade: freeze weapon tier, but mark them as inactive (fleeing)
                            crim_comp->is_active = false;
                            crim_comp->is_air_shooter = false;
                            crim_comp->is_fleeing = true;
                            LOGI("⚡️ [ECS WeaponSwitch] Criminal %p downgraded weapon to %d. Classifying as Inactive & Fleeing of initial category %d.", 
                                 ped, ev.current_weapon, crim_comp->initial_weapon_category);
                        }
                    }

                    bool firearm = (ev.current_weapon >= WEAPON_PISTOL && ev.current_weapon <= WEAPON_MINIGUN);
                    if (firearm) {
                        bool escalated = false;
                        if (criminal_idx < g_active_crime.criminal_is_firearm.size()) {
                            if (!g_active_crime.criminal_is_firearm[criminal_idx]) {
                                g_active_crime.criminal_is_firearm[criminal_idx] = true;
                                escalated = true;
                            }
                        }
                        if (!g_active_crime.is_firearm) {
                            g_active_crime.is_firearm = true;
                            escalated = true;
                        }

                        if (escalated) {
                            LOGI("⚡️ [ECS CopWeaponSelectionSystem] Criminal %p switched weapon to firearm %d! Escalating case immediately.", ped, ev.current_weapon);
                        }
                    }
                }
            }

            if (is_our_criminal) {
                update_cops_targeting_criminal_event_driven(ped);
            }
        }
    });

    // 4. CopStuckAndWeaponSelectionSystem: 周期性 Tick 事件
    ecs::EventDispatcher::get().subscribe<ecs::TickEvent>("TickEvent", [](const ecs::TickEvent& ev) {
        int64_t cur_time = ev.current_time_ms;

        // 1. CriminalComponent 周期维护
        auto criminals = ecs::EntityManager::get().get_entities_with<ecs::CriminalComponent>();
        for (auto crim_ent : criminals) {
            auto* ped = static_cast<CPed*>(crim_ent);
            if (!ped || !is_ped_pointer_valid_safe(ped) || (g_IsAlive && !g_IsAlive(ped))) {
                continue;
            }

            auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
            if (crim_comp) {
                // 1.1 检查受害者状态
                auto* victim_ped = static_cast<CPed*>(crim_comp->current_victim);
                if (victim_ped && is_ped_pointer_valid_safe(victim_ped)) {
                    if (g_IsAlive && !g_IsAlive(victim_ped)) {
                        crim_comp->is_active = false;
                        crim_comp->current_victim = nullptr;
                        LOGI("⚡ [ECS TickEvent] Criminal %p victim died. Classifying as inactive.", ped);
                    }
                }

                // 1.2 检查攻击超时 (8秒无攻击通报/伤害产生)
                if (crim_comp->is_active && (cur_time - crim_comp->last_attack_time_ms > 8000)) {
                    crim_comp->is_active = false;
                    crim_comp->is_air_shooter = false;
                    LOGI("⚡ [ECS TickEvent] Criminal %p active state timed out (8s no attack).", ped);
                }
            }

            update_primary_criminal_by_threat();
        }

        // 2. CopStuckAndWeaponSelectionSystem:
        // 遍历所有已绑定的警员实体，进行自动化战术升级与卡死检测
        auto cops = ecs::EntityManager::get().get_entities_with<ecs::CopComponent>();
        for (auto cop_ent : cops) {
            auto* cop = static_cast<CPed*>(cop_ent);
            if (!cop || !is_ped_pointer_valid_safe(cop) || (g_IsAlive && !g_IsAlive(cop))) {
                continue;
            }

            auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
            auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);

            // 2.1 警员卡死检测与智能重新寻路路由 (Automated Stuck routing)
            if (cop_comp) {
                // 每 2 秒进行一次卡死坐标检查
                if (ev.current_time_ms - cop_comp->last_stuck_check_time_ms > 2000) {
                    cop_comp->last_stuck_check_time_ms = ev.current_time_ms;
                    CVector pos = get_entity_pos(cop);

                    // 仅在非载具内追杀敌人的状态下检测卡死
                    if (!cop_comp->is_in_vehicle && combat_comp && combat_comp->target_entity) {
                        float dx = pos.x - cop_comp->last_pos_x;
                        float dy = pos.y - cop_comp->last_pos_y;
                        float dist_moved = sqrtf(dx * dx + dy * dy);

                        if (dist_moved < 0.20f) {
                            cop_comp->stuck_count++;
                            if (cop_comp->stuck_count >= 3) { // 连续卡死 6 秒以上
                                auto* target = static_cast<CPed*>(combat_comp->target_entity);
                                if (target && is_ped_pointer_valid_safe(target)) {
                                    LOGW("⚠️ [ECS StuckSolver] Ground cop %p is stuck pursuing %p. Force resetting task for pathfinding routing.", cop, target);
                                    make_single_cop_attack_criminal(cop, target, true);
                                }
                                cop_comp->stuck_count = 0;
                            }
                        } else {
                            cop_comp->stuck_count = 0;
                        }
                    } else {
                        cop_comp->stuck_count = 0;
                    }

                    cop_comp->last_pos_x = pos.x;
                    cop_comp->last_pos_y = pos.y;
                    cop_comp->last_pos_z = pos.z;
                }
            }

            // 2.3 步行警员响应调度沿途发生的同级及以上活跃犯罪 (EnRoute Rerouting for foot cops)
            if (cop_comp && !cop_comp->is_in_vehicle && combat_comp && combat_comp->target_entity) {
                std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
                std::shared_ptr<CrimeEvent> case_A = nullptr;
                for (const auto& crime : g_active_crimes) {
                    if (!crime || crime->cancelled) continue;
                    bool found = false;
                    if (crime->criminal == combat_comp->target_entity) {
                        found = true;
                    } else {
                        for (CPed* crim : crime->consolidated_criminals) {
                            if (crim == combat_comp->target_entity) {
                                found = true;
                                break;
                            }
                        }
                    }
                    if (found) {
                        case_A = crime;
                        break;
                    }
                }

                if (case_A) {
                    CVector cop_pos = get_entity_pos(cop);
                    std::shared_ptr<CrimeEvent> best_case_B = nullptr;
                    float best_dist = 999999.0f;

                    for (const auto& case_B : g_active_crimes) {
                        if (!case_B || case_B->cancelled || case_B == case_A) continue;
                        if (!case_B->criminal || !is_ped_pointer_valid_safe(case_B->criminal)) continue;

                        int threat_A = case_A->is_firearm ? 2 : 1;
                        int threat_B = case_B->is_firearm ? 2 : 1;

                        if (threat_B >= threat_A) {
                            CVector crim_pos = get_entity_pos(case_B->criminal);
                            float dx = cop_pos.x - crim_pos.x;
                            float dy = cop_pos.y - crim_pos.y;
                            float dz = cop_pos.z - crim_pos.z;
                            float dist = sqrtf(dx * dx + dy * dy + dz * dz);

                            float av_range = case_B->is_firearm ? 75.0f : 35.0f;

                            if (dist <= av_range && dist < best_dist) {
                                best_dist = dist;
                                best_case_B = case_B;
                            }
                        }
                    }

                    if (best_case_B) {
                        LOGI("🚨 [dispatchCenter - FootCopReroute] Foot cop %p (originally pursuing Case %llu, threat: %d) encountered Case %llu (threat: %d) en route! Rerouting to Case %llu (dist: %.1fm).",
                             cop, (unsigned long long)case_A->case_id, case_A->is_firearm ? 2 : 1,
                             (unsigned long long)best_case_B->case_id, best_case_B->is_firearm ? 2 : 1,
                             (unsigned long long)best_case_B->case_id, best_dist);

                        make_single_cop_attack_criminal(cop, best_case_B->criminal, true);
                        if (combat_comp) {
                            combat_comp->last_weapon_switch_time_ms = 0;
                        }
                    }
                }
            }

            // 2.2 智能武器选择与高响应收枪控制链 (零延迟、高响应性切枪与即时自动收枪机制)
            bool should_disarm = false;
            CPed* target = nullptr;

            if (combat_comp) {
                target = static_cast<CPed*>(combat_comp->target_entity);
            }

            // A. 自适应判定是否应当收枪退敌 (战斗结束、离队、车内或目标失效)
            if (cop_comp && cop_comp->is_in_vehicle) {
                should_disarm = true; // 载具内不需要本模块强行控制手持武器，交还底层的默认状态
            } else if (!target) {
                should_disarm = true; // 无追杀目标
            } else if (!is_ped_pointer_valid_safe(target)) {
                should_disarm = true; // 目标指针已失效或被引擎清理
            } else if (g_IsAlive && !g_IsAlive(target)) {
                should_disarm = true; // 目标已被击毙
            } else if (!g_crime_active.load()) {
                should_disarm = true; // 警情已全局解除
            } else {
                // 校验该目标是否属于任何一个活跃的犯罪现场
                bool target_is_active_criminal = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
                    for (const auto& crime : g_active_crimes) {
                        if (crime && !crime->cancelled) {
                            if (crime->criminal == target) {
                                target_is_active_criminal = true;
                                break;
                            }
                            for (CPed* c : crime->consolidated_criminals) {
                                if (c == target) {
                                    target_is_active_criminal = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!target_is_active_criminal) {
                    should_disarm = true; // 目标在任何警情中均已不再活跃，或被并案系统剔除
                }
            }

            // 追加测距脱离检测：如果追击目标超过 100 米未果，算作跟丢脱战，立即收枪
            if (!should_disarm && target) {
                CVector cop_pos = get_entity_pos(cop);
                CVector tgt_pos = get_entity_pos(target);
                float dx = cop_pos.x - tgt_pos.x;
                float dy = cop_pos.y - tgt_pos.y;
                float dz = cop_pos.z - tgt_pos.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dist > 100.0f) {
                    should_disarm = true;
                }
            }

            if (should_disarm) {
                // 警员当前并不处于任何有效的攻击或战斗态
                bool is_currently_armed = (combat_comp && combat_comp->current_weapon_type != (int)WEAPON_UNARMED);
                
                if (is_currently_armed) {
                    if (g_SetCurrentWeapon) {
                        g_SetCurrentWeapon(cop, WEAPON_UNARMED);
                    }
                    if (combat_comp) {
                        combat_comp->current_weapon_type = (int)WEAPON_UNARMED;
                        combat_comp->target_entity = nullptr;
                        combat_comp->last_weapon_switch_time_ms = ev.current_time_ms;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
                        g_cop_assigned_weapon.erase(cop);
                    }
                    LOGI("🎯 [ECS WeaponSelectionSystem - DISARM] Cop %p successfully disarmed (target dead or lost).", cop);
                }

                // 重点提升：既然此警员任务完全解除且已安全收枪，先命令其返回原本绑定的车辆
                if (cop_comp && !cop_comp->is_in_vehicle) {
                    bool is_driver = false;
                    void* bound_veh = find_bound_vehicle_of_cop(cop, is_driver);
                    if (bound_veh) {
                        // 驾驶员生存性晋升系统 (Driver Survivability Promotion)
                        // 如果当前警员是乘客，但该车原绑定的驾驶员已经殉职/不存在，则自动将该警员晋升为驾驶员！
                        if (!is_driver && !is_alive_bound_driver_exists(bound_veh)) {
                            is_driver = true;
                            // 同步更新绑定关系
                            {
                                std::lock_guard<std::mutex> lock(g_bindings_mutex);
                                for (auto& binding : g_cop_vehicle_bindings) {
                                    if (binding.cop == cop) {
                                        binding.as_driver = true;
                                        break;
                                    }
                                }
                            }
                            LOGW("👮 [ECS - DriverPromotion] Passenger cop %p promoted to DRIVER for vehicle %p because the bound driver is deceased/missing.", cop, bound_veh);
                        }
                        make_cop_enter_vehicle(cop, bound_veh, is_driver);
                    }
                }

                // 重点提升：既然此警员任务完全解除且已安全收枪，直接移除其绑定的战术组件，不再占用下一帧的轮询资源
                ecs::EntityManager::get().remove_component<ecs::CopComponent>(cop);
                ecs::EntityManager::get().remove_component<ecs::CombatComponent>(cop);
            } 
            else if (combat_comp && target) {
                // B. 自适应高灵敏战术武器切换
                bool firearm_threat = is_specific_criminal_armed_with_firearm(target);
                eWeaponType target_weapon = determine_weapon_for_cop(cop, target, firearm_threat);

                // 2 秒强制武器强化周期，用来解决由于下车动作、摔倒或受击导致游戏底层自动重置武器为拳头的 bug
                bool cop_is_in_veh = (find_vehicle_of_cop(cop) != nullptr);
                bool periodic_reinforce = (!cop_is_in_veh) && (ev.current_time_ms - combat_comp->last_weapon_switch_time_ms > 2000);

                if (target_weapon != (eWeaponType)combat_comp->current_weapon_type || periodic_reinforce) {
                    // 战术规则优化：
                    // 1. 紧急升级判定：如果是枪械威胁，属于紧急升级，一秒都不能等，直接无视冷却强制切枪！
                    // 2. 正常状态调整：对于普通距离或威胁调整，冷却时间大幅从 5 秒缩减到 1 秒 (1000ms)，保障姿态极速跟进，同时免除高频摆动。
                    bool is_urgent_upgrade = (target_weapon == WEAPON_PISTOL && combat_comp->current_weapon_type != (int)WEAPON_PISTOL);
                    bool time_cooldown_passed = (ev.current_time_ms - combat_comp->last_weapon_switch_time_ms > 1000);

                    if (is_urgent_upgrade || time_cooldown_passed || periodic_reinforce) {
                        if (g_GiveWeapon && g_SetCurrentWeapon) {
                            g_GiveWeapon(cop, target_weapon, 9999, true);
                            g_SetCurrentWeapon(cop, target_weapon);
                            combat_comp->current_weapon_type = (int)target_weapon;
                            combat_comp->last_weapon_switch_time_ms = ev.current_time_ms;
                            LOGI("🎯 [ECS WeaponSelectionSystem - SWITCH/REINFORCE] Cop %p weapon updated to match threat (urgent=%d, reinforce=%d): %d", 
                                 cop, is_urgent_upgrade, periodic_reinforce, (int)target_weapon);
                        }
                    }
                }
            }
        }
    });

    LOGI("✅ [ECS Engine] All systems successfully initialized!");
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

            // 初始化完成后，立刻清理磁盘上的 so 文件，做到完全无痕 (Stealth & Clean)
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
