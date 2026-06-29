#pragma once

#include <cstdint>

#include "game_types.hpp"

int get_ped_pool_handle(CPed* ped);
CPed* resolve_ped_from_handle(int handle);
bool ped_handle_matches(int handle, CPed* ped);
uint64_t get_case_id_for_ped(CPed* ped);
void sync_ped_pool_handle(CPed* ped);