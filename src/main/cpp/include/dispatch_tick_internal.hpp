#pragma once

#include <cstdint>
#include <memory>

#include "game_types.hpp"
#include "dispatch_types.hpp"

void dispatch_spawn_emergency_car(unsigned int model, CVector pos);
void apply_civilian_avoidance_field();
void dispatch_tick_process_crime(const std::shared_ptr<CrimeEvent>& crime, int64_t cur_time, bool do_siren_refresh);
