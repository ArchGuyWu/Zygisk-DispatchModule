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
#include "dispatch_timing.hpp"

void init_ecs_systems() {
    static std::atomic<bool> initialized{false};
    if (initialized.exchange(true)) {
        return;
    }
    LOGI("⚡️ [ECS Engine] Initializing ECS Systems...");

    // 1. CleanupSystem: 监听实体销毁事件
    ecs::EventDispatcher::get().subscribe<ecs::EntityCleanupEvent>("EntityCleanupEvent", [](const ecs::EntityCleanupEvent& ev) {
        if (ev.entity) {
            ecs::EntityManager::get().destroy_entity(ev.entity);
        }
    });

    // 2. CopDispatchSystem: 监听犯罪通报事件 & 伤害事件
    ecs::EventDispatcher::get().subscribe<ecs::CrimeReportEvent>("CrimeReportEvent", [](const ecs::CrimeReportEvent& ev) {
        auto* criminal = static_cast<CPed*>(ev.criminal);
        if (!criminal || !is_ped_pointer_valid_safe(criminal)) return;

        // 在 ECS 中注册并初始化/更新犯罪分子组件
        auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(criminal);
        if (!crim_comp) {
            crim_comp = ecs::EntityManager::get().add_component<ecs::CriminalComponent>(criminal, criminal);
            if (crim_comp) {
                crim_comp->first_detect_time_ms = ev.time_ms;
                crim_comp->initial_weapon_category = ev.weapon_category;
            }
        } else {
            // Weapon Downgrade Protection: Keep highest/first weapon category
            if (ev.weapon_category > crim_comp->initial_weapon_category) {
                crim_comp->initial_weapon_category = ev.weapon_category;
            }
        }

        if (crim_comp) {
            crim_comp->last_attack_time_ms = ev.time_ms;

            if (ev.victim) {
                crim_comp->is_active = true;
                crim_comp->is_air_shooter = false;
                crim_comp->is_fleeing = false;
                crim_comp->current_victim = ev.victim;
            } else {
                if (ev.weapon_category == 2) { // FIREARM
                    crim_comp->is_active = true; // Mark active for FIREARM_AIR_SHOOT
                    crim_comp->is_air_shooter = true;
                    crim_comp->is_fleeing = false;
                    crim_comp->current_victim = nullptr;
                } else {
                    crim_comp->is_active = false;
                    crim_comp->is_air_shooter = false;
                    crim_comp->is_fleeing = false;
                    crim_comp->current_victim = nullptr;
                }
            }
        }

        // 尝试并案
        bool merged = try_consolidate_crime(criminal, ev.location, ev.is_firearm);
        if (!merged) {
            // 如果未能并案，且符合激活条件，则激活为全新犯罪现场或顶替旧案
            if (should_activate_or_hijack_crime(ev.location, ev.is_firearm)) {
                std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);

                // 初始化并启动新案对象
                auto new_crime = std::make_shared<CrimeEvent>();
                new_crime->case_id = g_next_case_id++;
                new_crime->location = ev.location;
                new_crime->criminal = criminal;
                new_crime->is_firearm = ev.is_firearm;
                new_crime->consolidated_criminals.push_back(criminal);
                new_crime->criminal_is_firearm.push_back(ev.is_firearm);
                
                // 写入嫌疑人详细犯罪分级与状态
                CrimeEvent::CriminalState c_state;
                c_state.first_threat_category = ev.is_firearm ? 2 : (ev.weapon_category == 1 ? 1 : 0);
                c_state.current_threat_category = c_state.first_threat_category;
                c_state.is_active = crim_comp ? crim_comp->is_active : false;
                c_state.shooting_air = crim_comp ? crim_comp->is_air_shooter : false;
                c_state.fleeing = crim_comp ? crim_comp->is_fleeing : false;
                new_crime->criminal_states[criminal] = c_state;

                new_crime->dispatch_sent = false;
                new_crime->road_closure_active = false;
                new_crime->cops_killed = 0;
                new_crime->cancelled = false;
                new_crime->dispatch_state = STATE_IDLE; // STATE_IDLE

                g_active_crimes.push_back(new_crime);
                g_tracked_criminal.store(criminal);

                LOGI("📡 [ECS CopDispatchSystem] Activated new crime event Case %llu! Perp: %p, Firearm: %d, Pos: (%.1f, %.1f, %.1f)",
                     (unsigned long long)new_crime->case_id, criminal, ev.is_firearm, ev.location.x, ev.location.y, ev.location.z);
            }
        }

        // 重新评估案件主犯与警力路由
        update_primary_criminal_by_threat();
    });

    ecs::EventDispatcher::get().subscribe<ecs::DamageEvent>("DamageEvent", [](const ecs::DamageEvent& ev) {
        auto* victim_cop = static_cast<CPed*>(ev.victim);
        auto* attacker_perp = static_cast<CPed*>(ev.attacker);
        if (victim_cop && is_ped_pointer_valid_safe(victim_cop) &&
            attacker_perp && is_ped_pointer_valid_safe(attacker_perp)) {

            // 注册警员与其战斗状态
            auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(victim_cop);
            if (!cop_comp) {
                cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(victim_cop, victim_cop);
            }
            auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(victim_cop);
            if (!combat_comp) {
                combat_comp = ecs::EntityManager::get().add_component<ecs::CombatComponent>(victim_cop);
            }
            if (combat_comp) {
                combat_comp->target_entity = attacker_perp;
            }

            // 警员受袭，触发即时自卫反击 (不强制更新武器模型，防止重置攻击动画)
            make_single_cop_attack_criminal(victim_cop, attacker_perp, false);
        }
    });

    // 3. CopWeaponSelectionSystem: 监听武器切换事件 & 周期性 Tick 事件
    ecs::EventDispatcher::get().subscribe<ecs::WeaponSwitchEvent>("WeaponSwitchEvent", [](const ecs::WeaponSwitchEvent& ev) {
        auto* ped = static_cast<CPed*>(ev.ped);
        if (!ped || !is_ped_pointer_valid_safe(ped)) return;

        // 如果是执勤警员切枪，更新其余下的 CombatComponent 记录
        int ped_type = g_GetPedType ? g_GetPedType(ped) : 0;
        if (ped_type == PED_TYPE_COP) {
            auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
            if (combat) {
                combat->current_weapon_type = ev.current_weapon;
                combat->last_weapon_switch_time_ms = ev.time_ms;
            }
            return;
        }

        // 如果是处于侦测中/并案列表中的犯罪分子切枪，则处理案件的即时升级与降级冻结
        auto crime = find_crime_containing_criminal(ped);
        if (crime && !crime->cancelled) {
            bool is_our_criminal = false;
            size_t criminal_idx = 0;
            for (size_t idx = 0; idx < crime->consolidated_criminals.size(); ++idx) {
                if (crime->consolidated_criminals[idx] == ped) {
                    is_our_criminal = true;
                    criminal_idx = idx;
                    break;
                }
            }

            if (is_our_criminal) {
                auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
                if (crim_comp) {
                    int new_weap_cat = 0;
                    if (ev.current_weapon >= WEAPON_PISTOL && ev.current_weapon <= WEAPON_MINIGUN) {
                        new_weap_cat = 2; // FIREARM
                    } else if (ev.current_weapon == WEAPON_UNARMED) {
                        new_weap_cat = 0; // UNARMED
                    } else {
                        new_weap_cat = 1; // MELEE
                    }

                    if (new_weap_cat > crim_comp->initial_weapon_category) {
                        crim_comp->initial_weapon_category = new_weap_cat;
                        crim_comp->is_active = true;
                        crim_comp->is_fleeing = false;
                        crim_comp->is_air_shooter = false;
                        crim_comp->last_attack_time_ms = ev.time_ms;
                        LOGI("⚡️ [ECS WeaponSwitch] Criminal %p upgraded weapon category to %d! Escalated threat level to active.", ped, new_weap_cat);
                    } else if (new_weap_cat < crim_comp->initial_weapon_category) {
                        crim_comp->is_active = false;
                        crim_comp->is_air_shooter = false;
                        crim_comp->is_fleeing = true;
                        LOGI("⚡️ [ECS WeaponSwitch] Criminal %p downgraded weapon to %d. Classifying as Inactive & Fleeing of initial category %d.",
                             ped, ev.current_weapon, crim_comp->initial_weapon_category);
                    }
                }

                bool firearm = (ev.current_weapon >= WEAPON_PISTOL && ev.current_weapon <= WEAPON_MINIGUN);
                if (firearm) {
                    bool escalated = false;
                    if (criminal_idx < crime->criminal_is_firearm.size()) {
                        if (!crime->criminal_is_firearm[criminal_idx]) {
                            crime->criminal_is_firearm[criminal_idx] = true;
                            escalated = true;
                        }
                    }
                    if (!crime->is_firearm) {
                        crime->is_firearm = true;
                        escalated = true;
                    }

                    if (escalated) {
                        LOGI("⚡️ [ECS CopWeaponSelectionSystem] Criminal %p switched weapon to firearm %d! Escalating case %llu immediately.",
                             ped, ev.current_weapon, (unsigned long long)crime->case_id);
                    }
                }

                update_cops_targeting_criminal_event_driven(ped);
            }
        }
    });

    // 4. CopStuckAndWeaponSelectionSystem: 周期性 Tick 事件
    ecs::EventDispatcher::get().subscribe<ecs::TickEvent>("TickEvent", [](const ecs::TickEvent& ev) {
        int64_t cur_time = ev.current_time_ms;

        // 1. CriminalComponent 周期维护
        auto criminals = ecs::EntityManager::get().get_entities_with<ecs::CriminalComponent>();
        for (auto crim_ent : criminals) {
            auto* ped = static_cast<CPed*>(crim_ent);
            if (!ped || !is_ped_pointer_valid_safe(ped) || (g_IsAlive && !g_IsAlive(ped))) {
                ecs::EntityManager::get().destroy_entity(crim_ent);
                continue;
            }

            auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
            if (crim_comp) {
                // 1.1 检查受害者状态
                auto* victim_ped = static_cast<CPed*>(crim_comp->current_victim);
                if (victim_ped && is_ped_pointer_valid_safe(victim_ped)) {
                    if (g_IsAlive && !g_IsAlive(victim_ped)) {
                        crim_comp->is_active = false;
                        crim_comp->current_victim = nullptr;
                        LOGI("⚡ [ECS TickEvent] Criminal %p victim died. Classifying as inactive.", ped);
                    }
                }

                // 1.2 检查攻击超时 (8秒无攻击通报/伤害产生)
                if (crim_comp->is_active && (cur_time - crim_comp->last_attack_time_ms > 8000)) {
                    crim_comp->is_active = false;
                    crim_comp->is_air_shooter = false;
                    LOGI("⚡ [ECS TickEvent] Criminal %p active state timed out (8s no attack).", ped);
                }
            }

            update_primary_criminal_by_threat();
        }

        // 2. CopStuckAndWeaponSelectionSystem:
        // 遍历所有已绑定的警员实体，进行自动化战术升级与卡死检测
        auto cops = ecs::EntityManager::get().get_entities_with<ecs::CopComponent>();
        for (auto cop_ent : cops) {
            auto* cop = static_cast<CPed*>(cop_ent);
            if (!cop || !is_ped_pointer_valid_safe(cop) || (g_IsAlive && !g_IsAlive(cop))) {
                ecs::EntityManager::get().destroy_entity(cop_ent);
                continue;
            }

            auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
            auto* combat_comp = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);

            if (cop_comp) {
                bool actually_in_vehicle = find_vehicle_of_cop(cop) != nullptr;
                if (actually_in_vehicle) {
                    cop_comp->is_in_vehicle = true;
                    cop_comp->has_exited_vehicle = false;
                } else if (cop_comp->is_in_vehicle) {
                    cop_comp->is_in_vehicle = false;
                    cop_comp->has_exited_vehicle = true;
                }
            }

            // 2.1 警员卡死检测与智能重新寻路路由 (Automated Stuck routing)
            if (cop_comp) {
                // 每 2 秒进行一次卡死坐标检查
                if (ev.current_time_ms - cop_comp->last_stuck_check_time_ms > 2000) {
                    cop_comp->last_stuck_check_time_ms = ev.current_time_ms;
                    CVector pos = get_entity_pos(cop);

                    // 仅在非载具内追杀敌人的状态下检测卡死
                    if (!cop_comp->is_in_vehicle && combat_comp && combat_comp->target_entity) {
                        float dx = pos.x - cop_comp->last_pos_x;
                        float dy = pos.y - cop_comp->last_pos_y;
                        float dist_moved = sqrtf(dx * dx + dy * dy);

                        if (dist_moved < 0.20f) {
                            cop_comp->stuck_count++;
                            if (cop_comp->stuck_count >= 3) { // 连续卡死 6 秒以上
                                auto* target = static_cast<CPed*>(combat_comp->target_entity);
                                if (target && is_ped_pointer_valid_safe(target)) {
                                    LOGW("⚠️ [ECS StuckSolver] Ground cop %p is stuck pursuing %p. Force resetting task for pathfinding routing.", cop, target);
                                    make_single_cop_attack_criminal(cop, target, true);
                                }
                                cop_comp->stuck_count = 0;
                            }
                        } else {
                            cop_comp->stuck_count = 0;
                        }
                    } else {
                        cop_comp->stuck_count = 0;
                    }

                    cop_comp->last_pos_x = pos.x;
                    cop_comp->last_pos_y = pos.y;
                    cop_comp->last_pos_z = pos.z;
                }
            }

            // 2.3 步行警员响应调度沿途发生的同级及以上活跃犯罪 (EnRoute Rerouting for foot cops)
            if (cop_comp && !cop_comp->is_in_vehicle && combat_comp && combat_comp->target_entity) {
                std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
                std::shared_ptr<CrimeEvent> case_A = nullptr;
                for (const auto& crime : g_active_crimes) {
                    if (!crime || crime->cancelled) continue;
                    bool found = false;
                    if (crime->criminal == combat_comp->target_entity) {
                        found = true;
                    } else {
                        for (CPed* crim : crime->consolidated_criminals) {
                            if (crim == combat_comp->target_entity) {
                                found = true;
                                break;
                            }
                        }
                    }
                    if (found) {
                        case_A = crime;
                        break;
                    }
                }

                if (case_A) {
                    CVector cop_pos = get_entity_pos(cop);
                    std::shared_ptr<CrimeEvent> best_case_B = nullptr;
                    float best_dist = 999999.0f;

                    for (const auto& case_B : g_active_crimes) {
                        if (!case_B || case_B->cancelled || case_B == case_A) continue;
                        if (!case_B->criminal || !is_ped_pointer_valid_safe(case_B->criminal)) continue;

                        int threat_A = case_A->is_firearm ? 2 : 1;
                        int threat_B = case_B->is_firearm ? 2 : 1;

                        if (threat_B >= threat_A) {
                            CVector crim_pos = get_entity_pos(case_B->criminal);
                            float dx = cop_pos.x - crim_pos.x;
                            float dy = cop_pos.y - crim_pos.y;
                            float dz = cop_pos.z - crim_pos.z;
                            float dist = sqrtf(dx * dx + dy * dy + dz * dz);

                            float av_range = case_B->is_firearm ? 75.0f : 35.0f;

                            if (dist <= av_range && dist < best_dist) {
                                best_dist = dist;
                                best_case_B = case_B;
                            }
                        }
                    }

                    if (best_case_B) {
                        LOGI("🚨 [dispatchCenter - FootCopReroute] Foot cop %p (originally pursuing Case %llu, threat: %d) encountered Case %llu (threat: %d) en route! Rerouting to Case %llu (dist: %.1fm).",
                             cop, (unsigned long long)case_A->case_id, case_A->is_firearm ? 2 : 1,
                             (unsigned long long)best_case_B->case_id, best_case_B->is_firearm ? 2 : 1,
                             (unsigned long long)best_case_B->case_id, best_dist);

                        make_single_cop_attack_criminal(cop, best_case_B->criminal, true);
                        if (combat_comp) {
                            combat_comp->last_weapon_switch_time_ms = 0;
                        }
                    }
                }
            }

            // 2.2 智能武器选择与高响应收枪控制链 (零延迟、高响应性切枪与即时自动收枪机制)
            bool should_disarm = false;
            CPed* target = nullptr;

            if (combat_comp) {
                target = static_cast<CPed*>(combat_comp->target_entity);
            }

            // A. 自适应判定是否应当收枪退敌 (战斗结束、离队、车内或目标失效)
            if (cop_comp && cop_comp->is_in_vehicle) {
                should_disarm = true; // 载具内不需要本模块强行控制手持武器，交还底层的默认状态
            } else if (!target) {
                should_disarm = true; // 无追杀目标
            } else if (!is_ped_pointer_valid_safe(target)) {
                should_disarm = true; // 目标指针已失效或被引擎清理
            } else if (g_IsAlive && !g_IsAlive(target)) {
                should_disarm = true; // 目标已被击毙
            } else if (!g_crime_active.load()) {
                should_disarm = true; // 警情已全局解除
            } else {
                // 校验该目标是否属于任何一个活跃的犯罪现场
                bool target_is_active_criminal = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
                    for (const auto& crime : g_active_crimes) {
                        if (crime && !crime->cancelled) {
                            if (crime->criminal == target) {
                                target_is_active_criminal = true;
                                break;
                            }
                            for (CPed* c : crime->consolidated_criminals) {
                                if (c == target) {
                                    target_is_active_criminal = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!target_is_active_criminal) {
                    should_disarm = true; // 目标在任何警情中均已不再活跃，或被并案系统剔除
                }
            }

            CVector cop_pos = get_entity_pos(cop);
            bool within_native_av = dispatch_timing::is_cop_within_any_active_crime_av(cop_pos);

            // 追加测距脱离检测：如果追击目标超过 100 米未果，算作跟丢脱战，立即收枪
            if (!should_disarm && target && !within_native_av) {
                CVector tgt_pos = get_entity_pos(target);
                float dx = cop_pos.x - tgt_pos.x;
                float dy = cop_pos.y - tgt_pos.y;
                float dz = cop_pos.z - tgt_pos.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dist > 100.0f) {
                    should_disarm = true;
                }
            }

            // 视听范围内：禁止收枪/上车/移除战斗组件，必须参战
            if (should_disarm && within_native_av) {
                should_disarm = false;
                CPed* engage_target = nullptr;
                if (target && is_ped_pointer_valid_safe(target) && (g_IsAlive == nullptr || g_IsAlive(target))) {
                    engage_target = target;
                } else {
                    float best_dist_sq = 999999.0f;
                    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
                    for (const auto& crime : g_active_crimes) {
                        if (!crime || crime->cancelled) continue;
                        float av_range = dispatch_timing::get_av_range_for_crime(*crime);
                        float av_range_sq = av_range * av_range;
                        for (CPed* criminal : crime->consolidated_criminals) {
                            if (!criminal || !is_ped_pointer_valid_safe(criminal)) continue;
                            if (g_IsAlive && !g_IsAlive(criminal)) continue;
                            CVector crim_pos = get_entity_pos(criminal);
                            float dx = cop_pos.x - crim_pos.x;
                            float dy = cop_pos.y - crim_pos.y;
                            float dz = cop_pos.z - crim_pos.z;
                            float dist_sq = dx * dx + dy * dy + dz * dz;
                            if (dist_sq <= av_range_sq && dist_sq < best_dist_sq) {
                                best_dist_sq = dist_sq;
                                engage_target = criminal;
                            }
                        }
                    }
                }
                if (engage_target) {
                    make_single_cop_attack_criminal(cop, engage_target, true);
                    LOGI("🎯 [ECS WeaponSelectionSystem] Cop %p AV-forced re-engage -> criminal %p", cop, engage_target);
                }
            }

            if (should_disarm) {
                // 警员当前并不处于任何有效的攻击或战斗态
                bool is_currently_armed = (combat_comp && combat_comp->current_weapon_type != (int)WEAPON_UNARMED);
                
                if (is_currently_armed) {
                    if (g_SetCurrentWeapon) {
                        g_SetCurrentWeapon(cop, WEAPON_UNARMED);
                    }
                    if (combat_comp) {
                        combat_comp->current_weapon_type = (int)WEAPON_UNARMED;
                        combat_comp->target_entity = nullptr;
                        combat_comp->last_weapon_switch_time_ms = ev.current_time_ms;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_cop_assigned_weapon_mutex);
                        g_cop_assigned_weapon.erase(cop);
                    }
                    LOGI("🎯 [ECS WeaponSelectionSystem - DISARM] Cop %p successfully disarmed (target dead or lost).", cop);
                }

                bool keep_on_duty = should_block_cop_reenter_vehicle(cop);
                if (!keep_on_duty) {
                    // 重点提升：既然此警员任务完全解除且已安全收枪，先命令其返回原本绑定的车辆
                    if (cop_comp && !cop_comp->is_in_vehicle) {
                        bool is_driver = false;
                        void* bound_veh = find_bound_vehicle_of_cop(cop, is_driver);
                        if (bound_veh) {
                            // 驾驶员生存性晋升系统 (Driver Survivability Promotion)
                            // 如果当前警员是乘客，但该车原绑定的驾驶员已经殉职/不存在，则自动将该警员晋升为驾驶员！
                            if (!is_driver && !is_alive_bound_driver_exists(bound_veh)) {
                                is_driver = true;
                                // 同步更新绑定关系
                                {
                                    std::lock_guard<std::mutex> lock(g_bindings_mutex);
                                    for (auto& binding : g_cop_vehicle_bindings) {
                                        if (binding.cop == cop) {
                                            binding.as_driver = true;
                                            break;
                                        }
                                    }
                                }
                                LOGW("👮 [ECS - DriverPromotion] Passenger cop %p promoted to DRIVER for vehicle %p because the bound driver is deceased/missing.", cop, bound_veh);
                            }
                            make_cop_enter_vehicle(cop, bound_veh, is_driver);
                        }
                    }

                    // 重点提升：既然此警员任务完全解除且已安全收枪，直接移除其绑定的战术组件，不再占用下一帧的轮询资源
                    ecs::EntityManager::get().remove_component<ecs::CopComponent>(cop);
                    ecs::EntityManager::get().remove_component<ecs::CombatComponent>(cop);
                } else {
                    LOGI("🎯 [ECS WeaponSelectionSystem] Cop %p kept on foot near active crime (blocked vehicle re-enter).", cop);
                }
            } 
            else if (combat_comp && target) {
                // B. 自适应高灵敏战术武器切换
                bool firearm_threat = is_specific_criminal_armed_with_firearm(target);
                eWeaponType target_weapon = determine_weapon_for_cop(cop, target, firearm_threat);

                // 2 秒强制武器强化周期，用来解决由于下车动作、摔倒或受击导致游戏底层自动重置武器为拳头的 bug
                bool cop_is_in_veh = (find_vehicle_of_cop(cop) != nullptr);
                bool periodic_reinforce = (!cop_is_in_veh) && (ev.current_time_ms - combat_comp->last_weapon_switch_time_ms > 2000);

                if (target_weapon != (eWeaponType)combat_comp->current_weapon_type || periodic_reinforce) {
                    // 战术规则优化：
                    // 1. 紧急升级判定：如果是枪械威胁，属于紧急升级，一秒都不能等，直接无视冷却强制切枪！
                    // 2. 正常状态调整：对于普通距离或威胁调整，冷却时间大幅从 5 秒缩减到 1 秒 (1000ms)，保障姿态极速跟进，同时免除高频摆动。
                    bool is_urgent_upgrade = (target_weapon == WEAPON_PISTOL && combat_comp->current_weapon_type != (int)WEAPON_PISTOL);
                    bool time_cooldown_passed = (ev.current_time_ms - combat_comp->last_weapon_switch_time_ms > 1000);

                    if (is_urgent_upgrade || time_cooldown_passed || periodic_reinforce) {
                        if (g_GiveWeapon && g_SetCurrentWeapon) {
                            g_GiveWeapon(cop, target_weapon, 9999, true);
                            g_SetCurrentWeapon(cop, target_weapon);
                            combat_comp->current_weapon_type = (int)target_weapon;
                            combat_comp->last_weapon_switch_time_ms = ev.current_time_ms;
                            LOGI("🎯 [ECS WeaponSelectionSystem - SWITCH/REINFORCE] Cop %p weapon updated to match threat (urgent=%d, reinforce=%d): %d", 
                                 cop, is_urgent_upgrade, periodic_reinforce, (int)target_weapon);
                        }
                    }
                }
            }
        }
    });

    LOGI("✅ [ECS Engine] All systems successfully initialized!");
}
