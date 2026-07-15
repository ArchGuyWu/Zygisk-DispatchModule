//! Timing constants ported from `dispatch_timing.hpp`.

pub const AV_RANGE_FIREARM_M: f32 = 75.0;
pub const AV_RANGE_MELEE_M: f32 = 35.0;

pub const NEARBY_SEARCH_FIREARM_M: f32 = 150.0;
pub const NEARBY_SEARCH_MELEE_M: f32 = 80.0;
pub const NEARBY_RETRY_INTERVAL_MS: i64 = 3_000;
pub const POLICE_COMMIT_MAX_AV_FAILS: i32 = 8;
pub const NEARBY_ASSIGN_DEDUP_MS: i64 = 8_000;

pub const REINFORCEMENT_SPAWN_STAGGER_URBAN_MS: i64 = 2_500;
pub const REINFORCEMENT_SPAWN_STAGGER_RURAL_MS: i64 = 5_000;
pub const REINFORCEMENT_BATCH_RELEASE_TIMEOUT_MS: i64 = 50_000;

pub const ON_SCENE_DISPATCH_INTERVAL_MS: i64 = 2_500;
pub const SCENE_ARRIVAL_RADIUS_M: f32 = 40.0;
pub const NPC_CASE_PLAYER_DETACH_CANCEL_M: f32 = 150.0;
/// Player farther than this from the crime anchor → off-screen vehicle spawn is allowed.
pub const OFFSCREEN_SPAWN_MIN_PLAYER_DIST_M: f32 = 100.0;
pub const NPC_ON_SCENE_TIMEOUT_MS: i64 = 90_000;
pub const SPAWN_PENDING_TIMEOUT_MS: i64 = 45_000;
pub const SPAWN_ARRIVED_SCENE_TIMEOUT_MS: i64 = 60_000;

pub const REINFORCE_NEARBY_ATTEMPT_MS: i64 = 4_000;
/// Minimum interval between on-scene reinforcement evaluations.
pub const REINFORCEMENT_EVAL_INTERVAL_MS: i64 = 10_000;
/// En-route vehicle reroute evaluation interval (not every script tick).
pub const REROUTE_EVAL_INTERVAL_MS: i64 = 500;

pub const FOOT_ASSIGN_FIREARM_MS: i64 = 4_000;
pub const FOOT_ASSIGN_ACTIVE_WINDOW_MS: i64 = 15_000;

pub const VEHICLE_STAGING_OFFSET_M: f32 = 32.0;
pub const VEHICLE_STAGING_EXIT_MARGIN_M: f32 = 4.0;
pub const VEHICLE_BIKE_STAGING_OFFSET_M: f32 = 20.0;
pub const VEHICLE_STUCK_EXIT_MS: i64 = 8_000;
pub const VEHICLE_MAX_APPROACH_MS: i64 = 15_000;

pub const STUCK_DRIVER_RECRUIT_RADIUS_M: f32 = 80.0;
pub const VEHICLE_STUCK_STAGE1_MS: i64 = 3_500;
pub const VEHICLE_STUCK_STAGE2_MS: i64 = 7_000;
pub const VEHICLE_STAGE2_LONG_STUCK_MS: i64 = 12_000;
pub const VEHICLE_STUCK_INTERVENTION_COOLDOWN_MS: i64 = 6_000;
pub const VEHICLE_STAGE2_MIN_DIST_M: f32 = 40.0;
pub const ANTI_SPIN_CLOSE_RANGE_M: f32 = 60.0;
pub const TEMP_CLOSURE_RADIUS_M: f32 = 20.0;
pub const TEMP_CLOSURE_DEDUP_M: f32 = 15.0;
pub const TEMP_CLOSURE_TTL_MS: i64 = 15_000;

pub const NEARBY_FOOT_PREFER_DIST_M: f32 = 55.0;
pub const ACTIVE_CRIME_FOOT_DUTY_RADIUS_M: f32 = 80.0;
pub const VEHICLE_ENTER_COMMAND_COOLDOWN_MS: i64 = 10_000;
pub const COP_EXIT_COMBAT_BLOCK_MS: i64 = 8_000;

pub const STAGING_AREA_CLOSURE_RADIUS_M: f32 = 24.0;
pub const STAGING_AREA_CLOSURE_DEDUP_M: f32 = 16.0;

pub const EMERGENCY_EVAL_INTERVAL_MS: i64 = 2_000;
pub const AMBULANCE_SPAWN_DELAY_MIN_MS: i64 = 5_000;
pub const AMBULANCE_SPAWN_DELAY_MAX_MS: i64 = 8_000;
pub const FIRETRUCK_SPAWN_DELAY_MIN_MS: i64 = 3_000;
pub const FIRETRUCK_SPAWN_DELAY_MAX_MS: i64 = 5_000;

pub const DRIVE_STYLE_STOP_FOR_CARS: i32 = 0;
pub const DRIVE_STYLE_AVOID_CARS: i32 = 2;
pub const DRIVE_STYLE_RESPONSE_EMERGENCY: i32 = 4;
pub const DRIVE_STYLE_BRAKE_HOLD: i32 = 4;

pub fn av_range_for_firearm(is_firearm: bool) -> f32 {
    if is_firearm {
        AV_RANGE_FIREARM_M
    } else {
        AV_RANGE_MELEE_M
    }
}

pub fn driving_style_for_case(is_firearm: bool) -> i32 {
    if is_firearm {
        DRIVE_STYLE_AVOID_CARS
    } else {
        DRIVE_STYLE_RESPONSE_EMERGENCY
    }
}

pub fn elapsed_since_dispatch_timer(timer_start_ms: i64, now_ms: i64) -> i64 {
    now_ms.saturating_sub(timer_start_ms)
}

pub fn should_attempt_nearby_dispatch(last_attempt_ms: i64, now_ms: i64) -> bool {
    last_attempt_ms <= 0 || now_ms - last_attempt_ms >= NEARBY_RETRY_INTERVAL_MS
}

pub fn vehicle_staging_exit_radius(is_bike: bool) -> f32 {
    let offset = if is_bike {
        VEHICLE_BIKE_STAGING_OFFSET_M
    } else {
        VEHICLE_STAGING_OFFSET_M
    };
    offset + VEHICLE_STAGING_EXIT_MARGIN_M
}