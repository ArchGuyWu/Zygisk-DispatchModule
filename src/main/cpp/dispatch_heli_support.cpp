#include <algorithm>
#include <cmath>
#include <memory>

#include "dispatch_heli_support.hpp"
#include "pointer_sanitizer.hpp"
#include "dispatch_emergency_services.hpp"
#include "dispatch_threat.hpp"
#include "dispatch_tick_internal.hpp"
#include "dispatch_timing.hpp"
#include "game_config.hpp"
#include "mod_shared.hpp"

namespace dispatch_heli_support {

static int64_t g_last_heli_refresh_ms = 0;

static bool heli_apis_available() {
    return g_CreateCarForScript != nullptr && g_FlyAIHeliToTarget_FixedOrientation != nullptr;
}

static bool is_firearm_case(const CrimeEvent& crime) {
    if (crime.is_firearm) return true;
    int max_threat = dispatch_threat::threat_level_score(
        dispatch_threat::get_case_max_threat_level(crime));
    return max_threat >= dispatch_threat::threat_level_score(
        ecs::CriminalThreatLevel::FIREARM_INACTIVE);
}

static bool is_outdoor_position(CVector pos) {
    return pos.z >= 2.0f;
}

static bool should_dispatch_police_heli(const CrimeEvent& crime, int density) {
    if (!is_firearm_case(crime)) return false;
    if (!is_outdoor_position(get_crime_dispatch_position(crime))) return false;

    int active_firearm = dispatch_threat::threat_level_score(
        ecs::CriminalThreatLevel::FIREARM_ACTIVE);

    if (crime.cops_killed >= 2) return true;
    if (crime.cops_killed >= 1 && density >= 5 &&
        dispatch_threat::count_high_threats(crime) >= 2) {
        return true;
    }
    if (density >= 6 &&
        dispatch_threat::threat_level_score(
            dispatch_threat::get_case_max_threat_level(crime)) >= active_firearm) {
        return true;
    }
    return false;
}

int count_active_police_helis_near(CVector pos, float range) {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle) return 0;

    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool || !is_pointer_readable(pool)) return 0;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map || !is_pointer_readable(byte_map)) return 0;

    int count = 0;
    float range_sq = range * range;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag < 0) continue;

        int handle = (i << 8) | flag;
        void* veh = g_GetPoolVehicle(handle);
        if (!veh || !is_vehicle_pointer_valid(veh)) continue;
        if (get_entity_model_index(veh) != MODEL_POLICE_HELI) continue;

        CVector veh_pos = get_entity_pos(veh);
        float dx = veh_pos.x - pos.x;
        float dy = veh_pos.y - pos.y;
        float dz = veh_pos.z - pos.z;
        if ((dx * dx + dy * dy + dz * dz) <= range_sq) {
            count++;
        }
    }
    return count;
}

static CVector compute_hover_target(CVector incident_anchor) {
    CVector target = incident_anchor;
    target.z += dispatch_timing::HELI_HOVER_ALTITUDE_M;
    return target;
}

static CVector compute_heli_spawn_pos(CVector incident_anchor) {
    CVector spawn = dispatch_emergency_services::clamp_spawn_to_streaming_range(
        incident_anchor, get_spawn_target(incident_anchor));
    spawn.z += dispatch_timing::HELI_SPAWN_ALTITUDE_M;

    if (g_FindPlayerCoors) {
        CVector player_pos = g_FindPlayerCoors(0);
        float dx = spawn.x - player_pos.x;
        float dy = spawn.y - player_pos.y;
        float dist_xy = sqrtf(dx * dx + dy * dy);
        if (dist_xy > dispatch_timing::EMERGENCY_STREAMING_TARGET_DIST_M) {
            float scale = dispatch_timing::EMERGENCY_STREAMING_TARGET_DIST_M / dist_xy;
            spawn.x = player_pos.x + dx * scale;
            spawn.y = player_pos.y + dy * scale;
        }
    }
    return spawn;
}

static void command_heli_to_scene(void* heli, CVector incident_anchor) {
    if (!heli || !g_FlyAIHeliToTarget_FixedOrientation) return;
    CVector hover_target = compute_hover_target(incident_anchor);
    g_FlyAIHeliToTarget_FixedOrientation(heli, 0.0f, hover_target);
}

static void finalize_spawned_heli(
    const std::shared_ptr<CrimeEvent>& crime,
    void* heli,
    CVector incident_anchor) {
    if (!crime || !heli) return;

    crime->case_vehicles.push_back(heli);
    if (!crime->spawned_vehicle) {
        crime->spawned_vehicle = heli;
    }

    if (g_AddPoliceOccupants) {
        g_AddPoliceOccupants(reinterpret_cast<CVehicle*>(heli), true);
    }

    command_heli_to_scene(heli, incident_anchor);

    if (!is_vehicle_siren_awakened(heli) && g_VehicleInflictDamage) {
        CVector veh_pos = get_entity_pos(heli);
        g_VehicleInflictDamage(heli, nullptr, WEAPON_UNARMED, 0.0f, veh_pos);
        add_vehicle_siren_awakened(heli);
    }

    LOGI("✅ [HeliSupport] Case %llu police heli %p dispatched to (%.1f, %.1f, %.1f)",
         (unsigned long long)crime->case_id, heli,
         incident_anchor.x, incident_anchor.y, incident_anchor.z);
}

static void* spawn_police_heli(CVector spawn_pos) {
    if (!heli_apis_available()) return nullptr;

    void* heli = g_CreateCarForScript(
        static_cast<int>(MODEL_POLICE_HELI), spawn_pos, 1);
    if (heli && is_vehicle_pointer_valid(heli)) {
        return heli;
    }

    if (g_ScriptGenEmergencyCar || g_GenOneEmergencyCar) {
        g_is_generating_custom_dispatch.store(true);
        if (g_ScriptGenEmergencyCar) {
            g_ScriptGenEmergencyCar(MODEL_POLICE_HELI, spawn_pos);
        } else {
            g_GenOneEmergencyCar(MODEL_POLICE_HELI, spawn_pos);
        }
        g_is_generating_custom_dispatch.store(false);
        return find_closest_vehicle_to(spawn_pos, 40.0f);
    }

    return nullptr;
}

void refresh_active_helis(const std::shared_ptr<CrimeEvent>& crime) {
    if (!crime || crime->cancelled || crime->mod_heli_dispatched <= 0) return;
    if (!heli_apis_available()) return;

    int64_t cur_time = now_ms();
    if (cur_time - g_last_heli_refresh_ms < dispatch_timing::HELI_REFRESH_INTERVAL_MS) {
        return;
    }
    g_last_heli_refresh_ms = cur_time;

    CVector incident_anchor = get_crime_dispatch_position(*crime);
    for (void* veh : crime->case_vehicles) {
        if (!veh || !is_vehicle_pointer_valid(veh)) continue;
        if (get_entity_model_index(veh) != MODEL_POLICE_HELI) continue;
        command_heli_to_scene(veh, incident_anchor);
    }
}

void evaluate_case_heli_support(const std::shared_ptr<CrimeEvent>& crime, int64_t cur_time) {
    if (!crime || crime->cancelled) return;
    if (crime->mod_heli_dispatched > 0) return;
    if (crime->dispatch_state != STATE_ON_SCENE) return;
    if (!heli_apis_available()) return;

    CVector incident_anchor = get_crime_dispatch_position(*crime);
    if (!is_outdoor_position(incident_anchor)) return;

    if (g_FindPlayerCoors) {
        CVector player_pos = g_FindPlayerCoors(0);
        float dx = player_pos.x - incident_anchor.x;
        float dy = player_pos.y - incident_anchor.y;
        float dz = player_pos.z - incident_anchor.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist > 150.0f) return;
    }

    int density = std::max(
        count_criminals_near(incident_anchor, 40.0f),
        dispatch_threat::count_active_threats(*crime));
    if (!should_dispatch_police_heli(*crime, density)) return;

    if (count_active_police_helis_near(incident_anchor, dispatch_timing::HELI_GLOBAL_CAP_RANGE_M) >=
        dispatch_timing::MAX_POLICE_HELI_NEAR_PLAYER) {
        LOGI("🚫 [HeliSupport] Global heli cap reached near case %llu (deferred)",
             (unsigned long long)crime->case_id);
        return;
    }

    crime->mod_heli_dispatched = 1;
    CVector spawn_pos = compute_heli_spawn_pos(incident_anchor);
    int delay = get_random_range(
        dispatch_timing::HELI_SPAWN_DELAY_MIN_MS,
        dispatch_timing::HELI_SPAWN_DELAY_MAX_MS);

    LOGI("🚁 [HeliSupport] Scheduling police heli for case %llu in %dms "
         "(cops_killed=%d, density=%d, spawn_z=%.1f)",
         (unsigned long long)crime->case_id, delay,
         crime->cops_killed, density, spawn_pos.z);

    crime->pending_tasks.push_back({
        cur_time + delay,
        [crime, spawn_pos, incident_anchor]() {
            if (crime->cancelled) {
                crime->mod_heli_dispatched = 0;
                return;
            }

            if (count_active_police_helis_near(
                    incident_anchor, dispatch_timing::HELI_GLOBAL_CAP_RANGE_M) >=
                dispatch_timing::MAX_POLICE_HELI_NEAR_PLAYER) {
                LOGI("🚫 [HeliSupport] Heli spawn aborted for case %llu (cap reached at spawn time)",
                     (unsigned long long)crime->case_id);
                crime->mod_heli_dispatched = 0;
                return;
            }

            void* heli = spawn_police_heli(spawn_pos);
            if (!heli) {
                LOGW("⚠️ [HeliSupport] Failed to spawn police heli for case %llu",
                     (unsigned long long)crime->case_id);
                crime->mod_heli_dispatched = 0;
                return;
            }

            finalize_spawned_heli(crime, heli, incident_anchor);
        }
    });
}

} // namespace dispatch_heli_support