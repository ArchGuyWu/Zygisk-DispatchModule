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
#include "dispatch_cop_attack_vehicle_internal.hpp"
#include "dispatch_timing.hpp"


void cop_attack_dispatch_vehicle_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    CVector target_crime_pos,
    void* veh) {
    if (!is_vehicle_pointer_valid(veh)) {
        return;
    }

    CopAttackVehicleSession session;
    session.veh_pos = get_entity_pos(veh);
    float v_dx = session.veh_pos.x - target_crime_pos.x;
    float v_dy = session.veh_pos.y - target_crime_pos.y;
    float v_dz = session.veh_pos.z - target_crime_pos.z;
    session.v_dist = sqrtf(v_dx * v_dx + v_dy * v_dy + v_dz * v_dz);

                            int64_t last_armed = 0;
                            for (const auto& item : ctx.armed_cops_time_snapshot) {
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
                                for (auto& item : ctx.armed_cops_time_snapshot) {
                                    if (item.first == ped) {
                                        item.second = now_ms();
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    ctx.armed_cops_time_snapshot.push_back({ped, now_ms()});
                                }
                                ctx.pending_armed_cops_time.push_back({ped, now_ms()});

                                bool found_w = false;
                                for (auto& item : ctx.cop_assigned_weapon_snapshot) {
                                    if (item.first == ped) {
                                        item.second = target_weapon;
                                        found_w = true;
                                        break;
                                    }
                                }
                                if (!found_w) {
                                    ctx.cop_assigned_weapon_snapshot.push_back({ped, target_weapon});
                                }
                                ctx.pending_cop_assigned_weapon.push_back({ped, target_weapon});
                            }

                            session.first_seen = 0;
                            bool found_disp = false;
                            for (const auto& item : ctx.dispatched_vehicles_time_snapshot) {
                                if (item.first == veh) {
                                    session.first_seen = item.second;
                                    found_disp = true;
                                    break;
                                }
                            }
                            if (!found_disp) {
                                session.now_time = now_ms();
                                ctx.dispatched_vehicles_time_snapshot.push_back({veh, session.now_time});
                                ctx.pending_dispatched_vehicles_time.push_back({veh, session.now_time});
                                session.first_seen = session.now_time;
                            }
                            session.elapsed = now_ms() - session.first_seen;

                            float av_range = ctx.av_range_sq > 0.0f
                                ? sqrtf(ctx.av_range_sq)
                                : dispatch_timing::AV_RANGE_FIREARM_M;
                            bool within_native_av = session.v_dist < av_range;

                            bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                            float exit_dist = is_bike ? 12.0f : 16.0f;
                            // 视听内无条件下车参战；否则按距离/卡死/超时复合判定
                            bool should_exit = within_native_av ||
                                               (session.v_dist < exit_dist) ||
                                               (session.elapsed > dispatch_timing::VEHICLE_STUCK_EXIT_MS && session.v_dist < 60.0f) ||
                                               (session.elapsed > dispatch_timing::VEHICLE_MAX_APPROACH_MS);

                            if (should_exit) {
                                if (!ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh)) {
                                    // 1. 原地目标诱骗急刹：将 Autopilot 目标设为当前坐标，让其平稳减速刹停，绝不上人行道
                                    if (g_GetCarToGoToCoors) {
                                        g_GetCarToGoToCoors(veh, &session.veh_pos, 4, false); // Mode 4 (DF_STOP_CAR) 瞬间手刹锁死
                                    }
                                    // 2. 战术提前离车：拉开交火线包抄
                                    dispatch_tell_occupants_to_leave_car(veh);
                                    if (g_VehicleInflictDamage) {
                                        g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, session.veh_pos);
                                    }
                                    ctx.vehicles_emptied_snapshot.push_back(veh); // 局部同步
                                    ctx.pending_vehicles_emptied.push_back(veh);

                                    if (veh == ctx.crime_case->spawned_vehicle) {
                                        if (ctx.crime_case) ctx.crime_case->occupants_ordered_out = true;
                                    }

                                    if (target_criminal && is_ped_pointer_valid_safe(target_criminal)) {
                                        make_single_cop_attack_criminal(ped, target_criminal, true);
                                        LOGI("Vehicle exit combat dispatch: Assigned cop %p to attack criminal %p after leaving %p",
                                             ped, target_criminal, veh);
                                    }

                                    LOGI("Vehicle exit triggered (dist=%.1f, elapsed=%lld ms): Stopped Autopilot & Ordered cops to leave vehicle %p", 
                                         session.v_dist, (long long)session.elapsed, veh);
                                }
                            } else {

        session.already_dispatched = ctx.vector_contains(ctx.vehicles_ordered_to_scene_snapshot, veh) ||
            ctx.vector_contains(ctx.vehicles_siren_awakened_snapshot, veh) ||
            veh == ctx.crime_case->spawned_vehicle;

        if (session.already_dispatched) {
            cop_attack_vehicle_stuck_monitor(ctx, session, ped, veh, target_criminal, target_crime_pos);
            cop_attack_vehicle_unstuck_intervene(ctx, session, ped, veh, target_criminal, target_crime_pos);
        }

        if (!within_native_av && !session.already_dispatched && ctx.active_vehicles_count >= ctx.max_vehicles) {
            return;
        }
        cop_attack_vehicle_initial_order(ctx, session, ped, veh, target_criminal, target_crime_pos);
    }
}
