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
#include "dispatch_threat.hpp"

// =====================================================================
// 犯罪事件追踪系统
// =====================================================================
std::recursive_mutex g_crime_mutex;
std::vector<std::shared_ptr<CrimeEvent>> g_active_crimes;
uint64_t g_next_case_id = 1;


// 兼容层：原全局活跃标志兼容
bool CrimeActiveCompat::load() const {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) return true;
    }
    return false;
}

CrimeActiveCompat g_crime_active;

// 兼容层：dummy 案件
std::shared_ptr<CrimeEvent> g_dummy_crime = []() {
    auto d = std::make_shared<CrimeEvent>();
    d->cancelled = true;
    return d;
}();

std::shared_ptr<CrimeEvent> get_primary_active_crime() {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    if (g_active_crimes.empty()) {
        return g_dummy_crime;
    }
    CVector player_pos{0, 0, 0};
    if (g_FindPlayerCoors) {
        player_pos = g_FindPlayerCoors(0);
    }
    std::shared_ptr<CrimeEvent> best_crime = nullptr;
    float min_dist = 999999.0f;
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            float dx = crime->location.x - player_pos.x;
            float dy = crime->location.y - player_pos.y;
            float dz = crime->location.z - player_pos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < min_dist) {
                min_dist = dist;
                best_crime = crime;
            }
        }
    }
    if (best_crime) return best_crime;
    for (const auto& crime : g_active_crimes) {
        if (crime) return crime;
    }
    return g_dummy_crime;
}

std::shared_ptr<CrimeEvent> find_crime_containing_criminal(CPed* criminal) {
    if (!criminal) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (const auto& crime : g_active_crimes) {
        if (!crime || crime->cancelled) continue;
        if (crime->criminal == criminal) return crime;
        for (CPed* c : crime->consolidated_criminals) {
            if (c == criminal) return crime;
        }
    }
    return nullptr;
}

bool any_active_firearm_case_blocking(CPed* criminal) {
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (const auto& crime : g_active_crimes) {
        if (!crime || crime->cancelled || !crime->is_firearm) continue;
        bool in_case = (crime->criminal == criminal);
        if (!in_case) {
            for (CPed* c : crime->consolidated_criminals) {
                if (c == criminal) {
                    in_case = true;
                    break;
                }
            }
        }
        if (!in_case) return true;
    }
    return false;
}

// 玩家协助追踪
std::atomic<CPed*> g_tracked_criminal{nullptr};
std::atomic<int64_t> g_last_assist_time_ms{0};
std::atomic<bool>    g_is_generating_custom_dispatch{false};    // 是否正在生成自定义调度车辆

// 玩家在协助警察时造成的流弹/误伤优化追踪
std::atomic<bool>    g_player_stray_bullet_flag{false};         // 是否误伤了市民
std::atomic<int64_t> g_player_stray_bullet_time{0};             // 市民误伤时间戳
std::atomic<int>     g_friendly_fire_cop_hits{0};               // 误伤警察次数计数
std::atomic<int64_t> g_last_friendly_fire_cop_time{0};          // 最后一次误伤警察时间戳
std::atomic<bool>    g_player_friendly_fire_blocked{false};     // 是否拦截本次误伤通缉

std::vector<AttackedNPC> g_player_attacked_npcs;
std::mutex g_attacked_npcs_mutex;

std::vector<CommandedCriminal> g_commanded_criminals;
std::mutex g_commanded_criminals_mutex;

std::vector<TemporaryRoadClosure> g_temp_road_closures;
std::mutex                         g_temp_closures_mutex;



// =====================================================================
// 时间工具
// =====================================================================
int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int get_random_range(int min_val, int max_val) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> distr(min_val, max_val);
    return distr(gen);
}

// Decision Engine Optimization
// 1. 多犯罪 NPC 响应只应该发生在原版中非枪械攻击（<= 35m）/枪械攻击（<= 75m）的视听范围内。
// 2. 仅当视听范围内不存在枪械攻击时，才允许激活（或劫持）一处远端范围的枪械攻击响应。
// 3. 远端范围的非枪械攻击响应，仅当视听范围内无任何活跃犯罪，且警力不足（周边 60m 无警员或有警员牺牲）时才激活一处。
bool should_activate_or_hijack_crime(CVector crime_pos, bool firearm) {
    if (!g_FindPlayerCoors) return true; // 降级默认允许
    
    CVector player_pos = g_FindPlayerCoors(0);
    float p_dx = crime_pos.x - player_pos.x;
    float p_dy = crime_pos.y - player_pos.y;
    float p_dz = crime_pos.z - player_pos.z;
    float dist_to_player = sqrtf(p_dx * p_dx + p_dy * p_dy + p_dz * p_dz);
    
    const float AV_RANGE_FIREARM = 75.0f;
    const float AV_RANGE_MELEE = 35.0f;
    
    // 1. 判定新发生的犯罪是否在其原本的视听范围内 (Local AV)
    bool in_local_av = false;
    if (firearm) {
        in_local_av = (dist_to_player <= AV_RANGE_FIREARM);
    } else {
        in_local_av = (dist_to_player <= AV_RANGE_MELEE);
    }
    
    // 规则 1：如果犯罪发生在其原本的视听范围内，始终允许激活
    if (in_local_av) {
        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
        int active_count = 0;
        for (const auto& crime : g_active_crimes) {
            if (crime && !crime->cancelled) {
                active_count++;
            }
        }
        if (active_count >= 4) {
            // 尝试找一个最远的或者威胁更低的非枪械远端案件取消，给这个本地案件腾位
            std::shared_ptr<CrimeEvent> worst_crime = nullptr;
            float max_dist = -1.0f;
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    float dx = crime->location.x - player_pos.x;
                    float dy = crime->location.y - player_pos.y;
                    float dz = crime->location.z - player_pos.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    float limit = crime->is_firearm ? AV_RANGE_FIREARM : AV_RANGE_MELEE;
                    if (dist > limit && dist > max_dist) {
                        max_dist = dist;
                        worst_crime = crime;
                    }
                }
            }
            if (worst_crime) {
                worst_crime->cancelled = true;
                LOGI("Cancelled distant case %llu (dist=%.1f) to make room for local crime scene", (unsigned long long)worst_crime->case_id, max_dist);
                return true;
            }
            LOGW("Blocked local crime scene because all 4 concurrent case slots are occupied by local active cases");
            return false;
        }
        LOGI("Crime within original AV range (Firearm=%d, Dist=%.1f) -> Normal trigger", firearm, dist_to_player);
        return true;
    }
    
    // 2. 远端犯罪过滤
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    int remote_firearm_count = 0;
    int remote_melee_count = 0;
    int local_count = 0;
    int total_active = 0;
    
    for (const auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            total_active++;
            float dx = crime->location.x - player_pos.x;
            float dy = crime->location.y - player_pos.y;
            float dz = crime->location.z - player_pos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            float limit = crime->is_firearm ? AV_RANGE_FIREARM : AV_RANGE_MELEE;
            if (dist <= limit) {
                local_count++;
            } else {
                if (crime->is_firearm) {
                    remote_firearm_count++;
                } else {
                    remote_melee_count++;
                }
            }
        }
    }
    
    if (total_active >= 4) {
        LOGI("Blocked remote crime (Dist=%.1f) because 4 concurrent case slots are already occupied", dist_to_player);
        return false; // 已满，不接受远端犯罪
    }
    
    if (firearm) {
        // 规则 2：远端枪械犯罪（dist > 75.0m）
        // “仅当不存在远端枪械攻击时，才允许激活一处远端范围的枪械攻击响应”
        if (remote_firearm_count > 0) {
            LOGI("Blocked remote firearm crime (Dist=%.1f) because another remote firearm crime is already active", dist_to_player);
            return false;
        }
        
        // 如果远端有非枪械犯罪，我们可以给新枪械犯罪让路
        if (remote_melee_count > 0) {
            for (auto& crime : g_active_crimes) {
                if (crime && !crime->cancelled) {
                    float dx = crime->location.x - player_pos.x;
                    float dy = crime->location.y - player_pos.y;
                    float dz = crime->location.z - player_pos.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist > AV_RANGE_MELEE && !crime->is_firearm) {
                        crime->cancelled = true;
                        LOGI("Remote firearm crime (Dist=%.1f) hijacks remote melee crime %llu (Dist=%.1f)", dist_to_player, (unsigned long long)crime->case_id, dist);
                        return true;
                    }
                }
            }
        }
        
        LOGI("Activated remote firearm crime response (Dist=%.1f)", dist_to_player);
        return true;
    } else {
        // 规则 3：远端非枪械犯罪（dist > 35.0m）
        // “远端非枪械犯罪仅当无任何活跃犯罪（或总活跃很少且警力不足）才激活”
        if (total_active > 0) {
            LOGI("Blocked remote melee crime (Dist=%.1f) because there are already active cases", dist_to_player);
            return false;
        }
        
        // 警力不足急需支援
        bool police_insufficient = false;
        if (g_FindDistToNearestCop) {
            float dist_to_cop = g_FindDistToNearestCop(PED_TYPE_COP, player_pos);
            if (dist_to_cop > 60.0f) {
                police_insufficient = true;
            }
        }
        
        if (police_insufficient) {
            LOGI("Activated remote melee crime response (Dist=%.1f) under low police situation", dist_to_player);
            return true;
        }
        
        LOGI("Blocked remote melee crime (Dist=%.1f) because local area is safe/police are abundant", dist_to_player);
        return false;
    }
}

// =====================================================================
// Case consolidation: merge crimes in the same area
// =====================================================================

CVector get_crime_dispatch_position(const CrimeEvent& crime) {
    return crime.dispatch_anchor;
}

bool try_consolidate_crime(CPed* perpetrator, CVector crime_pos, bool firearm, int weapon_category) {
    if (!perpetrator || !is_ped_pointer_valid_safe(perpetrator)) return false;
    if (!g_FindPlayerCoors) return false;
    
    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    
    // 寻找 40 米半径内最近的活跃案件
    std::shared_ptr<CrimeEvent> best_crime = nullptr;
    float min_dist = 999999.0f;
    for (auto& crime : g_active_crimes) {
        if (crime && !crime->cancelled) {
            float dx = crime_pos.x - crime->location.x;
            float dy = crime_pos.y - crime->location.y;
            float dz = crime_pos.z - crime->location.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist <= 40.0f && dist < min_dist) {
                min_dist = dist;
                best_crime = crime;
            }
        }
    }
    
    if (best_crime) {
        if (!dispatch_threat::should_merge_into_case(*best_crime, perpetrator, firearm, weapon_category)) {
            return false;
        }

        bool found = false;
        for (size_t i = 0; i < best_crime->consolidated_criminals.size(); ++i) {
            if (best_crime->consolidated_criminals[i] == perpetrator) {
                found = true;
                if (firearm && !best_crime->criminal_is_firearm[i]) {
                    best_crime->criminal_is_firearm[i] = true;
                    LOGI("Consolidated criminal %p escalated to firearm weapon in same area!", perpetrator);
                    
                    auto it = best_crime->criminal_states.find(perpetrator);
                    if (it != best_crime->criminal_states.end()) {
                        it->second.current_threat_category = 2;
                        if (it->second.first_threat_category < 2) {
                            it->second.first_threat_category = 2; // 升级首次攻击类别，因为升级了
                        }
                    }
                }
                break;
            }
        }
        
        if (!found) {
            best_crime->consolidated_criminals.push_back(perpetrator);
            best_crime->criminal_is_firearm.push_back(firearm);
            
            // 初始化嫌疑人详细状态
            CrimeEvent::CriminalState c_state;
            c_state.first_threat_category = firearm ? 2 : (weapon_category == 1 ? 1 : 0);
            c_state.current_threat_category = c_state.first_threat_category;
            c_state.is_active = true;
            c_state.shooting_air = false;
            c_state.fleeing = false;
            best_crime->criminal_states[perpetrator] = c_state;
            
            LOGI("📡 [dispatchCenter - CaseMerge] NPC criminal %p merged into active case %llu in same area. Total criminals: %zu", 
                 perpetrator, (unsigned long long)best_crime->case_id, best_crime->consolidated_criminals.size());
        }
        
        // 若新罪犯使用了枪械，自动升级整个案件的枪械属性
        if (firearm && !best_crime->is_firearm) {
            best_crime->is_firearm = true;
            LOGI("📡 [dispatchCenter - CaseMerge] Case %llu escalated to FIREARM due to consolidated criminal %p", (unsigned long long)best_crime->case_id, perpetrator);
        }
        
        dispatch_threat::refresh_crime_dispatch_anchor(*best_crime);

        // 立即让周边警员也攻击这个新罪犯
        make_cops_attack_criminal(perpetrator);
        return true; // 成功并案
    }
    return false; // 未能并案
}
