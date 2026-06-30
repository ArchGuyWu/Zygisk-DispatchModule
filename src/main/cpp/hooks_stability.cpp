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

#include "log.hpp"
#include "game_config.hpp"
#include "game_types.hpp"
#include "pointer_sanitizer.hpp"
#include "mod_shared.hpp"
#include "ecs_engine.hpp"

// =====================================================================
// Stability hook helpers
// =====================================================================
inline void sanitize_task_pointers(void* task, int max_size_bytes = 256) {
    // Blind memory scan disabled: 8-byte stepping can mistake floats/CVector fields
    // for stale pointers and null them out, causing fault-at-0x10 crashes.
    // Re-enable only with per-task-class offsets from reverse engineering.
}

inline void sanitize_unsafe_subtask_at(void* task, size_t offset) {
    if (!task || !is_pointer_readable(task)) return;
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task) + offset);
    if (!is_pointer_readable(sub_slot)) return;
    void* sub = *sub_slot;
    if (sub && !is_task_vtable_safe(sub)) {
        LOGW("⚠️ [Subtask Sanitizer] Clearing unsafe subtask %p inside task %p", sub, task);
        *sub_slot = nullptr;
    }
}

inline void sanitize_task_manager_slots(void* task_mgr, const char* log_tag) {
    if (!task_mgr || !is_pointer_readable(task_mgr)) return;
    for (int i = 0; i < 11; ++i) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(task_mgr) + i * 8);
        if (!is_pointer_readable(task_slot)) continue;
        void* task = *task_slot;
        if (task && !is_task_vtable_safe(task)) {
            LOGW("⚠️ [%s] Clearing unsafe task %p at slot %d", log_tag, task, i);
            *task_slot = nullptr;
        }
    }
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
    if (!self || !is_pointer_readable(self)) return;

    if (self) {
        // CTaskManager contains primary tasks (5 slots) and secondary tasks (6 slots)
        // Total 11 slots of pointers starting at offset 0 of CTaskManager.
        for (int i = 0; i < 11; ++i) {
            void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8);
            if (!is_pointer_readable(task_slot)) continue;
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
    if (!ped || !is_pointer_readable(ped)) return;
    void* intel = get_ped_intelligence(reinterpret_cast<CPed*>(ped));
    if (intel && is_pointer_readable(intel)) {
        sanitize_task_manager_slots(reinterpret_cast<char*>(intel) + 8, "ScanForAttractorsInRange");
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

inline void sanitize_optional_ped_at_slot(void** slot, const char* label) {
    if (!slot || !is_pointer_readable(slot)) return;
    void* ped = *slot;
    if (!ped) return;
    if (!is_ped_pointer_valid_safe(ped)) {
        LOGW("⚠️ [Ped Slot Sanitizer] Clearing invalid ped %p (%s)", ped, label);
        *slot = nullptr;
        return;
    }
    sanitize_ped_tasks(ped);
}

// GangFollower leader/partner: only clear unreadable or zero-filled peds (AGENTS.md).
// Do not clear merely because the ped is absent from the pool — that can false-positive
// and leave nullptr at task+0x18, which the original reads at +0x498 without a null check.
inline void sanitize_unsafe_task_slot_at(void* obj, size_t offset, const char* label) {
    if (!obj || !is_pointer_readable(obj)) return;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
    if (!is_pointer_readable(slot)) return;
    void* task = *slot;
    if (task && !is_task_vtable_safe(task)) {
        LOGW("⚠️ [%s] Clearing unsafe task %p at +0x%zx", label, task, offset);
        *slot = nullptr;
    }
}

inline void sanitize_gang_follower_ped_slot(void** slot, const char* label) {
    if (!slot || !is_pointer_readable(slot)) return;
    void* ped = *slot;
    if (!ped) return;

    if (!is_pointer_readable(ped)) {
        LOGW("⚠️ [GangFollower] Clearing unreadable %s ped %p", label, ped);
        *slot = nullptr;
        return;
    }
    void** vtable_slot = reinterpret_cast<void**>(ped);
    if (is_pointer_readable(vtable_slot) && *vtable_slot == nullptr) {
        LOGW("⚠️ [GangFollower] Clearing zero-filled %s ped %p", label, ped);
        *slot = nullptr;
        return;
    }
    if (is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
}


void* g_stub_ccgf_control = nullptr;
fn_ControlSubTask_t g_orig_ccgf_control = nullptr;
void* proxy_ccgf_control(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;

    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }

    if (self) {
        char* self_bytes = reinterpret_cast<char*>(self);
        sanitize_unsafe_subtask_at(self, 0x10);
        sanitize_gang_follower_ped_slot(reinterpret_cast<void**>(self_bytes + 0x18), "leader");
        sanitize_gang_follower_ped_slot(reinterpret_cast<void**>(self_bytes + 0x20), "partner");

        // Original dereferences leader+0x498 with no null guard (tombstone_01–04, fault 0x498).
        // Skip only when leader slot is empty after sanitization — not intercepting ped args.
        void** leader_slot = reinterpret_cast<void**>(self_bytes + 0x18);
        if (!is_pointer_readable(leader_slot) || !*leader_slot) {
            LOGW("⚠️ [GangFollower::ControlSubTask] no leader after sanitize — skip original");
            return nullptr;
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
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
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
    if (self && !is_pointer_readable(self)) {
        return nullptr;
    }
    if (self) {
        void** vtable_ptr = reinterpret_cast<void**>(self);
        if (is_pointer_readable(vtable_ptr) && *vtable_ptr == nullptr) {
            LOGW("⚠️ [PartnerGreet::GetPartnerSequence] zero-filled self %p — skip original", self);
            return nullptr;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_partner_greet_get_sequence, self);
}

// --- CAEPedSpeechAudioEntity::PlayLoadedSound Hook ---
void* g_stub_play_loaded_sound = nullptr;
fn_PlayLoadedSound_t g_orig_play_loaded_sound = nullptr;

void proxy_play_loaded_sound(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;

    // ped null: engine cbz at 52c120c handles safely. Only guard paths where
    // ped is non-null but intel/speech chain is null (52c1398–52c13a4, no cbz).
    if (self) {
        void** p_ped = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x8);
        if (is_pointer_readable(p_ped) && *p_ped) {
            void* ped = *p_ped;
            if (!is_pointer_readable(ped)) return;

            void** p_intel = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
            if (!is_pointer_readable(p_intel) || !*p_intel) {
                LOGW("⚠️ [PlayLoadedSound] ped %p has null intel — skip (engine path 52c1398 has no cbz)", ped);
                return;
            }
            void* intel = *p_intel;
            if (!is_pointer_readable(intel)) return;

            void** p_speech = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x48);
            if (!is_pointer_readable(p_speech) || !*p_speech) {
                LOGW("⚠️ [PlayLoadedSound] intel %p has null speech mgr — skip (engine path 52c13a4 has no cbz)", intel);
                return;
            }
            if (!is_pointer_readable(*p_speech)) return;
        }
    }
    SHADOWHOOK_CALL_PREV(proxy_play_loaded_sound, self);
}

// --- CCarGenerator::CheckIfWithinRangeOfAnyPlayers Hook ---
void* g_stub_check_if_within_range = nullptr;
fn_CheckIfWithinRange_t g_orig_check_if_within_range = nullptr;
bool proxy_check_if_within_range(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;

    // nullptr player: let engine handle. Only block stale non-null ped pointers (tombstone_36).
    if (g_FindPlayerPed) {
        void* player = g_FindPlayerPed(0);
        if (player && !is_ped_pointer_valid_safe(player)) {
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
        // Other ped at +0x18: engine walks its intel task slots → vtable+0x18 (tombstone_23).
        sanitize_optional_ped_at_slot(
            reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18),
            "AvoidOtherPed other");
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_avoid_ped_control, self, ped);
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
    // Engine cbz chain at 559ac3c–559ac54 handles null rw_clump / sub-fields safely.
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
        sanitize_task_manager_slots(reinterpret_cast<char*>(self) + 8, "ProcessStaticCounter");
    }
    SHADOWHOOK_CALL_PREV(proxy_process_static_counter, self);
}


void* g_stub_cbuoyancy_process_buoyancy = nullptr;
fn_cBuoyancy_ProcessBuoyancy_t g_orig_cbuoyancy_process_buoyancy = nullptr;

bool proxy_cbuoyancy_process_buoyancy(void* self, void* physical, float f1, void* vec1, void* vec2) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (physical && !is_pointer_readable(physical)) return false;
    // vec2 write at 57ff744 (tombstone_13) — reject wild output pointers from caller stack.
    if (vec1 && !is_pointer_readable(vec1)) return false;
    if (vec2 && !is_pointer_readable(vec2)) return false;
    if (physical && is_ped_pointer_valid_safe(physical)) {
        sanitize_ped_tasks(physical);
    }
    return SHADOWHOOK_CALL_PREV(proxy_cbuoyancy_process_buoyancy, self, physical, f1, vec1, vec2);
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

inline void sanitize_sequence_task_slots(void* self) {
    if (!self || !is_pointer_readable(self)) return;
    for (int i = 0; i < 8; ++i) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + i * 8 + 0x20);
        if (!is_pointer_readable(slot)) continue;
        void* task = *slot;
        if (task && !is_task_vtable_safe(task)) {
            LOGW("⚠️ [Sequence] Clearing unsafe task %p at index %d", task, i);
            *slot = nullptr;
        }
    }
}

void proxy_sequence_flush(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    if (self) {
        sanitize_sequence_task_slots(self);
    }
    SHADOWHOOK_CALL_PREV(proxy_sequence_flush, self);
}

// --- CTaskComplexSequence::CreateNextSubTask Hook ---
void* g_stub_sequence_create_next_sub_task = nullptr;
fn_SequenceCreateNextSubTask_t g_orig_sequence_create_next_sub_task = nullptr;

void* proxy_sequence_create_next_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;
    if (self) {
        sanitize_sequence_task_slots(self);
    }
    return SHADOWHOOK_CALL_PREV(proxy_sequence_create_next_sub_task, self, ped);
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
        sanitize_unsafe_subtask_at(self, 0x10);
        sanitize_unsafe_task_slot_at(self, 0x28, "BeInGroup secondary");
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_be_in_group_control_sub_task, self, ped);
}

// --- CPedGroupIntelligence::GetTaskMain Hook ---
// BeInGroup::ControlSubTask calls GetTaskMain then dereferences task+vtable+0x28 without
// a null-vtable guard (tombstone_05, fault 0x28 on zero-filled group task).
void* g_stub_get_task_main = nullptr;
fn_GetTaskMain_t g_orig_get_task_main = nullptr;

void* proxy_get_task_main(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    void* result = SHADOWHOOK_CALL_PREV(proxy_get_task_main, self, ped);
    if (result && !is_task_vtable_safe(result)) {
        LOGW("⚠️ [GetTaskMain] unsafe task %p — returning nullptr", result);
        return nullptr;
    }
    return result;
}

// --- CCarEnterExit::GetNearestCarDoor Hook ---
// Iterates ped task slots and calls vtable+0x28; zero-filled tasks trigger pure virtual (tombstone_06).
void* g_stub_get_nearest_car_door = nullptr;
fn_GetNearestCarDoor_t g_orig_get_nearest_car_door = nullptr;

bool proxy_get_nearest_car_door(void* ped, void* vehicle, void* out_vec, int* door_index) {
    SHADOWHOOK_STACK_SCOPE();
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    return SHADOWHOOK_CALL_PREV(proxy_get_nearest_car_door, ped, vehicle, out_vec, door_index);
}

// --- IKChainManager_c::Update Hook ---
void* g_stub_ik_chain_update = nullptr;
fn_IKChainUpdate_t g_orig_ik_chain_update = nullptr;

inline bool ik_chain_has_null_entity_slot(void* self) {
    if (!self || !is_pointer_readable(self)) return false;
    void** head_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x1600);
    if (!is_pointer_readable(head_slot)) return false;
    void* node = *head_slot;
    for (int guard = 0; node && guard < 64; ++guard) {
        if (!is_pointer_readable(node)) return true;
        void** entity_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x10);
        if (!is_pointer_readable(entity_slot) || !*entity_slot) {
            return true;
        }
        void** next_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x8);
        if (!is_pointer_readable(next_slot)) return true;
        node = *next_slot;
    }
    return false;
}

void proxy_ik_chain_update(void* self, float dt) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    // Original cbz x0 falls through to ldr [x0,#0x20] (tombstone_09, fault 0x20).
    if (self && ik_chain_has_null_entity_slot(self)) {
        LOGW("⚠️ [IKChainManager::Update] chain node with null entity — skip original");
        return;
    }
    SHADOWHOOK_CALL_PREV(proxy_ik_chain_update, self, dt);
}

// --- IKChainManager_c::IsFacingTarget Hook ---
// ped→intel→+0x58→indexed node→+0x18: engine skips null at +0x10 but not at +0x18 (tombstone_16–19, fault 0x20).
void* g_stub_ik_chain_is_facing_target = nullptr;
fn_IKChainIsFacingTarget_t g_orig_ik_chain_is_facing_target = nullptr;

// ped→intel→+0x58→+0x10 node (MakeAbortable / IK abort paths; tombstone_25).
inline bool ped_intel_ik_table_node_unsafe(void* ped) {
    if (!ped || !is_pointer_readable(ped)) return true;
    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot) || !*intel_slot) return true;
    void* intel = *intel_slot;
    if (!is_pointer_readable(intel)) return true;
    void** table_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x58);
    if (!is_pointer_readable(table_slot) || !*table_slot) return true;
    void* table = *table_slot;
    if (!is_pointer_readable(table)) return true;
    void** node_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(table) + 0x10);
    if (!is_pointer_readable(node_slot) || !*node_slot) return true;
    return !is_pointer_readable(*node_slot);
}

inline bool ped_heading_abort_write_safe(void* ped) {
    if (!ped || !is_pointer_readable(ped)) return false;
    if (!is_ped_pointer_valid_safe(ped)) return false;
    if (!is_pointer_readable(reinterpret_cast<char*>(ped) + 0x784)) return false;
    if (!is_pointer_readable(reinterpret_cast<char*>(ped) + 0x788)) return false;
    void** aux_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x7e8);
    if (is_pointer_readable(aux_slot) && *aux_slot) {
        void* aux = *aux_slot;
        if (!is_pointer_readable(aux)) return false;
        if (!is_pointer_readable(reinterpret_cast<char*>(aux) + 0x20)) return false;
    }
    return true;
}

inline bool ik_facing_target_ped_chain_unsafe(void* ped, int index) {
    if (!ped || !is_pointer_readable(ped)) return true;
    if (index < 0 || index > 32) return true;

    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot) || !*intel_slot) return true;
    void* intel = *intel_slot;
    if (!is_pointer_readable(intel)) return true;

    void** table_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x58);
    if (!is_pointer_readable(table_slot) || !*table_slot) return true;
    void* table = *table_slot;
    if (!is_pointer_readable(table)) return true;

    void** node_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(table) + index * 8 + 0x10);
    if (!is_pointer_readable(node_slot) || !*node_slot) return true;
    void* node = *node_slot;
    if (!is_pointer_readable(node)) return true;

    void** target_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x18);
    if (!is_pointer_readable(target_slot) || !*target_slot) return true;
    void* target = *target_slot;
    if (!is_pointer_readable(target)) return true;

    void** matrix_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(target) + 0x20);
    return !is_pointer_readable(matrix_slot) || !*matrix_slot;
}

bool proxy_ik_chain_is_facing_target(void* self, void* ped, int index) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (self && ik_chain_has_null_entity_slot(self)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (ped && ik_facing_target_ped_chain_unsafe(ped, index)) {
        LOGW("⚠️ [IKChainManager::IsFacingTarget] broken ped intel chain (idx=%d) — return false", index);
        return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_ik_chain_is_facing_target, self, ped, index);
}

// --- CTaskManager::GetSimplestActiveTask Hook ---
// Primary slots may hold zero-filled tasks; vtable+0x18 call crashes (tombstone_20, fault 0x18).
void* g_stub_get_simplest_active_task = nullptr;
fn_GetSimplestActiveTask_t g_orig_get_simplest_active_task = nullptr;

void* proxy_get_simplest_active_task(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return nullptr;
    sanitize_task_manager_slots(self, "GetSimplestActiveTask");
    return SHADOWHOOK_CALL_PREV(proxy_get_simplest_active_task, self);
}

// --- CEventScriptCommand::GetEventPriority Hook ---
void* g_stub_event_script_command_get_priority = nullptr;
fn_EventScriptCommandGetPriority_t g_orig_event_script_command_get_priority = nullptr;

int proxy_event_script_command_get_priority(void* self) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && is_pointer_readable(self)) {
        void** task_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
        if (is_pointer_readable(task_slot)) {
            void* task = *task_slot;
            if (task && !is_task_vtable_safe(task)) {
                LOGW("⚠️ [CEventScriptCommand::GetEventPriority] clearing unsafe task %p", task);
                *task_slot = nullptr;
            }
            // Engine cbz at +0x18 task (54ce890) handles nullptr — let CALL_PREV run.
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_event_script_command_get_priority, self);
}

// --- CPedGroupIntelligence::FlushTasks Hook ---
void* g_stub_flush_tasks = nullptr;
fn_FlushTasks_t g_orig_flush_tasks = nullptr;

inline void sanitize_ped_task_pair_slots(void* pair) {
    if (!pair || !is_pointer_readable(pair)) return;
    static constexpr size_t kTaskOffsets[] = {0x8, 0x28, 0x48, 0x68, 0x88, 0xa8, 0xc8, 0xe8};
    for (size_t off : kTaskOffsets) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(pair) + off);
        if (!is_pointer_readable(slot)) continue;
        void* task = *slot;
        if (task && !is_task_vtable_safe(task)) {
            LOGW("⚠️ [FlushTasks] Clearing unsafe task %p at pair+0x%zx", task, off);
            *slot = nullptr;
        }
    }
}

void proxy_flush_tasks(void* self, void* pair, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (pair) {
        sanitize_ped_task_pair_slots(pair);
    }
    SHADOWHOOK_CALL_PREV(proxy_flush_tasks, self, pair, ped);
}

// --- CCam::Process_FollowPed_SA Hook ---
void* g_stub_process_follow_ped_sa = nullptr;
fn_ProcessFollowPedSA_t g_orig_process_follow_ped_sa = nullptr;

void proxy_process_follow_ped_sa(void* self, const CVector& target, float f1, float f2, float f3, bool b1) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    SHADOWHOOK_CALL_PREV(proxy_process_follow_ped_sa, self, target, f1, f2, f3, b1);
}

// --- MakeAbortable shared guards (CEventHandler::HandleEvents hot path) ---
inline bool make_abortable_event_unsafe(void* event) {
    if (!event) return false;
    if (!is_userspace_address(event) || !is_pointer_readable(event)) return true;
    void** vtable_slot = reinterpret_cast<void**>(event);
    return !is_pointer_readable(vtable_slot) || !*vtable_slot;
}

inline bool make_abortable_ped_field_unsafe(void* ped, size_t field_offset) {
    if (!ped) return false;
    if (!is_pointer_readable(ped)) return true;
    if (!is_ped_pointer_valid_safe(ped)) return true;
    return !is_pointer_readable(reinterpret_cast<char*>(ped) + field_offset);
}

inline bool task_subtask_vtable_fn_unsafe(void* self, size_t subtask_offset, size_t vtable_offset) {
    if (!self || !is_pointer_readable(self)) return true;
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + subtask_offset);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) return false;
    void* sub = *sub_slot;
    if (!is_task_vtable_safe(sub)) return true;
    void** fn_slot = reinterpret_cast<void**>(
        reinterpret_cast<char*>(*reinterpret_cast<void**>(sub)) + vtable_offset);
    return !is_pointer_readable(fn_slot) || !*fn_slot || !is_pointer_readable(*fn_slot);
}

// --- CTaskComplexLeaveCar::MakeAbortable Hook ---
void* g_stub_leave_car_make_abortable = nullptr;
fn_LeaveCarMakeAbortable_t g_orig_leave_car_make_abortable = nullptr;

bool proxy_leave_car_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;

    if (self) {
        sanitize_unsafe_subtask_at(self, 0x10);
    }
    return SHADOWHOOK_CALL_PREV(proxy_leave_car_make_abortable, self, ped, priority, event);
}

// --- CTaskSimpleGoToPoint::MakeAbortable Hook ---
void* g_stub_goto_point_make_abortable = nullptr;
fn_GoToPointMakeAbortable_t g_orig_goto_point_make_abortable = nullptr;

bool proxy_goto_point_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (ped && ped_intel_ik_table_node_unsafe(ped)) {
        LOGW("⚠️ [GoToPoint::MakeAbortable] broken ped intel ik chain — return false");
        return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_goto_point_make_abortable, self, ped, priority, event);
}

// --- CTaskSimpleAchieveHeading::MakeAbortable Hook ---
void* g_stub_achieve_heading_make_abortable = nullptr;
fn_AchieveHeadingMakeAbortable_t g_orig_achieve_heading_make_abortable = nullptr;

bool proxy_achieve_heading_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (ped && ped_intel_ik_table_node_unsafe(ped)) {
        LOGW("⚠️ [AchieveHeading::MakeAbortable] broken ped intel ik chain — return false");
        return false;
    }
    if (ped && !ped_heading_abort_write_safe(ped)) {
        LOGW("⚠️ [AchieveHeading::MakeAbortable] stale ped %p heading fields — return false", ped);
        return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_achieve_heading_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexFollowPointRoute::MakeAbortable Hook ---
void* g_stub_follow_point_route_make_abortable = nullptr;
fn_FollowPointRouteMakeAbortable_t g_orig_follow_point_route_make_abortable = nullptr;

bool proxy_follow_point_route_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) {
        LOGW("⚠️ [FollowPointRoute::MakeAbortable] stale event %p — return false", event);
        return false;
    }
    if (self) {
        void** route_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x30);
        if (is_pointer_readable(route_slot) && *route_slot && !is_pointer_readable(*route_slot)) {
            return false;
        }
        sanitize_unsafe_subtask_at(self, 0x10);
        if (task_subtask_vtable_fn_unsafe(self, 0x10, 0x28)) return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_follow_point_route_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexKillCriminal::MakeAbortable Hook ---
// Mod injects KillCriminal tasks; stale m_pTarget (typically +0x18) → fault 0x0 (tombstone_33).
void* g_stub_kill_criminal_make_abortable = nullptr;
fn_KillCriminalMakeAbortable_t g_orig_kill_criminal_make_abortable = nullptr;

inline bool kill_criminal_task_target_unsafe(void* self) {
    if (!self || !is_pointer_readable(self)) return true;
    void** target_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
    if (!is_pointer_readable(target_slot)) return false;
    void* target = *target_slot;
    if (!target) return false;
    return !is_ped_pointer_valid_safe(reinterpret_cast<CPed*>(target));
}

bool proxy_kill_criminal_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    if (kill_criminal_task_target_unsafe(self)) {
        void** target_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x18);
        if (is_pointer_readable(target_slot) && *target_slot) {
            LOGW("⚠️ [KillCriminal::MakeAbortable] stale target %p — clear and abort", *target_slot);
            *target_slot = nullptr;
        }
        sanitize_unsafe_subtask_at(self, 0x10);
        return true;
    }
    sanitize_unsafe_subtask_at(self, 0x10);
    if (task_subtask_vtable_fn_unsafe(self, 0x10, 0x28)) return true;
    return SHADOWHOOK_CALL_PREV(proxy_kill_criminal_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexFallAndGetUp::MakeAbortable Hook ---
void* g_stub_fall_and_get_up_make_abortable = nullptr;
fn_FallAndGetUpMakeAbortable_t g_orig_fall_and_get_up_make_abortable = nullptr;

bool proxy_fall_and_get_up_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    sanitize_unsafe_subtask_at(self, 0x10);
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [FallAndGetUp::MakeAbortable] missing subtask — return true");
        return true;
    }
    return SHADOWHOOK_CALL_PREV(proxy_fall_and_get_up_make_abortable, self, ped, priority, event);
}

// --- CTaskComplexPlayHandSignalAnim::ControlSubTask Hook ---
void* g_stub_play_hand_signal_control_sub_task = nullptr;
fn_PlayHandSignalControlSubTask_t g_orig_play_hand_signal_control_sub_task = nullptr;

void* proxy_play_hand_signal_control_sub_task(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (!self || !is_pointer_readable(self)) return nullptr;
    if (ped && !is_pointer_readable(ped)) return nullptr;
    sanitize_unsafe_subtask_at(self, 0x10);
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) {
        LOGW("⚠️ [PlayHandSignalAnim::ControlSubTask] no subtask — skip original");
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_play_hand_signal_control_sub_task, self, ped);
}

// --- CTaskComplexKillPedOnFoot::MakeAbortable Hook ---
void* g_stub_kill_ped_on_foot_make_abortable = nullptr;
fn_KillPedOnFootMakeAbortable_t g_orig_kill_ped_on_foot_make_abortable = nullptr;

bool proxy_kill_ped_on_foot_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) return false;
    if (ped && make_abortable_ped_field_unsafe(ped, 0x63c)) {
        LOGW("⚠️ [KillPedOnFoot::MakeAbortable] stale ped %p +0x63c — return true", ped);
        return true;
    }
    if (self) {
        sanitize_unsafe_subtask_at(self, 0x10);
        if (task_subtask_vtable_fn_unsafe(self, 0x10, 0x38)) return true;
    }
    return SHADOWHOOK_CALL_PREV(proxy_kill_ped_on_foot_make_abortable, self, ped, priority, event);
}

// --- CTaskSimpleAnim::MakeAbortable Hook ---
void* g_stub_simple_anim_make_abortable = nullptr;
fn_SimpleAnimMakeAbortable_t g_orig_simple_anim_make_abortable = nullptr;

bool proxy_simple_anim_make_abortable(void* self, void* ped, int priority, void* event) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return false;
    if (ped && !is_pointer_readable(ped)) return false;
    if (event && make_abortable_event_unsafe(event)) {
        LOGW("⚠️ [SimpleAnim::MakeAbortable] stale event %p — return false", event);
        return false;
    }
    if (self) {
        sanitize_unsafe_subtask_at(self, 0x18);
        if (task_subtask_vtable_fn_unsafe(self, 0x18, 0x28)) return false;
    }
    return SHADOWHOOK_CALL_PREV(proxy_simple_anim_make_abortable, self, ped, priority, event);
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
        sanitize_unsafe_subtask_at(self, 0x10);
        // Original dereferences [self+0x10] without null check (tombstone_07, fault 0x0).
        void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x10);
        if (!is_pointer_readable(sub_slot) || !*sub_slot) {
            LOGW("⚠️ [Facial::ControlSubTask] no subtask after sanitize — skip original");
            return nullptr;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_facial_control_sub_task, self, ped);
}

// --- CTaskSimpleIKManager::ProcessPed Hook ---
// Subtasks at +0x10/+0x18: engine loads vtable then [vtable+0x48] with no null-vtable guard (tombstone_12).
void* g_stub_ik_manager_process_ped = nullptr;
fn_IKManagerProcessPed_t g_orig_ik_manager_process_ped = nullptr;

void proxy_ik_manager_process_ped(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    if (ped && !is_pointer_readable(ped)) return;

    if (self) {
        for (size_t off : {0x10u, 0x18u}) {
            sanitize_unsafe_subtask_at(self, off);
        }
    }
    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_ped_tasks(ped);
    }
    SHADOWHOOK_CALL_PREV(proxy_ik_manager_process_ped, self, ped);
}

// --- CCarCtrl::IsPoliceVehicleInPursuit Hook ---
// Outer entry takes pool index; inner thunk at +0x2b8 loads vehicle+0x10 without guard (tombstone_14/21/22/24).
void* g_stub_is_police_vehicle_in_pursuit = nullptr;
fn_IsPoliceVehicleInPursuit_t g_orig_is_police_vehicle_in_pursuit = nullptr;
void* g_stub_vehicle_pursuit_ai_thunk = nullptr;
fn_VehiclePursuitAiThunk_t g_orig_vehicle_pursuit_ai_thunk = nullptr;

inline bool vehicle_ai_subobject_chain_safe(void* vehicle) {
    if (!vehicle || !is_pointer_readable(vehicle)) return false;
    void** sub_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(vehicle) + 0x10);
    if (!is_pointer_readable(sub_slot) || !*sub_slot) return false;
    void* sub = *sub_slot;
    if (!is_pointer_readable(sub)) return false;
    void** vtable_slot = reinterpret_cast<void**>(sub);
    if (!is_pointer_readable(vtable_slot) || !*vtable_slot) return false;
    void** fn_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(*vtable_slot) + 0x38);
    return is_pointer_readable(fn_slot) && *fn_slot && is_pointer_readable(*fn_slot);
}

void* proxy_vehicle_pursuit_ai_thunk(void* vehicle) {
    SHADOWHOOK_STACK_SCOPE();
    if (!vehicle_ai_subobject_chain_safe(vehicle)) {
        LOGW("⚠️ [IsPoliceVehicleInPursuit thunk] vehicle %p ai chain unsafe — skip", vehicle);
        return nullptr;
    }
    return SHADOWHOOK_CALL_PREV(proxy_vehicle_pursuit_ai_thunk, vehicle);
}

bool proxy_is_police_vehicle_in_pursuit(int vehicle_index) {
    SHADOWHOOK_STACK_SCOPE();
    if (vehicle_index < 0) return false;
    if (g_GetPoolVehicle) {
        void* vehicle = g_GetPoolVehicle(vehicle_index);
        if (vehicle && !vehicle_ai_subobject_chain_safe(vehicle)) {
            return false;
        }
    }
    return SHADOWHOOK_CALL_PREV(proxy_is_police_vehicle_in_pursuit, vehicle_index);
}

// --- CTaskComplexWanderStandard::LookForChatPartners Hook ---
// Scans partner intel task slots +0x8…+0x28; zero-filled task → vtable+0x28 (tombstone_15).
void* g_stub_wander_look_for_chat_partners = nullptr;
fn_WanderLookForChatPartners_t g_orig_wander_look_for_chat_partners = nullptr;

inline void sanitize_wander_chat_partner_cache(void* ped) {
    if (!ped || !is_pointer_readable(ped)) return;
    void** intel_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(ped) + 0x5e8);
    if (!is_pointer_readable(intel_slot)) return;
    void* intel = *intel_slot;
    if (!intel || !is_pointer_readable(intel)) return;

    for (int i = 0; i < 16; ++i) {
        void** partner_slot = reinterpret_cast<void**>(reinterpret_cast<char*>(intel) + 0x228 + i * 8);
        if (!is_pointer_readable(partner_slot)) continue;
        void* partner = *partner_slot;
        if (partner && is_ped_pointer_valid_safe(partner)) {
            sanitize_ped_tasks(partner);
        }
    }
    sanitize_task_manager_slots(reinterpret_cast<char*>(intel) + 8, "WanderLookForChatPartners");
}

void proxy_wander_look_for_chat_partners(void* self, void* ped) {
    SHADOWHOOK_STACK_SCOPE();
    if (self && !is_pointer_readable(self)) return;
    if (ped && !is_pointer_readable(ped)) return;

    if (ped && is_ped_pointer_valid_safe(ped)) {
        sanitize_wander_chat_partner_cache(ped);
        sanitize_ped_tasks(ped);
    }
    SHADOWHOOK_CALL_PREV(proxy_wander_look_for_chat_partners, self, ped);
}
