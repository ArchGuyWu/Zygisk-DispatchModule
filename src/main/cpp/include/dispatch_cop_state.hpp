#pragma once

#include <cstdint>
#include <vector>

#include "game_types.hpp"

namespace dispatch_cop_state {

int64_t get_last_assign_ms(CPed* cop, CPed* criminal);
void record_assign(CPed* cop, CPed* criminal, uint64_t case_id, int64_t time_ms);
eWeaponType get_assigned_weapon(CPed* cop);
int64_t get_last_armed_ms(CPed* cop);
void record_weapon(CPed* cop, eWeaponType weapon, int64_t time_ms);
bool is_dispatch_active(CPed* cop);
void set_dispatch_active(CPed* cop, bool active);
std::vector<CPed*> collect_active_dispatch_cops();
std::vector<CPed*> collect_cops_targeting_criminal(CPed* criminal, int64_t within_ms);
std::vector<CPed*> collect_cops_targeting_case(uint64_t case_id, int criminal_handle, int64_t within_ms);
CPed* resolve_combat_target(CPed* cop);
void set_self_defense_target(CPed* cop, CPed* attacker, int64_t time_ms);
CPed* get_self_defense_target(CPed* cop, int64_t cur_time);
bool blocks_mass_assign(CPed* cop, CPed* proposed_target, int64_t cur_time);
void bind_criminal_case(CPed* criminal, uint64_t case_id);
void purge_cop_combat_state(CPed* ped);
void clear_all_cop_dispatch_state();

} // namespace dispatch_cop_state