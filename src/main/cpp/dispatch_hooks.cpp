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


// =====================================================================
// Hook 1: CCrime::ReportCrime (检测犯罪事件)
//
// 符号: _ZN6CCrime11ReportCrimeE10eCrimeTypeP7CEntityP4CPed
// 签名: static void CCrime::ReportCrime(eCrimeType, CEntity* victim, CPed* perpetrator)
// =====================================================================
void proxy_report_crime(eCrimeType crime_type, CEntity* victim, CPed* perpetrator) {
    SHADOWHOOK_STACK_SCOPE();

    if (perpetrator && g_GetPedType && g_FindPlayerPed && is_ped_pointer_valid_safe(perpetrator)) {
        int perp_type = g_GetPedType(perpetrator);
        CPlayerPed* player = g_FindPlayerPed(0);

        // --- 玩家免责逻辑 ---
        if (player && reinterpret_cast<CPed*>(player) == perpetrator) {
            CPed* tracked = g_tracked_criminal.load();
            if (tracked != nullptr && victim == reinterpret_cast<CEntity*>(tracked)) {
                LOGI("Player assisting police against tracked criminal -> CCrime::ReportCrime BLOCKED");
                return;  // 不报告犯罪，玩家免遭通缉
            }
        }

        // --- NPC 犯罪检测逻辑 (事件驱动) ---
        bool is_fire = (crime_type == CRIME_FIRE_WEAPON);
        if ((victim || is_fire) && perp_type != PED_TYPE_PLAYER && perp_type != PED_TYPE_COP) {
            bool firearm = is_firearm(crime_type);
            int weap_cat = 0;
            if (firearm) {
                weap_cat = 2; // FIREARM
            } else {
                if (crime_type == CRIME_KILL_PED_NO_GUN || crime_type == CRIME_DAMAGED_PED || crime_type == CRIME_DAMAGED_COP) {
                    weap_cat = 1; // MELEE
                } else {
                    weap_cat = 0; // UNARMED
                }
            }
            CVector crime_pos = get_entity_pos(perpetrator);

            ecs::EventDispatcher::get().dispatch(ecs::CrimeReportEvent(perpetrator, victim, crime_pos, firearm, weap_cat, now_ms()));
        }
    }

    SHADOWHOOK_CALL_PREV(proxy_report_crime, crime_type, victim, perpetrator);
}

// =====================================================================
// Hook 2: CEventHandler::RegisterKill (检测警察阵亡)
//
// 符号: _ZN13CEventHandler12RegisterKillEPK4CPedPK7CEntity11eWeaponTypeb
// 签名: void CEventHandler::RegisterKill(CPed const*, CEntity const*, eWeaponType, bool)
// =====================================================================
void proxy_register_kill(const CPed* dead_ped,
                                 const CEntity* killer,
                                 eWeaponType weapon,
                                 bool unk) {
    SHADOWHOOK_STACK_SCOPE();

    if (dead_ped && g_GetPedType && is_ped_pointer_valid_safe(const_cast<CPed*>(dead_ped))) {
        int dead_type = g_GetPedType(dead_ped);

        // 如果玩家杀死了警察，取消玩家协助警方的免责状态
        if (dead_type == PED_TYPE_COP && g_FindPlayerPed) {
            CPlayerPed* player = g_FindPlayerPed(0);
            if (player && killer == reinterpret_cast<const CEntity*>(player)) {
                g_player_friendly_fire_blocked.store(false);
                g_friendly_fire_cop_hits.store(999); // 设为极高，让免责失效
                LOGW("Player directly killed cop %p -> Exemption immediately revoked!", dead_ped);
            }
        }

        // 犯罪 NPC 被杀 → 并案逻辑处理与事件解决判定
        {
            std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    auto& list = crime->consolidated_criminals;
                    auto& is_fire_list = crime->criminal_is_firearm;
                    auto it = std::find(list.begin(), list.end(), const_cast<CPed*>(dead_ped));
                    if (it != list.end()) {
                        size_t idx = std::distance(list.begin(), it);
                        LOGI("📡 [dispatchCenter - CaseMerge] Consolidated criminal NPC %p killed in case %llu (Index: %zu)", dead_ped, (unsigned long long)crime->case_id, idx);
                        
                        // 从并案列表和详细状态中擦除
                        list.erase(it);
                        if (idx < is_fire_list.size()) {
                            is_fire_list.erase(is_fire_list.begin() + idx);
                        }
                        crime->criminal_states.erase(const_cast<CPed*>(dead_ped));

                        // 如果被杀的是当前 primary criminal
                        if (crime->criminal == dead_ped) {
                            if (!list.empty()) {
                                // 还有其他同伙，将 primary criminal 转移到下一个存活的罪犯上
                                crime->criminal = list.front();
                                if (g_tracked_criminal.load() == dead_ped) {
                                    g_tracked_criminal.store(list.front());
                                }
                                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Primary criminal %p killed. Shifted primary tracking to %p.", 
                                     (unsigned long long)crime->case_id, dead_ped, crime->criminal);
                            } else {
                                // 所有同伙都死光了 -> 事件解决并结案
                                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: All consolidated criminal NPCs killed -> crime event resolved", (unsigned long long)crime->case_id);
                                crime->cancelled = true;
                                if (g_tracked_criminal.load() == dead_ped) {
                                    g_tracked_criminal.store(nullptr);
                                }
                                cleanup_single_case_vehicles(crime);
                            }
                        } else {
                            if (list.empty()) {
                                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: All consolidated criminal NPCs killed -> crime event resolved", (unsigned long long)crime->case_id);
                                crime->cancelled = true;
                                if (g_tracked_criminal.load() == dead_ped) {
                                    g_tracked_criminal.store(nullptr);
                                }
                                cleanup_single_case_vehicles(crime);
                            }
                        }
                    }
                }
            }
        }

        // 警察被杀 → 针对 80 米内最近的活跃案件触发增援
        if (dead_type == PED_TYPE_COP) {
            std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
            CVector cop_pos = get_entity_pos(const_cast<CPed*>(dead_ped));
            
            std::shared_ptr<CrimeEvent> nearest_crime = nullptr;
            float min_dist_sq = 80.0f * 80.0f; // 仅计算 80 米内的案件
            
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    float dx = cop_pos.x - crime->location.x;
                    float dy = cop_pos.y - crime->location.y;
                    float dz = cop_pos.z - crime->location.z;
                    float dist_sq = dx * dx + dy * dy + dz * dz;
                    if (dist_sq < min_dist_sq) {
                        min_dist_sq = dist_sq;
                        nearest_crime = crime;
                    }
                }
            }
            
            if (nearest_crime) {
                nearest_crime->cops_killed++;
                int killed = nearest_crime->cops_killed;
                LOGI("📡 [dispatchCenter] Cop killed at scene of case %llu (total: %d, dist: %.1f) -> triggering reinforcement!", 
                     (unsigned long long)nearest_crime->case_id, killed, sqrtf(min_dist_sq));
            } else {
                LOGI("📡 [dispatchCenter] Cop killed far from any active case -> ignored");
            }
        }
    }

    if (dead_ped && is_ped_pointer_valid_safe(const_cast<CPed*>(dead_ped))) {
        ecs::EventDispatcher::get().dispatch(ecs::EntityCleanupEvent(const_cast<CPed*>(dead_ped), true));
    }

    SHADOWHOOK_CALL_PREV(proxy_register_kill, dead_ped, killer, weapon, unk);
}

// =====================================================================
// Hook 3: CWanted::SetWantedLevel (玩家通缉拦截)
//
// 符号: _ZN7CWanted14SetWantedLevelEi
// 签名: void CWanted::SetWantedLevel(int level)
// =====================================================================
void proxy_set_wanted_level(void* this_wanted, int level) {
    SHADOWHOOK_STACK_SCOPE();

    bool block_wanted = false;
    bool assisting = g_crime_active.load() || (now_ms() - g_last_assist_time_ms.load() < 4000);

    if (assisting) {
        if (g_friendly_fire_cop_hits.load() > 3) {
            block_wanted = false;
        } else {
            // 1. 原生协助判定：如果玩家正锁死目标犯人，或最近对犯人造成了伤害
            bool assist = false;
            if (g_FindPlayerPed && g_GetWeaponLockOnTarget) {
                CPlayerPed* player = g_FindPlayerPed(0);
                if (player) {
                    CEntity* target = g_GetWeaponLockOnTarget(player);
                    CPed* criminal = g_tracked_criminal.load();
                    if (target && target == reinterpret_cast<CEntity*>(criminal)) {
                        assist = true;
                    }
                }
            }
            if (!assist && (now_ms() - g_last_assist_time_ms.load() < 4000)) {
                assist = true;
            }

            if (assist) {
                block_wanted = true;
            } else {
                // 2. 检查市民流弹保护：最近 3 秒内发生过市民误伤
                if (g_player_stray_bullet_flag.load() && (now_ms() - g_player_stray_bullet_time.load() < 3000)) {
                    block_wanted = true;
                    LOGI("SetWantedLevel: Civilian stray bullet protection triggered -> Block wanted level %d", level);
                }
                // 3. 检查警员轻微误伤保护：误伤处于拦截状态且最后误伤在 3 秒内
                else if (g_player_friendly_fire_blocked.load() && (now_ms() - g_last_friendly_fire_cop_time.load() < 3000)) {
                    block_wanted = true;
                    LOGI("SetWantedLevel: Cop friendly fire protection active -> Block wanted level %d", level);
                }
            }
        }
    }

    if (level >= 1 && block_wanted) {
        LOGI("Player wanted level %d BLOCKED (Assisting=%d, CivilianStray=%d, CopFriendlyFire=%d)",
             level, assisting ? 1 : 0, g_player_stray_bullet_flag.load() ? 1 : 0, g_player_friendly_fire_blocked.load() ? 1 : 0);
        return;
    }

    g_player_wanted_level.store(level);
    SHADOWHOOK_CALL_PREV(proxy_set_wanted_level, this_wanted, level);
}

// =====================================================================
// Hook 4: CWeapon::GenerateDamageEvent (核心：检测 NPC 犯罪与伤害输出)
// =====================================================================
bool proxy_generate_damage_event(CPed* victim,
                                         CEntity* perpetrator,
                                         eWeaponType weaponType,
                                         int damage,
                                         int pedPiece,
                                         int direction) {
    SHADOWHOOK_STACK_SCOPE();

    if (perpetrator && victim && g_GetPedType && g_FindPlayerPed) {
        if (is_ped_pointer_valid_safe(perpetrator) && is_ped_pointer_valid_safe(victim)) {
            int perp_type = g_GetPedType(perpetrator);
            int victim_type = g_GetPedType(victim);
            CPlayerPed* player = g_FindPlayerPed(0);

            // 1. 如果是玩家攻击 NPC -> 记录协助状态，更新自卫判定，并处理流弹市民与友好火力警员的免责检测
            if (player && perpetrator == reinterpret_cast<CEntity*>(player) && victim != reinterpret_cast<CPed*>(player)) {
                CPed* tracked = g_tracked_criminal.load();
                if (tracked && victim == tracked) {
                    g_last_assist_time_ms.store(now_ms());
                } else {
                    // 玩家伤害了非追踪犯人 -> 检查当前是否在警方协助状态下，以此优化误伤体验
                    bool assisting = g_crime_active.load() || (now_ms() - g_last_assist_time_ms.load() < 4000);
                    if (assisting) {
                        if (victim_type == PED_TYPE_COP) {
                            // 误伤警员判定
                            int64_t cur_time = now_ms();
                            if (cur_time - g_last_friendly_fire_cop_time.load() > 5000) {
                                g_friendly_fire_cop_hits.store(1); // 超过5s未误击，重置计数
                            } else {
                                g_friendly_fire_cop_hits.fetch_add(1);
                            }
                            g_last_friendly_fire_cop_time.store(cur_time);

                            int hits = g_friendly_fire_cop_hits.load();
                            if (hits <= 3) {
                                g_player_friendly_fire_blocked.store(true);
                                LOGI("Player accidentally hit cop (hits=%d/3 within 5s) during assistance -> Flagged friendly fire exemption", hits);
                            } else {
                                g_player_friendly_fire_blocked.store(false);
                                LOGW("Player attacked cop repeatedly (hits=%d) -> friendly fire exemption REVOKED", hits);
                            }
                        } else {
                            // 误击普通市民 -> 标记流弹免责，有效期 3 秒
                            g_player_stray_bullet_flag.store(true);
                            g_player_stray_bullet_time.store(now_ms());
                            LOGI("Player stray bullet hit civilian %p (type=%d) during assistance -> Civilian protection active", victim, victim_type);
                        }
                    }
                }

                // 记录玩家对当前 NPC 的攻击，时效为 30 秒 (用于 NPC 自卫判定)
                std::lock_guard<std::mutex> lock(g_attacked_npcs_mutex);
                int64_t cur_time = now_ms();
                // 手动清理过期记录，并顺便清理掉已经无效的 NPC
                for (auto it = g_player_attacked_npcs.begin(); it != g_player_attacked_npcs.end();) {
                    if (cur_time - it->attack_time > 30000 || !is_ped_pointer_valid_safe(it->npc)) {
                        it = g_player_attacked_npcs.erase(it);
                    } else {
                        ++it;
                    }
                }
                bool found = false;
                for (auto& item : g_player_attacked_npcs) {
                    if (item.npc == victim) {
                        item.attack_time = cur_time;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    g_player_attacked_npcs.push_back({victim, cur_time});
                    LOGI("Recorded player attack on NPC %p for self-defense check", victim);
                }
            }

            // 1b. 警员受击自卫事件驱动：如果受害者是执勤警员，且攻击者是普通市民/黑帮NPC（非警察/非玩家），且伤害大于0
            if (victim_type == PED_TYPE_COP && perpetrator && perp_type != PED_TYPE_COP && perp_type != PED_TYPE_PLAYER && damage > 0) {
                ecs::EventDispatcher::get().dispatch(ecs::DamageEvent(victim, perpetrator, (int)weaponType, false, now_ms()));
            }

            // 2. 检测 NPC 犯罪行为 (非玩家且非警察攻击他人)
            if (player && perpetrator != reinterpret_cast<CEntity*>(player) && perp_type != PED_TYPE_COP && perpetrator != reinterpret_cast<CEntity*>(victim)) {
                // 自卫判定：如果受害人是玩家，且该 NPC 在最近 30 秒内被玩家先动手打过，判定为 NPC 正当防卫，不视作犯罪
                bool is_self_defense = false;
                if (victim == reinterpret_cast<CPed*>(player)) {
                    bool already_criminal = false;
                    if (find_crime_containing_criminal(reinterpret_cast<CPed*>(perpetrator))) {
                        already_criminal = true;
                    }
                    if (!already_criminal) {
                        std::lock_guard<std::mutex> lock(g_attacked_npcs_mutex);
                        int64_t cur_time = now_ms();
                        for (const auto& item : g_player_attacked_npcs) {
                            if (item.npc == reinterpret_cast<CPed*>(perpetrator) && (cur_time - item.attack_time <= 30000)) {
                                is_self_defense = true;
                                break;
                            }
                        }
                    }
                }

                if (is_self_defense) {
                    LOGI("NPC %p attacks player in self-defense (player initiated attack first) -> BLOCKED from wanted/dispatch", perpetrator);
                } else {
                    int weap_cat = 0;
                    if (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN) {
                        weap_cat = 2; // FIREARM
                    } else if (weaponType == WEAPON_UNARMED) {
                        weap_cat = 0; // UNARMED
                    } else {
                        weap_cat = 1; // MELEE
                    }
                    bool firearm = (weap_cat == 2);
                    CVector crime_pos = get_entity_pos(perpetrator);

                    ecs::EventDispatcher::get().dispatch(ecs::CrimeReportEvent(reinterpret_cast<CPed*>(perpetrator), victim, crime_pos, firearm, weap_cat, now_ms()));
                }
            }
        }
    }

    return SHADOWHOOK_CALL_PREV(proxy_generate_damage_event, victim, perpetrator, weaponType, damage, pedPiece, direction);
}

static void handle_damage_event(CEntity* damageSource, eWeaponType weaponType) {
    if (!damageSource || !g_GetPedType || !g_FindPlayerPed) return;

    if (is_ped_pointer_valid_safe(damageSource)) {
        CPed* perpetrator = reinterpret_cast<CPed*>(damageSource);
        int perp_type = g_GetPedType(perpetrator);
        CPlayerPed* player = g_FindPlayerPed(0);

        if (player && perpetrator != reinterpret_cast<CPed*>(player) && perp_type != PED_TYPE_COP) {
            // 自卫判定：如果该 NPC 在最近 30 秒内被玩家打过，判定为自卫，不视作犯罪
            bool is_self_defense = false;
            {
                std::lock_guard<std::mutex> lock(g_attacked_npcs_mutex);
                int64_t cur_time = now_ms();
                for (const auto& item : g_player_attacked_npcs) {
                    if (item.npc == perpetrator && (cur_time - item.attack_time <= 30000)) {
                        is_self_defense = true;
                        break;
                    }
                }
            }

            if (is_self_defense) {
                LOGI("NPC %p attacks in self-defense -> BLOCKED from wanted/dispatch", perpetrator);
            } else {
                int weap_cat = 0;
                if (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN) {
                    weap_cat = 2; // FIREARM
                } else if (weaponType == WEAPON_UNARMED) {
                    weap_cat = 0; // UNARMED
                } else {
                    weap_cat = 1; // MELEE
                }
                bool firearm = (weap_cat == 2);
                CVector crime_pos = get_entity_pos(perpetrator);

                ecs::EventDispatcher::get().dispatch(ecs::CrimeReportEvent(perpetrator, nullptr, crime_pos, firearm, weap_cat, now_ms()));
            }
        }
    }
}

void proxy_event_damage_ctor_c1(void* event_this,
                                        CEntity* damageSource,
                                        unsigned int startTime,
                                        eWeaponType weaponType,
                                        int pieceType,
                                        unsigned char damageSeverity,
                                        bool b1,
                                        bool b2) {
    SHADOWHOOK_STACK_SCOPE();
    handle_damage_event(damageSource, weaponType);
    SHADOWHOOK_CALL_PREV(proxy_event_damage_ctor_c1, event_this, damageSource, startTime, weaponType, pieceType, damageSeverity, b1, b2);
}

void proxy_event_damage_ctor_c2(void* event_this,
                                        CEntity* damageSource,
                                        unsigned int startTime,
                                        eWeaponType weaponType,
                                        int pieceType,
                                        unsigned char damageSeverity,
                                        bool b1,
                                        bool b2) {
    SHADOWHOOK_STACK_SCOPE();
    handle_damage_event(damageSource, weaponType);
    SHADOWHOOK_CALL_PREV(proxy_event_damage_ctor_c2, event_this, damageSource, startTime, weaponType, pieceType, damageSeverity, b1, b2);
}

// =====================================================================
// Hook 2b: CPed::SetCurrentWeapon (事件驱动：监控犯罪嫌疑人切枪)
// =====================================================================
void proxy_SetCurrentWeapon(CPed* ped, eWeaponType weaponType) {
    SHADOWHOOK_STACK_SCOPE();

    if (ped && is_ped_pointer_valid_safe(ped)) {
        int prev_weap = 0;
        auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ped);
        if (combat) {
            prev_weap = combat->current_weapon_type;
        }
        ecs::EventDispatcher::get().dispatch(ecs::WeaponSwitchEvent(ped, prev_weap, (int)weaponType, now_ms()));
        std::shared_ptr<CrimeEvent> belonging_crime = nullptr;
        size_t criminal_idx = 0;
        bool is_our_criminal = false;
        
        {
            std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    for (size_t idx = 0; idx < crime->consolidated_criminals.size(); ++idx) {
                        if (crime->consolidated_criminals[idx] == ped) {
                            belonging_crime = crime;
                            criminal_idx = idx;
                            is_our_criminal = true;
                            break;
                        }
                    }
                    if (is_our_criminal) break;
                }
            }
            
            if (is_our_criminal && belonging_crime) {
                int current_threat = 0;
                if (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN) {
                    current_threat = 2; // FIREARM
                } else if (weaponType == WEAPON_UNARMED) {
                    current_threat = 0; // UNARMED
                } else {
                    current_threat = 1; // MELEE
                }
                
                auto it = belonging_crime->criminal_states.find(ped);
                if (it != belonging_crime->criminal_states.end()) {
                    int prev_first = it->second.first_threat_category;
                    it->second.current_threat_category = current_threat;
                    
                    if (current_threat > prev_first) {
                        // 武器升级：提高其优先级，并升级首要威胁
                        it->second.first_threat_category = current_threat;
                        it->second.is_active = true; // 升级后立即设为活跃
                        LOGI("⚡️ [Event-Driven] Criminal %p upgraded weapon. Threat level raised to %d (first_threat escalated).", ped, current_threat);
                        
                        if (current_threat == 2) {
                            belonging_crime->is_firearm = true;
                            if (criminal_idx < belonging_crime->criminal_is_firearm.size()) {
                                belonging_crime->criminal_is_firearm[criminal_idx] = true;
                            }
                        }
                    } else if (current_threat < prev_first) {
                        // 武器降级：保留首次攻击时的武器类别（武器降级保护），但归类为不活跃级别
                        it->second.is_active = false;
                        LOGI("⚡️ [Event-Driven] Criminal %p degraded weapon to %d (first_threat was %d). Classified as Inactive for tactical balance.", ped, current_threat, prev_first);
                    } else {
                        // 武器分类相同
                        if (current_threat == 2) {
                            it->second.is_active = true;
                        }
                    }
                }
            }
        }
        
        if (is_our_criminal && belonging_crime) {
            // 在锁外调用以规避死锁，对响应的地面巡警触发高优先级的攻击与即时武器更新
            update_cops_targeting_criminal_event_driven(ped);
            
            bool firearm = (weaponType >= WEAPON_PISTOL && weaponType <= WEAPON_MINIGUN);
            if (firearm) {
                make_cops_attack_criminal_immediate(ped);
            }
        }
    }

    SHADOWHOOK_CALL_PREV(proxy_SetCurrentWeapon, ped, weaponType);
}

