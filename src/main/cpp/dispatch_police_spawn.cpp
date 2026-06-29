#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "dispatch_police_spawn.hpp"
#include "dispatch_threat.hpp"
#include "dispatch_tick_internal.hpp"
#include "dispatch_timing.hpp"
#include "ecs_engine.hpp"
#include "game_config.hpp"
#include "mod_shared.hpp"

namespace dispatch_police_spawn {

enum class MapRegion {
    LOS_SANTOS,
    SAN_FIERRO,
    LAS_VENTURAS,
    COUNTRYSIDE,
};

// 与 GTA SA 城区边界对齐（CCarCtrl 应急车辆分区近似）
static MapRegion detect_map_region(CVector pos) {
    const bool in_ls = (pos.x > 44.0f && pos.x < 2990.0f &&
                        pos.y > -2890.0f && pos.y < -760.0f);
    const bool in_sf = (pos.x > -2990.0f && pos.x < -850.0f &&
                        pos.y > -1410.0f && pos.y < 1420.0f);
    const bool in_lv = (pos.x > 860.0f && pos.x < 2990.0f &&
                        pos.y > 590.0f && pos.y < 2990.0f);

    if (in_lv) return MapRegion::LAS_VENTURAS;
    if (in_sf) return MapRegion::SAN_FIERRO;
    if (in_ls) return MapRegion::LOS_SANTOS;
    return MapRegion::COUNTRYSIDE;
}

static const char* region_name(MapRegion region) {
    switch (region) {
        case MapRegion::LOS_SANTOS: return "LS";
        case MapRegion::SAN_FIERRO: return "SF";
        case MapRegion::LAS_VENTURAS: return "LV";
        default: return "Rural";
    }
}

unsigned int get_local_patrol_model(CVector pos) {
    switch (detect_map_region(pos)) {
        case MapRegion::LOS_SANTOS:
            return MODEL_POLICE_CAR;
        case MapRegion::SAN_FIERRO:
            return MODEL_POLICE_CAR_SF;
        case MapRegion::LAS_VENTURAS:
            return MODEL_POLICE_CAR_LV;
        default:
            return MODEL_POLICE_RANGER;
    }
}

static void append_patrol_unit(std::vector<PoliceSpawnUnit>& plan, CVector pos) {
    plan.push_back({get_local_patrol_model(pos), false});
}

bool is_swat_model(unsigned int model) {
    return model == MODEL_SWAT_VAN || model == MODEL_SWAT_WATER;
}

bool is_police_dispatch_model(unsigned int model) {
    return model == MODEL_POLICE_CAR || model == MODEL_POLICE_CAR_SF ||
           model == MODEL_POLICE_CAR_LV || model == MODEL_POLICE_RANGER ||
           model == MODEL_POLICE_BIKE || model == MODEL_SWAT_VAN ||
           model == MODEL_SWAT_WATER || model == MODEL_FBI_RANCHER ||
           model == MODEL_FBI_TRUCK;
}

static bool is_firearm_case(const CrimeEvent& crime) {
    if (crime.is_firearm) return true;
    int max_threat = dispatch_threat::threat_level_score(
        dispatch_threat::get_case_max_threat_level(crime));
    return max_threat >= dispatch_threat::threat_level_score(
        ecs::CriminalThreatLevel::FIREARM_INACTIVE);
}

// FBI 不轻易出动：仅重大枪械威胁、高伤亡或极高密度时才考虑
static bool should_dispatch_fbi_rancher(const CrimeEvent& crime, int density) {
    int max_threat = dispatch_threat::threat_level_score(
        dispatch_threat::get_case_max_threat_level(crime));
    int active_firearm = dispatch_threat::threat_level_score(
        ecs::CriminalThreatLevel::FIREARM_ACTIVE);

    if (!is_firearm_case(crime)) return false;
    if (crime.cops_killed >= 2) return true;
    if (density >= 6 && max_threat >= active_firearm) return true;
    if (crime.cops_killed >= 1 && density >= 5 &&
        dispatch_threat::count_high_threats(crime) >= 2) {
        return true;
    }
    return false;
}

// FBI Truck 门槛更高：仅最重案或连续重大伤亡
static bool should_dispatch_fbi_truck(const CrimeEvent& crime, int density) {
    if (!should_dispatch_fbi_rancher(crime, density)) return false;
    if (crime.cops_killed >= 3) return true;
    if (density >= 6 && dispatch_threat::count_high_threats(crime) >= 2) return true;
    if (crime.cops_killed >= 2 && density >= 6) return true;
    return false;
}

static void append_unique_unit(std::vector<PoliceSpawnUnit>& plan, PoliceSpawnUnit unit) {
    for (const auto& existing : plan) {
        if (existing.model == unit.model) return;
    }
    plan.push_back(unit);
}

std::vector<PoliceSpawnUnit> build_initial_spawn_plan(
    const CrimeEvent& crime,
    int density,
    bool swat_already) {
    std::vector<PoliceSpawnUnit> plan;
    const bool firearm = is_firearm_case(crime);
    const CVector loc = get_crime_dispatch_position(crime);

    if (density >= 6 && !swat_already) {
        append_unique_unit(plan, {MODEL_SWAT_VAN, true});
        if (should_dispatch_fbi_truck(crime, density)) {
            append_unique_unit(plan, {MODEL_FBI_TRUCK, false});
            append_patrol_unit(plan, loc);
        } else if (should_dispatch_fbi_rancher(crime, density)) {
            append_unique_unit(plan, {MODEL_FBI_RANCHER, false});
            append_patrol_unit(plan, loc);
        } else {
            append_patrol_unit(plan, loc);
            append_patrol_unit(plan, loc);
        }
        return plan;
    }

    if (density >= 3 || (density >= 6 && swat_already)) {
        if (firearm) {
            append_patrol_unit(plan, loc);
            if (should_dispatch_fbi_rancher(crime, density)) {
                append_unique_unit(plan, {MODEL_FBI_RANCHER, false});
            } else {
                append_patrol_unit(plan, loc);
            }
        } else {
            append_unique_unit(plan, {MODEL_POLICE_BIKE, false});
            append_patrol_unit(plan, loc);
        }
        return plan;
    }

    if (firearm) {
        append_patrol_unit(plan, loc);
    } else if ((crime.case_id % 2) == 0) {
        append_unique_unit(plan, {MODEL_POLICE_BIKE, false});
    } else {
        append_patrol_unit(plan, loc);
    }
    return plan;
}

std::vector<PoliceSpawnUnit> build_reinforcement_spawn_plan(
    const CrimeEvent& crime,
    int reinforcement_wave,
    int density,
    bool swat_already) {
    std::vector<PoliceSpawnUnit> plan;
    const bool firearm = is_firearm_case(crime);
    const CVector loc = get_crime_dispatch_position(crime);

    if (reinforcement_wave >= 3 && density >= 5 && !swat_already) {
        append_unique_unit(plan, {MODEL_SWAT_VAN, true});
        if (should_dispatch_fbi_truck(crime, density)) {
            append_unique_unit(plan, {MODEL_FBI_TRUCK, false});
        } else {
            append_patrol_unit(plan, loc);
        }
        return plan;
    }

    if (density >= 3 || reinforcement_wave >= 2) {
        if (firearm) {
            append_patrol_unit(plan, loc);
            if (reinforcement_wave >= 3 && should_dispatch_fbi_rancher(crime, density)) {
                append_unique_unit(plan, {MODEL_FBI_RANCHER, false});
            } else {
                append_patrol_unit(plan, loc);
            }
        } else {
            append_unique_unit(plan, {MODEL_POLICE_BIKE, false});
            append_patrol_unit(plan, loc);
        }
        return plan;
    }

    if (firearm) {
        append_patrol_unit(plan, loc);
    } else if ((crime.case_id + reinforcement_wave) % 2 == 0) {
        append_unique_unit(plan, {MODEL_POLICE_BIKE, false});
    } else {
        append_patrol_unit(plan, loc);
    }
    return plan;
}

struct PoliceSpawnChain {
    std::shared_ptr<CrimeEvent> crime;
    CVector target_pos;
    CVector loc;
    CPed* criminal = nullptr;
    std::vector<PoliceSpawnUnit> units;
    std::vector<CVector> spawn_positions;
    std::vector<void*> spawned;
    std::string reason;
};

static void finalize_spawned_unit(
    const std::shared_ptr<CrimeEvent>& crime,
    void* veh,
    CVector loc,
    CPed* criminal,
    const PoliceSpawnUnit& unit,
    bool set_primary) {
    if (!veh || !crime) return;

    if (set_primary || !crime->spawned_vehicle) {
        crime->spawned_vehicle = veh;
    }
    crime->case_vehicles.push_back(veh);
    if (unit.register_swat) {
        register_spawned_swat(veh);
    }
    command_cop_vehicle_to_scene(veh, loc);
    setup_dispatched_cops(veh, criminal);
}

static void* find_spawned_police_vehicle_near(
    CVector search_pos,
    unsigned int expected_model,
    const std::vector<void*>& spawned,
    float max_dist) {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle) return nullptr;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool) return nullptr;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return nullptr;

    void* closest = nullptr;
    float closest_dist = max_dist;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag < 0) continue;

        int handle = (i << 8) | flag;
        void* veh = g_GetPoolVehicle(handle);
        if (!veh || !is_vehicle_pointer_valid(veh)) continue;

        bool already_claimed = false;
        for (void* ignore : spawned) {
            if (veh == ignore) {
                already_claimed = true;
                break;
            }
        }
        if (already_claimed) continue;

        unsigned short model = get_entity_model_index(veh);
        if (model != expected_model) continue;

        CVector veh_pos = get_entity_pos(veh);
        float dx = veh_pos.x - search_pos.x;
        float dy = veh_pos.y - search_pos.y;
        float dz = veh_pos.z - search_pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist < closest_dist) {
            closest_dist = dist;
            closest = veh;
        }
    }
    return closest;
}

static void schedule_unit_at(const std::shared_ptr<PoliceSpawnChain>& chain, size_t index);

static void schedule_next_unit(const std::shared_ptr<PoliceSpawnChain>& chain, size_t index) {
    if (!chain || !chain->crime || index + 1 >= chain->units.size()) return;
    chain->crime->pending_tasks.push_back({
        now_ms() + dispatch_timing::VEHICLE_IDENTIFY_STAGGER_MS,
        [chain, index]() { schedule_unit_at(chain, index + 1); }
    });
}

static void try_identify_spawned_unit(
    const std::shared_ptr<PoliceSpawnChain>& chain,
    size_t index,
    int attempt,
    bool already_respawned);

static void schedule_unit_at(const std::shared_ptr<PoliceSpawnChain>& chain, size_t index) {
    if (!chain || !chain->crime || index >= chain->units.size()) return;

    const PoliceSpawnUnit& unit = chain->units[index];
    CVector spawn_pos = dispatch_spawn_emergency_car(unit.model, chain->target_pos, chain->loc);
    if (index >= chain->spawn_positions.size()) {
        chain->spawn_positions.resize(index + 1);
    }
    chain->spawn_positions[index] = spawn_pos;

    chain->crime->pending_tasks.push_back({
        now_ms() + dispatch_timing::VEHICLE_IDENTIFY_DELAY_MS,
        [chain, index]() { try_identify_spawned_unit(chain, index, 1, false); }
    });
}

static void try_identify_spawned_unit(
    const std::shared_ptr<PoliceSpawnChain>& chain,
    size_t index,
    int attempt,
    bool already_respawned) {
    if (!chain || !chain->crime || chain->crime->cancelled || index >= chain->units.size()) {
        return;
    }

    CVector search_pos = (index < chain->spawn_positions.size())
                             ? chain->spawn_positions[index]
                             : chain->target_pos;
    const PoliceSpawnUnit& unit = chain->units[index];

    void* veh = find_spawned_police_vehicle_near(
        search_pos,
        unit.model,
        chain->spawned,
        dispatch_timing::VEHICLE_IDENTIFY_RADIUS_M);

    if (veh) {
        chain->spawned.push_back(veh);
        finalize_spawned_unit(
            chain->crime, veh, chain->loc, chain->criminal, unit, index == 0);
        if (index == 0) {
            int64_t configured_at = now_ms();
            chain->crime->vehicle_spawn_pending = false;
            chain->crime->on_scene_start = configured_at;
            chain->crime->spawn_time_ms = configured_at;
        }
        LOGI("✅ [PoliceSpawn] %s unit %zu model=%u configured for case %llu (attempt=%d, dist_anchor=%.1fm)",
             chain->reason.c_str(), index, unit.model,
             (unsigned long long)chain->crime->case_id, attempt,
             dispatch_timing::VEHICLE_IDENTIFY_RADIUS_M);
        schedule_next_unit(chain, index);
        return;
    }

    if (attempt < dispatch_timing::VEHICLE_IDENTIFY_MAX_ATTEMPTS) {
        LOGW("⚠️ [PoliceSpawn] %s identify retry %d/%d model=%u near (%.1f, %.1f, %.1f) case %llu",
             chain->reason.c_str(), attempt,
             dispatch_timing::VEHICLE_IDENTIFY_MAX_ATTEMPTS, unit.model,
             search_pos.x, search_pos.y, search_pos.z,
             (unsigned long long)chain->crime->case_id);
        chain->crime->pending_tasks.push_back({
            now_ms() + dispatch_timing::VEHICLE_IDENTIFY_RETRY_MS,
            [chain, index, attempt, already_respawned]() {
                try_identify_spawned_unit(chain, index, attempt + 1, already_respawned);
            }
        });
        return;
    }

    if (!already_respawned) {
        LOGW("⚠️ [PoliceSpawn] %s failed to identify model=%u for case %llu after %d attempts — respawning once",
             chain->reason.c_str(), unit.model,
             (unsigned long long)chain->crime->case_id,
             dispatch_timing::VEHICLE_IDENTIFY_MAX_ATTEMPTS);

        CVector respawn_pos = dispatch_spawn_emergency_car(unit.model, chain->target_pos, chain->loc);
        if (index >= chain->spawn_positions.size()) {
            chain->spawn_positions.resize(index + 1);
        }
        chain->spawn_positions[index] = respawn_pos;

        chain->crime->pending_tasks.push_back({
            now_ms() + dispatch_timing::VEHICLE_IDENTIFY_DELAY_MS,
            [chain, index]() { try_identify_spawned_unit(chain, index, 1, true); }
        });
        return;
    }

    LOGW("⚠️ [PoliceSpawn] %s gave up on model=%u for case %llu after respawn",
         chain->reason.c_str(), unit.model,
         (unsigned long long)chain->crime->case_id);
    schedule_next_unit(chain, index);
}

void schedule_police_vehicle_spawns(
    const std::shared_ptr<CrimeEvent>& crime,
    CVector target_pos,
    CVector loc,
    CPed* criminal,
    const std::vector<PoliceSpawnUnit>& units,
    const char* reason) {
    if (!crime || units.empty()) return;

    auto chain = std::make_shared<PoliceSpawnChain>();
    chain->crime = crime;
    chain->target_pos = target_pos;
    chain->loc = loc;
    chain->criminal = criminal;
    chain->units = units;
    chain->reason = reason ? reason : "spawn";

    crime->pending_tasks.push_back({
        now_ms(),
        [chain]() {
            if (!chain->crime || chain->crime->cancelled) return;

            CVector loc = get_crime_dispatch_position(*chain->crime);
            MapRegion region = detect_map_region(loc);
            LOGI("🚓 [PoliceSpawn] %s case %llu -> %zu units (region=%s, patrol=%u, firearm=%d, cops_killed=%d)",
                 chain->reason.c_str(), (unsigned long long)chain->crime->case_id,
                 chain->units.size(), region_name(region), get_local_patrol_model(loc),
                 is_firearm_case(*chain->crime) ? 1 : 0, chain->crime->cops_killed);
            for (size_t i = 0; i < chain->units.size(); i++) {
                const char* tag = "";
                if (chain->units[i].register_swat) tag = " (SWAT)";
                else if (chain->units[i].model == MODEL_FBI_RANCHER ||
                         chain->units[i].model == MODEL_FBI_TRUCK) tag = " (FBI)";
                LOGI("   +- [%zu] model=%u%s", i, chain->units[i].model, tag);
            }

            schedule_unit_at(chain, 0);
        }
    });
}

} // namespace dispatch_police_spawn