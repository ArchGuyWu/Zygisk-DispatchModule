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


void cop_attack_vehicle_unstuck_intervene(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos) {
                                    // 卡死 3.5 秒以上，且距离上一次干预过去 6 秒以上，触发干预
                                    // =====================================================================
                                    // [Multi-Stage Unstucking System]: Water escape, stage 1 nudge, stage 2 warp.
                                    // =====================================================================
                                    float dx_v = target_crime_pos.x - session.current_pos.x;
                                    float dy_v = target_crime_pos.y - session.current_pos.y;
                                    float dz_v = target_crime_pos.z - session.current_pos.z;
                                    float dist_v = sqrtf(dx_v * dx_v + dy_v * dy_v + dz_v * dz_v);

                                    int64_t stuck_duration = (session.tracker.stuck_since > 0) ? (session.now_time - session.tracker.stuck_since) : 0;

                                    // A. Predictive/Active Water Rescue
                                    bool is_water_stuck = (session.current_pos.z < 1.0f && dist_v > 15.0f); 
                                    if (is_water_stuck && !ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh)) {
                                        if (g_VehicleInflictDamage) {
                                            g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, session.current_pos);
                                        }
                                        ctx.vehicles_emptied_snapshot.push_back(veh);
                                        ctx.pending_vehicles_emptied.push_back(veh);
                                        if (veh == ctx.crime_case->spawned_vehicle) {
                                            if (ctx.crime_case) ctx.crime_case->occupants_ordered_out = true;
                                        }
                                        LOGW("[dispatchCenter - WaterAvoidance] Vehicle %p at extreme low sea level (Z=%.2f). Emergency bulk exit!", veh, session.current_pos.z);
                                    }

                                    // B. Multi-Stage Intervention Trigger Decision
                                    bool trigger_stage1 = false;
                                    bool trigger_stage2 = false;

                                    bool cop_visible = is_cop_visible_to_player(veh, session.current_pos.x, session.current_pos.y, session.current_pos.z);

                                    if (stuck_duration >= 7000 && !cop_visible && dist_v > 40.0f) {
                                        trigger_stage2 = true;
                                    } else if (stuck_duration > 3500) {
                                        if (session.now_time - session.tracker.last_intervention_time > 6000) {
                                            trigger_stage1 = true;
                                        }
                                    }

                                    if (trigger_stage2) {
                                        session.tracker.last_intervention_time = session.now_time;
                                        session.tracker.stuck_since = 0; // Reset stuck timer on successful teleport

                                        if (session.found_stuck) {
                                            ctx.stuck_vehicles_snapshot[session.stuck_idx].second = session.tracker;
                                        } else {
                                            for (auto& item : ctx.stuck_vehicles_snapshot) {
                                                if (item.first == veh) {
                                                    item.second = session.tracker;
                                                    break;
                                                }
                                            }
                                        }
                                        ctx.pending_stuck_vehicles.push_back({veh, session.tracker});

                                        float warp_factor = 25.0f / dist_v;
                                        if (warp_factor > 0.8f) warp_factor = 0.5f; 
                                        bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                        float height_offset = is_bike ? 0.15f : 0.8f;
                                        CVector warp_pos = {
                                            session.current_pos.x + dx_v * warp_factor,
                                            session.current_pos.y + dy_v * warp_factor,
                                            session.current_pos.z + dz_v * warp_factor + height_offset
                                        };
                                        
                                        set_entity_pos(veh, warp_pos);
                                        if (is_bike) {
                                            stabilize_motorcycle(veh);
                                        }
                                        command_vehicle_ai(veh, target_crime_pos, dist_v);
                                        LOGI("[dispatchCenter - Stage 2 Warp] Teleported vehicle %p forward 25m to break deadlock. (visible=%d, stuck_duration=%lld ms)", 
                                             veh, cop_visible, (long long)stuck_duration);
                                    }
                                    else if (trigger_stage1) {
                                        session.tracker.last_intervention_time = session.now_time;

                                        if (session.found_stuck) {
                                            ctx.stuck_vehicles_snapshot[session.stuck_idx].second = session.tracker;
                                        } else {
                                            for (auto& item : ctx.stuck_vehicles_snapshot) {
                                                if (item.first == veh) {
                                                    item.second = session.tracker;
                                                    break;
                                                }
                                            }
                                        }
                                        ctx.pending_stuck_vehicles.push_back({veh, session.tracker});

                                        LOGI("[dispatchCenter - Stage 1 Nudge] Stuck rescue (Stage 1) initiated for %p (stuck_duration=%lld ms)...", veh, (long long)stuck_duration);

                                        // 1. Temporarily disable stuck route section within 20m
                                        if (g_ThePaths && g_SwitchRoadsOffInArea) {
                                            g_SwitchRoadsOffInArea(
                                                g_ThePaths,
                                                session.current_pos.x - 20.0f, session.current_pos.y - 20.0f, session.current_pos.z - 8.0f,
                                                session.current_pos.x + 20.0f, session.current_pos.y + 20.0f, session.current_pos.z + 8.0f,
                                                true, true, false
                                            );
                                            ctx.pending_temp_closures.push_back({session.current_pos, 20.0f, session.now_time + 15000});
                                        }

                                        // 2. Clear traffic obstacles within 15m
                                        if (g_ms_pVehiclePool && g_GetPoolVehicle) {
                                            void* v_pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
                                            if (v_pool) {
                                                char* v_byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(v_pool) + 8);
                                                int v_size = *reinterpret_cast<int*>(reinterpret_cast<char*>(v_pool) + 16);
                                                if (v_byte_map) {
                                                    for (int j = 0; j < v_size; j++) {
                                                        signed char v_flag = v_byte_map[j];
                                                        if (v_flag >= 0) {
                                                            int v_handle = (j << 8) | v_flag;
                                                            void* other_veh = g_GetPoolVehicle(v_handle);
                                                            if (other_veh && other_veh != veh && is_vehicle_pointer_valid(other_veh)) {
                                                                bool is_other_cop = ctx.vector_contains(ctx.vehicles_ordered_to_scene_snapshot, other_veh) || 
                                                                                    ctx.vector_contains(ctx.vehicles_siren_awakened_snapshot, other_veh) || 
                                                                                    other_veh == ctx.crime_case->spawned_vehicle;
                                                                if (!is_other_cop) {
                                                                    CVector other_pos = get_entity_pos(other_veh);
                                                                    float ov_dx = other_pos.x - session.current_pos.x;
                                                                    float ov_dy = other_pos.y - session.current_pos.y;
                                                                    float ov_dz = other_pos.z - session.current_pos.z;
                                                                    float ov_dist = sqrtf(ov_dx * ov_dx + ov_dy * ov_dy + ov_dz * ov_dz);

                                                                    if (ov_dist < 15.0f) {
                                                                        bool is_visible = is_pos_visible_to_player_camera(other_pos);
                                                                        if (!is_visible) {
                                                                            CVector far_away = {other_pos.x, other_pos.y, other_pos.z - 50.0f};
                                                                            set_entity_pos(other_veh, far_away);
                                                                            LOGI("   +- Traffic Blockage Cleared (Unseen): Teleported vehicle %p underground", other_veh);
                                                                        } else {
                                                                            LOGI("   +- Traffic Blockage Skipped (Seen): Avoid visible popping, waiting for player to look away");
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // 3. Reverse backing nudge if visible to pull away from obstacles, or forward warp if unseen
                                        if (dist_v > 5.0f) {
                                            CVector nudged_pos;
                                                                                         if (cop_visible) {
                                                                                             bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                             // Pull away backwards! Calculate current forward vector from matrix
                                                                                             float f_x = 0.0f, f_y = 0.0f, f_z = 0.0f;
                                                                                             if (g_GetMatrix) {
                                                                                                 CMatrix* mat = g_GetMatrix(veh);
                                                                                                 if (mat) {
                                                                                                     f_x = mat->up_x;
                                                                                                     f_y = mat->up_y;
                                                                                                     f_z = mat->up_z;
                                                                                                 }
                                                                                             }
                                                                                             float f_len = sqrtf(f_x * f_x + f_y * f_y + f_z * f_z);
                                                                                             if (f_len > 0.01f) {
                                                                                                 f_x /= f_len; f_y /= f_len; f_z /= f_len;
                                                                                             } else {
                                                                                                 f_x = 0.0f; f_y = 1.0f; f_z = 0.0f;
                                                                                             }
                                                                                             // Nudge backwards by 3.5 meters, lift by 0.60m or 0.12m for bike
                                                                                             float height_lift = is_bike ? 0.12f : 0.60f;
                                                                                             nudged_pos.x = session.current_pos.x - f_x * 3.5f;
                                                                                             nudged_pos.y = session.current_pos.y - f_y * 3.5f;
                                                                                             nudged_pos.z = session.current_pos.z + height_lift;
                                                                                             LOGI("   +- Nudged vehicle %p BACKWARDS 3.5m and elevated %.2fm to pull away from obstacles (visible).", veh, height_lift);
                                                                                         } else {
                                                                                             bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                             // Unseen: Nudge towards destination forward by 12.0m to cross walls quickly
                                                                                             float nx = dx_v / dist_v;
                                                                                             float ny = dy_v / dist_v;
                                                                                             float nz = dz_v / dist_v;
                                                                                             float height_lift = is_bike ? 0.15f : 0.75f;
                                                                                             nudged_pos.x = session.current_pos.x + nx * 12.0f;
                                                                                             nudged_pos.y = session.current_pos.y + ny * 12.0f;
                                                                                             nudged_pos.z = session.current_pos.z + nz * 0.1f + height_lift;
                                                                                             LOGI("   +- Nudged vehicle %p FORWARD 12.0m (unseen warp) and elevated %.2fm to bypass obstacles.", veh, height_lift);
                                                                                         }

                                                                                         bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                         
                                                                                         set_entity_pos(veh, nudged_pos);
                                                                                         if (is_bike) {
                                                                                             stabilize_motorcycle(veh);
                                                                                         }
                                                                                         
                                                                                         command_vehicle_ai(veh, target_crime_pos, dist_v);
                                        }
                                    }
}
