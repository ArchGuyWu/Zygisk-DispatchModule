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
#include "dispatch_timing.hpp"
#include "dispatch_cop_state.hpp"


void cop_attack_dispatch_foot_cop(
    CopAttackContext& ctx,
    CPed* ped,
    CPed* target_criminal,
    CVector target_crime_pos,
    float dist_sq) {
    float scan_range_sq = ctx.av_range_sq > 0.0f
        ? ctx.av_range_sq
        : (dispatch_timing::AV_RANGE_FIREARM_M * dispatch_timing::AV_RANGE_FIREARM_M);
    if (dist_sq > scan_range_sq) {
        return;
    }

    bool within_native_av = ctx.av_range_sq > 0.0f && dist_sq <= ctx.av_range_sq;
    int64_t last_assign = dispatch_cop_state::get_last_assign_ms(ped, target_criminal);

    bool already_targeting = false;
    if (g_GetWeaponLockOnTarget && g_GetWeaponLockOnTarget(ped) == reinterpret_cast<CEntity*>(target_criminal)) {
        already_targeting = true;
    }

    bool is_specific_firearm = is_specific_criminal_armed_with_firearm(target_criminal);
    eWeaponType target_weapon = determine_weapon_for_cop(ped, target_criminal, is_specific_firearm);
    bool is_melee = (target_weapon == WEAPON_NIGHTSTICK || target_weapon == WEAPON_UNARMED || target_weapon < 22);

    bool just_exited_vehicle = is_cop_currently_exiting(ped);
    if (!just_exited_vehicle) {
        auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
        if (cop_comp && cop_comp->has_exited_vehicle) {
            just_exited_vehicle = true;
        }
    }

    if (!within_native_av) {
        if (!just_exited_vehicle &&
            (already_targeting || (now_ms() - last_assign < (is_melee ?
                dispatch_timing::FOOT_ASSIGN_MELEE_MS : dispatch_timing::FOOT_ASSIGN_FIREARM_MS)))) {
            return;
        }
    }

    bool already_assigned_foot_cop = already_targeting ||
        (now_ms() - last_assign < dispatch_timing::FOOT_ASSIGN_ACTIVE_WINDOW_MS);
    if (!within_native_av && !already_assigned_foot_cop && ctx.active_foot_cops_count >= ctx.max_foot_cops) {
        return;
    }

    make_single_cop_attack_criminal(ped, target_criminal, within_native_av);

    if (!already_assigned_foot_cop) {
        ctx.active_foot_cops_count++;
    }
    LOGI("🎯 [Foot Cop Dispatch] %s combat dispatch to cop %p -> criminal %p (active_foot_cops=%d/%d, dist=%.1f)",
         within_native_av ? "AV-forced" : "Native-first",
         ped, target_criminal, ctx.active_foot_cops_count, ctx.max_foot_cops, sqrtf(dist_sq));
}