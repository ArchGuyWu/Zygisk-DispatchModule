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


void cop_attack_vehicle_initial_order(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos) {
                                // 1. 命令车辆驶向现场（仅触发一次）
                                if (!ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh) && !ctx.vector_contains(ctx.vehicles_ordered_to_scene_snapshot, veh) && is_vehicle_occupied_by_driver(veh)) {
                                    command_vehicle_ai(veh, target_crime_pos, session.v_dist);
                                    ctx.vehicles_ordered_to_scene_snapshot.push_back(veh); // 局部同步
                                    ctx.pending_vehicles_ordered_to_scene.push_back(veh);

                                    if (!session.already_dispatched) {
                                        ctx.counted_vehicles.push_back(veh);
                                        ctx.active_vehicles_count++;
                                        session.already_dispatched = true;
                                    }
                                    LOGI("Vehicle order sent (dist=%.1f): Commanded vehicle %p to drive to scene (active_vehicles=%d/%d)", 
                                         session.v_dist, veh, ctx.active_vehicles_count, ctx.max_vehicles);
                                }

                                // 2. 动态警笛唤醒
                                if (!ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh) && session.v_dist < 90.0f && !ctx.vector_contains(ctx.vehicles_siren_awakened_snapshot, veh)) {
                                    if (g_GetCarToGoToCoors) {
                                        g_GetCarToGoToCoors(veh, &target_crime_pos, 0, false);
                                    }
                                    if (g_VehicleInflictDamage) {
                                        g_VehicleInflictDamage(veh, reinterpret_cast<CEntity*>(target_criminal), WEAPON_UNARMED, 0.0f, session.veh_pos);
                                    }
                                    ctx.vehicles_siren_awakened_snapshot.push_back(veh); // 局部同步
                                    ctx.pending_vehicles_siren_awakened.push_back(veh);

                                    if (!session.already_dispatched) {
                                        ctx.counted_vehicles.push_back(veh);
                                        ctx.active_vehicles_count++;
                                        session.already_dispatched = true;
                                    }
                                    LOGI("Vehicle siren awakened (dist=%.1f): Reset autopilot mission & Inflicted physical 0-damage (active_vehicles=%d/%d)", 
                                         session.v_dist, ctx.active_vehicles_count, ctx.max_vehicles);
                                }

}
