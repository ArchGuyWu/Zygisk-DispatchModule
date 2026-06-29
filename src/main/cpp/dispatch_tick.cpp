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
// Dispatch tick entry + case cleanup
// =====================================================================
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
    LOGI("📡 [dispatchCenter - GC] Cleaned up vehicles and route closures for case %llu", (unsigned long long)crime->case_id);
}

void proxy_the_scripts_process() {
    SHADOWHOOK_STACK_SCOPE();

    if (g_orig_the_scripts_process) {
        g_orig_the_scripts_process();
    }

    on_main_thread_tick();
}
