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
#include "dispatch_emergency_services.hpp"
#include "dispatch_hit_and_run.hpp"
#include "dispatch_cop_state.hpp"
#include "vanilla_qol_fixes.hpp"


// =====================================================================
// Dispatch tick entry + case cleanup
// =====================================================================
static std::atomic<bool> g_prev_save_loading{false};

void purge_dispatch_state_for_save_load() {
    std::vector<std::shared_ptr<CrimeEvent>> crimes_snapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
        crimes_snapshot.swap(g_active_crimes);
    }
    for (auto& crime : crimes_snapshot) {
        if (!crime) continue;
        crime->cancelled = true;
        cleanup_single_case_vehicles(crime);
    }

    g_tracked_criminal.store(nullptr);
    g_player_stray_bullet_flag.store(false);
    g_player_stray_bullet_time.store(0);
    g_friendly_fire_cop_hits.store(0);
    g_last_friendly_fire_cop_time.store(0);
    g_player_friendly_fire_blocked.store(false);
    g_player_wanted_level.store(0);
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
        std::lock_guard<std::mutex> lock_sc(g_vehicles_mutex);
        g_vehicles_ordered_to_scene.clear();
    }
    {
        std::lock_guard<std::mutex> lock_sa(g_vehicles_siren_awakened_mutex);
        g_vehicles_siren_awakened.clear();
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
    LOGI("💾 [SaveLoad] Purged mod dispatch state");
}

void poll_save_load_transition() {
    poll_save_load_hydration_state();
    poll_vanilla_qol_fixes();
    const bool loading = is_save_load_active();
    const bool prev = g_prev_save_loading.exchange(loading, std::memory_order_acq_rel);
    if (loading && !prev) {
        purge_dispatch_state_for_save_load();
    }
}
void cleanup_single_case_vehicles(std::shared_ptr<CrimeEvent> crime) {
    if (!crime) return;
    
    // 1. 解除封路
    if (crime->road_closure_active) {
        if (g_ThePaths && g_SwitchRoadsOffInArea) {
            CVector center = crime->road_closure_center;
            g_SwitchRoadsOffInArea(
                g_ThePaths,
                center.x - 50.0f, center.y - 50.0f, center.z - 15.0f,
                center.x + 50.0f, center.y + 50.0f, center.z + 15.0f,
                false, true, false
            );
            crime->road_closure_active = false;
            LOGI("🚧 [dispatchCenter - Cordon] Lifted road closure for case %llu", (unsigned long long)crime->case_id);
        }
    }
    
    // 2. 精准擦除当前案件的警车绑定
    std::lock_guard<std::mutex> lock_emp(g_vehicles_emptied_mutex);
    std::lock_guard<std::mutex> lock_veh(g_vehicles_mutex);
    std::lock_guard<std::mutex> lock_dis(g_dispatched_vehicles_time_mutex);
    std::lock_guard<std::mutex> lock_sc(g_vehicles_siren_awakened_mutex);
    std::lock_guard<std::mutex> lock_spawn(g_spawned_cop_vehicles_mutex);
    
    for (void* veh : crime->case_vehicles) {
        if (veh) {
            g_vehicles_emptied.erase(veh);
            g_vehicles_ordered_to_scene.erase(veh);
            g_dispatched_vehicles_time.erase(veh);
            g_vehicles_siren_awakened.erase(veh);
            
            auto it = std::find(g_spawned_cop_vehicles.begin(), g_spawned_cop_vehicles.end(), veh);
            if (it != g_spawned_cop_vehicles.end()) {
                g_spawned_cop_vehicles.erase(it);
            }
        }
    }
    crime->case_vehicles.clear();
    dispatch_emergency_services::clear_case_emergency_records(crime->case_id);
    LOGI("📡 [dispatchCenter - GC] Cleaned up vehicles and route closures for case %llu", (unsigned long long)crime->case_id);
}

void proxy_the_scripts_process() {
    SHADOWHOOK_STACK_SCOPE();

    if (g_orig_the_scripts_process) {
        g_orig_the_scripts_process();
    }

    poll_save_load_transition();
    if (!is_mod_dispatch_paused()) {
        on_main_thread_tick();
    }
}
