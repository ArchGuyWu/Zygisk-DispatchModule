#include "dispatch_timing.hpp"

#include "mod_shared.hpp"

namespace dispatch_timing {

float get_av_range_for_crime(const CrimeEvent& crime) {
    return crime.is_firearm ? AV_RANGE_FIREARM_M : AV_RANGE_MELEE_M;
}

int64_t get_natural_response_grace_ms(const CrimeEvent& crime) {
    return crime.is_firearm ? NATURAL_RESPONSE_GRACE_FIREARM_MS : NATURAL_RESPONSE_GRACE_MELEE_MS;
}

bool is_cop_within_native_av(float dist_to_cop, const CrimeEvent& crime) {
    return dist_to_cop < get_av_range_for_crime(crime);
}

int compute_spawn_fallback_delay_ms(bool is_firearm) {
    if (is_firearm) {
        return get_random_range(SPAWN_FALLBACK_FIREARM_MIN_MS, SPAWN_FALLBACK_FIREARM_MAX_MS);
    }
    return get_random_range(SPAWN_FALLBACK_MELEE_MIN_MS, SPAWN_FALLBACK_MELEE_MAX_MS);
}

int64_t elapsed_since_dispatch_timer(const CrimeEvent& crime, int64_t cur_time_ms) {
    if (crime.timer_start <= 0) return 0;
    return cur_time_ms - crime.timer_start;
}

bool should_attempt_nearby_dispatch(const CrimeEvent& crime, int64_t cur_time_ms) {
    int64_t elapsed = elapsed_since_dispatch_timer(crime, cur_time_ms);
    if (elapsed < get_natural_response_grace_ms(crime)) {
        return false;
    }
    if (crime.last_nearby_dispatch_attempt_ms <= 0) {
        return true;
    }
    return (cur_time_ms - crime.last_nearby_dispatch_attempt_ms) >= NEARBY_RETRY_INTERVAL_MS;
}

void record_nearby_dispatch_attempt(CrimeEvent& crime, int64_t cur_time_ms) {
    crime.last_nearby_dispatch_attempt_ms = cur_time_ms;
}

} // namespace dispatch_timing