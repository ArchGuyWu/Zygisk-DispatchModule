#pragma once

#include <cstdint>
#include <memory>

#include "game_types.hpp"
#include "dispatch_types.hpp"

CVector dispatch_spawn_emergency_car(unsigned int model, CVector pos, CVector incident_anchor);
void apply_civilian_avoidance_field();
void dispatch_tick_process_crime(const std::shared_ptr<CrimeEvent>& crime, int64_t cur_time, bool do_siren_refresh);
void dispatch_tick_state_idle(const std::shared_ptr<CrimeEvent>& crime);
void dispatch_tick_state_timing(const std::shared_ptr<CrimeEvent>& crime);
void dispatch_tick_state_on_scene(const std::shared_ptr<CrimeEvent>& crime);
void dispatch_tick_state_cleanup(const std::shared_ptr<CrimeEvent>& crime);
