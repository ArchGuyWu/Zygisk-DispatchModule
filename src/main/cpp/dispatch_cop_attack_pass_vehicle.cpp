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
#include "dispatch_cop_state.hpp"


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

                            if (find_vehicle_of_cop_cached(ped) == nullptr && !is_cop_currently_exiting(ped)) {
                                int64_t last_armed = dispatch_cop_state::get_last_armed_ms(ped);
                                if (now_ms() - last_armed > 5000) {
                                    bool is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
                                    eWeaponType target_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);
                                    if (g_GiveWeapon && g_SetCurrentWeapon) {
                                        g_GiveWeapon(ped, target_weapon, 9999, true);
                                        g_SetCurrentWeapon(ped, target_weapon);
                                    }
                                    dispatch_cop_state::record_weapon(ped, target_weapon, now_ms());
                                }
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
                            float exit_dist = is_bike
                                ? (dispatch_timing::VEHICLE_BIKE_STAGING_OFFSET_M +
                                   dispatch_timing::VEHICLE_STAGING_EXIT_MARGIN_M)
                                : (dispatch_timing::VEHICLE_STAGING_OFFSET_M +
                                   dispatch_timing::VEHICLE_STAGING_EXIT_MARGIN_M);
                            // 视听内无条件下车参战；否则按距离/卡死/超时复合判定
                            bool should_exit = within_native_av ||
                                               (session.v_dist < exit_dist) ||
                                               (session.elapsed > dispatch_timing::VEHICLE_STUCK_EXIT_MS && session.v_dist < 60.0f) ||
                                               (session.elapsed > dispatch_timing::VEHICLE_MAX_APPROACH_MS);

                            if (should_exit) {
                                if (!ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh)) {
                                    dispatch_tell_occupants_to_leave_car(veh);
                                    ctx.vehicles_emptied_snapshot.push_back(veh);
                                    ctx.pending_vehicles_emptied.push_back(veh);

                                    if (veh == ctx.crime_case->spawned_vehicle) {
                                        if (ctx.crime_case) ctx.crime_case->occupants_ordered_out = true;
                                    }

                                    LOGI("Vehicle exit triggered (dist=%.1f, elapsed=%lld ms): Stopped & ordered leave %p (foot combat deferred)",
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

                            if (ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh) &&
                                !is_cop_currently_exiting(ped) &&
                                find_vehicle_of_cop_cached(ped) == nullptr &&
                                target_criminal && is_ped_pointer_valid_safe(target_criminal)) {
                                make_single_cop_attack_criminal(ped, target_criminal, true);
                            }
}
