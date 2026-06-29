#include "dispatch_timing.hpp"

#include "mod_shared.hpp"

namespace dispatch_timing {

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
    if (elapsed < NATURAL_RESPONSE_GRACE_MS) {
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