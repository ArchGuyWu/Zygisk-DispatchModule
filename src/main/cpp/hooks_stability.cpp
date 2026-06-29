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

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
#include "ecs_engine.hpp"

// =====================================================================
// 🤝 [CTaskComplexPartner & derived Hooks]：防止各种伴随/打招呼序列空指针、野指针或零值状态导致的 Sequence Manager 空指针闪退
// =====================================================================
inline bool is_sequence_manager_safe() {
    if (!g_CSequenceManager_ms_instance || !*g_CSequenceManager_ms_instance) {
        return false;
    }
    void* manager = *g_CSequenceManager_ms_instance;
    return is_pointer_readable(manager);
}

inline bool is_partner_task_safe(void* self) {
    if (!self || !is_task_vtable_safe(self)) {
        return false;
    }
    return true;
}

inline void sanitize_partner_task_pointers(void* task) {
    // 禁用伙伴任务“净化器”以防误伤非指针数据成员
}

inline void sanitize_task_pointers(void* task, int max_size_bytes = 256) {
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

// --- CTaskSimpleHoldEntity::SetPedPosition Hooks ---

void* g_stub_set_ped_pos = nullptr;
fn_SetPedPosition_t g_orig_set_ped_pos = nullptr;
void proxy_set_ped_pos(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) {
        LOGW("⚠️ [SetPedPosition] unsafe self! Skipping.");
        return;
    }
    if (ped && !is_pointer_readable(ped)) {
        LOGW("⚠️ [SetPedPosition] unsafe ped! Skipping.");
        return;
    }
    if (self && ped) {
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
    }
    SHADOWHOOK_CALL_PREV(proxy_set_ped_pos, self, ped);
}


// =====================================================================
// 🛠️ [CTaskManager & CAttractorScanner Safety Hooks]
// =====================================================================
void* g_stub_manage_tasks = nullptr;
fn_ManageTasks_t g_orig_manage_tasks = nullptr;


inline bool is_task_simple(void* task) {
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

void sanitize_task_chain(void* task, int depth = 0) {
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

void proxy_manage_tasks(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;

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

void* g_stub_scan_for_attractors_in_range = nullptr;
fn_ScanForAttractorsInRange_t g_orig_scan_for_attractors_in_range = nullptr;

void proxy_scan_for_attractors_in_range(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (ped && !is_ped_pointer_valid_safe(ped)) return;
    if (ped) {
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
    }
    SHADOWHOOK_CALL_PREV(proxy_scan_for_attractors_in_range, self, ped);
}

// --- CTaskComplexGangFollower::ControlSubTask Hook ---
inline void sanitize_task_tree(void* task) {
    if (!task || !is_pointer_readable(task)) return;
    
    // Only CTaskComplex has m_pSubTask at offset 16 (0x10).
    // Wiping offset 16 of a CTaskSimple will corrupt its member variables!
    if (!is_task_simple(task)) {
        void** p_sub = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + 0x10);
        if (is_pointer_readable(p_sub)) {
            void* sub = *p_sub;
            if (sub) {
                if (!is_task_vtable_safe(sub)) {
                    LOGW("⚠️ [Task Tree Sanitizer] Clearing unsafe subtask %p inside parent task %p", sub, task);
                    *p_sub = nullptr;
                } else {
                    sanitize_task_tree(sub);
                }
            }
        }
    }
}

inline void sanitize_ped_tasks(void* ped) {
    if (!ped || !is_pointer_readable(ped)) return;
    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot)) return;
    void* intel = *intel_slot;
    if (!intel || !is_pointer_readable(intel)) return;
    void* task_mgr = reinterpret_cast<char*>(intel) + 8;
    if (!is_pointer_readable(task_mgr)) return;
    for (int i = 0; i < 11; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (is_pointer_readable(task_slot)) {
            void* task = *task_slot;
            if (task) {
                if (!is_task_vtable_safe(task)) {
                    LOGW("⚠️ [Task Sanitizer] Clearing unsafe/zeroed task %p at slot %d in ped %p", task, i, ped);
                    *task_slot = nullptr;
                } else {
                    sanitize_task_tree(task);
                }
            }
        }
    }
}


void* g_stub_ccgf_control = nullptr;
fn_ControlSubTask_t g_orig_ccgf_control = nullptr;
void* proxy_ccgf_control(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) {
        LOGW("⚠️ [GangFollower::ControlSubTask] unsafe self!");
        return nullptr;
    }
    if (ped && !is_pointer_readable(ped)) {
        LOGW("⚠️ [GangFollower::ControlSubTask] unsafe ped!");
        return nullptr;
    }

    if (ped) {
        // Sanitize follower ped
        sanitize_ped_tasks(ped);
    }

    if (self) {
        // Sanitize leader and partner peds
        char* self_bytes = reinterpret_cast<char*>(self);
        void** p_leader = reinterpret_cast<void**>(self_bytes + 0x18);
        if (is_pointer_readable(p_leader)) {
            sanitize_ped_tasks(*p_leader);
        }
        void** p_partner = reinterpret_cast<void**>(self_bytes + 0x20);
        if (is_pointer_readable(p_partner)) {
            sanitize_ped_tasks(*p_partner);
        }
    }

    return SHADOWHOOK_CALL_PREV(proxy_ccgf_control, self, ped);
}


// --- CTaskComplexUsePairedAttractor::CreateNextSubTask Hook ---
void* g_stub_paired_attractor_create_next_sub_task = nullptr;
fn_PairedAttractorCreateNextSubTask_t g_orig_paired_attractor_create_next_sub_task = nullptr;

void* proxy_paired_attractor_create_next_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (ped && !is_pointer_readable(ped)) {
        return nullptr;
    }
    
    if (ped) {
        // Sanitize the ped's task manager
        sanitize_ped_tasks(ped);

        // 校验 Ped 是否真的拥有活动的 TASK_COMPLEX_USE_PAIRED_ATTRACTOR (246 / 0xf6)
        // 防止 FindActiveTaskByType 返回 nullptr 后官方引擎因缺少空指针校验而在 0x5709844 处解引用崩溃
        void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
        if (is_pointer_readable(intel_slot)) {
            void* intel = *intel_slot;
            if (intel && is_pointer_readable(intel)) {
                void* task_mgr = reinterpret_cast<char*>(intel) + 8;
                bool has_paired_attractor = false;
                if (is_pointer_readable(task_mgr)) {
                    for (int i = 0; i < 11; ++i) {
                        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
                        if (is_pointer_readable(task_slot)) {
                            void* task = *task_slot;
                            if (task && is_task_vtable_safe(task)) {
                                typedef int (*fn_GetTaskType_t)(void* t);
                                void** vtable = *reinterpret_cast<void***>(task);
                                if (is_pointer_readable(vtable + 5)) {
                                    fn_GetTaskType_t get_type = reinterpret_cast<fn_GetTaskType_t>(vtable[5]);
                                    if (get_type(task) == 246) {
                                        has_paired_attractor = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                if (!has_paired_attractor) {
                    LOGW("⚠️ [PairedAttractor Sanitizer] Ped %p does not have active TASK_COMPLEX_USE_PAIRED_ATTRACTOR (246), intercepting CreateNextSubTask to prevent crash!", ped);
                    return nullptr;
                }
            }
        }
    }

    return SHADOWHOOK_CALL_PREV(proxy_paired_attractor_create_next_sub_task, self, ped);
}

// --- CTaskComplexFacial Destructor Hook ---
void* g_stub_facial_dtor = nullptr;
fn_FacialDtor_t g_orig_facial_dtor = nullptr;

void proxy_facial_dtor(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;

    if (self) {
        void** p_sub = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (is_pointer_readable(p_sub)) {
            void* sub = *p_sub;
            if (sub && !is_task_vtable_safe(sub)) {
                LOGW("⚠️ [Facial Dtor] Clearing unsafe subtask %p inside facial task %p before destruction", sub, self);
                *p_sub = nullptr;
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_facial_dtor, self);
}

// --- CTaskManager::FindActiveTaskByType Hook ---
void* g_stub_find_active_task = nullptr;
fn_FindActiveTask_t g_orig_find_active_task = nullptr;

void* proxy_find_active_task(void* self, int type) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return nullptr;

    if (self) {
        // CTaskManager contains primary tasks (5 slots) and secondary tasks (6 slots)
        // Total 11 slots of pointers starting at offset 0 of CTaskManager.
        for (int i = 0; i < 11; ++i) {
            void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
            if (is_pointer_readable(task_slot)) {
                void* task = *task_slot;
                if (task) {
                    if (!is_task_vtable_safe(task)) {
                        LOGW("⚠️ [FindActiveTaskByType Sanitizer] Sanitizing unsafe task %p at slot %d inside CTaskManager %p", task, i, self);
                        *task_slot = nullptr;
                    } else {
                        sanitize_task_tree(task);
                    }
                }
            }
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_find_active_task, self, type);
}


// --- CTaskManager Destructor Hook ---
void* g_stub_task_manager_destructor = nullptr;
fn_TaskManagerDestructor_t g_orig_task_manager_destructor = nullptr;

void proxy_task_manager_destructor(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;

    if (self) {
        for (int i = 0; i < 11; ++i) {
            void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
            if (is_pointer_readable(task_slot)) {
                void* task = *task_slot;
                if (task) {
                    if (!is_task_vtable_safe(task)) {
                        LOGW("⚠️ [CTaskManager Destructor Sanitizer] Sanitizing unsafe task %p at slot %d inside CTaskManager %p before destruction", task, i, self);
                        *task_slot = nullptr;
                    }
                }
            }
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_task_manager_destructor, self);
}

// --- CTaskComplexPartnerGreet::GetPartnerSequence Hook ---
void* g_stub_partner_greet_get_sequence = nullptr;
fn_GetPartnerSequence_t g_orig_partner_greet_get_sequence = nullptr;

void* proxy_partner_greet_get_sequence(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_task_vtable_safe(self)) {
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_partner_greet_get_sequence, self);
}

// --- CAEPedSpeechAudioEntity::PlayLoadedSound Hook ---
void* g_stub_play_loaded_sound = nullptr;
fn_PlayLoadedSound_t g_orig_play_loaded_sound = nullptr;

void proxy_play_loaded_sound(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;

    if (self) {
        void** p_ped = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x8);
        if (is_pointer_readable(p_ped)) {
            void* ped = *p_ped;
            if (ped && is_pointer_readable(ped)) {
                void** p_intel = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
                if (is_pointer_readable(p_intel)) {
                    void* intel = *p_intel;
                    if (intel && is_pointer_readable(intel)) {
                        void** p_speech = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x48);
                        if (is_pointer_readable(p_speech)) {
                            void* speech = *p_speech;
                            if (speech && is_pointer_readable(speech)) {
                                SHADOWHOOK_CALL_PREV(proxy_play_loaded_sound, self);
                                return;
                            }
                        }
                    }
                }
            }
        }
        LOGW("⚠️ [PlayLoadedSound Sanitizer] Skipping PlayLoadedSound on %p to prevent null speech manager crash!", self);
    } else {
        SHADOWHOOK_CALL_PREV(proxy_play_loaded_sound, self);
    }
}

// --- CCarGenerator::CheckIfWithinRangeOfAnyPlayers Hook ---
void* g_stub_check_if_within_range = nullptr;
fn_CheckIfWithinRange_t g_orig_check_if_within_range = nullptr;
void*** g_p_ms_pPedPool = nullptr;  // resolved in module.cpp hook_thread_func

bool proxy_check_if_within_range(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    
    if (g_FindPlayerPed) {
        void* player = g_FindPlayerPed(0);
        if (!player || !is_ped_pointer_valid_safe(player)) {
            return false;
        }
        void* player1 = g_FindPlayerPed(1);
        if (player1 && !is_ped_pointer_valid_safe(player1)) {
            return false;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_check_if_within_range, self);
}

// --- CTaskComplexAvoidOtherPedWhileWandering::ControlSubTask Hook ---
void* g_stub_avoid_ped_control = nullptr;
fn_AvoidPedControl_t g_orig_avoid_ped_control = nullptr;

void* proxy_avoid_ped_control(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && is_pointer_readable(self)) {
        void** subtask_ptr = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (is_pointer_readable(subtask_ptr)) {
            void* subtask = *subtask_ptr;
            if (subtask && !is_task_vtable_safe(subtask)) {
                LOGW("⚠️ [ControlSubTask Sanitizer] Sanitizing unsafe subtask %p inside CTaskComplexAvoidOtherPedWhileWandering %p", subtask, self);
                *subtask_ptr = nullptr;
            }
        }
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_avoid_ped_control, self, ped);
}


// --- CTaskGangHassleVehicle::CalcTargetOffset Hook ---
void* g_stub_CalcTargetOffset = nullptr;
fn_CalcTargetOffset_t g_orig_CalcTargetOffset = nullptr;

void proxy_CalcTargetOffset(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    
    if (self) {
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
    }
    
    SHADOWHOOK_CALL_PREV(proxy_CalcTargetOffset, self);
}

// --- CPed::DoFootLanded Hook ---
void* g_stub_do_foot_landed = nullptr;
fn_DoFootLanded_t g_orig_do_foot_landed = nullptr;

void proxy_do_foot_landed(void* ped, bool left_foot, unsigned char surface_type) {
    SHADOWHOOK_STACK_SCOPE();
    if (ped && (!is_ped_pointer_valid_safe(ped) || !*reinterpret_cast<void**>(ped))) {
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_do_foot_landed, ped, left_foot, surface_type);
}

void* g_stub_add_police_occupants = nullptr;
fn_AddPoliceOccupants_t g_orig_add_police_occupants = nullptr;

void proxy_add_police_occupants(CVehicle* vehicle, bool bSirenOrAlarm) {
    SHADOWHOOK_STACK_SCOPE();
    SHADOWHOOK_CALL_PREV(proxy_add_police_occupants, vehicle, bSirenOrAlarm);
    bind_vehicle_occupants(vehicle);
}

// =====================================================================
// 🛡️ [CPed::PlayFootSteps Hook]：防止转场期间玩家 Clump 临时脱离导致空指针解引用闪退
// =====================================================================
void* g_stub_play_footsteps = nullptr;
fn_PlayFootSteps_t g_orig_play_footsteps = nullptr;

void proxy_play_footsteps(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;

    if (self) {
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
    }

    SHADOWHOOK_CALL_PREV(proxy_play_footsteps, self);
}


// =====================================================================
// 🛡️ [cBuoyancy::ProcessBuoyancy Hook]：防止物理浮力计算期间/之后任务被销毁导致 CPed::ProcessBuoyancy 闪退 (静态函数，首参为 CPhysical*)
// =====================================================================
// =====================================================================
// 🛡️ [CPed::ProcessBuoyancy Hook]：防止 ProcessBuoyancy 期间任务槽被置空/野指针导致虚表解引用闪退
// =====================================================================
void* g_stub_process_buoyancy = nullptr;
fn_ProcessBuoyancy_t g_orig_process_buoyancy = nullptr;

void proxy_process_buoyancy(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    sanitize_ped_tasks(self);
    SHADOWHOOK_CALL_PREV(proxy_process_buoyancy, self);
}

// --- CPedIntelligence::ProcessStaticCounter Hook ---
void* g_stub_process_static_counter = nullptr;
fn_ProcessStaticCounter_t g_orig_process_static_counter = nullptr;

void proxy_process_static_counter(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    if (self) {
        void** p_ped = reinterpret_cast<void**>(self);
        if (is_pointer_readable(p_ped)) {
            sanitize_ped_tasks(*p_ped);
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_process_static_counter, self);
}


void* g_stub_cbuoyancy_process_buoyancy = nullptr;
fn_cBuoyancy_ProcessBuoyancy_t g_orig_cbuoyancy_process_buoyancy = nullptr;

bool proxy_cbuoyancy_process_buoyancy(void* physical, float f1, void* vec1, void* vec2) {
    SHADOWHOOK_STACK_SCOPE();
    if (physical && !is_pointer_readable(physical)) return false;
    bool res = SHADOWHOOK_CALL_PREV(proxy_cbuoyancy_process_buoyancy, physical, f1, vec1, vec2);
    if (!res) {
        if (physical && is_ped_pointer_valid_safe(physical)) {
            sanitize_ped_tasks(physical);
        }
    }
    return res;
}

// =====================================================================
// 🛡️ [u_strlen_64 Hook]：防止 ICU 计算 Unicode 字符串长度时传入野指针闪退
// =====================================================================
void* g_stub_u_strlen = nullptr;
fn_u_strlen_t g_orig_u_strlen = nullptr;

int32_t proxy_u_strlen(const void* s) {
    SHADOWHOOK_STACK_SCOPE();
    if (s && !is_pointer_readable(s)) {
        LOGW("⚠️ [u_strlen_64 Hook] wild pointer detected! Returning 0.");
        return 0;
    }
    return SHADOWHOOK_CALL_PREV(proxy_u_strlen, s);
}

// --- CTaskComplexSequence::Flush Hook ---
void* g_stub_sequence_flush = nullptr;
fn_SequenceFlush_t g_orig_sequence_flush = nullptr;

void proxy_sequence_flush(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;

    if (self) {
        // Sanitize any zero-filled task pointers inside the sequence
        sanitize_task_pointers(self, 128);
    }
    SHADOWHOOK_CALL_PREV(proxy_sequence_flush, self);
}

// --- CTaskSimpleEvasiveStep::FinishAnimEvasiveStepCB Hook ---
void* g_stub_finish_anim_evasive_step_cb = nullptr;
fn_FinishAnimEvasiveStepCB_t g_orig_finish_anim_evasive_step_cb = nullptr;

void proxy_finish_anim_evasive_step_cb(void* anim, void* context) {
    SHADOWHOOK_STACK_SCOPE();
    if (context && !is_pointer_readable(context)) {
        LOGW("⚠️ [EvasiveStep CB] context (%p) is invalid/unreadable! Intercepting callback to prevent crash.", context);
        return;
    }
    if (context) {
        // Check if the context object (the task) is zero-filled
        void** vtable_ptr = reinterpret_cast<void**>(context);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [EvasiveStep CB] context (%p) is zero-filled! Intercepting callback to prevent crash.", context);
            return;
        }
    }
    if (anim && !is_pointer_readable(anim)) {
        LOGW("⚠️ [EvasiveStep CB] anim (%p) is invalid/unreadable! Intercepting callback to prevent crash.", anim);
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_finish_anim_evasive_step_cb, anim, context);
}

// --- CTaskComplexBeInGroup::ControlSubTask Hook ---
void* g_stub_be_in_group_control_sub_task = nullptr;
fn_BeInGroupControlSubTask_t g_orig_be_in_group_control_sub_task = nullptr;

void* proxy_be_in_group_control_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;

    if (self) {
        void** vtable_ptr = reinterpret_cast<void**>(self);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [BeInGroup] self (%p) is zero-filled! Intercepting ControlSubTask to prevent crash.", self);
            return nullptr;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_be_in_group_control_sub_task, self, ped);
}

// --- IKChainManager_c::Update Hook ---
void* g_stub_ik_chain_update = nullptr;
fn_IKChainUpdate_t g_orig_ik_chain_update = nullptr;

void proxy_ik_chain_update(void* self, float dt) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    SHADOWHOOK_CALL_PREV(proxy_ik_chain_update, self, dt);
}

// --- CCam::Process_FollowPed_SA Hook ---
void* g_stub_process_follow_ped_sa = nullptr;
fn_ProcessFollowPedSA_t g_orig_process_follow_ped_sa = nullptr;

void proxy_process_follow_ped_sa(void* self, const CVector& target, float f1, float f2, float f3, bool b1) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    SHADOWHOOK_CALL_PREV(proxy_process_follow_ped_sa, self, target, f1, f2, f3, b1);
}

// --- CTaskComplexLeaveCar::MakeAbortable Hook ---
void* g_stub_leave_car_make_abortable = nullptr;
fn_LeaveCarMakeAbortable_t g_orig_leave_car_make_abortable = nullptr;

bool proxy_leave_car_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;

    if (self) {
        // Offset 0x10 is m_pSubTask in CTaskComplex
        void** p_sub = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (is_pointer_readable(p_sub)) {
            void* sub = *p_sub;
            if (sub == nullptr) {
                return true; // If subtask is null, the task is already abortable
            }
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_leave_car_make_abortable, self, ped, priority, event);
}

// --- CCarAI::UpdateCarAI Hook ---
void* g_stub_update_car_ai = nullptr;
fn_UpdateCarAI_t g_orig_update_car_ai = nullptr;

void proxy_update_car_ai(void* vehicle) {
    SHADOWHOOK_STACK_SCOPE();
    if (vehicle && !is_pointer_readable(vehicle)) return;
    if (vehicle) {
        void** vtable_ptr = reinterpret_cast<void**>(vehicle);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [UpdateCarAI] vehicle (%p) is zero-filled! Intercepting to prevent crash.", vehicle);
            return;
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_update_car_ai, vehicle);
}

// --- CTaskComplexFacial::ControlSubTask Hook ---
void* g_stub_facial_control_sub_task = nullptr;
fn_FacialControlSubTask_t g_orig_facial_control_sub_task = nullptr;

void* proxy_facial_control_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;

    if (self) {
        // Offset 0x10 is m_pSubTask in CTaskComplex
        void** p_sub = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (is_pointer_readable(p_sub)) {
            void* sub = *p_sub;
            if (sub == nullptr) {
                return nullptr; // Prevent crash when subtask is null
            }
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_facial_control_sub_task, self, ped);
}
