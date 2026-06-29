#pragma once

#include <cstdint>

#include "include/game_types.hpp"

// Stability-hook callback typedefs (shared by hooks_stability.cpp and module.cpp hook installer)
typedef void (*fn_SetPedPosition_t)(void* self, void* ped);
typedef void (*fn_ManageTasks_t)(void* self);
typedef bool (*fn_IsSimple_t)(void* self);
typedef void (*fn_ScanForAttractorsInRange_t)(void* self, void* ped);
typedef void* (*fn_ControlSubTask_t)(void* self, void* ped);
typedef void* (*fn_PairedAttractorCreateNextSubTask_t)(void* self, void* ped);
typedef void (*fn_FacialDtor_t)(void* self);
typedef void* (*fn_FindActiveTask_t)(void* self, int type);
typedef void (*fn_TaskManagerDestructor_t)(void* self);
typedef void* (*fn_GetPartnerSequence_t)(void* self);
typedef void (*fn_PlayLoadedSound_t)(void* self);
typedef bool (*fn_CheckIfWithinRange_t)(void* self);
typedef void* (*fn_AvoidPedControl_t)(void* self, void* ped);
typedef void (*fn_CalcTargetOffset_t)(void* self);
typedef void (*fn_DoFootLanded_t)(void* ped, bool left_foot, unsigned char surface_type);
typedef void (*fn_PlayFootSteps_t)(void* self);
typedef void (*fn_ProcessBuoyancy_t)(void* self);
typedef void (*fn_ProcessStaticCounter_t)(void* self);
typedef bool (*fn_cBuoyancy_ProcessBuoyancy_t)(void* physical, float f1, void* vec1, void* vec2);
typedef int32_t (*fn_u_strlen_t)(const void* s);
typedef void (*fn_SequenceFlush_t)(void* self);
typedef void (*fn_FinishAnimEvasiveStepCB_t)(void* anim, void* context);
typedef void* (*fn_BeInGroupControlSubTask_t)(void* self, void* ped);
typedef void (*fn_IKChainUpdate_t)(void* self, float dt);
typedef void (*fn_ProcessFollowPedSA_t)(void* self, const CVector& target, float f1, float f2, float f3, bool b1);
typedef bool (*fn_LeaveCarMakeAbortable_t)(void* self, void* ped, int priority, void* event);
typedef void (*fn_UpdateCarAI_t)(void* vehicle);
typedef void* (*fn_FacialControlSubTask_t)(void* self, void* ped);

extern fn_SetPedPosition_t g_orig_set_ped_pos;
extern fn_ManageTasks_t g_orig_manage_tasks;
extern fn_ScanForAttractorsInRange_t g_orig_scan_for_attractors_in_range;
extern fn_ControlSubTask_t g_orig_ccgf_control;
extern fn_PairedAttractorCreateNextSubTask_t g_orig_paired_attractor_create_next_sub_task;
extern fn_FacialDtor_t g_orig_facial_dtor;
extern fn_FindActiveTask_t g_orig_find_active_task;
extern fn_TaskManagerDestructor_t g_orig_task_manager_destructor;
extern fn_GetPartnerSequence_t g_orig_partner_greet_get_sequence;
extern fn_PlayLoadedSound_t g_orig_play_loaded_sound;
extern fn_CheckIfWithinRange_t g_orig_check_if_within_range;
extern fn_AvoidPedControl_t g_orig_avoid_ped_control;
extern fn_CalcTargetOffset_t g_orig_CalcTargetOffset;
extern fn_DoFootLanded_t g_orig_do_foot_landed;
extern fn_AddPoliceOccupants_t g_orig_add_police_occupants;
extern fn_PlayFootSteps_t g_orig_play_footsteps;
extern fn_ProcessBuoyancy_t g_orig_process_buoyancy;
extern fn_ProcessStaticCounter_t g_orig_process_static_counter;
extern fn_cBuoyancy_ProcessBuoyancy_t g_orig_cbuoyancy_process_buoyancy;
extern fn_u_strlen_t g_orig_u_strlen;
extern fn_SequenceFlush_t g_orig_sequence_flush;
extern fn_FinishAnimEvasiveStepCB_t g_orig_finish_anim_evasive_step_cb;
extern fn_BeInGroupControlSubTask_t g_orig_be_in_group_control_sub_task;
extern fn_IKChainUpdate_t g_orig_ik_chain_update;
extern fn_ProcessFollowPedSA_t g_orig_process_follow_ped_sa;
extern fn_LeaveCarMakeAbortable_t g_orig_leave_car_make_abortable;
extern fn_UpdateCarAI_t g_orig_update_car_ai;
extern fn_FacialControlSubTask_t g_orig_facial_control_sub_task;