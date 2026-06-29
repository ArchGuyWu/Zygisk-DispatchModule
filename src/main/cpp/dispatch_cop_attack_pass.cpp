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
#include <unordered_set>
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

namespace {

struct CopPoolEntry {
    CPed* ped = nullptr;
    CVector cop_pos{};
    void* vehicle = nullptr;
    CPed* target_criminal = nullptr;
    CVector target_crime_pos{};
    float dist_sq = 0.0f;
};

static bool is_crime_criminal_ped(CopAttackContext& ctx, CPed* ped) {
    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        for (CPed* c : ctx.crime_case->consolidated_criminals) {
            if (ped == c) return true;
        }
        return false;
    }
    return ped == ctx.criminal;
}

static void pick_target_for_cop(CopAttackContext& ctx, CPed* ped, CVector cop_pos,
                                CPed*& target_criminal, CVector& target_crime_pos) {
    target_criminal = ctx.criminal;
    target_crime_pos = ctx.crime_pos;
    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        CPed* picked = dispatch_threat::pick_criminal_target_for_cop(ped, *ctx.crime_case);
        if (picked) {
            target_criminal = picked;
            target_crime_pos = get_entity_pos(picked);
        }
    }
}

static int64_t get_last_assign_ms(CopAttackContext& ctx, CPed* ped) {
    int64_t last_assign = 0;
    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        for (CPed* c : ctx.crime_case->consolidated_criminals) {
            auto key = std::make_pair(ped, c);
            for (const auto& item : ctx.cop_attack_assign_time_snapshot) {
                if (item.first == key) {
                    last_assign = std::max(last_assign, item.second);
                    break;
                }
            }
        }
    } else {
        auto key = std::make_pair(ped, ctx.criminal);
        for (const auto& item : ctx.cop_attack_assign_time_snapshot) {
            if (item.first == key) {
                last_assign = item.second;
                break;
            }
        }
    }
    return last_assign;
}

static bool is_targeting_case_criminal(CopAttackContext& ctx, CPed* ped) {
    if (!g_GetWeaponLockOnTarget) return false;
    CEntity* target = g_GetWeaponLockOnTarget(ped);
    if (!target) return false;
    if (ctx.crime_case && !ctx.crime_case->cancelled) {
        for (CPed* c : ctx.crime_case->consolidated_criminals) {
            if (target == reinterpret_cast<CEntity*>(c)) return true;
        }
        return false;
    }
    return target == reinterpret_cast<CEntity*>(ctx.criminal);
}

static void build_cop_pool_entries(CopAttackContext& ctx, std::vector<CopPoolEntry>& entries) {
    entries.clear();
    entries.reserve(64);

    std::unordered_set<CPed*> seen;
    {
        std::lock_guard<std::mutex> lock(g_dispatch_active_cops_mutex);
        for (CPed* ped : g_dispatch_active_cops) {
            if (!ped || !is_ped_pointer_valid_safe(ped)) continue;
            if (g_IsAlive && !g_IsAlive(ped)) continue;
            if (g_GetPedType && g_GetPedType(ped) != PED_TYPE_COP) continue;
            if (is_crime_criminal_ped(ctx, ped)) continue;

            CopPoolEntry entry;
            entry.ped = ped;
            entry.cop_pos = get_entity_pos(ped);
            pick_target_for_cop(ctx, ped, entry.cop_pos, entry.target_criminal, entry.target_crime_pos);
            float dx = entry.cop_pos.x - entry.target_crime_pos.x;
            float dy = entry.cop_pos.y - entry.target_crime_pos.y;
            float dz = entry.cop_pos.z - entry.target_crime_pos.z;
            entry.dist_sq = dx * dx + dy * dy + dz * dz;
            entry.vehicle = find_vehicle_of_cop_cached(ped);
            entries.push_back(entry);
            seen.insert(ped);
        }
    }

    float scan_range_sq = ctx.av_range_sq > 0.0f
        ? ctx.av_range_sq
        : (75.0f * 75.0f);

    for (int i = 0; i < ctx.pool_size; i++) {
        signed char flag = ctx.byte_map[i];
        if (flag < 0) continue;

        int handle = (i << 8) | flag;
        CPed* ped = g_GetPoolPed(handle);
        if (!ped || !is_ped_pointer_valid_safe(ped)) continue;
        if (seen.count(ped) > 0) continue;
        if (g_IsAlive && !g_IsAlive(ped)) continue;
        if (g_GetPedType(ped) != PED_TYPE_COP) continue;
        if (is_crime_criminal_ped(ctx, ped)) continue;

        CopPoolEntry entry;
        entry.ped = ped;
        entry.cop_pos = get_entity_pos(ped);
        pick_target_for_cop(ctx, ped, entry.cop_pos, entry.target_criminal, entry.target_crime_pos);
        float dx = entry.cop_pos.x - entry.target_crime_pos.x;
        float dy = entry.cop_pos.y - entry.target_crime_pos.y;
        float dz = entry.cop_pos.z - entry.target_crime_pos.z;
        entry.dist_sq = dx * dx + dy * dy + dz * dz;
        if (entry.dist_sq > scan_range_sq) continue;

        entry.vehicle = find_vehicle_of_cop_cached(ped);
        entries.push_back(entry);
        seen.insert(ped);
    }
}

static void count_active_from_entries(CopAttackContext& ctx, const std::vector<CopPoolEntry>& entries) {
    ctx.active_vehicles_count = 0;
    ctx.active_foot_cops_count = 0;
    ctx.counted_vehicles.clear();

    for (const CopPoolEntry& entry : entries) {
        if (entry.vehicle && is_vehicle_pointer_valid(entry.vehicle)) {
            if (ctx.vector_contains(ctx.vehicles_ordered_to_scene_snapshot, entry.vehicle) ||
                ctx.vector_contains(ctx.vehicles_siren_awakened_snapshot, entry.vehicle) ||
                (ctx.crime_case && entry.vehicle == ctx.crime_case->spawned_vehicle)) {
                if (!ctx.vector_contains(ctx.counted_vehicles, entry.vehicle)) {
                    ctx.counted_vehicles.push_back(entry.vehicle);
                    ctx.active_vehicles_count++;
                }
            }
        } else {
            int64_t last_assign = get_last_assign_ms(ctx, entry.ped);
            bool already_targeting = is_targeting_case_criminal(ctx, entry.ped);
            if (already_targeting || (now_ms() - last_assign < 15000)) {
                ctx.active_foot_cops_count++;
            }
        }
    }
}

} // namespace

void cop_attack_single_pass_dispatch(CopAttackContext& ctx) {
    std::vector<CopPoolEntry> entries;
    build_cop_pool_entries(ctx, entries);
    count_active_from_entries(ctx, entries);
    cop_attack_compute_quotas(ctx);

    for (const CopPoolEntry& entry : entries) {
        if (entry.vehicle && is_vehicle_pointer_valid(entry.vehicle)) {
            cop_attack_dispatch_vehicle_cop(
                ctx, entry.ped, entry.target_criminal, entry.target_crime_pos, entry.vehicle);
        } else {
            cop_attack_dispatch_foot_cop(
                ctx, entry.ped, entry.target_criminal, entry.target_crime_pos, entry.dist_sq);
        }
    }
}

