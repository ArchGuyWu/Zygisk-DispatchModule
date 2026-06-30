#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "third_party/xdl/xdl.h"
#include "log.hpp"
#include "game_types.hpp"
#include "dispatch_types.hpp"
#include "hooks_stability_types.hpp"

// --- 符号解析宏 ---
#define RESOLVE_SYM(handle, var, mangled, type) do { \
    var = reinterpret_cast<type>(xdl_sym(handle, mangled, nullptr)); \
    if (var) LOGI("  ✅ " #var " -> %p", (void*)var); \
    else     LOGW("  ⚠️ " #var " not found (%s)", mangled); \
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

inline constexpr int TASK_COMPLEX_KILL_CRIMINAL = 1105;

// --- 运行时符号指针 ---
extern fn_FindPlayerPed_t            g_FindPlayerPed;
extern fn_FindPlayerCoors_t          g_FindPlayerCoors;
extern fn_GetPedType_t               g_GetPedType;
extern fn_GetMatrix_t                g_GetMatrix;
extern fn_FindDistToNearestPedOfType_t g_FindDistToNearestCop;
extern fn_ScriptGenEmergencyCar_t    g_ScriptGenEmergencyCar;
extern fn_GenOneEmergencyCar_t       g_GenOneEmergencyCar;
extern fn_CreateCarForScript_t       g_CreateCarForScript;
extern fn_FlyAIHeliToTarget_FixedOrientation_t g_FlyAIHeliToTarget_FixedOrientation;
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
extern fn_RegisterKill_t             g_orig_register_kill;
extern fn_SetWantedLevel_orig_t      g_orig_set_wanted;
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

typedef void (*fn_WantedUpdate_t)(void*);
typedef CPed* (*fn_AddPed_t)(int, unsigned int, const CVector&, bool);

extern void* g_stub_wanted_update;
extern fn_WantedUpdate_t g_orig_wanted_update;
extern void* g_stub_add_ped;
extern fn_AddPed_t g_orig_add_ped;
extern void* g_stub_generate_one_emergency_car;
extern fn_GenOneEmergencyCar_t g_orig_generate_one_emergency_car;
extern void* g_stub_script_generate_one_emergency_car;
extern fn_ScriptGenEmergencyCar_t g_orig_script_generate_one_emergency_car;
extern void* g_stub_tell_occupants_leave_car;
extern fn_TellOccupantsToLeaveCar_t g_orig_tell_occupants_leave_car;

// Gameplay hook proxies (defined in dispatch_logic.cpp)
void proxy_report_crime(eCrimeType crime_type, CEntity* victim, CPed* perpetrator);
void proxy_register_kill(const CPed* dead_ped, const CEntity* killer, eWeaponType weapon, bool unk);
void proxy_set_wanted_level(void* this_wanted, int level);
bool proxy_generate_damage_event(CPed* victim, CEntity* perpetrator, eWeaponType weaponType,
                                   int damage, int pedPiece, int direction);
void proxy_event_damage_ctor_c1(void* event_this, CEntity* damageSource, unsigned int startTime,
                                eWeaponType weaponType, int pieceType, unsigned char damageSeverity,
                                bool b1, bool b2);
void proxy_event_damage_ctor_c2(void* event_this, CEntity* damageSource, unsigned int startTime,
                                eWeaponType weaponType, int pieceType, unsigned char damageSeverity,
                                bool b1, bool b2);
void proxy_SetCurrentWeapon(CPed* ped, eWeaponType weaponType);
void proxy_the_scripts_process();
void proxy_wanted_update(void* this_wanted);
CPed* proxy_add_ped(int pedType, unsigned int modelIndex, const CVector& pos, bool bUnknown);
void proxy_generate_one_emergency_car(unsigned int model, CVector pos);
void proxy_script_generate_one_emergency_car(unsigned int model, CVector pos);
void proxy_tell_occupants_leave_car(void* vehicle);

// Stability hook stubs (defined in hooks_stability.cpp)
extern void* g_stub_set_ped_pos;
extern void* g_stub_manage_tasks;
extern void* g_stub_scan_for_attractors_in_range;
extern void* g_stub_ccgf_control;
extern void* g_stub_paired_attractor_create_next_sub_task;
extern void* g_stub_facial_dtor;
extern void* g_stub_find_active_task;
extern void* g_stub_task_manager_destructor;
extern void* g_stub_partner_greet_get_sequence;
extern void* g_stub_play_loaded_sound;
extern void* g_stub_check_if_within_range;
extern void* g_stub_avoid_ped_control;
extern void* g_stub_add_police_occupants;
extern void* g_stub_play_footsteps;
extern void* g_stub_process_buoyancy;
extern void* g_stub_process_static_counter;
extern void* g_stub_cbuoyancy_process_buoyancy;
extern void* g_stub_u_strlen;
extern void* g_stub_sequence_flush;
extern void* g_stub_finish_anim_evasive_step_cb;
extern void* g_stub_be_in_group_control_sub_task;
extern void* g_stub_be_in_group_make_abortable;
extern void* g_stub_ik_chain_update;
extern void* g_stub_process_follow_ped_sa;
extern void* g_stub_leave_car_make_abortable;
extern void* g_stub_update_car_ai;
extern void* g_stub_facial_control_sub_task;
extern void* g_stub_get_task_main;
extern void* g_stub_get_nearest_car_door;
extern void* g_stub_sequence_create_next_sub_task;
extern void* g_stub_event_script_command_get_priority;
extern void* g_stub_event_script_command_d0;
extern void* g_stub_event_script_command_d1;
extern void* g_stub_get_simplest_task_ei;
extern void* g_stub_flush_tasks;
extern void* g_stub_ik_manager_process_ped;
extern void* g_stub_is_police_vehicle_in_pursuit;
extern void* g_stub_vehicle_pursuit_ai_thunk;
extern void* g_stub_wander_look_for_chat_partners;
extern void* g_stub_ik_chain_is_facing_target;
extern void* g_stub_get_simplest_active_task;
extern void* g_stub_goto_point_make_abortable;
extern void* g_stub_achieve_heading_make_abortable;
extern void* g_stub_follow_point_route_make_abortable;
extern void* g_stub_kill_ped_on_foot_make_abortable;
extern void* g_stub_kill_ped_on_foot_armed_make_abortable;
extern void* g_stub_event_walk_into_vehicle_affects_ped;
extern void* g_stub_kill_criminal_make_abortable;
extern void* g_stub_fall_and_get_up_make_abortable;
extern void* g_stub_play_hand_signal_control_sub_task;
extern void* g_stub_simple_anim_make_abortable;
extern void* g_stub_simple_arrest_ped_make_abortable;
extern void* g_stub_complex_arrest_ped_make_abortable;
extern void* g_stub_process_after_proc_col;
extern void* g_stub_scan_for_collision_events;
extern void* g_stub_compute_ped_collision_with_ped_response;

// Dispatch state (defined in dispatch_logic.cpp)
extern std::recursive_mutex g_crime_mutex;
extern std::vector<std::shared_ptr<CrimeEvent>> g_active_crimes;
extern uint64_t g_next_case_id;
extern CrimeActiveCompat g_crime_active;
extern std::shared_ptr<CrimeEvent> g_dummy_crime;
extern std::atomic<CPed*> g_tracked_criminal;
extern std::atomic<int64_t> g_last_assist_time_ms;
extern std::atomic<bool> g_player_stray_bullet_flag;
extern std::atomic<int64_t> g_player_stray_bullet_time;
extern std::atomic<int> g_friendly_fire_cop_hits;
extern std::atomic<int64_t> g_last_friendly_fire_cop_time;
extern std::atomic<bool> g_player_friendly_fire_blocked;
extern std::vector<AttackedNPC> g_player_attacked_npcs;
extern std::mutex g_attacked_npcs_mutex;
extern std::vector<CommandedCriminal> g_commanded_criminals;
extern std::mutex g_commanded_criminals_mutex;
extern std::vector<TemporaryRoadClosure> g_temp_road_closures;
extern std::mutex g_temp_closures_mutex;
extern std::vector<CopVehicleBinding> g_cop_vehicle_bindings;
extern std::mutex g_bindings_mutex;
extern std::vector<void*> g_spawned_cop_vehicles;
extern std::mutex g_spawned_cop_vehicles_mutex;
extern std::vector<CopExitRecord> g_cop_exits;
extern std::mutex g_exits_mutex;
extern std::set<void*> g_vehicles_emptied;
extern std::mutex g_vehicles_emptied_mutex;
extern std::map<void*, int64_t> g_dispatched_vehicles_time;
extern std::mutex g_dispatched_vehicles_time_mutex;

extern std::set<void*> g_vehicles_ordered_to_scene;
extern std::mutex g_vehicles_mutex;
extern std::set<void*> g_vehicles_siren_awakened;
extern std::mutex g_vehicles_siren_awakened_mutex;
extern std::map<void*, StuckTracker> g_stuck_vehicles;
extern std::mutex g_stuck_vehicles_mutex;
extern std::set<void*> g_spawned_swats;
extern std::mutex g_spawned_swats_mutex;
extern std::map<void*, StuckTracker> g_emergency_stuck_vehicles;
extern std::mutex g_emergency_stuck_vehicles_mutex;
extern std::unordered_map<void*, int64_t> g_civilian_panic_timers;

std::shared_ptr<CrimeEvent> get_primary_active_crime();
std::shared_ptr<CrimeEvent> find_crime_containing_criminal(CPed* criminal);
bool any_active_firearm_case_blocking(CPed* criminal);

// --- 跨文件共享函数 ---
bool is_firearm(eCrimeType crime);
bool is_gang_or_criminal(int ped_type);
bool get_fire_position(void* fire, CVector& out_pos);
void stabilize_motorcycle(void* veh);
void zero_entity_velocity(void* entity);
void set_entity_pos(void* entity, CVector pos);
bool is_pos_visible_to_player_camera(CVector pos);
int count_criminals_near(CVector pos, float radius);
CPed* find_best_criminal_target_for_cop(CPed* cop, CVector crime_pos, float radius);

bool is_ped_pointer_valid_safe(void* target_ped);
bool is_task_vtable_safe(void* task);
bool is_vehicle_pointer_valid(void* target_veh);
bool is_entity_pointer_valid(void* entity);
CVector get_entity_pos(void* entity);
unsigned short get_entity_model_index(void* entity);
int64_t now_ms();
int get_random_range(int min_val, int max_val);
CVector get_spawn_target(CVector crime_pos);
void bind_vehicle_occupants(void* vehicle);
void record_exit_start_for_occupants(void* vehicle);
bool is_vehicle_emptied(void* vehicle);
void add_vehicle_emptied(void* vehicle);
void remove_vehicle_emptied(void* vehicle);
void vehicle_stop_for_exit(void* vehicle);
bool vehicle_has_physical_driver(void* vehicle);
bool should_prefer_foot_mobilization(CPed* cop, CVector crime_pos, void* bound_vehicle);
void clear_vehicle_driver_locks();
void clear_vehicle_enter_command_timestamps();
bool is_vehicle_ordered_to_scene(void* vehicle);
void add_vehicle_ordered_to_scene(void* vehicle);
bool is_vehicle_siren_awakened(void* vehicle);
void add_vehicle_siren_awakened(void* vehicle);
bool is_vehicle_occupied_by_driver(void* veh);
CVector compute_vehicle_staging_decoy(CVector crime_pos, CVector vehicle_pos);
void command_vehicle_ai(void* vehicle, const CVector& target_loc, float dist_to_target);
void command_cop_vehicle_to_scene(void* vehicle, const CVector& target_loc);
void* get_ped_event_group(CPed* ped);
bool is_cop_visible_to_player(void* veh, float current_x, float current_y, float current_z);
bool is_swat_van_nearby(CVector pos, float radius);
void register_spawned_swat(void* vehicle);
void setup_dispatched_cops(void* vehicle, CPed* criminal);
bool try_consolidate_crime(CPed* criminal, CVector location, bool is_firearm, int weapon_category = 1);
CVector get_crime_dispatch_position(const CrimeEvent& crime);
bool should_activate_or_hijack_crime(CVector location, bool is_firearm);
void update_primary_criminal_by_threat();
void* find_vehicle_of_cop(CPed* cop);
void* find_vehicle_of_cop_cached(CPed* cop);
void purge_dispatch_state_for_ped(CPed* ped);
bool apply_cop_weapon_for_combat(CPed* cop, CPed* criminal, bool force_update);
bool force_cop_native_redispatch(CPed* cop, CPed* criminal);
CPed* get_vehicle_driver_ped(void* vehicle);
void* find_closest_vehicle_to(CVector pos, float max_dist, void* ignore_veh = nullptr);
void* find_bound_vehicle_of_cop(CPed* cop, bool& out_is_driver);
bool is_alive_bound_driver_exists(void* vehicle);
void* get_ped_intelligence(CPed* ped);
void dispatch_tell_occupants_to_leave_car(void* vehicle);
bool is_cop_currently_exiting(CPed* cop);
bool should_block_cop_reenter_vehicle(CPed* cop);
void make_cop_enter_vehicle(CPed* cop, void* vehicle, bool as_driver);
int compute_nearby_cop_quota_for_crime(const std::shared_ptr<CrimeEvent>& crime);
float compute_nearby_cop_search_radius(const std::shared_ptr<CrimeEvent>& crime);
int dispatch_nearby_available_cops_to_crime(const std::shared_ptr<CrimeEvent>& crime, int max_cops, float search_radius);
int dispatch_nearby_available_cops_for_crime_auto(const std::shared_ptr<CrimeEvent>& crime);
void make_cops_attack_criminal(CPed* criminal);
void make_cops_attack_criminal_immediate(CPed* criminal);
void cleanup_single_case_vehicles(std::shared_ptr<CrimeEvent> crime);
void emergency_vehicles_tick();
void on_main_thread_tick();
void make_single_cop_attack_criminal(CPed* cop, CPed* criminal, bool force_weapon_update);
void update_cops_targeting_criminal_event_driven(CPed* criminal);
bool is_specific_criminal_armed_with_firearm(CPed* criminal);
eWeaponType determine_weapon_for_cop(CPed* cop, CPed* target, bool firearm_threat);

void hook_thread_func();
void init_ecs_systems();
void extract_nothing_so(const char* process_name);
void cleanup_nothing_so(const char* process_name);

extern "C" void* safe_pure_virtual_stub();