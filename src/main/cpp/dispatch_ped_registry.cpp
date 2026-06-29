#include "dispatch_ped_registry.hpp"

#include "ecs_engine.hpp"
#include "mod_shared.hpp"

int get_ped_pool_handle(CPed* ped) {
    if (!ped || !is_ped_pointer_valid_safe(ped) || !g_ms_pPedPool || !g_GetPoolPed) {
        return -1;
    }

    auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ped);
    if (cop_comp && cop_comp->pool_handle >= 0) {
        CPed* resolved = resolve_ped_from_handle(cop_comp->pool_handle);
        if (resolved == ped) {
            return cop_comp->pool_handle;
        }
    }

    auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
    if (crim_comp && crim_comp->pool_handle >= 0) {
        CPed* resolved = resolve_ped_from_handle(crim_comp->pool_handle);
        if (resolved == ped) {
            return crim_comp->pool_handle;
        }
    }

    void* pool = *reinterpret_cast<void**>(g_ms_pPedPool);
    if (!pool) return -1;

    char* byte_map = *reinterpret_cast<char**>(reinterpret_cast<char*>(pool) + 8);
    int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(pool) + 16);
    if (!byte_map) return -1;

    for (int i = 0; i < size; i++) {
        signed char flag = byte_map[i];
        if (flag < 0) continue;
        int handle = (i << 8) | flag;
        if (g_GetPoolPed(handle) == ped) {
            if (cop_comp) cop_comp->pool_handle = handle;
            if (crim_comp) crim_comp->pool_handle = handle;
            return handle;
        }
    }
    return -1;
}

CPed* resolve_ped_from_handle(int handle) {
    if (handle < 0 || !g_GetPoolPed) return nullptr;
    CPed* ped = g_GetPoolPed(handle);
    if (!ped || !is_ped_pointer_valid_safe(ped)) return nullptr;
    return ped;
}

bool ped_handle_matches(int handle, CPed* ped) {
    if (handle < 0 || !ped) return false;
    CPed* resolved = resolve_ped_from_handle(handle);
    return resolved == ped;
}

uint64_t get_case_id_for_ped(CPed* ped) {
    if (!ped) return 0;
    auto crime = find_crime_containing_criminal(ped);
    if (crime) return crime->case_id;

    auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
    if (crim_comp && crim_comp->case_id > 0) {
        return crim_comp->case_id;
    }

    std::lock_guard<std::recursive_mutex> lock(g_crime_mutex);
    for (const auto& crime_evt : g_active_crimes) {
        if (!crime_evt || crime_evt->cancelled) continue;
        if (crime_evt->criminal == ped) return crime_evt->case_id;
        for (CPed* c : crime_evt->consolidated_criminals) {
            if (c == ped) return crime_evt->case_id;
        }
    }
    return 0;
}

void sync_ped_pool_handle(CPed* ped) {
    get_ped_pool_handle(ped);
}