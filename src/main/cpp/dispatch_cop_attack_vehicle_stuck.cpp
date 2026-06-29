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


void cop_attack_vehicle_stuck_monitor(
    CopAttackContext& ctx,
    CopAttackVehicleSession& session,
    CPed* ped,
    void* veh,
    CPed* target_criminal,
    CVector target_crime_pos) {
                                    session.current_pos = get_entity_pos(veh);
                                    session.now_time = now_ms();
                                    bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                    if (is_bike && g_GetMatrix) {
                                        CMatrix* mat = g_GetMatrix(veh);
                                        if (mat && mat->at_z < 0.8f) { // Tilting > 36.8 degrees
                                            stabilize_motorcycle(veh);
                                        }
                                    }

                                    session.found_stuck = false;
                                    session.stuck_idx = 0;
                                    for (size_t idx = 0; idx < ctx.stuck_vehicles_snapshot.size(); ++idx) {
                                        if (ctx.stuck_vehicles_snapshot[idx].first == veh) {
                                            session.tracker = ctx.stuck_vehicles_snapshot[idx].second;
                                            session.found_stuck = true;
                                            session.stuck_idx = idx;
                                            break;
                                        }
                                    }

                                    if (!session.found_stuck) {
                                        session.tracker.last_pos = session.current_pos;
                                        session.tracker.last_check_time = session.now_time;
                                        session.tracker.stuck_since = 0;
                                        session.tracker.last_intervention_time = 0;
                                        
                                        ctx.stuck_vehicles_snapshot.push_back({veh, session.tracker}); // 局部同步
                                        ctx.pending_stuck_vehicles.push_back({veh, session.tracker});
                                    } else {
                                        float time_diff = (session.now_time - session.tracker.last_check_time) / 1000.0f;
                                        if (time_diff >= 1.0f) { // 每秒检查一次
                                            float dx_s = session.current_pos.x - session.tracker.last_pos.x;
                                            float dy_s = session.current_pos.y - session.tracker.last_pos.y;
                                            float dz_s = session.current_pos.z - session.tracker.last_pos.z;
                                            float dist_moved = sqrtf(dx_s * dx_s + dy_s * dy_s + dz_s * dz_s);
                                            float speed = dist_moved / time_diff;

                                            // [Proactive Water Escape]: Hollywood cinematic shoreline rescue
                                            if (speed >= 3.0f && dist_moved > 0.1f) {
                                                float dir_x = dx_s / dist_moved;
                                                float dir_y = dy_s / dist_moved;
                                                float dir_z = dz_s / dist_moved;

                                                float lookahead = speed * 1.5f; // Lookahead 1.5 seconds
                                                CVector p_pos = {
                                                    session.current_pos.x + dir_x * lookahead,
                                                    session.current_pos.y + dir_y * lookahead,
                                                    session.current_pos.z + dir_z * lookahead
                                                };

                                                // If predicted to hit low elevation (water level), but perp is on land and veh is currently safe
                                                bool will_fall = (p_pos.z < 1.0f && session.current_pos.z >= 2.0f && target_crime_pos.z >= 2.5f);
                                                if (will_fall && !ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh)) {
                                                    if (g_VehicleInflictDamage) {
                                                        g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, session.current_pos);
                                                    }
                                                    ctx.vehicles_emptied_snapshot.push_back(veh);
                                                    ctx.pending_vehicles_emptied.push_back(veh);
                                                    if (veh == ctx.crime_case->spawned_vehicle) {
                                                        if (ctx.crime_case) ctx.crime_case->occupants_ordered_out = true;
                                                    }
                                                    LOGW("[dispatchCenter - ProactiveWaterRescue] CINEMATIC RESCUE! Vehicle %p predicted to plunge into deep water. Safe bulk exit triggered!", veh);
                                                }
                                            }

                                            // [Anti-Spin Guard]: Detect circling behavior (crucial for motorcycles)
                                            float cur_dir_x = 0.0f;
                                            float cur_dir_y = 0.0f;
                                            if (dist_moved > 0.05f) {
                                                cur_dir_x = dx_s / dist_moved;
                                                cur_dir_y = dy_s / dist_moved;
                                            }

                                            if (dist_moved > 0.5f) {
                                                if (session.tracker.last_dir_x != 0.0f || session.tracker.last_dir_y != 0.0f) {
                                                    float dir_dot = cur_dir_x * session.tracker.last_dir_x + cur_dir_y * session.tracker.last_dir_y;
                                                    if (dir_dot < 0.85f) { // 宽限至 31.8度角，更容易灵敏捕获摩托画圈
                                                        session.tracker.spin_count++;
                                                    } else {
                                                        session.tracker.spin_count = 0;
                                                    }
                                                }
                                                session.tracker.last_dir_x = cur_dir_x;
                                                session.tracker.last_dir_y = cur_dir_y;
                                            } else {
                                                session.tracker.spin_count = 0;
                                            }

                                            if (session.tracker.spin_count >= 3) {
                                                float dx_vc = target_crime_pos.x - session.current_pos.x;
                                                float dy_vc = target_crime_pos.y - session.current_pos.y;
                                                float dz_vc = target_crime_pos.z - session.current_pos.z;
                                                float dist_vc = sqrtf(dx_vc * dx_vc + dy_vc * dy_vc + dz_vc * dz_vc);

                                                if (dist_vc < 60.0f) {
                                                    // A. Close range: Force immediate emergency exit
                                                    if (!ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh)) {
                                                        g_GetCarToGoToCoors(veh, &session.current_pos, 4, false); // Emergency handbrake stop
                                                        if (g_TellOccupantsToLeaveCar) {
                                                            g_TellOccupantsToLeaveCar(veh);
                                                        }
                                                        ctx.vehicles_emptied_snapshot.push_back(veh);
                                                        ctx.pending_vehicles_emptied.push_back(veh);
                                                        if (veh == ctx.crime_case->spawned_vehicle) {
                                                            if (ctx.crime_case) ctx.crime_case->occupants_ordered_out = true;
                                                        }
                                                        LOGW("🔄 [Anti-Spin Guard] Circle spinning detected in close range (%.1fm). Safe bulk exit triggered!", dist_vc);
                                                    }
                                                } else {
                                                     // B. Far range: Force reverse nudge and physical 120-degree yaw rotation to break physical circling loop
                                                     bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                     if (is_bike) {
                                                         float dx_aim = target_crime_pos.x - session.current_pos.x;
                                                         float dy_aim = target_crime_pos.y - session.current_pos.y;
                                                         float dist_aim_2d = sqrtf(dx_aim * dx_aim + dy_aim * dy_aim);
                                                         if (dist_aim_2d > 0.01f) {
                                                             dx_aim /= dist_aim_2d;
                                                             dy_aim /= dist_aim_2d;
                                                         } else {
                                                             dx_aim = 0.0f;
                                                             dy_aim = 1.0f;
                                                         }

                                                         if (g_GetMatrix) {
                                                             CMatrix* mat = g_GetMatrix(veh);
                                                             if (mat) {
                                                                 mat->up_x = dx_aim;
                                                                 mat->up_y = dy_aim;
                                                                 mat->up_z = 0.0f;

                                                                 mat->at_x = 0.0f;
                                                                 mat->at_y = 0.0f;
                                                                 mat->at_z = 1.0f;

                                                                 mat->right_x = dy_aim;
                                                                 mat->right_y = -dx_aim;
                                                                 mat->right_z = 0.0f;
                                                             }
                                                         }

                                                         CVector realigned_pos = {
                                                             session.current_pos.x + dx_aim * 5.0f,
                                                             session.current_pos.y + dy_aim * 5.0f,
                                                             session.current_pos.z + 0.15f
                                                         };
                                                         
                                                         set_entity_pos(veh, realigned_pos);
                                                         stabilize_motorcycle(veh);
                                                         
                                                         command_vehicle_ai(veh, target_crime_pos, dist_vc);
                                                         LOGW("🔄🏍️ [Anti-Spin Guard - BIKE] Realignment Orbit Break. Pointed directly to crime scene (%.1fm) and teleported forward 5m.", dist_vc);
                                                     } else {
                                                         bool is_seen = is_cop_visible_to_player(veh, session.current_pos.x, session.current_pos.y, session.current_pos.z);
                                                         float f_x = 0.0f, f_y = 0.0f, f_z = 0.0f;
                                                         if (g_GetMatrix) {
                                                             CMatrix* mat = g_GetMatrix(veh);
                                                             if (mat) {
                                                                 f_x = mat->up_x;
                                                                 f_y = mat->up_y;
                                                                 f_z = mat->up_z;

                                                                 if (!is_seen) {
                                                                     // 物理朝向旋转 120 度打破画圆惯性：cos = -0.5, sin = 0.866
                                                                     float new_up_x = mat->up_x * (-0.5f) - mat->up_y * 0.866f;
                                                                     float new_up_y = mat->up_x * 0.866f + mat->up_y * (-0.5f);
                                                                     float new_right_x = mat->right_x * (-0.5f) - mat->right_y * 0.866f;
                                                                     float new_right_y = mat->right_x * 0.866f + mat->right_y * (-0.5f);

                                                                     mat->up_x = new_up_x;
                                                                     mat->up_y = new_up_y;
                                                                     mat->right_x = new_right_x;
                                                                     mat->right_y = new_right_y;
                                                                 }
                                                             }
                                                         }
                                                         float f_len = sqrtf(f_x * f_x + f_y * f_y + f_z * f_z);
                                                         if (f_len > 0.01f) {
                                                             f_x /= f_len; f_y /= f_len; f_z /= f_len;
                                                         } else {
                                                             f_x = 0.0f; f_y = 1.0f; f_z = 0.0f;
                                                         }
                                                         CVector reverse_pos;
                                                         if (is_seen) {
                                                             reverse_pos = {
                                                                 session.current_pos.x - f_x * 3.5f,
                                                                 session.current_pos.y - f_y * 3.5f,
                                                                 session.current_pos.z + 0.3f
                                                             };
                                                         } else {
                                                             reverse_pos = {
                                                                 session.current_pos.x - f_x * 4.5f,
                                                                 session.current_pos.y - f_y * 4.5f,
                                                                 session.current_pos.z + 0.5f
                                                             };
                                                         }
                                                         
                                                         set_entity_pos(veh, reverse_pos);
                                                         
                                                         command_vehicle_ai(veh, target_crime_pos, dist_vc);
                                                         LOGW("🔄 [Anti-Spin Guard] Circle spinning detected in far range (%.1fm, seen=%d). Forced reverse nudge & yaw break.", dist_vc, is_seen);
                                                     }}
                                                session.tracker.spin_count = 0;
                                                session.tracker.last_intervention_time = session.now_time;
                                            }

                                            // [ACC Unified Fleet Control - Upgraded to Full Fleet & Bypass Nudge]: Prevents front and side rear-ends of any vehicles (Unrestricted by movement)
                                            float dir_x = 0.0f;
                                            float dir_y = 0.0f;
                                            float dir_z = 0.0f;
                                            if (dist_moved > 0.05f) {
                                                dir_x = dx_s / dist_moved;
                                                dir_y = dy_s / dist_moved;
                                                dir_z = dz_s / dist_moved;
                                            } else {
                                                if (g_GetMatrix) {
                                                    CMatrix* mat = g_GetMatrix(veh);
                                                    if (mat) {
                                                        dir_x = mat->up_x;
                                                        dir_y = mat->up_y;
                                                        dir_z = mat->up_z;
                                                    }
                                                }
                                                float d_len = sqrtf(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);
                                                if (d_len > 0.01f) {
                                                    dir_x /= d_len;
                                                    dir_y /= d_len;
                                                    dir_z /= d_len;
                                                } else {
                                                    dir_x = 0.0f;
                                                    dir_y = 1.0f;
                                                    dir_z = 0.0f;
                                                }
                                            }

                                            // =====================================================================
                                            // 🚨 [dispatchCenter - ACC Criminal Avoidance] 罪犯高优先级物理避让，保障绝不撞击/怼罪犯
                                            // =====================================================================
                                            bool ped_blocked = false;
                                            if (target_criminal && is_ped_pointer_valid_safe(target_criminal)) {
                                                CVector other_pos = get_entity_pos(target_criminal);
                                                float ox = other_pos.x - session.current_pos.x;
                                                float oy = other_pos.y - session.current_pos.y;
                                                float oz = other_pos.z - session.current_pos.z;
                                                float cop_dist = sqrtf(ox * ox + oy * oy + oz * oz);

                                                if (cop_dist < 10.0f) {
                                                    float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                    if (dot_p > 0.0f && dot_p < 10.0f) {
                                                        float lat_dist_sq = (cop_dist * cop_dist) - (dot_p * dot_p);
                                                        if (lat_dist_sq < 4.84f) { // 2.2米内横向偏离（直接阻挡在行进轨迹上）
                                                            ped_blocked = true;
                                                            if ((speed < 1.2f || dist_moved < 1.2f) && cop_dist < 8.0f) {
                                                                float side_sign = (((uintptr_t)veh) & 1) ? 1.0f : -1.0f;
                                                                float lx = -dir_y * side_sign;
                                                                float ly = dir_x * side_sign;
                                                                bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                float side_nudge = is_bike ? 0.8f : 1.8f;
                                                                float height_lift = is_bike ? 0.05f : 0.0f;
                                                                CVector detour_pos = {
                                                                    session.current_pos.x + lx * side_nudge + dir_x * 0.8f,
                                                                    session.current_pos.y + ly * side_nudge + dir_y * 0.8f,
                                                                    session.current_pos.z + height_lift
                                                                };
                                                                set_entity_pos(veh, detour_pos);
                                                                if (is_bike) {
                                                                    stabilize_motorcycle(veh);
                                                                }LOGI("[dispatchCenter - ACC Bypass Ped] Vehicle %p blocked by TARGET CRIMINAL %p (dist=%.1f, speed=%.2f). Detoured (side=%.1f)", 
                                                                     veh, target_criminal, cop_dist, speed, side_sign);
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            if (!ped_blocked && g_ms_pVehiclePool && g_GetPoolVehicle) {
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
                                                                    CVector other_pos = get_entity_pos(other_veh);
                                                                    float ox = other_pos.x - session.current_pos.x;
                                                                    float oy = other_pos.y - session.current_pos.y;
                                                                    float oz = other_pos.z - session.current_pos.z;
                                                                    float cop_dist = sqrtf(ox * ox + oy * oy + oz * oz);

                                                                    if (cop_dist < 10.0f) {
                                                                        float dot_p = ox * dir_x + oy * dir_y + oz * dir_z;
                                                                        if (dot_p > 0.0f && dot_p < 10.0f) {
                                                                            float lat_dist_sq = (cop_dist * cop_dist) - (dot_p * dot_p);
                                                                            if (lat_dist_sq < 4.84f) { // Under 2.2m lateral deviation (directly in path)
                                                                                // Dynamic smart bypass detour nudge if extremely slow or stationary (stuck behind civilians)
                                                                                if ((speed < 1.2f || dist_moved < 1.2f) && cop_dist < 8.0f) {
                                                                                    float side_sign = (((uintptr_t)veh) & 1) ? 1.0f : -1.0f;
                                                                                    float lx = -dir_y * side_sign;
                                                                                    float ly = dir_x * side_sign;
                                                                                    bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                                    float side_nudge = is_bike ? 0.8f : 1.8f;
                                                                                    float height_lift = is_bike ? 0.05f : 0.0f;
                                                                                    CVector detour_pos = {
                                                                                        session.current_pos.x + lx * side_nudge + dir_x * 0.8f,
                                                                                        session.current_pos.y + ly * side_nudge + dir_y * 0.8f,
                                                                                        session.current_pos.z + height_lift
                                                                                    };
                                                                                    
                                                                                    set_entity_pos(veh, detour_pos);
                                                                                    if (is_bike) {
                                                                                        stabilize_motorcycle(veh);
                                                                                    }
                                                                                    LOGI("[dispatchCenter - ACC Bypass] Vehicle %p blocked by %p (dist=%.1f, speed=%.2f). Smooth detour nudge (side=%.1f) by 1.8m side, 0.8m forward.", 
                                                                                         veh, other_veh, cop_dist, speed, side_sign);
                                                                                }
                                                                                break; // Handle one roadblock vehicle per tick to avoid jitter
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            // =====================================================================
                                            // 🔥 [dispatchCenter - Fire Source Detection & Avoidance] 火源自动检测与避让系统
                                            // =====================================================================
                                            if (g_FireManager && g_FindNearestFire) {
                                                void* nearest_fire = g_FindNearestFire(g_FireManager, session.current_pos, true, true);
                                                if (nearest_fire) {
                                                    CVector fire_pos;
                                                    if (get_fire_position(nearest_fire, fire_pos)) {
                                                        float fx = fire_pos.x - session.current_pos.x;
                                                        float fy = fire_pos.y - session.current_pos.y;
                                                        float fz = fire_pos.z - session.current_pos.z;
                                                        float fire_dist = sqrtf(fx * fx + fy * fy + fz * fz);

                                                        if (fire_dist < 15.0f) {
                                                            // A. 被火源包围/受困紧急逃生 (距离过近且处于低速/停滞状态)
                                                            if (fire_dist < 6.5f && speed < 2.0f) {
                                                                if (!ctx.vector_contains(ctx.vehicles_emptied_snapshot, veh)) {
                                                                    if (g_GetCarToGoToCoors) {
                                                                        g_GetCarToGoToCoors(veh, &session.current_pos, 4, false); // 瞬间急刹
                                                                    }
                                                                    if (g_TellOccupantsToLeaveCar) {
                                                                        g_TellOccupantsToLeaveCar(veh); // 强令离开，防烧死
                                                                    }
                                                                    if (g_VehicleInflictDamage) {
                                                                        g_VehicleInflictDamage(veh, target_criminal ? reinterpret_cast<CEntity*>(target_criminal) : nullptr, WEAPON_UNARMED, 0.0f, session.current_pos);
                                                                    }
                                                                    ctx.vehicles_emptied_snapshot.push_back(veh);
                                                                    ctx.pending_vehicles_emptied.push_back(veh);
                                                                    if (veh == ctx.crime_case->spawned_vehicle) {
                                                                        if (ctx.crime_case) ctx.crime_case->occupants_ordered_out = true;
                                                                    }
                                                                    LOGW("🔥 [dispatchCenter - FireEmergencyExit] TRAPPED BY FIRE! Vehicle %p is too close to fire (dist=%.1f, speed=%.2f). Safe bulk exit triggered!", veh, fire_dist, speed);
                                                                }
                                                            } 
                                                            // B. 正常行驶中火源主动避让 (火源处于行进路线上)
                                                            else {
                                                                float dot_p = fx * dir_x + fy * dir_y + fz * dir_z;
                                                                if (dot_p > 0.0f && dot_p < 15.0f) { // 火源在车头15米内
                                                                    float lat_dist_sq = (fire_dist * fire_dist) - (dot_p * dot_p);
                                                                    if (lat_dist_sq < 25.0f) { // 横向偏离在5.0米内 (直接物理阻挡)
                                                                        // 计算垂直向量决定往哪侧闪避
                                                                        float lx = -dir_y;
                                                                        float ly = dir_x;
                                                                        float side_dot = fx * lx + fy * ly;
                                                                        float side_sign = (side_dot > 0.0f) ? -1.0f : 1.0f; // 避开火源那一侧

                                                                        // 计算安全的避让偏离点并微调位置重设 autopilot
                                                                        bool is_bike = (get_entity_model_index(veh) == MODEL_POLICE_BIKE);
                                                                        float side_nudge = is_bike ? 2.5f : 5.5f;
                                                                        float height_lift = is_bike ? 0.05f : 0.0f;
                                                                        CVector detour_pos = {
                                                                            session.current_pos.x + lx * side_sign * side_nudge - dir_x * 1.5f,
                                                                            session.current_pos.y + ly * side_sign * side_nudge - dir_y * 1.5f,
                                                                            session.current_pos.z + height_lift
                                                                        };

                                                                        set_entity_pos(veh, detour_pos);
                                                                        if (is_bike) {
                                                                            stabilize_motorcycle(veh);
                                                                        }LOGW("🔥 [dispatchCenter - FireAvoidanceDetour] Vehicle %p blocked by fire (dist=%.1f) in front. Detouring to safe side (side_sign=%.1f).", veh, fire_dist, side_sign);
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            // Double-insurance stuck detection: low speed OR very low physical physical movement distance in 1 sec (crucial for narrow U-Turns)
                                            if (speed < 0.8f || dist_moved < 1.2f) {
                                                if (session.tracker.stuck_since == 0) {
                                                    session.tracker.stuck_since = session.now_time;
                                                }
                                            } else {
                                                session.tracker.stuck_since = 0; // Moving normally, reset stuck timer
                                            }

                                            session.tracker.last_pos = session.current_pos;
                                            session.tracker.last_check_time = session.now_time;
                                            
                                            ctx.stuck_vehicles_snapshot[session.stuck_idx].second = session.tracker; // Local sync
                                            ctx.pending_stuck_vehicles.push_back({veh, session.tracker});
                                        }
                                    }

}
