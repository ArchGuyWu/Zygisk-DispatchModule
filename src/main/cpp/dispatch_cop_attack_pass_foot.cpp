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


void cop_attack_dispatch_foot_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    CVector target_crime_pos,
    float dist_sq) {
    CVector cop_pos = get_entity_pos(ped);
                    // 地面警员只在 70m 内响应（保持最高效追杀半径）
                    if (dist_sq > 70.0f * 70.0f) {
                        return;
                    }

                    // =====================================================================
                    // 地面警员的事件驱动型分派调度与高频限流控制 (3 秒限频 & 15 秒存留状态查询)
                    // =====================================================================
                    int64_t last_assign = 0;
                    auto key_assign = std::make_pair(ped, target_criminal);
                    for (const auto& item : ctx.cop_attack_assign_time_snapshot) {
                        if (item.first == key_assign) {
                            last_assign = item.second;
                            break;
                        }
                    }

                    bool already_targeting = false;
                    if (g_GetWeaponLockOnTarget && g_GetWeaponLockOnTarget(ped) == reinterpret_cast<CEntity*>(target_criminal)) {
                        already_targeting = true;
                    }

                    // 提前确定分配的目标武器，判断是否是近战武器，用于高频打断防护
                    bool is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
                    eWeaponType target_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);
                    bool is_melee = (target_weapon == WEAPON_NIGHTSTICK || target_weapon == WEAPON_UNARMED || target_weapon < 22);

                    // =====================================================================
                    // 🚀 【核心修复】动态战术武器切换逻辑（针对已锁定目标/已处于响应的警员）
                    // =====================================================================
                    bool is_assigned_or_targeting = already_targeting || (now_ms() - last_assign < 15000);
                    if (is_assigned_or_targeting) {
                        int64_t last_armed = 0;
                        for (const auto& item : ctx.armed_cops_time_snapshot) {
                            if (item.first == ped) {
                                last_armed = item.second;
                                break;
                            }
                        }

                        eWeaponType last_assigned_weapon = WEAPON_UNARMED;
                        for (const auto& item : ctx.cop_assigned_weapon_snapshot) {
                            if (item.first == ped) {
                                last_assigned_weapon = item.second;
                                break;
                            }
                        }

                        // 如果是要切成手枪，而当前又是警棍/无武器，则说明是紧急枪械升级，必须强行无视 1.5 秒冷却，确保瞬间切枪
                        bool is_upgrading_to_firearm = (target_weapon == WEAPON_PISTOL && last_assigned_weapon != WEAPON_PISTOL);
                        if (now_ms() - last_armed > 1500 || is_upgrading_to_firearm) {
                            if (target_weapon != last_assigned_weapon) {
                                if (g_GiveWeapon && g_SetCurrentWeapon) {
                                    g_GiveWeapon(ped, target_weapon, 9999, true);
                                    g_SetCurrentWeapon(ped, target_weapon);
                                }
                                LOGI("🔄 [Weapon Dynamic Switch] Ground cop %p switched weapon from %d to %d (dist=%.1f, bypass_cooldown=%d)", 
                                     ped, last_assigned_weapon, target_weapon, sqrtf(dist_sq), is_upgrading_to_firearm);

                                // 局部同步更新
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

                                // 更新上次武装时间
                                bool found_t = false;
                                for (auto& item : ctx.armed_cops_time_snapshot) {
                                    if (item.first == ped) {
                                        item.second = now_ms();
                                        found_t = true;
                                        break;
                                    }
                                }
                                if (!found_t) {
                                    ctx.armed_cops_time_snapshot.push_back({ped, now_ms()});
                                }
                                ctx.pending_armed_cops_time.push_back({ped, now_ms()});
                            }
                        }
                    }

                    bool just_exited_vehicle = is_cop_currently_exiting(ped);
                    if (!just_exited_vehicle) {
                        auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
                        if (cop_comp && cop_comp->has_exited_vehicle) {
                            just_exited_vehicle = true;
                        }
                    }

                    // 【核心近战防抖限流控制】：由于近战没有 LockOnTarget 瞄准状态，already_targeting 始终为 false。
                    // 因而，对近战警员采用 15 秒（在场存留时限）长限流拦截；枪械警员继续保持 3 秒心跳唤醒。这可确保近战挥舞不被 0 伤物理受击打断！
                    if (!just_exited_vehicle &&
                        (already_targeting || (now_ms() - last_assign < (is_melee ? 15000 : 3000)))) {
                        return; // 3 秒（枪械）或 15 秒（近战）内已指派过，或者已在瞄准，跳过
                    }

                    // 判定是否是已经响应过的地面警员（使用 15 秒内分派或正在瞄准）
                    bool already_assigned_foot_cop = already_targeting || (now_ms() - last_assign < 15000);
                    if (!already_assigned_foot_cop && ctx.active_foot_cops_count >= ctx.max_foot_cops) {
                        // 超过地面警员配额上限且在现场目击范围 (30m) 之外，直接跳过，使其优雅走过
                        return;
                    }

                    // 原生 + 事件混合唤醒（地面警以事件兜底保证响应，不再因枪击案+玩家近场而跳过）
                    make_single_cop_attack_criminal(ped, target_criminal, false);

                    int64_t assign_time = now_ms();
                    bool found_assign = false;
                    for (auto& item : ctx.cop_attack_assign_time_snapshot) {
                        if (item.first == key_assign) {
                            item.second = assign_time;
                            found_assign = true;
                            break;
                        }
                    }
                    if (!found_assign) {
                        ctx.cop_attack_assign_time_snapshot.push_back({key_assign, assign_time});
                    }

                    is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
                    eWeaponType chosen_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);

                    bool found_w = false;
                    for (auto& item : ctx.cop_assigned_weapon_snapshot) {
                        if (item.first == ped) {
                            item.second = chosen_weapon;
                            found_w = true;
                            break;
                        }
                    }
                    if (!found_w) {
                        ctx.cop_assigned_weapon_snapshot.push_back({ped, chosen_weapon});
                    }

                    bool found_t = false;
                    for (auto& item : ctx.armed_cops_time_snapshot) {
                        if (item.first == ped) {
                            item.second = assign_time;
                            found_t = true;
                            break;
                        }
                    }
                    if (!found_t) {
                        ctx.armed_cops_time_snapshot.push_back({ped, assign_time});
                    }

                    if (!already_assigned_foot_cop) {
                        ctx.active_foot_cops_count++;
                    }
                    LOGI("🎯 [Foot Cop Dispatch] Native-first combat dispatch to cop %p -> criminal %p (active_foot_cops=%d/%d)",
                         ped, target_criminal, ctx.active_foot_cops_count, ctx.max_foot_cops);
}
