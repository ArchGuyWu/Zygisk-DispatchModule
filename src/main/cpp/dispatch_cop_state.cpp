#include "dispatch_cop_state.hpp"

#include "dispatch_ped_registry.hpp"
#include "ecs_engine.hpp"
#include "mod_shared.hpp"

namespace dispatch_cop_state {

static ecs::CombatComponent* get_or_create_combat(CPed* cop) {
    auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (!combat) {
        combat = ecs::EntityManager::get().add_component<ecs::CombatComponent>(cop);
    }
    return combat;
}

static ecs::CopComponent* get_or_create_cop(CPed* cop) {
    auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
    if (!cop_comp) {
        cop_comp = ecs::EntityManager::get().add_component<ecs::CopComponent>(cop, cop);
    }
    return cop_comp;
}

int64_t get_last_assign_ms(CPed* cop, CPed* criminal) {
    if (!cop || !criminal) return 0;
    auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (!combat) return 0;

    int criminal_handle = get_ped_pool_handle(criminal);
    if (combat->target_ped_handle >= 0 && criminal_handle >= 0 &&
        combat->target_ped_handle != criminal_handle) {
        return 0;
    }
    if (combat->target_entity != criminal && combat->target_ped_handle >= 0) {
        CPed* resolved = resolve_ped_from_handle(combat->target_ped_handle);
        if (resolved != criminal) return 0;
    }
    return combat->last_dispatch_assign_ms;
}

void record_assign(CPed* cop, CPed* criminal, uint64_t case_id, int64_t time_ms) {
    if (!cop || !criminal) return;
    auto* combat = get_or_create_combat(cop);
    auto* cop_comp = get_or_create_cop(cop);
    if (!combat || !cop_comp) return;

    sync_ped_pool_handle(cop);
    sync_ped_pool_handle(criminal);

    combat->target_entity = criminal;
    combat->target_ped_handle = get_ped_pool_handle(criminal);
    combat->target_case_id = case_id;
    combat->assigned_case_id = case_id;
    combat->last_dispatch_assign_ms = time_ms;
    cop_comp->last_assign_time_ms = time_ms;
    cop_comp->dispatch_active = true;
    cop_comp->pool_handle = get_ped_pool_handle(cop);
}

eWeaponType get_assigned_weapon(CPed* cop) {
    auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (!combat) return WEAPON_UNARMED;
    return static_cast<eWeaponType>(combat->current_weapon_type);
}

int64_t get_last_armed_ms(CPed* cop) {
    auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (!combat) return 0;
    return combat->last_armed_time_ms;
}

void record_weapon(CPed* cop, eWeaponType weapon, int64_t time_ms) {
    auto* combat = get_or_create_combat(cop);
    if (!combat) return;
    combat->current_weapon_type = static_cast<int>(weapon);
    combat->last_armed_time_ms = time_ms;
    combat->last_weapon_switch_time_ms = time_ms;
}

bool is_dispatch_active(CPed* cop) {
    auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
    return cop_comp && cop_comp->dispatch_active;
}

void set_dispatch_active(CPed* cop, bool active) {
    auto* cop_comp = get_or_create_cop(cop);
    if (cop_comp) cop_comp->dispatch_active = active;
}

std::vector<CPed*> collect_active_dispatch_cops() {
    std::vector<CPed*> cops;
    for (auto ent : ecs::EntityManager::get().get_entities_with<ecs::CopComponent>()) {
        auto* cop = static_cast<CPed*>(ent);
        if (!cop || !is_ped_pointer_valid_safe(cop)) continue;
        if (g_IsAlive && !g_IsAlive(cop)) continue;
        auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(cop);
        if (cop_comp && cop_comp->dispatch_active) {
            cops.push_back(cop);
        }
    }
    return cops;
}

std::vector<CPed*> collect_cops_targeting_criminal(CPed* criminal, int64_t within_ms) {
    std::vector<CPed*> cops;
    if (!criminal) return cops;

    int criminal_handle = get_ped_pool_handle(criminal);
    uint64_t case_id = get_case_id_for_ped(criminal);
    int64_t cur_time = now_ms();

    for (auto ent : ecs::EntityManager::get().get_entities_with<ecs::CombatComponent>()) {
        auto* cop = static_cast<CPed*>(ent);
        if (!cop || !is_ped_pointer_valid_safe(cop)) continue;
        auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
        if (!combat) continue;
        if (cur_time - combat->last_dispatch_assign_ms > within_ms) continue;

        bool match = false;
        if (criminal_handle >= 0 && combat->target_ped_handle == criminal_handle) {
            match = true;
        } else if (combat->target_entity == criminal) {
            match = true;
        }
        if (match && case_id > 0 && combat->target_case_id > 0 && combat->target_case_id != case_id) {
            continue;
        }
        if (match) cops.push_back(cop);
    }
    return cops;
}

std::vector<CPed*> collect_cops_targeting_case(uint64_t case_id, int criminal_handle, int64_t within_ms) {
    std::vector<CPed*> cops;
    int64_t cur_time = now_ms();

    for (auto ent : ecs::EntityManager::get().get_entities_with<ecs::CombatComponent>()) {
        auto* cop = static_cast<CPed*>(ent);
        if (!cop || !is_ped_pointer_valid_safe(cop)) continue;
        auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
        if (!combat) continue;
        if (combat->assigned_case_id != case_id && combat->target_case_id != case_id) continue;
        if (criminal_handle >= 0 && combat->target_ped_handle != criminal_handle) continue;
        if (cur_time - combat->last_dispatch_assign_ms > within_ms) continue;
        cops.push_back(cop);
    }
    return cops;
}

CPed* resolve_combat_target(CPed* cop) {
    auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(cop);
    if (!combat) return nullptr;

    if (combat->target_ped_handle >= 0) {
        CPed* resolved = resolve_ped_from_handle(combat->target_ped_handle);
        if (resolved && is_ped_pointer_valid_safe(resolved)) {
            if (combat->target_entity != resolved) {
                combat->target_entity = resolved;
            }
            return resolved;
        }
    }

    CPed* target = static_cast<CPed*>(combat->target_entity);
    if (target && is_ped_pointer_valid_safe(target)) {
        combat->target_ped_handle = get_ped_pool_handle(target);
        return target;
    }

    combat->target_entity = nullptr;
    combat->target_ped_handle = -1;
    combat->target_case_id = 0;
    return nullptr;
}

void bind_criminal_case(CPed* criminal, uint64_t case_id) {
    if (!criminal || case_id == 0) return;
    auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(criminal);
    if (!crim_comp) {
        crim_comp = ecs::EntityManager::get().add_component<ecs::CriminalComponent>(criminal, criminal);
    }
    if (crim_comp) {
        crim_comp->case_id = case_id;
        crim_comp->pool_handle = get_ped_pool_handle(criminal);
    }
}

void purge_cop_combat_state(CPed* ped) {
    if (!ped) return;

    set_dispatch_active(ped, false);

    for (auto ent : ecs::EntityManager::get().get_entities_with<ecs::CombatComponent>()) {
        auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ent);
        if (!combat) continue;
        if (combat->target_entity == ped ||
            (combat->target_ped_handle >= 0 &&
             resolve_ped_from_handle(combat->target_ped_handle) == ped)) {
            combat->target_entity = nullptr;
            combat->target_ped_handle = -1;
            combat->target_case_id = 0;
        }
    }

    auto* crim_comp = ecs::EntityManager::get().get_component<ecs::CriminalComponent>(ped);
    if (crim_comp) {
        crim_comp->current_victim = nullptr;
    }
}

void clear_all_cop_dispatch_state() {
    for (auto ent : ecs::EntityManager::get().get_entities_with<ecs::CopComponent>()) {
        auto* cop_comp = ecs::EntityManager::get().get_component<ecs::CopComponent>(ent);
        if (cop_comp) {
            cop_comp->dispatch_active = false;
            cop_comp->last_assign_time_ms = 0;
        }
    }
    for (auto ent : ecs::EntityManager::get().get_entities_with<ecs::CombatComponent>()) {
        auto* combat = ecs::EntityManager::get().get_component<ecs::CombatComponent>(ent);
        if (combat) {
            combat->target_entity = nullptr;
            combat->target_ped_handle = -1;
            combat->target_case_id = 0;
            combat->assigned_case_id = 0;
            combat->last_dispatch_assign_ms = 0;
            combat->last_armed_time_ms = 0;
            combat->current_weapon_type = static_cast<int>(WEAPON_UNARMED);
        }
    }
}

} // namespace dispatch_cop_state