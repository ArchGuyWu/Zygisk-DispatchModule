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
#include <cinttypes>
#include <sys/mman.h>
#include <functional>

#include "shadowhook.h"
#include "third_party/xdl/xdl.h"

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
#include "manage_tasks_guard.hpp"
#include "vanilla_qol_fixes.hpp"

typedef void (*fn_GenericGameStorageLoad_t)(void* self, bool flag);
typedef void (*fn_GenericGameStorageLoadGame_t)(void* self);
typedef void (*fn_GenericGameStorageRestoreForStartLoad_t)();
typedef void (*fn_GenericGameStorageGenericLoad_t)(int slot, bool* out);
typedef void (*fn_GenericGameStorageAfterSuccessLoad_t)();
typedef void (*fn_CGameLogicInitAtStartOfGame_t)();
extern void proxy_generic_game_storage_load(void* self, bool flag);
extern void proxy_generic_game_storage_load_game(void* self);
extern void proxy_generic_game_storage_restore_for_start_load();
extern void proxy_generic_game_storage_generic_load(int slot, bool* out);
extern void proxy_generic_game_storage_after_success_load();
extern void proxy_cgame_logic_init_at_start_of_game();
extern void* g_stub_generic_game_storage_load;
extern void* g_stub_generic_game_storage_load_game;
extern void* g_stub_generic_game_storage_restore_for_start_load;
extern void* g_stub_generic_game_storage_generic_load;
extern void* g_stub_generic_game_storage_after_success_load;
extern void* g_stub_cgame_logic_init_at_start_of_game;
extern fn_GenericGameStorageLoad_t g_orig_generic_game_storage_load;
extern fn_GenericGameStorageLoadGame_t g_orig_generic_game_storage_load_game;
extern fn_GenericGameStorageRestoreForStartLoad_t g_orig_generic_game_storage_restore_for_start_load;
extern fn_GenericGameStorageGenericLoad_t g_orig_generic_game_storage_generic_load;
extern fn_GenericGameStorageAfterSuccessLoad_t g_orig_generic_game_storage_after_success_load;
extern fn_CGameLogicInitAtStartOfGame_t g_orig_cgame_logic_init_at_start_of_game;

extern void* g_stub_create_car_for_script;
extern fn_CreateCarForScript_t g_orig_create_car_for_script;
void* proxy_create_car_for_script(int modelid, CVector posn, unsigned char flag);
extern void* g_stub_add_criminal_to_kill;
extern fn_AddCriminalToKill_t g_orig_add_criminal_to_kill;
extern void proxy_add_criminal_to_kill(void* cop, CPed* criminal);
extern void* g_stub_fly_ai_heli_to_target;
extern fn_FlyAIHeliToTarget_FixedOrientation_t g_orig_fly_ai_heli_to_target;
extern void proxy_fly_ai_heli_to_target(void* pHeli, float orientation, CVector posn, bool bAvoidCollision);

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
    RESOLVE_SYM(lib, g_CreateCarForScript,
        "_ZN8CCarCtrl18CreateCarForScriptEi7CVectorh",
        fn_CreateCarForScript_t);
    RESOLVE_SYM(lib, g_FlyAIHeliToTarget_FixedOrientation,
        "_ZN8CCarCtrl34FlyAIHeliToTarget_FixedOrientationEP5CHelif7CVectorb",
        fn_FlyAIHeliToTarget_FixedOrientation_t);
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
        reinterpret_cast<void**>(&g_orig_register_kill));
    if (g_stub_register_kill) LOGI("✅ Hooked CEventHandler::RegisterKill");
    else LOGE("❌ Failed to hook RegisterKill: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CWanted::SetWantedLevel
    g_stub_set_wanted = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN7CWanted14SetWantedLevelEi",
        reinterpret_cast<void*>(proxy_set_wanted_level),
        reinterpret_cast<void**>(&g_orig_set_wanted));
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

    // Hook CCarCtrl::CreateCarForScript (block native script spawns during load/menu)
    g_stub_create_car_for_script = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN8CCarCtrl18CreateCarForScriptEi7CVectorh",
        reinterpret_cast<void*>(proxy_create_car_for_script),
        reinterpret_cast<void**>(&g_orig_create_car_for_script));
    if (g_stub_create_car_for_script) LOGI("✅ Hooked CCarCtrl::CreateCarForScript");
    else LOGE("❌ Failed to hook CCarCtrl::CreateCarForScript: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCopPed::AddCriminalToKill (block native hate inject during load)
    g_stub_add_criminal_to_kill = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN7CCopPed17AddCriminalToKillEP4CPed",
        reinterpret_cast<void*>(proxy_add_criminal_to_kill),
        reinterpret_cast<void**>(&g_orig_add_criminal_to_kill));
    if (g_stub_add_criminal_to_kill) LOGI("✅ Hooked CCopPed::AddCriminalToKill");
    else LOGE("❌ Failed to hook CCopPed::AddCriminalToKill: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarCtrl::FlyAIHeliToTarget_FixedOrientation (DE build adds trailing bool param)
    g_stub_fly_ai_heli_to_target = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN8CCarCtrl34FlyAIHeliToTarget_FixedOrientationEP5CHelif7CVectorb",
        reinterpret_cast<void*>(proxy_fly_ai_heli_to_target),
        reinterpret_cast<void**>(&g_orig_fly_ai_heli_to_target));
    if (g_stub_fly_ai_heli_to_target) LOGI("✅ Hooked CCarCtrl::FlyAIHeliToTarget_FixedOrientation");
    else LOGE("❌ Failed to hook FlyAIHeliToTarget_FixedOrientation: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // 4b. CTaskSimpleHoldEntity::SetPedPosition
    g_stub_set_ped_pos = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CTaskSimpleHoldEntity14SetPedPositionEP4CPed",
        reinterpret_cast<void*>(proxy_set_ped_pos),
        reinterpret_cast<void**>(&g_orig_set_ped_pos));
    if (g_stub_set_ped_pos) LOGI("✅ Hooked CTaskSimpleHoldEntity::SetPedPosition");
    else LOGE("❌ Failed to hook CTaskSimpleHoldEntity::SetPedPosition: %s", shadowhook_to_errmsg(shadowhook_get_errno()));



    // Scene transition globals (interior warp / yellow marker entry)
    RESOLVE_SYM(lib, g_entry_exit_ms_bWarping, "_ZN10CEntryExit11ms_bWarpingE", bool*);
    RESOLVE_SYM(lib, g_we_are_in_interior_transition,
                "_ZN17CEntryExitManager25WeAreInInteriorTransitionEv", fn_WeAreInInteriorTransition_t);
    RESOLVE_SYM(lib, g_generic_game_storage_ms_bLoading,
                "_ZN19CGenericGameStorage11ms_bLoadingE", bool*);
    RESOLVE_SYM(lib, g_generic_game_storage_ms_b_failed,
                "_ZN19CGenericGameStorage10ms_bFailedE", bool*);
    RESOLVE_SYM(lib, g_cgame_logic_game_state,
                "_ZN10CGameLogic9GameStateE", uint8_t*);
    RESOLVE_SYM(lib, g_streaming_ms_num_models_requested,
                "_ZN10CStreaming21ms_numModelsRequestedE", uint32_t*);
    RESOLVE_SYM(lib, g_cgame_logic_skip_state,
                "_ZN10CGameLogic9SkipStateE", uint32_t*);
    RESOLVE_SYM(lib, g_cgame_logic_skip_timer,
                "_ZN10CGameLogic9SkipTimerE", uint32_t*);
    RESOLVE_SYM(lib, g_is_skip_waiting_for_script_to_fade_in,
                "_ZN10CGameLogic30IsSkipWaitingForScriptToFadeInEv",
                fn_IsSkipWaitingForScriptToFadeIn_t);

    void* load_sym = xdl_sym(lib, "_ZN19CGenericGameStorage4LoadEb", nullptr);
    if (load_sym) {
        g_stub_generic_game_storage_load = shadowhook_hook_sym_addr(
            load_sym,
            reinterpret_cast<void*>(proxy_generic_game_storage_load),
            reinterpret_cast<void**>(&g_orig_generic_game_storage_load));
        if (g_stub_generic_game_storage_load) {
            LOGI("✅ Hooked CGenericGameStorage::Load");
        }
    }
    void* load_game_sym = xdl_sym(lib, "_ZN19CGenericGameStorage8LoadGameEv", nullptr);
    if (load_game_sym) {
        g_stub_generic_game_storage_load_game = shadowhook_hook_sym_addr(
            load_game_sym,
            reinterpret_cast<void*>(proxy_generic_game_storage_load_game),
            reinterpret_cast<void**>(&g_orig_generic_game_storage_load_game));
        if (g_stub_generic_game_storage_load_game) {
            LOGI("✅ Hooked CGenericGameStorage::LoadGame");
        }
    }

    #define HOOK_SAVE_LOAD_SYM(mangled, proxy_fn, stub_var, orig_var, label) do { \
        void* sym = xdl_sym(lib, (mangled), nullptr); \
        if (!sym) { \
            LOGW("⚠️ %s symbol not found (%s)", (label), (mangled)); \
            break; \
        } \
        (stub_var) = shadowhook_hook_sym_addr(sym, reinterpret_cast<void*>(proxy_fn), \
                                              reinterpret_cast<void**>(&(orig_var))); \
        if (stub_var) LOGI("✅ Hooked %s", (label)); \
        else LOGE("❌ Failed to hook %s: %s", (label), shadowhook_to_errmsg(shadowhook_get_errno())); \
    } while (0)

    HOOK_SAVE_LOAD_SYM("_ZN19CGenericGameStorage19RestoreForStartLoadEv",
                       proxy_generic_game_storage_restore_for_start_load,
                       g_stub_generic_game_storage_restore_for_start_load,
                       g_orig_generic_game_storage_restore_for_start_load,
                       "CGenericGameStorage::RestoreForStartLoad");
    HOOK_SAVE_LOAD_SYM("_ZN19CGenericGameStorage11GenericLoadEiRb",
                       proxy_generic_game_storage_generic_load,
                       g_stub_generic_game_storage_generic_load,
                       g_orig_generic_game_storage_generic_load,
                       "CGenericGameStorage::GenericLoad");
    HOOK_SAVE_LOAD_SYM("_ZN19CGenericGameStorage34DoGameSpecificStuffAfterSucessLoadEv",
                       proxy_generic_game_storage_after_success_load,
                       g_stub_generic_game_storage_after_success_load,
                       g_orig_generic_game_storage_after_success_load,
                       "CGenericGameStorage::DoGameSpecificStuffAfterSucessLoad");
    HOOK_SAVE_LOAD_SYM("_ZN10CGameLogic17InitAtStartOfGameEv",
                       proxy_cgame_logic_init_at_start_of_game,
                       g_stub_cgame_logic_init_at_start_of_game,
                       g_orig_cgame_logic_init_at_start_of_game,
                       "CGameLogic::InitAtStartOfGame");
    #undef HOOK_SAVE_LOAD_SYM

    void* manage_tasks_sym = xdl_sym(lib, "_ZN12CTaskManager11ManageTasksEv", nullptr);
    if (manage_tasks_sym) {
        if (install_manage_tasks_inbody_guards(manage_tasks_sym)) {
            LOGI("✅ Patched ManageTasks in-body null guards @ +0x168/+0x278");
        } else {
            LOGE("❌ Failed to patch ManageTasks in-body null guards");
        }
    } else {
        LOGE("❌ ManageTasks symbol not found for in-body guard patch");
    }

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

    g_stub_intel_find_task_by_type = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK16CPedIntelligence14FindTaskByTypeEi",
        reinterpret_cast<void*>(proxy_intel_find_task_by_type),
        reinterpret_cast<void**>(&g_orig_intel_find_task_by_type));
    if (g_stub_intel_find_task_by_type) LOGI("✅ Hooked CPedIntelligence::FindTaskByType");
    else LOGE("❌ Failed to hook CPedIntelligence::FindTaskByType: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_wander_cop_look_for_criminals = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CTaskComplexWanderCop16LookForCriminalsEP4CPed",
        reinterpret_cast<void*>(proxy_wander_cop_look_for_criminals),
        reinterpret_cast<void**>(&g_orig_wander_cop_look_for_criminals));
    if (g_stub_wander_cop_look_for_criminals) LOGI("✅ Hooked CTaskComplexWanderCop::LookForCriminals");
    else LOGE("❌ Failed to hook CTaskComplexWanderCop::LookForCriminals: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_kill_criminal_create_first_sub_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexKillCriminal18CreateFirstSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_kill_criminal_create_first_sub_task),
        reinterpret_cast<void**>(&g_orig_kill_criminal_create_first_sub_task));
    if (g_stub_kill_criminal_create_first_sub_task) LOGI("✅ Hooked CTaskComplexKillCriminal::CreateFirstSubTask");
    else LOGE("❌ Failed to hook CTaskComplexKillCriminal::CreateFirstSubTask: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));


    // Hook CTaskManager Destructor D1+D2 (tombstone_41 crashes in D2Ev, not D1Ev alone).
    // DE build aliases D1Ev/D2Ev to the same address — hook once to avoid duplicate-hook error.
    void* task_mgr_d1_sym = xdl_sym(lib, "_ZN12CTaskManagerD1Ev", nullptr);
    void* task_mgr_d2_sym = xdl_sym(lib, "_ZN12CTaskManagerD2Ev", nullptr);

    g_stub_task_manager_destructor = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN12CTaskManagerD1Ev",
        reinterpret_cast<void*>(proxy_task_manager_destructor),
        reinterpret_cast<void**>(&g_orig_task_manager_destructor));
    if (g_stub_task_manager_destructor) LOGI("✅ Hooked CTaskManager::~CTaskManager (D1)");
    else LOGE("❌ Failed to hook CTaskManager D1: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    if (task_mgr_d1_sym && task_mgr_d2_sym && task_mgr_d1_sym == task_mgr_d2_sym) {
        g_stub_task_manager_destructor_d2 = g_stub_task_manager_destructor;
        g_orig_task_manager_destructor_d2 = g_orig_task_manager_destructor;
        LOGI("ℹ️ CTaskManager D2Ev aliases D1Ev @ %p — single hook covers both", task_mgr_d1_sym);
    } else {
        g_stub_task_manager_destructor_d2 = shadowhook_hook_sym_name(
            TARGET_LIB,
            "_ZN12CTaskManagerD2Ev",
            reinterpret_cast<void*>(proxy_task_manager_destructor),
            reinterpret_cast<void**>(&g_orig_task_manager_destructor_d2));
        if (g_stub_task_manager_destructor_d2) LOGI("✅ Hooked CTaskManager::~CTaskManager (D2)");
        else LOGE("❌ Failed to hook CTaskManager D2: %s",
                  shadowhook_to_errmsg(shadowhook_get_errno()));
    }

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

    // Hook CPlayerPed::ProcessControl (读档水合期 ped+0x44 空指针闪退, tombstone_31)
    g_stub_player_ped_process_control = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN10CPlayerPed14ProcessControlEv",
        reinterpret_cast<void*>(proxy_player_ped_process_control),
        reinterpret_cast<void**>(&g_orig_player_ped_process_control));
    if (g_stub_player_ped_process_control) LOGI("✅ Hooked CPlayerPed::ProcessControl");
    else LOGE("❌ Failed to hook CPlayerPed::ProcessControl: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CPlayerInfo::Process (GenericLoad 期 null this / 半水合 intel 解引用, tombstone_23/24)
    g_stub_player_info_process = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN11CPlayerInfo7ProcessEi",
        reinterpret_cast<void*>(proxy_player_info_process),
        reinterpret_cast<void**>(&g_orig_player_info_process));
    if (g_stub_player_info_process) LOGI("✅ Hooked CPlayerInfo::Process");
    else LOGE("❌ Failed to hook CPlayerInfo::Process: %s",
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







    // Hook u_strlen_64 / u_strlen (防止 ICU 字符串长度计算传入野指针崩溃)
    g_stub_u_strlen = shadowhook_hook_sym_name(
        TARGET_LIB,
        "u_strlen_64",
        reinterpret_cast<void*>(proxy_u_strlen),
        reinterpret_cast<void**>(&g_orig_u_strlen));
    if (g_stub_u_strlen) LOGI("✅ Hooked u_strlen_64");
    else LOGE("❌ Failed to hook u_strlen_64: %s", shadowhook_to_errmsg(shadowhook_get_errno()));
    if (shadowhook_hook_sym_name(
            TARGET_LIB,
            "u_strlen",
            reinterpret_cast<void*>(proxy_u_strlen),
            nullptr)) {
        LOGI("✅ Hooked u_strlen");
    }

    // Hook CTaskComplexSequence::Flush (防止删除/清空序列时因序列中残留零填充任务导致闪退)
    g_stub_sequence_flush = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN20CTaskComplexSequence5FlushEv",
        reinterpret_cast<void*>(proxy_sequence_flush),
        reinterpret_cast<void**>(&g_orig_sequence_flush));
    if (g_stub_sequence_flush) LOGI("✅ Hooked CTaskComplexSequence::Flush");
    else LOGE("❌ Failed to hook CTaskComplexSequence::Flush: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskComplexSequence::CreateNextSubTask (序列推进时对零填充任务解引用 vtable+0x10 闪退)
    g_stub_sequence_create_next_sub_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN20CTaskComplexSequence17CreateNextSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_sequence_create_next_sub_task),
        reinterpret_cast<void**>(&g_orig_sequence_create_next_sub_task));
    if (g_stub_sequence_create_next_sub_task) LOGI("✅ Hooked CTaskComplexSequence::CreateNextSubTask");
    else LOGE("❌ Failed to hook CTaskComplexSequence::CreateNextSubTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

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

    g_stub_be_in_group_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CTaskComplexBeInGroup13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_be_in_group_make_abortable),
        reinterpret_cast<void**>(&g_orig_be_in_group_make_abortable));
    if (g_stub_be_in_group_make_abortable) LOGI("✅ Hooked CTaskComplexBeInGroup::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexBeInGroup::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CPedGroupIntelligence::GetTaskMain (BeInGroup 在组任务虚表为 null 时解引用 vtable+0x28 闪退)
    g_stub_get_task_main = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK21CPedGroupIntelligence11GetTaskMainEP4CPed",
        reinterpret_cast<void*>(proxy_get_task_main),
        reinterpret_cast<void**>(&g_orig_get_task_main));
    if (g_stub_get_task_main) LOGI("✅ Hooked CPedGroupIntelligence::GetTaskMain");
    else LOGE("❌ Failed to hook CPedGroupIntelligence::GetTaskMain: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CPedGroupIntelligence::FlushTasks (刷新组任务时对零填充任务析构闪退)
    g_stub_flush_tasks = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CPedGroupIntelligence10FlushTasksEP12CPedTaskPairP4CPed",
        reinterpret_cast<void*>(proxy_flush_tasks),
        reinterpret_cast<void**>(&g_orig_flush_tasks));
    if (g_stub_flush_tasks) LOGI("✅ Hooked CPedGroupIntelligence::FlushTasks");
    else LOGE("❌ Failed to hook CPedGroupIntelligence::FlushTasks: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook IKChainManager_c::Update
    g_stub_ik_chain_update = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN16IKChainManager_c6UpdateEf",
        reinterpret_cast<void*>(proxy_ik_chain_update),
        reinterpret_cast<void**>(&g_orig_ik_chain_update));
    if (g_stub_ik_chain_update) LOGI("✅ Hooked IKChainManager_c::Update");
    else LOGE("❌ Failed to hook IKChainManager_c::Update: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook IKChainManager_c::IsFacingTarget (ped intel chain +0x18 null → fault 0x20)
    g_stub_ik_chain_is_facing_target = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN16IKChainManager_c14IsFacingTargetEP4CPedi",
        reinterpret_cast<void*>(proxy_ik_chain_is_facing_target),
        reinterpret_cast<void**>(&g_orig_ik_chain_is_facing_target));
    if (g_stub_ik_chain_is_facing_target) LOGI("✅ Hooked IKChainManager_c::IsFacingTarget");
    else LOGE("❌ Failed to hook IKChainManager_c::IsFacingTarget: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskManager::GetSimplestActiveTask (zero-filled primary task → vtable+0x18 crash)
    g_stub_get_simplest_active_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK12CTaskManager21GetSimplestActiveTaskEv",
        reinterpret_cast<void*>(proxy_get_simplest_active_task),
        reinterpret_cast<void**>(&g_orig_get_simplest_active_task));
    if (g_stub_get_simplest_active_task) LOGI("✅ Hooked CTaskManager::GetSimplestActiveTask");
    else LOGE("❌ Failed to hook CTaskManager::GetSimplestActiveTask: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskManager::GetSimplestTaskEi (indexed slot → vtable+0x18, tombstone_41 pattern)
    g_stub_get_simplest_task_ei = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK12CTaskManager15GetSimplestTaskEi",
        reinterpret_cast<void*>(proxy_get_simplest_task_ei),
        reinterpret_cast<void**>(&g_orig_get_simplest_task_ei));
    if (g_stub_get_simplest_task_ei) LOGI("✅ Hooked CTaskManager::GetSimplestTaskEi");
    else LOGE("❌ Failed to hook CTaskManager::GetSimplestTaskEi: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CEventScriptCommand::GetEventPriority (事件优先级查询时对零填充任务调 vtable+0x28 闪退)
    g_stub_event_script_command_get_priority = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK19CEventScriptCommand16GetEventPriorityEv",
        reinterpret_cast<void*>(proxy_event_script_command_get_priority),
        reinterpret_cast<void**>(&g_orig_event_script_command_get_priority));
    if (g_stub_event_script_command_get_priority) LOGI("✅ Hooked CEventScriptCommand::GetEventPriority");
    else LOGE("❌ Failed to hook CEventScriptCommand::GetEventPriority: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CEventScriptCommand D0/D1 (析构 +0x18 task → vtable+0x8, tombstone_43)
    g_stub_event_script_command_d0 = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN19CEventScriptCommandD0Ev",
        reinterpret_cast<void*>(proxy_event_script_command_d0),
        reinterpret_cast<void**>(&g_orig_event_script_command_d0));
    if (g_stub_event_script_command_d0) LOGI("✅ Hooked CEventScriptCommand::D0");
    else LOGE("❌ Failed to hook CEventScriptCommand::D0: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_event_script_command_d1 = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN19CEventScriptCommandD1Ev",
        reinterpret_cast<void*>(proxy_event_script_command_d1),
        reinterpret_cast<void**>(&g_orig_event_script_command_d1));
    if (g_stub_event_script_command_d1) LOGI("✅ Hooked CEventScriptCommand::D1");
    else LOGE("❌ Failed to hook CEventScriptCommand::D1: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CEntryExit transitions (interior warp — gate mod dispatch, sanitize player tasks)
    g_stub_entry_exit_transition_started = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN10CEntryExit17TransitionStartedEP4CPed",
        reinterpret_cast<void*>(proxy_entry_exit_transition_started),
        reinterpret_cast<void**>(&g_orig_entry_exit_transition_started));
    if (g_stub_entry_exit_transition_started) LOGI("✅ Hooked CEntryExit::TransitionStarted");
    else LOGE("❌ Failed to hook CEntryExit::TransitionStarted: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_entry_exit_transition_finished = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN10CEntryExit18TransitionFinishedEP4CPed",
        reinterpret_cast<void*>(proxy_entry_exit_transition_finished),
        reinterpret_cast<void**>(&g_orig_entry_exit_transition_finished));
    if (g_stub_entry_exit_transition_finished) LOGI("✅ Hooked CEntryExit::TransitionFinished");
    else LOGE("❌ Failed to hook CEntryExit::TransitionFinished: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCam::Process_FollowPed_SA (post-deserialize stale/null CCam, tombstone_27/28)
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

    // Hook CTaskSimpleGoToPoint::MakeAbortable (CEventHandler path, ped intel+0x58 chain; tombstone_25)
    g_stub_goto_point_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN20CTaskSimpleGoToPoint13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_goto_point_make_abortable),
        reinterpret_cast<void**>(&g_orig_goto_point_make_abortable));
    if (g_stub_goto_point_make_abortable) LOGI("✅ Hooked CTaskSimpleGoToPoint::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskSimpleGoToPoint::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskSimpleAchieveHeading::MakeAbortable (stale ped +0x788 write; tombstone_26/27)
    g_stub_achieve_heading_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN25CTaskSimpleAchieveHeading13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_achieve_heading_make_abortable),
        reinterpret_cast<void**>(&g_orig_achieve_heading_make_abortable));
    if (g_stub_achieve_heading_make_abortable) LOGI("✅ Hooked CTaskSimpleAchieveHeading::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskSimpleAchieveHeading::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_follow_point_route_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN28CTaskComplexFollowPointRoute13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_follow_point_route_make_abortable),
        reinterpret_cast<void**>(&g_orig_follow_point_route_make_abortable));
    if (g_stub_follow_point_route_make_abortable) LOGI("✅ Hooked CTaskComplexFollowPointRoute::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexFollowPointRoute::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_kill_ped_on_foot_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN25CTaskComplexKillPedOnFoot13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_kill_ped_on_foot_make_abortable),
        reinterpret_cast<void**>(&g_orig_kill_ped_on_foot_make_abortable));
    if (g_stub_kill_ped_on_foot_make_abortable) LOGI("✅ Hooked CTaskComplexKillPedOnFoot::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexKillPedOnFoot::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_kill_ped_on_foot_armed_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN30CTaskComplexKillPedOnFootArmed13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_kill_ped_on_foot_armed_make_abortable),
        reinterpret_cast<void**>(&g_orig_kill_ped_on_foot_armed_make_abortable));
    if (g_stub_kill_ped_on_foot_armed_make_abortable) LOGI("✅ Hooked CTaskComplexKillPedOnFootArmed::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexKillPedOnFootArmed::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_event_walk_into_vehicle_affects_ped = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZNK30CEventPotentialWalkIntoVehicle10AffectsPedEP4CPed",
        reinterpret_cast<void*>(proxy_event_walk_into_vehicle_affects_ped),
        reinterpret_cast<void**>(&g_orig_event_walk_into_vehicle_affects_ped));
    if (g_stub_event_walk_into_vehicle_affects_ped) LOGI("✅ Hooked CEventPotentialWalkIntoVehicle::AffectsPed");
    else LOGE("❌ Failed to hook CEventPotentialWalkIntoVehicle::AffectsPed: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_kill_criminal_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexKillCriminal13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_kill_criminal_make_abortable),
        reinterpret_cast<void**>(&g_orig_kill_criminal_make_abortable));
    if (g_stub_kill_criminal_make_abortable) LOGI("✅ Hooked CTaskComplexKillCriminal::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexKillCriminal::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_fall_and_get_up_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN24CTaskComplexFallAndGetUp13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_fall_and_get_up_make_abortable),
        reinterpret_cast<void**>(&g_orig_fall_and_get_up_make_abortable));
    if (g_stub_fall_and_get_up_make_abortable) LOGI("✅ Hooked CTaskComplexFallAndGetUp::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexFallAndGetUp::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_play_hand_signal_control_sub_task = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN30CTaskComplexPlayHandSignalAnim14ControlSubTaskEP4CPed",
        reinterpret_cast<void*>(proxy_play_hand_signal_control_sub_task),
        reinterpret_cast<void**>(&g_orig_play_hand_signal_control_sub_task));
    if (g_stub_play_hand_signal_control_sub_task) LOGI("✅ Hooked CTaskComplexPlayHandSignalAnim::ControlSubTask");
    else LOGE("❌ Failed to hook CTaskComplexPlayHandSignalAnim::ControlSubTask: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_simple_anim_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN15CTaskSimpleAnim13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_simple_anim_make_abortable),
        reinterpret_cast<void**>(&g_orig_simple_anim_make_abortable));
    if (g_stub_simple_anim_make_abortable) LOGI("✅ Hooked CTaskSimpleAnim::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskSimpleAnim::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_simple_arrest_ped_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN20CTaskSimpleArrestPed13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_simple_arrest_ped_make_abortable),
        reinterpret_cast<void**>(&g_orig_simple_arrest_ped_make_abortable));
    if (g_stub_simple_arrest_ped_make_abortable) LOGI("✅ Hooked CTaskSimpleArrestPed::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskSimpleArrestPed::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_complex_arrest_ped_make_abortable = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN21CTaskComplexArrestPed13MakeAbortableEP4CPediPK6CEvent",
        reinterpret_cast<void*>(proxy_complex_arrest_ped_make_abortable),
        reinterpret_cast<void**>(&g_orig_complex_arrest_ped_make_abortable));
    if (g_stub_complex_arrest_ped_make_abortable) LOGI("✅ Hooked CTaskComplexArrestPed::MakeAbortable");
    else LOGE("❌ Failed to hook CTaskComplexArrestPed::MakeAbortable: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_process_after_proc_col = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN16CPedIntelligence19ProcessAfterProcColEv",
        reinterpret_cast<void*>(proxy_process_after_proc_col),
        reinterpret_cast<void**>(&g_orig_process_after_proc_col));
    if (g_stub_process_after_proc_col) LOGI("✅ Hooked CPedIntelligence::ProcessAfterProcCol");
    else LOGE("❌ Failed to hook CPedIntelligence::ProcessAfterProcCol: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_scan_for_events = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN13CEventScanner13ScanForEventsER4CPed",
        reinterpret_cast<void*>(proxy_scan_for_events),
        reinterpret_cast<void**>(&g_orig_scan_for_events));
    if (g_stub_scan_for_events) LOGI("✅ Hooked CEventScanner::ScanForEvents");
    else LOGE("❌ Failed to hook CEventScanner::ScanForEvents: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_scan_for_collision_events = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN22CCollisionEventScanner22ScanForCollisionEventsER4CPedP11CEventGroup",
        reinterpret_cast<void*>(proxy_scan_for_collision_events),
        reinterpret_cast<void**>(&g_orig_scan_for_collision_events));
    if (g_stub_scan_for_collision_events) LOGI("✅ Hooked CCollisionEventScanner::ScanForCollisionEvents");
    else LOGE("❌ Failed to hook CCollisionEventScanner::ScanForCollisionEvents: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    g_stub_compute_ped_collision_with_ped_response = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN13CEventHandler34ComputePedCollisionWithPedResponseEP6CEventP5CTaskS3_",
        reinterpret_cast<void*>(proxy_compute_ped_collision_with_ped_response),
        reinterpret_cast<void**>(&g_orig_compute_ped_collision_with_ped_response));
    if (g_stub_compute_ped_collision_with_ped_response) {
        LOGI("✅ Hooked CEventHandler::ComputePedCollisionWithPedResponse");
    } else {
        LOGE("❌ Failed to hook CEventHandler::ComputePedCollisionWithPedResponse: %s",
             shadowhook_to_errmsg(shadowhook_get_errno()));
    }

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

    // Hook CCarEnterExit::GetNearestCarDoor (上车门检测遍历任务槽时零填充任务触发纯虚函数)
    g_stub_get_nearest_car_door = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN13CCarEnterExit17GetNearestCarDoorERK4CPedRK8CVehicleR7CVectorRi",
        reinterpret_cast<void*>(proxy_get_nearest_car_door),
        reinterpret_cast<void**>(&g_orig_get_nearest_car_door));
    if (g_stub_get_nearest_car_door) LOGI("✅ Hooked CCarEnterExit::GetNearestCarDoor");
    else LOGE("❌ Failed to hook CCarEnterExit::GetNearestCarDoor: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CTaskSimpleIKManager::ProcessPed (IK 子任务零填充虚表 → fault 0x48)
    g_stub_ik_manager_process_ped = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN20CTaskSimpleIKManager10ProcessPedEP4CPed",
        reinterpret_cast<void*>(proxy_ik_manager_process_ped),
        reinterpret_cast<void**>(&g_orig_ik_manager_process_ped));
    if (g_stub_ik_manager_process_ped) LOGI("✅ Hooked CTaskSimpleIKManager::ProcessPed");
    else LOGE("❌ Failed to hook CTaskSimpleIKManager::ProcessPed: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    // Hook CCarCtrl::IsPoliceVehicleInPursuit (null pool slot @ +0x29c, thunk @ +0x2b8; tombstone_29/30)
    g_stub_is_police_vehicle_in_pursuit = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN8CCarCtrl24IsPoliceVehicleInPursuitEi",
        reinterpret_cast<void*>(proxy_is_police_vehicle_in_pursuit),
        reinterpret_cast<void**>(&g_orig_is_police_vehicle_in_pursuit));
    if (g_stub_is_police_vehicle_in_pursuit) LOGI("✅ Hooked CCarCtrl::IsPoliceVehicleInPursuit");
    else LOGE("❌ Failed to hook CCarCtrl::IsPoliceVehicleInPursuit: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

    void* pursuit_sym = xdl_sym(lib, "_ZN8CCarCtrl24IsPoliceVehicleInPursuitEi", nullptr);
    if (pursuit_sym) {
        void* ldr_thunk_addr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(pursuit_sym) + 0x29c);
        g_stub_vehicle_pursuit_ldr_thunk_29c = shadowhook_hook_func_addr(
            ldr_thunk_addr,
            reinterpret_cast<void*>(proxy_vehicle_pursuit_ldr_thunk_29c),
            reinterpret_cast<void**>(&g_orig_vehicle_pursuit_ldr_thunk_29c));
        if (g_stub_vehicle_pursuit_ldr_thunk_29c) {
            LOGI("✅ Hooked IsPoliceVehicleInPursuit ldr thunk @ +0x29c (%p)",
                 ldr_thunk_addr);
        } else {
            LOGE("❌ Failed to hook IsPoliceVehicleInPursuit +0x29c thunk: %s",
                 shadowhook_to_errmsg(shadowhook_get_errno()));
        }

        void* thunk_addr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(pursuit_sym) + 0x2b8);
        g_stub_vehicle_pursuit_ai_thunk = shadowhook_hook_func_addr(
            thunk_addr,
            reinterpret_cast<void*>(proxy_vehicle_pursuit_ai_thunk),
            reinterpret_cast<void**>(&g_orig_vehicle_pursuit_ai_thunk));
        if (g_stub_vehicle_pursuit_ai_thunk) {
            LOGI("✅ Hooked IsPoliceVehicleInPursuit vehicle-ai thunk @ +0x2b8 (%p)",
                 thunk_addr);
        } else {
            LOGE("❌ Failed to hook IsPoliceVehicleInPursuit +0x2b8 thunk: %s",
                 shadowhook_to_errmsg(shadowhook_get_errno()));
        }
    }

    // Hook CTaskComplexWanderStandard::LookForChatPartners (伙伴 intel 任务槽零填充 → fault 0x28)
    g_stub_wander_look_for_chat_partners = shadowhook_hook_sym_name(
        TARGET_LIB,
        "_ZN26CTaskComplexWanderStandard19LookForChatPartnersEP4CPed",
        reinterpret_cast<void*>(proxy_wander_look_for_chat_partners),
        reinterpret_cast<void**>(&g_orig_wander_look_for_chat_partners));
    if (g_stub_wander_look_for_chat_partners) LOGI("✅ Hooked CTaskComplexWanderStandard::LookForChatPartners");
    else LOGE("❌ Failed to hook CTaskComplexWanderStandard::LookForChatPartners: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));

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

    install_vanilla_qol_fixes(lib);

    init_ecs_systems();

    LOGI("=== All hooks installed ===");
    LOGI("=== Police Intervention System ACTIVE ===");
    // Termux logcat often drops INFO from game UID — duplicate as WARN for capture script.
    LOGW("✅ [Mod] All hooks installed — safe to Load Game / Continue");
}
