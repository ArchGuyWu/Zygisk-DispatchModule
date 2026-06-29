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
#include "dispatch_tick_internal.hpp"
#include "dispatch_threat.hpp"
#include "dispatch_emergency_services.hpp"
#include "dispatch_heli_support.hpp"
#include "game_config.hpp"


void dispatch_tick_process_crime(
    const std::shared_ptr<CrimeEvent>& crime,
    int64_t cur_time,
    bool do_siren_refresh) {
    if (crime && !crime->cancelled) {
        dispatch_threat::refresh_crime_dispatch_anchor(*crime);
    }

    // 刷新各个案件独立拥有的警车驾驶路径与警笛
    if (do_siren_refresh) {
        std::lock_guard<std::mutex> lock_sc(g_vehicles_mutex);
        CVector dispatch_pos = get_crime_dispatch_position(*crime);
        for (void* veh : crime->case_vehicles) {
            if (!is_vehicle_pointer_valid(veh) || is_vehicle_emptied(veh)) continue;
            unsigned int model = get_entity_model_index(veh);
            if (model == MODEL_POLICE_HELI) {
                dispatch_heli_support::refresh_active_helis(crime);
            } else if (model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK) {
                dispatch_emergency_services::command_emergency_vehicle_to_scene(veh, model, dispatch_pos);
            } else {
                command_cop_vehicle_to_scene(veh, dispatch_pos);
            }
        }
    }

    // 检查活动犯罪分子是否依然有效 (并案机制：遍历并清理已失效或已死亡的犯罪NPC)
    {
        auto& list = crime->consolidated_criminals;
        auto& is_fire_list = crime->criminal_is_firearm;

        // 1. 清理列表中所有已失效 (despawned) 的 NPC
        for (auto it = list.begin(); it != list.end(); ) {
            if (!*it || !is_ped_pointer_valid_safe(*it)) {
                size_t idx = std::distance(list.begin(), it);
                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Consolidated criminal NPC %p is no longer valid (despawned) -> removing from case",
                     (unsigned long long)crime->case_id, *it);
                it = list.erase(it);
                if (idx < is_fire_list.size()) {
                    is_fire_list.erase(is_fire_list.begin() + idx);
                }
            } else {
                ++it;
            }
        }

        // 2. 如果当前 primary criminal 已经不合法，从并案列表中转移主犯
        if (crime->criminal && !is_ped_pointer_valid_safe(crime->criminal)) {
            if (!list.empty()) {
                crime->criminal = list.front();
                if (crime == get_primary_active_crime()) {
                    g_tracked_criminal.store(list.front());
                }
                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Primary criminal was despawned. Shifted primary tracking to %p.",
                     (unsigned long long)crime->case_id, crime->criminal);
            } else {
                LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: Active criminal NPC is no longer valid (despawned) and no other consolidated criminals -> cancelling crime event",
                     (unsigned long long)crime->case_id);
                crime->cancelled = true;
            }
        } else if (list.empty()) {
            LOGI("📡 [dispatchCenter - CaseMerge] Case %llu: All consolidated criminals are invalid/despawned -> cancelling crime event",
                 (unsigned long long)crime->case_id);
            crime->cancelled = true;
        }
    }

    // 轮询并执行过期的异步挂起任务 (串行，线程安全，并在回调中避免迭代器失效)
    if (!crime->cancelled) {
        int64_t now = now_ms();
        std::vector<CrimeEvent::DelayedTask> tasks_to_execute;

        for (auto it = crime->pending_tasks.begin(); it != crime->pending_tasks.end(); ) {
            if (now >= it->execute_time_ms) {
                tasks_to_execute.push_back(*it);
                it = crime->pending_tasks.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& task : tasks_to_execute) {
            if (!crime->cancelled) {
                task.callback();
            }
        }
    }

    // 对每一个案件独立运行其调度状态机
    if (!crime->cancelled) {
        switch (crime->dispatch_state) {
            case STATE_IDLE:
                dispatch_tick_state_idle(crime);
                break;
            case STATE_TIMING:
                dispatch_tick_state_timing(crime);
                break;
            case STATE_ON_SCENE:
                dispatch_tick_state_on_scene(crime);
                break;
            case STATE_CLEANUP:
                dispatch_tick_state_cleanup(crime);
                break;
            default:
                break;
        }
    }
}