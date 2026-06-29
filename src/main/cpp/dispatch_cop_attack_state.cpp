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
#include "dispatch_cop_attack_internal.hpp"


void CopAttackContext::reset() {
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

}

bool CopAttackContext::vector_contains(const std::vector<void*>& vec, void* val) const {
    for (void* item : vec) {
        if (item == val) return true;
    }
    return false;
}

void cop_attack_snapshot_globals(CopAttackContext& ctx) {
    // 一站式极速拷贝多线程全局状态
    {
        std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
        ctx.cop_attack_assign_time_snapshot.assign(g_cop_attack_assign_time.begin(), g_cop_attack_assign_time.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
        ctx.armed_cops_time_snapshot.assign(g_armed_cops_time.begin(), g_armed_cops_time.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
        ctx.cop_assigned_weapon_snapshot.assign(g_cop_assigned_weapon.begin(), g_cop_assigned_weapon.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
        ctx.dispatched_vehicles_time_snapshot.assign(g_dispatched_vehicles_time.begin(), g_dispatched_vehicles_time.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
        ctx.stuck_vehicles_snapshot.assign(g_stuck_vehicles.begin(), g_stuck_vehicles.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
        ctx.vehicles_emptied_snapshot.assign(g_vehicles_emptied.begin(), g_vehicles_emptied.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_mutex);
        ctx.vehicles_ordered_to_scene_snapshot.assign(g_vehicles_ordered_to_scene.begin(), g_vehicles_ordered_to_scene.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
        ctx.vehicles_siren_awakened_snapshot.assign(g_vehicles_siren_awakened.begin(), g_vehicles_siren_awakened.end());
    }

}

void cop_attack_detect_firearm_threat(CopAttackContext& ctx) {
    ctx.is_active_firearm = false;
    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        if (ctx.crime_case->is_firearm) {
            ctx.is_active_firearm = true;
        } else {
            auto& list = ctx.crime_case->consolidated_criminals;
            auto& is_fire_list = ctx.crime_case->criminal_is_firearm;
            for (size_t idx = 0; idx < list.size(); ++idx) {
                if (list[idx] == ctx.criminal && idx < is_fire_list.size() && is_fire_list[idx]) {
                    ctx.is_active_firearm = true;
                    break;
                }
            }
        }
    }

}

void cop_attack_pass1_count_active(CopAttackContext& ctx) {
    ctx.active_vehicles_count = 0;
    ctx.active_foot_cops_count = 0;

    for (int i = 0; i < ctx.pool_size; i++) {
        signed char flag = ctx.byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            CPed* ped = g_GetPoolPed(handle);
            
            // 判定该 NPC 是否属于并案列表中的罪犯之一
            bool is_cop_not_criminal = true;
            if (ctx.crime_case && !ctx.crime_case->cancelled) {
                for (CPed* c : ctx.crime_case->consolidated_criminals) {
                    if (ped == c) {
                        is_cop_not_criminal = false;
                        break;
                    }
                }
            } else {
                if (ped == ctx.criminal) is_cop_not_criminal = false;
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
                        if (ctx.vector_contains(ctx.vehicles_ordered_to_scene_snapshot, veh) || 
                            ctx.vector_contains(ctx.vehicles_siren_awakened_snapshot, veh) || 
                            veh == ctx.crime_case->spawned_vehicle) {
                            if (!ctx.vector_contains(ctx.counted_vehicles, veh)) {
                                ctx.counted_vehicles.push_back(veh);
                                ctx.active_vehicles_count++;
                            }
                        }
                    } else {
                        // 地面散步/巡逻警员
                        int64_t last_assign = 0;
                        if (ctx.crime_case && !ctx.crime_case->cancelled) {
                            for (CPed* c : ctx.crime_case->consolidated_criminals) {
                                auto key = std::make_pair(ped, c);
                                for (const auto& item : ctx.cop_attack_assign_time_snapshot) {
                                    if (item.first == key) {
                                        last_assign = std::max(last_assign, item.second);
                                        break;
                                    }
                                }
                            }
                        } else {
                            auto key = std::make_pair(ped, ctx.criminal);
                            for (const auto& item : ctx.cop_attack_assign_time_snapshot) {
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
                                if (ctx.crime_case && !ctx.crime_case->cancelled) {
                                    for (CPed* c : ctx.crime_case->consolidated_criminals) {
                                        if (target == reinterpret_cast<CEntity*>(c)) {
                                            already_targeting = true;
                                            break;
                                        }
                                    }
                                } else {
                                    if (target == reinterpret_cast<CEntity*>(ctx.criminal)) {
                                        already_targeting = true;
                                    }
                                }
                            }
                        }

                        // 如果 15 秒内分派过任务，或者已经正在瞄准任何嫌疑人，则视为已被调度地面警员
                        if (already_targeting || (now_ms() - last_assign < 15000)) {
                            ctx.active_foot_cops_count++;
                        }
                    }
                }
            }
        }
    }

}

void cop_attack_compute_quotas(CopAttackContext& ctx) {
    ctx.max_vehicles = 2;
    ctx.max_foot_cops = 2;

    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        auto& list = ctx.crime_case->consolidated_criminals;
        auto& is_fire_list = ctx.crime_case->criminal_is_firearm;
        
        int total_criminals = list.size();
        int armed_criminals = 0;
        for (size_t idx = 0; idx < list.size(); ++idx) {
            if (idx < is_fire_list.size() && is_fire_list[idx]) {
                armed_criminals++;
            }
        }
        
        int cops_killed = ctx.crime_case->cops_killed;
        
        if (cops_killed > 0) {
            ctx.max_vehicles = 3;
            ctx.max_foot_cops = 4;
            LOGI("📊 [dispatchCenter - Quota] Escalated to Max quota (Vehicles=%d, FootCops=%d) due to cop casualties (%d)", 
                 ctx.max_vehicles, ctx.max_foot_cops, cops_killed);
        } else if (armed_criminals == 0) {
            if (total_criminals <= 1) {
                ctx.max_vehicles = 1;
                ctx.max_foot_cops = 1;
            } else {
                ctx.max_vehicles = 2;
                ctx.max_foot_cops = 2;
            }
            LOGI("📊 [dispatchCenter - Quota] Melee-only crime. Quota set to (Vehicles=%d, FootCops=%d) for %d criminals", 
                 ctx.max_vehicles, ctx.max_foot_cops, total_criminals);
        } else {
            float armed_ratio = (float)armed_criminals / (float)total_criminals;
            if (armed_ratio < 0.5f) {
                ctx.max_vehicles = 2;
                ctx.max_foot_cops = 2;
            } else {
                if (total_criminals <= 2) {
                    ctx.max_vehicles = 2;
                    ctx.max_foot_cops = 3;
                } else {
                    ctx.max_vehicles = 3;
                    ctx.max_foot_cops = 4;
                }
            }
            LOGI("📊 [dispatchCenter - Quota] Firearm crime (Armed: %d/%d, Ratio: %.1f%%). Quota set to (Vehicles=%d, FootCops=%d)", 
                 armed_criminals, total_criminals, armed_ratio * 100.0f, ctx.max_vehicles, ctx.max_foot_cops);
        }
    } else {
        ctx.max_vehicles = 2;
        ctx.max_foot_cops = 2;
    }

}

void cop_attack_commit_pending(CopAttackContext& ctx) {
    if (!ctx.pending_armed_cops_time.empty()) {
        std::lock_guard<std::mutex> lock(g_armed_cops_mutex);
        for (const auto& item : ctx.pending_armed_cops_time) {
            g_armed_cops_time[item.first] = item.second;
        }
    }
    if (!ctx.pending_cop_assigned_weapon.empty()) {
        std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
        for (const auto& item : ctx.pending_cop_assigned_weapon) {
            g_cop_assigned_weapon[item.first] = item.second;
        }
    }
    if (!ctx.pending_dispatched_vehicles_time.empty()) {
        std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
        for (const auto& item : ctx.pending_dispatched_vehicles_time) {
            g_dispatched_vehicles_time[item.first] = item.second;
        }
    }
    if (!ctx.pending_vehicles_emptied.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
        for (void* veh : ctx.pending_vehicles_emptied) {
            g_vehicles_emptied.insert(veh);
        }
    }
    if (!ctx.pending_vehicles_ordered_to_scene.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_mutex);
        for (void* veh : ctx.pending_vehicles_ordered_to_scene) {
            g_vehicles_ordered_to_scene.insert(veh);
        }
    }
    if (!ctx.pending_vehicles_siren_awakened.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
        for (void* veh : ctx.pending_vehicles_siren_awakened) {
            g_vehicles_siren_awakened.insert(veh);
        }
    }
    if (!ctx.pending_stuck_vehicles.empty()) {
        std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
        for (const auto& item : ctx.pending_stuck_vehicles) {
            g_stuck_vehicles[item.first] = item.second;
        }
    }
    if (!ctx.pending_cop_attack_assign_time.empty()) {
        std::lock_guard<std::mutex> lock(g_cop_attack_assign_mutex);
        for (const auto& item : ctx.pending_cop_attack_assign_time) {
            g_cop_attack_assign_time[item.first] = item.second;
        }
    }
    if (!ctx.pending_temp_closures.empty()) {
        std::lock_guard<std::mutex> lock(g_temp_closures_mutex);
        for (const auto& item : ctx.pending_temp_closures) {
            g_temp_road_closures.push_back(item);
        }
    }

}
