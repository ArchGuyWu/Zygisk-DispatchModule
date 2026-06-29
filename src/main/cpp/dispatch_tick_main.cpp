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
#include "dispatch_reroute.hpp"
#include "dispatch_cop_state.hpp"
#include "dispatch_emergency_services.hpp"
#include "dispatch_hit_and_run.hpp"


// =====================================================================
// Main dispatch tick orchestrator
// =====================================================================
static int64_t g_last_tick_time_ms = 0;

// =====================================================================
// Custom dispatch vehicle spawn wrapper
// =====================================================================
CVector dispatch_spawn_emergency_car(unsigned int model, CVector pos, CVector incident_anchor) {
    pos = dispatch_emergency_services::clamp_spawn_to_streaming_range(incident_anchor, pos);
    g_is_generating_custom_dispatch.store(true);
    if (g_ScriptGenEmergencyCar) {
        g_ScriptGenEmergencyCar(model, pos);
    } else if (g_GenOneEmergencyCar) {
        g_GenOneEmergencyCar(model, pos);
    }
    g_is_generating_custom_dispatch.store(false);
    return pos;
}
void on_main_thread_tick() {
    // 1. 每帧高频平滑避让更新（无感避让）
    apply_civilian_avoidance_field();

    int64_t cur_time = now_ms();
    if (cur_time - g_last_tick_time_ms < 250) {
        return;
    }
    g_last_tick_time_ms = cur_time;

    ecs::EventDispatcher::get().dispatch(ecs::TickEvent(cur_time));

    if (!g_FindPlayerPed || !g_FindPlayerPed(0)) {
        return; // 不在游戏内
    }

    // 运行急救与消防车辆的自主脱困、避障与高级物理救援心跳
    emergency_vehicles_tick();

    // 周期性释放已到期的临时道路关闭
    {
        std::lock_guard<std::mutex> temp_lock(g_temp_closures_mutex);
        for (auto it = g_temp_road_closures.begin(); it != g_temp_road_closures.end(); ) {
            if (cur_time >= it->reopen_time_ms) {
                if (g_ThePaths && g_SwitchRoadsOffInArea) {
                    g_SwitchRoadsOffInArea(
                        g_ThePaths,
                        it->center.x - it->radius, it->center.y - it->radius, it->center.z - 8.0f,
                        it->center.x + it->radius, it->center.y + it->radius, it->center.z + 8.0f,
                        false, true, false
                    );
                    LOGI("🚧 [dispatchCenter - TempClosure] Reopened route section around (%.1f, %.1f, %.1f) after delay", 
                         it->center.x, it->center.y, it->center.z);
                }
                it = g_temp_road_closures.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);

    // 复制一份 shared_ptr 数组快照，绝对规避在派发和回调过程中（同一线程上重入时）导致 `g_active_crimes` 扩容而引发的迭代器失效 crash
    std::vector<std::shared_ptr<CrimeEvent>> crimes_snapshot = g_active_crimes;

    bool any_active_case = false;
    for (const auto& crime : crimes_snapshot) {
        if (crime && !crime->cancelled) {
            any_active_case = true;
            break;
        }
    }

    // Periodically bind occupants of active mod-spawned vehicles
    if (any_active_case) {
        std::lock_guard<std::mutex> lock_sp(g_spawned_cop_vehicles_mutex);
        for (void* veh : g_spawned_cop_vehicles) {
            if (is_vehicle_pointer_valid(veh)) {
                bind_vehicle_occupants(veh);
            }
        }
        for (const auto& crime : crimes_snapshot) {
            if (!crime || crime->cancelled) continue;
            for (void* veh : crime->case_vehicles) {
                if (!veh || !is_vehicle_pointer_valid(veh)) continue;
                unsigned int model = get_entity_model_index(veh);
                if (model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK ||
                    model == MODEL_POLICE_CAR || model == MODEL_POLICE_BIKE ||
                    model == MODEL_SWAT_VAN) {
                    bind_vehicle_occupants(veh);
                }
            }
        }
    }

    static int64_t last_siren_refresh = 0;
    bool do_siren_refresh = (cur_time - last_siren_refresh > 1500);
    if (do_siren_refresh) {
        last_siren_refresh = cur_time;
    }

    // =====================================================================
    // 👻 [dispatchCenter - GhostVehicleGuard] 幽灵车急停锁定保护（防止半路警员跌落摩托，空车狂奔）
    // =====================================================================
    if (any_active_case) {
        for (const auto& crime : crimes_snapshot) {
            if (!crime || crime->cancelled) continue;
            for (void* veh : crime->case_vehicles) {
                if (is_vehicle_pointer_valid(veh) && !is_vehicle_emptied(veh)) {
                    if (get_entity_model_index(veh) == MODEL_POLICE_HELI) continue;
                    if (!is_vehicle_occupied_by_driver(veh)) {
                        CVector veh_pos = get_entity_pos(veh);
                        if (g_GetCarToGoToCoors) {
                            g_GetCarToGoToCoors(veh, &veh_pos, 4, false); // Mode 4 (DF_STOP_CAR) 瞬间手刹锁死
                        }
                        add_vehicle_emptied(veh);
                        LOGW("⚠️ [dispatchCenter - GhostVehicleGuard] Handbrake-locked ghost vehicle %p (no driver). Marked as emptied.", veh);
                    }
                }
            }
        }
    }

    dispatch_reroute::apply_enroute_vehicle_reroutes(crimes_snapshot);
    dispatch_emergency_services::emergency_services_tick(crimes_snapshot, cur_time);

    for (auto& crime : crimes_snapshot) {
        if (!crime || crime->cancelled) {
            continue;
        }
        dispatch_tick_process_crime(crime, cur_time, do_siren_refresh);
    }

    // 垃圾回收阶段：清理所有标记为已取消 (cancelled) 或已经转为 STATE_CLEANUP 的案件
    for (auto it = g_active_crimes.begin(); it != g_active_crimes.end(); ) {
        if ((*it)->cancelled || (*it)->dispatch_state == STATE_CLEANUP) {
            LOGI("📡 [dispatchCenter - CaseGC] Erasing case %llu from active crimes list", (unsigned long long)(*it)->case_id);
            cleanup_single_case_vehicles(*it);
            it = g_active_crimes.erase(it);
        } else {
            ++it;
        }
    }

    // Fallback 清理层：如果当前没有任何活跃案件存在，则统一恢复/释放全局所有的向下兼容级和未分组表映射
    if (g_active_crimes.empty()) {
        g_tracked_criminal.store(nullptr);

        g_player_stray_bullet_flag.store(false);
        g_player_stray_bullet_time.store(0);
        g_friendly_fire_cop_hits.store(0);
        g_last_friendly_fire_cop_time.store(0);
        g_player_friendly_fire_blocked.store(false);
        {
            std::lock_guard<std::mutex> lock_emp(g_vehicles_emptied_mutex);
            g_vehicles_emptied.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
            g_dispatched_vehicles_time.clear();
        }
        dispatch_cop_state::clear_all_cop_dispatch_state();
        {
            std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
            g_stuck_vehicles.clear();
        }
        {
            std::lock_guard<std::mutex> lock_sc(g_vehicles_mutex);
            g_vehicles_ordered_to_scene.clear();
        }
        {
            std::lock_guard<std::mutex> lock_sa(g_vehicles_siren_awakened_mutex);
            g_vehicles_siren_awakened.clear();
        }
        {
            std::lock_guard<std::mutex> lock_swat(g_spawned_swats_mutex);
            g_spawned_swats.clear();
        }
        {
            std::lock_guard<std::mutex> lock_bind(g_bindings_mutex);
            g_cop_vehicle_bindings.clear();
        }
        clear_vehicle_driver_locks();
        clear_vehicle_enter_command_timestamps();
        {
            std::lock_guard<std::mutex> lock_ex(g_exits_mutex);
            g_cop_exits.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_spawned_cop_vehicles_mutex);
            g_spawned_cop_vehicles.clear();
        }
        dispatch_emergency_services::clear_all_emergency_records();
        dispatch_hit_and_run::clear_all_pending();
    }
}

