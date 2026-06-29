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
#include "dispatch_threat.hpp"


void CopAttackContext::reset() {
    thread_local static bool initialized = false;
    if (!initialized) {
        dispatched_vehicles_time_snapshot.reserve(128);
        stuck_vehicles_snapshot.reserve(128);
        vehicles_emptied_snapshot.reserve(128);
        vehicles_ordered_to_scene_snapshot.reserve(128);
        vehicles_siren_awakened_snapshot.reserve(128);

        pending_dispatched_vehicles_time.reserve(128);
        pending_vehicles_emptied.reserve(128);
        pending_vehicles_ordered_to_scene.reserve(128);
        pending_vehicles_siren_awakened.reserve(128);
        pending_stuck_vehicles.reserve(128);
        pending_temp_closures.reserve(128);

        counted_vehicles.reserve(128);
        initialized = true;
    }

    dispatched_vehicles_time_snapshot.clear();
    stuck_vehicles_snapshot.clear();
    vehicles_emptied_snapshot.clear();
    vehicles_ordered_to_scene_snapshot.clear();
    vehicles_siren_awakened_snapshot.clear();

    pending_dispatched_vehicles_time.clear();
    pending_vehicles_emptied.clear();
    pending_vehicles_ordered_to_scene.clear();
    pending_vehicles_siren_awakened.clear();
    pending_stuck_vehicles.clear();
    pending_temp_closures.clear();

    counted_vehicles.clear();

}

bool CopAttackContext::vector_contains(const std::vector<void*>& vec, void* val) const {
    for (void* item : vec) {
        if (item == val) return true;
    }
    return false;
}

void cop_attack_snapshot_globals(CopAttackContext& ctx) {
    {
        std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
        ctx.dispatched_vehicles_time_snapshot.assign(g_dispatched_vehicles_time.begin(), g_dispatched_vehicles_time.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
        ctx.stuck_vehicles_snapshot.assign(g_stuck_vehicles.begin(), g_stuck_vehicles.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
        ctx.vehicles_emptied_snapshot.assign(g_vehicles_emptied.begin(), g_vehicles_emptied.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_mutex);
        ctx.vehicles_ordered_to_scene_snapshot.assign(g_vehicles_ordered_to_scene.begin(), g_vehicles_ordered_to_scene.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
        ctx.vehicles_siren_awakened_snapshot.assign(g_vehicles_siren_awakened.begin(), g_vehicles_siren_awakened.end());
    }

}

void cop_attack_detect_firearm_threat(CopAttackContext& ctx) {
    ctx.is_active_firearm = false;
    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        if (ctx.crime_case->is_firearm) {
            ctx.is_active_firearm = true;
        } else {
            auto& list = ctx.crime_case->consolidated_criminals;
            auto& is_fire_list = ctx.crime_case->criminal_is_firearm;
            for (size_t idx = 0; idx < list.size(); ++idx) {
                if (list[idx] == ctx.criminal && idx < is_fire_list.size() && is_fire_list[idx]) {
                    ctx.is_active_firearm = true;
                    break;
                }
            }
        }
    }

}

void cop_attack_compute_quotas(CopAttackContext& ctx) {
    ctx.max_vehicles = 2;
    ctx.max_foot_cops = 2;

    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        dispatch_threat::ResponseQuota quota = dispatch_threat::compute_response_quota(
            *ctx.crime_case, ctx.crime_case->cops_killed);
        ctx.max_vehicles = quota.max_vehicles;
        ctx.max_foot_cops = quota.max_foot_cops;
    }

}

void cop_attack_commit_pending(CopAttackContext& ctx) {
    if (!ctx.pending_dispatched_vehicles_time.empty()) {
        std::lock_guard<std::mutex> lock(g_dispatched_vehicles_time_mutex);
        for (const auto& item : ctx.pending_dispatched_vehicles_time) {
            g_dispatched_vehicles_time[item.first] = item.second;
        }
    }
    if (!ctx.pending_vehicles_emptied.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_emptied_mutex);
        for (void* veh : ctx.pending_vehicles_emptied) {
            g_vehicles_emptied.insert(veh);
        }
    }
    if (!ctx.pending_vehicles_ordered_to_scene.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_mutex);
        for (void* veh : ctx.pending_vehicles_ordered_to_scene) {
            g_vehicles_ordered_to_scene.insert(veh);
        }
    }
    if (!ctx.pending_vehicles_siren_awakened.empty()) {
        std::lock_guard<std::mutex> lock(g_vehicles_siren_awakened_mutex);
        for (void* veh : ctx.pending_vehicles_siren_awakened) {
            g_vehicles_siren_awakened.insert(veh);
        }
    }
    if (!ctx.pending_stuck_vehicles.empty()) {
        std::lock_guard<std::mutex> lock(g_stuck_vehicles_mutex);
        for (const auto& item : ctx.pending_stuck_vehicles) {
            g_stuck_vehicles[item.first] = item.second;
        }
    }
    if (!ctx.pending_temp_closures.empty()) {
        std::lock_guard<std::mutex> lock(g_temp_closures_mutex);
        for (const auto& item : ctx.pending_temp_closures) {
            g_temp_road_closures.push_back(item);
        }
    }

}
