#pragma once

#include <cstdint>
#include <memory>

#include "dispatch_types.hpp"

namespace dispatch_heli_support {

void evaluate_case_heli_support(const std::shared_ptr<CrimeEvent>& crime, int64_t cur_time);
void refresh_active_helis(const std::shared_ptr<CrimeEvent>& crime);

int count_active_police_helis_near(CVector pos, float range);

} // namespace dispatch_heli_support