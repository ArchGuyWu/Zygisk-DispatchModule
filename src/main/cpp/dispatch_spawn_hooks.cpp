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
#include "game_config.hpp"


// =====================================================================
// Wanted-system spawn interception
// =====================================================================
std::atomic<bool> g_in_wanted_update{false};

void* g_stub_wanted_update = nullptr;
fn_WantedUpdate_t g_orig_wanted_update = nullptr;

void proxy_wanted_update(void* this_wanted) {
    SHADOWHOOK_STACK_SCOPE();
    g_in_wanted_update.store(true);
    SHADOWHOOK_CALL_PREV(proxy_wanted_update, this_wanted);
    g_in_wanted_update.store(false);
}

void* g_stub_add_ped = nullptr;
fn_AddPed_t g_orig_add_ped = nullptr;

CPed* proxy_add_ped(int pedType, unsigned int modelIndex, const CVector& pos, bool bUnknown) {
    SHADOWHOOK_STACK_SCOPE();

    if (!is_mod_dispatch_paused() && pedType == 6 && g_in_wanted_update.load()) { // PED_TYPE_COP = 6
        LOGI("🚫 [trueDispatch] Intercepted and blocked wanted-system forced cop spawn! Model: %u, Pos: (%.1f, %.1f, %.1f)", modelIndex, pos.x, pos.y, pos.z);
        return nullptr;
    }

    return SHADOWHOOK_CALL_PREV(proxy_add_ped, pedType, modelIndex, pos, bUnknown);
}

// =====================================================================
// =====================================================================
// Emergency vehicle spawn distance workaround (mobile draw distance)
// =====================================================================
static bool is_police_vehicle_model(unsigned int model) {
    return (model == 596 || model == 597 || model == 598 || model == 599 ||  // Cop Cars (LSPD, SFPD, LVPD, Ranger)
            model == 523 ||                                                 // Cop Bike
            model == 427 || model == 601 ||                                 // SWAT Enforcer, SWAT Water Cannon
            model == 490 || model == 528 ||                                 // FBI Rancher, FBI Truck
            model == 433 || model == 432);                                  // Barracks (Army Truck), Rhino (Tank)
}

static int count_active_police_vehicles_near_player(float range) {
    if (!g_ms_pVehiclePool || !g_GetPoolVehicle || !g_FindPlayerCoors) return 0;
    void* pool = *reinterpret_cast<void**>(g_ms_pVehiclePool);
    if (!pool || !is_pointer_readable(pool)) return 0;

    char** p_byte_map = reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    if (!is_pointer_readable(p_byte_map)) return 0;
    char* byte_map = *p_byte_map;
    if (!byte_map || !is_pointer_readable(byte_map)) return 0;

    int* p_size = reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!is_pointer_readable(p_size)) return 0;
    int size = *p_size;

    CVector player_pos = g_FindPlayerCoors(0);
    int count = 0;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag >= 0) {
            int handle = (i << 8) | flag;
            void* veh = g_GetPoolVehicle(handle);
            if (veh && is_pointer_readable(veh)) {
                unsigned short model = get_entity_model_index(veh);
                if (is_police_vehicle_model(model)) {
                    CVector pos = get_entity_pos(veh);
                    float dx = pos.x - player_pos.x;
                    float dy = pos.y - player_pos.y;
                    float dz = pos.z - player_pos.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist < range) {
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

static bool relocate_police_car_spawn(unsigned int model, CVector& pos) {
    int wanted_level = g_player_wanted_level.load();
    if (wanted_level <= 0) {
        return true; // Allow ambient/parked police cars to spawn normally without relocation or blocking
    }

    int max_cars = 0;
    if (wanted_level == 1 || wanted_level == 2) {
        max_cars = 1;
    } else if (wanted_level == 3 || wanted_level == 4) {
        max_cars = 2;
    } else if (wanted_level >= 5) {
        max_cars = 3;
    }

    int active_cars = count_active_police_vehicles_near_player(150.0f);
    if (active_cars >= max_cars) {
        LOGI("🚫 [trueDispatch] Blocked native cop spawn (Cap reached: %d/%d). Model: %u", active_cars, max_cars, model);
        return false;
    }

    static std::atomic<uint64_t> last_spawn_time_ms{0};
    uint64_t now = now_ms();
    if (now - last_spawn_time_ms.load() < 15000) { // 15s cooldown
        LOGI("🚫 [trueDispatch] Blocked native cop spawn (Cooldown active). Model: %u", model);
        return false;
    }

    CVector player_pos = g_FindPlayerCoors ? g_FindPlayerCoors(0) : pos;
    CVector target_pos = get_spawn_target(player_pos);

    float dx = target_pos.x - player_pos.x;
    float dy = target_pos.y - player_pos.y;
    float dz = target_pos.z - player_pos.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    if (dist > 65.0f) {
        float scale = 60.0f / dist;
        target_pos.x = player_pos.x + dx * scale;
        target_pos.y = player_pos.y + dy * scale;
        target_pos.z = player_pos.z + dz * scale;
    }

    LOGI("🚓 [trueDispatch] Relocating native cop spawn! Model: %u, original pos: (%.1f, %.1f, %.1f), relocated to (%.1f, %.1f, %.1f), dist: %.1f m",
         model, pos.x, pos.y, pos.z, target_pos.x, target_pos.y, target_pos.z, dist);

    last_spawn_time_ms.store(now);
    pos = target_pos;
    return true;
}

void* g_stub_generate_one_emergency_car = nullptr;
fn_GenOneEmergencyCar_t g_orig_generate_one_emergency_car = nullptr;

void proxy_generate_one_emergency_car(unsigned int model, CVector pos) {
    SHADOWHOOK_STACK_SCOPE();

    // Tombstone_23/24: passthrough CALL_PREV during load still spawns vehicles whose
    // AI/tasks race UE hydration — drop the spawn entirely while dispatch is paused.
    if (is_mod_dispatch_paused()) {
        return;
    }

    if (is_police_vehicle_model(model)) {
        if (!g_is_generating_custom_dispatch.load()) {
            if (!relocate_police_car_spawn(model, pos)) {
                return;
            }
        }
    }

    if (model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK) {
        if (!g_is_generating_custom_dispatch.load()) {
            LOGI("🚫 [ModEMS] Blocked native emergency spawn (model=%u) — mod dispatch handles ambulance/firetruck",
                 model);
            return;
        }
        pos = dispatch_emergency_services::clamp_spawn_to_streaming_range(pos, pos);
    }

    SHADOWHOOK_CALL_PREV(proxy_generate_one_emergency_car, model, pos);
}

void* g_stub_script_generate_one_emergency_car = nullptr;
fn_ScriptGenEmergencyCar_t g_orig_script_generate_one_emergency_car = nullptr;

void proxy_script_generate_one_emergency_car(unsigned int model, CVector pos) {
    SHADOWHOOK_STACK_SCOPE();

    if (is_mod_dispatch_paused()) {
        return;
    }

    // We do NOT block or relocate scripted police cars to ensure 100% mission compatibility

    if (model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK) {
        pos = dispatch_emergency_services::clamp_spawn_to_streaming_range(pos, pos);
    }

    SHADOWHOOK_CALL_PREV(proxy_script_generate_one_emergency_car, model, pos);
}

void* g_stub_tell_occupants_leave_car = nullptr;
fn_TellOccupantsToLeaveCar_t g_orig_tell_occupants_leave_car = nullptr;

void proxy_tell_occupants_leave_car(void* vehicle) {
    SHADOWHOOK_STACK_SCOPE();
    if (!is_mod_dispatch_paused()) {
        bind_vehicle_occupants(vehicle); // Bind them here before they leave!
        record_exit_start_for_occupants(vehicle);
    }
    SHADOWHOOK_CALL_PREV(proxy_tell_occupants_leave_car, vehicle);
}
