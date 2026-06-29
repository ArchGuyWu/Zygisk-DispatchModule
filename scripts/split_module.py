#!/usr/bin/env python3
"""Split module.cpp into multiple translation units."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP_DIR = ROOT / "src/main/cpp"
MODULE = CPP_DIR / "module.cpp"

# 1-based inclusive line ranges to EXTRACT (removed from module.cpp)
RANGES = {
    "hooks_stability.cpp": [(5241, 5788), (6082, 6338)],
    "hook_install.cpp": [(6006, 6080), (6342, 6935)],
    "ecs_systems.cpp": [(6940, 7435)],
}

COMMON_INCLUDES = '''\
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

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"
#include "sh_nothing_bin.h"

#include "include/log.hpp"
#include "include/game_config.hpp"
#include "include/game_types.hpp"
#include "include/pointer_sanitizer.hpp"
#include "include/mod_shared.hpp"
#include "ecs_engine.hpp"
'''

SHARED_HEADER = '''\
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "third_party/xdl/xdl.h"
#include "include/game_types.hpp"

// --- 符号解析宏 ---
#define RESOLVE_SYM(handle, var, mangled, type) do { \\
    var = reinterpret_cast<type>(xdl_sym(handle, mangled, nullptr)); \\
    if (var) LOGI("  ✅ " #var " -> %p", (void*)var); \\
    else     LOGW("  ⚠️ " #var " not found (%s)", mangled); \\
} while(0)

class FMalloc {
public:
    virtual ~FMalloc() {}
    virtual void* Malloc(size_t Count, uint32_t Alignment = 0) = 0;
    virtual void* Realloc(void* Original, size_t Count, uint32_t Alignment = 0) = 0;
    virtual void Free(void* Original) = 0;
};

// Extra typedefs used by hooks / dispatch
typedef void (*fn_TheScriptsProcess_t)();
typedef void (*fn_EventDamage_ctor_t)(void* event_this, CEntity* damageSource, unsigned int startTime, eWeaponType weaponType, int pieceType, unsigned char damageSeverity, bool b1, bool b2);
typedef void* (*fn_FindNearestFire_t)(void* fire_manager_this, const CVector& pos, bool bCheckScriptFires, bool bCheckNormalFires);
typedef void (*fn_CEventGunShot_ctor_t)(void*, CEntity*, CVector, CVector, bool);
typedef void (*fn_CEventGunShot_dtor_t)(void*);
typedef void (*fn_CEventGroup_Add_t)(void*, void*, bool);
typedef bool (*fn_IsDriver_t)(const void* vehicle_this, const CPed* ped);
typedef bool (*fn_IsPassenger_t)(const void* vehicle_this, const CPed* ped);
typedef void (*fn_TellOccupantsToLeaveCar_t)(void* vehicle);
typedef void* (*fn_GetPoolVehicle_t)(int);
typedef void (*fn_GetCarToGoToCoors_t)(void* vehicle, CVector* coors, int drivingMode, bool flag);
typedef void (*fn_SwitchRoadsOffInArea_t)(void* instance, float minX, float minY, float minZ, float maxX, float maxY, float maxZ, bool bSwitchOff, bool bKeepVehicles, bool bAllowBoats);
typedef void* (*fn_TaskNew_t)(unsigned long);
typedef void (*fn_AddTaskPrimaryMaybeInGroup_t)(void* ped_intel_this, CTask* task, bool writeToEventLog);
typedef void* (*fn_FindTaskByType_t)(const void* ped_intel_this, int task_type);
typedef CVector (*fn_GetGameCamPosition_t)(void*);
typedef CVector (*fn_GetLookDirection_t)(void*);

extern constexpr int TASK_COMPLEX_KILL_CRIMINAL;

// --- 运行时符号指针 ---
extern fn_FindPlayerPed_t            g_FindPlayerPed;
extern fn_FindPlayerCoors_t          g_FindPlayerCoors;
extern fn_GetPedType_t               g_GetPedType;
extern fn_GetMatrix_t                g_GetMatrix;
extern fn_FindDistToNearestPedOfType_t g_FindDistToNearestCop;
extern fn_ScriptGenEmergencyCar_t    g_ScriptGenEmergencyCar;
extern fn_GenOneEmergencyCar_t       g_GenOneEmergencyCar;
extern fn_AddPoliceOccupants_t       g_AddPoliceOccupants;
extern fn_AddCriminalToKill_t        g_AddCriminalToKill;
extern fn_GiveWeapon_t               g_GiveWeapon;
extern fn_SetCurrentWeapon_t         g_SetCurrentWeapon;
extern fn_GiveWeaponAtStartOfFight_t g_GiveWeaponAtStartOfFight;
extern void* g_FireManager;
extern fn_FindNearestFire_t g_FindNearestFire;
extern fn_CEventGunShot_ctor_t g_CEventGunShot_ctor;
extern fn_CEventGunShot_dtor_t g_CEventGunShot_dtor;
extern fn_CEventGroup_Add_t    g_CEventGroup_Add;
extern fn_RegisterKill_t             g_RegisterKill;
extern fn_GetWeaponLockOnTarget_t    g_GetWeaponLockOnTarget;
extern fn_IsAlive_t                  g_IsAlive;
extern fn_VehicleInflictDamage_t     g_VehicleInflictDamage;
extern fn_GetPoolPed_t               g_GetPoolPed;
extern void*                         g_ms_pPedPool;
extern void**                        g_CSequenceManager_ms_instance;
extern void*                         g_ms_pVehiclePool;
extern void*                         g_TheCamera;
extern fn_GetGameCamPosition_t       g_GetGameCamPosition;
extern fn_GetLookDirection_t         g_GetLookDirection;
extern fn_IsDriver_t                g_IsDriver;
extern fn_IsPassenger_t             g_IsPassenger;
extern fn_TellOccupantsToLeaveCar_t g_TellOccupantsToLeaveCar;
extern fn_GetPoolVehicle_t          g_GetPoolVehicle;
extern fn_GetCarToGoToCoors_t        g_GetCarToGoToCoors;
extern fn_SwitchRoadsOffInArea_t     g_SwitchRoadsOffInArea;
extern void*                         g_ThePaths;
extern fn_TaskNew_t                  g_TaskNew;
extern fn_TaskKillCriminal_ctor_t    g_TaskKillCriminal_ctor;
extern fn_SetTask_t                  g_SetTask;
extern fn_TaskEnterCar_ctor_t        g_TaskEnterCar_ctor;
extern void*                         g_vtable_KillCriminal;
extern void*                         g_vtable_EnterCar;
extern void*                         g_vtable_CTask;
extern void*                         g_vtable_CTaskSimple;
extern void*                         g_vtable_CTaskComplex;
extern void*                         g_vtable_CEvent;
extern fn_AddTaskPrimaryMaybeInGroup_t g_AddTaskPrimaryMaybeInGroup;
extern fn_FindTaskByType_t g_FindTaskByType;
extern void* g_stub_report_crime;
extern fn_ReportCrime_orig_t g_orig_report_crime;
extern void* g_stub_register_kill;
extern void* g_stub_set_wanted;
extern void* g_stub_generate_damage_event;
extern fn_GenerateDamageEvent_orig_t g_orig_generate_damage_event;
extern void* g_stub_the_scripts_process;
extern fn_TheScriptsProcess_t g_orig_the_scripts_process;
extern void* g_stub_event_damage_ctor_c1;
extern void* g_stub_event_damage_ctor_c2;
extern fn_EventDamage_ctor_t g_orig_event_damage_ctor_c1;
extern fn_EventDamage_ctor_t g_orig_event_damage_ctor_c2;
extern void* g_stub_set_current_weapon;
extern fn_SetCurrentWeapon_t g_orig_SetCurrentWeapon;
extern std::atomic<int> g_player_wanted_level;
extern void* g_pure_virtual_target;
extern FMalloc** g_p_GMalloc;
extern std::atomic<bool> g_in_wanted_update;
extern std::atomic<bool> g_is_generating_custom_dispatch;

// --- 跨文件共享函数 ---
bool is_ped_pointer_valid_safe(void* target_ped);
bool is_task_vtable_safe(void* task);
bool is_vehicle_pointer_valid(void* target_veh);
bool is_entity_pointer_valid(void* entity);
CVector get_entity_pos(void* entity);
unsigned short get_entity_model_index(void* entity);
uint64_t now_ms();
CVector get_spawn_target(const CVector& player_pos);
void bind_vehicle_occupants(void* vehicle);
void record_exit_start_for_occupants(void* vehicle);
bool try_consolidate_crime(void* criminal, CVector location, bool is_firearm);
bool should_activate_or_hijack_crime(CVector location, bool is_firearm);
void update_primary_criminal_by_threat();
void* find_vehicle_of_cop(void* cop);
void make_cop_enter_vehicle(void* cop, void* vehicle, bool as_driver);
bool is_specific_criminal_armed_with_firearm(void* criminal);
eWeaponType determine_weapon_for_cop(void* cop, void* target, bool firearm_threat);

void hook_thread_func();
void init_ecs_systems();
void extract_nothing_so(const char* process_name);
void cleanup_nothing_so(const char* process_name);

extern "C" void* safe_pure_virtual_stub();
'''


def line_set(ranges: list[tuple[int, int]]) -> set[int]:
    s: set[int] = set()
    for a, b in ranges:
        s.update(range(a, b + 1))
    return s


def strip_static(line: str) -> str:
    if line.startswith("static "):
        return line[7:]
    if line.startswith("static inline "):
        return line[14:]
    return line


def main() -> None:
    lines = MODULE.read_text(encoding="utf-8").splitlines(keepends=True)
    extract_lines = line_set(sum(RANGES.values(), []))

    extracted: dict[str, list[str]] = {name: [] for name in RANGES}
    for name, ranges in RANGES.items():
        for a, b in ranges:
            extracted[name].extend(lines[a - 1 : b])

    remaining: list[str] = []
    for i, line in enumerate(lines, start=1):
        if i not in extract_lines:
            remaining.append(line)

    # module.cpp header + include mod_shared
    header_end = 51  # through ecs_engine include
    new_module: list[str] = []
    for i, line in enumerate(remaining, start=1):
        new_module.append(line)

    # Rewrite module.cpp includes section
    text = "".join(new_module)
    text = text.replace('#include "ecs_engine.hpp"\n', '#include "ecs_engine.hpp"\n#include "include/mod_shared.hpp"\n', 1)

    # Convert top-level static globals in module.cpp to non-static definitions
    def demote_static_globals(content: str) -> str:
        out = []
        for line in content.splitlines(keepends=True):
            if re.match(r"^static (fn_|void\*|void\*\*|std::|FMalloc|constexpr)", line):
                out.append(strip_static(line))
            else:
                out.append(line)
        return "".join(out)

    text = demote_static_globals(text)

    # Write extracted files
    (CPP_DIR / "include" / "mod_shared.hpp").write_text(SHARED_HEADER, encoding="utf-8")

    for fname, chunks in extracted.items():
        fixed = []
        for line in chunks:
            if re.match(r"^static ", line):
                fixed.append(strip_static(line))
            else:
                fixed.append(line)
        content = COMMON_INCLUDES + "\n" + "".join(fixed)
        (CPP_DIR / fname).write_text(content, encoding="utf-8")

    MODULE.write_text(text, encoding="utf-8")
    print("Split complete:")
    for fname in RANGES:
        print(f"  {fname}: {(CPP_DIR / fname).stat().st_size} bytes")
    print(f"  module.cpp: {MODULE.stat().st_size} bytes")


if __name__ == "__main__":
    main()