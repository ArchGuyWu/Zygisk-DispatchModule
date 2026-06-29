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
    const CVector& target_crime_pos,
    float dist_sq) {
                    // 地面警员只在 70m 内响应（保持最高效追杀半径）
                    if (dist_sq > 70.0f * 70.0f) {
                        continue;
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

                    // 【核心近战防抖限流控制】：由于近战没有 LockOnTarget 瞄准状态，already_targeting 始终为 false。
                    // 因而，对近战警员采用 15 秒（在场存留时限）长限流拦截；枪械警员继续保持 3 秒心跳唤醒。这可确保近战挥舞不被 0 伤物理受击打断！
                    if (already_targeting || (now_ms() - last_assign < (is_melee ? 15000 : 3000))) {
                        continue; // 3 秒（枪械）或 15 秒（近战）内已指派过，或者已在瞄准，跳过
                    }

                    // 40m 内的野生巡警纳入响应
                    bool is_extremely_nearby = false;
                    {
                        float p_dist = 99999.0f;
                        if (g_FindPlayerCoors) {
                            CVector player_pos = g_FindPlayerCoors(0);
                            float p_dx = cop_pos.x - player_pos.x;
                            float p_dy = cop_pos.y - player_pos.y;
                            float p_dz = cop_pos.z - player_pos.z;
                            p_dist = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
                        }
                        float c_dist = sqrtf(dist_sq);
                        if (c_dist < 40.0f || p_dist < 40.0f) {
                            is_extremely_nearby = true;
                        }
                    }

                    // 判定是否是已经响应过的地面警员（使用 15 秒内分派或正在瞄准）
                    bool already_assigned_foot_cop = already_targeting || (now_ms() - last_assign < 15000);
                    if (!already_assigned_foot_cop && ctx.active_foot_cops_count >= ctx.max_foot_cops) {
                        // 超过地面警员配额上限且在现场目击范围 (30m) 之外，直接跳过，使其优雅走过
                        continue;
                    }

                    // 避免穿帮音效：如果当前活跃案件是枪击案 (is_firearm == true)，听觉自然唤醒。但极近距离强行唤醒。
                    if (!is_extremely_nearby && g_crime_active.load() && ctx.crime_case->is_firearm && !ctx.crime_case->cancelled) {
                        if (g_FindPlayerCoors) {
                            CVector player_pos = g_FindPlayerCoors(0);
                            float p_dx = cop_pos.x - player_pos.x;
                            float p_dy = cop_pos.y - player_pos.y;
                            float p_dz = cop_pos.z - player_pos.z;
                            float p_dist = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
                            if (p_dist < 45.0f) {
                                continue;
                            }
                        }
                    }

                    // =====================================================================
                    // 智能双保险唤醒方案（假枪声为主，0 伤害物理注入为兜底备用）
                    // =====================================================================
                    bool dispatched_via_noise = false;
                    is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
                    eWeaponType chosen_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);

                    // 一律使用虚拟枪声事件进行静默、无穿帮唤醒（已集成 GMalloc，完全消除闪退风险）
                    if (g_CEventGunShot_ctor && g_CEventGunShot_dtor && g_CEventGroup_Add) {
                        void* event_group = get_ped_event_group(ped);
                        if (event_group) {
                            alignas(16) char event_buf[256];
                            memset(event_buf, 0, sizeof(event_buf));
                            CVector start_pos = cop_pos; // 惊雷在耳：声源直接设在警员耳边
                            CVector target_pos(cop_pos.x, cop_pos.y, cop_pos.z + 1.0f);
                            
                            g_CEventGunShot_ctor(event_buf, reinterpret_cast<CEntity*>(target_criminal), start_pos, target_pos, false);
                            g_CEventGroup_Add(event_group, event_buf, false);
                            g_CEventGunShot_dtor(event_buf);
                            
                            // 动态赋予最优战术武器
                            if (g_GiveWeapon && g_SetCurrentWeapon) {
                                g_GiveWeapon(ped, chosen_weapon, 9999, true);
                                g_SetCurrentWeapon(ped, chosen_weapon);
                            }

                            // Register ground cop to ECS
                            {
                                auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
                                if (!cop_comp) {
                                    cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(ped, ped);
                                }
                                auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
                                if (!combat_comp) {
                                    combat_comp = ecs::EntityManager::get().add_component<ecs::CombatComponent>(ped);
                                }
                                if (combat_comp) {
                                    combat_comp->target_entity = target_criminal;
                                }
                            }
                            
                            bool found_assign = false;
                            for (auto& item : ctx.cop_attack_assign_time_snapshot) {
                                if (item.first == key_assign) {
                                    item.second = now_ms();
                                    found_assign = true;
                                    break;
                                }
                            }
                            if (!found_assign) {
                                ctx.cop_attack_assign_time_snapshot.push_back({key_assign, now_ms()});
                            }
                            ctx.pending_cop_attack_assign_time.push_back({key_assign, now_ms()});

                            // 同时记录到分配武器
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
                            ctx.pending_cop_assigned_weapon.push_back({ped, chosen_weapon});

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

                            if (!already_assigned_foot_cop) {
                                ctx.active_foot_cops_count++;
                                already_assigned_foot_cop = true;
                            }
                            LOGI("🎯 [Virtual Sound Event] Dispatched logic sound event (Weapon=%d) to cop %p towards ctx.criminal %p (active_foot_cops=%d/%d)", 
                                 chosen_weapon, ped, target_criminal, ctx.active_foot_cops_count, ctx.max_foot_cops);
                            dispatched_via_noise = true;
                        }
                    }

                    // 平滑降级
                    if (!dispatched_via_noise && g_orig_generate_damage_event) {
                        if (g_GiveWeapon && g_SetCurrentWeapon) {
                            g_GiveWeapon(ped, chosen_weapon, 9999, true);
                            g_SetCurrentWeapon(ped, chosen_weapon);
                        }
                        g_orig_generate_damage_event(ped, reinterpret_cast<CEntity*>(target_criminal), chosen_weapon, 0, 3, 0);

                        // Register ground cop to ECS
                        {
                            auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
                            if (!cop_comp) {
                                cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(ped, ped);
                            }
                            auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
                            if (!combat_comp) {
                                combat_comp = ecs::EntityManager::get().add_component<ecs::CombatComponent>(ped);
                            }
                            if (combat_comp) {
                                combat_comp->target_entity = target_criminal;
                            }
                        }
                        
                        bool found_assign = false;
                        for (auto& item : ctx.cop_attack_assign_time_snapshot) {
                            if (item.first == key_assign) {
                                item.second = now_ms();
                                found_assign = true;
                                break;
                            }
                        }
                        if (!found_assign) {
                            ctx.cop_attack_assign_time_snapshot.push_back({key_assign, now_ms()});
                        }
                        ctx.pending_cop_attack_assign_time.push_back({key_assign, now_ms()});

                        // 同时记录到分配武器
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
                        ctx.pending_cop_assigned_weapon.push_back({ped, chosen_weapon});

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

                        if (!already_assigned_foot_cop) {
                            ctx.active_foot_cops_count++;
                            already_assigned_foot_cop = true;
                        }
                        LOGI("⚠️ [Fallback 0-Damage] Inflicted 0 damage (Weapon=%d) to cop %p by criminal %p (active_foot_cops=%d/%d)", 
                             chosen_weapon, ped, target_criminal, ctx.active_foot_cops_count, ctx.max_foot_cops);
                    }
                }

}
