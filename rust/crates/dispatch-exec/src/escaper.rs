//! Vehicle unstuck helpers (ported from `dispatch_vehicle_escaper.cpp`).

use dispatch_core::WorldPos;

use crate::timing::{
    ANTI_SPIN_CLOSE_RANGE_M, TEMP_CLOSURE_DEDUP_M, TEMP_CLOSURE_RADIUS_M, TEMP_CLOSURE_TTL_MS,
    VEHICLE_BIKE_STAGING_OFFSET_M, VEHICLE_STAGE2_LONG_STUCK_MS, VEHICLE_STAGE2_MIN_DIST_M,
    VEHICLE_STAGING_EXIT_MARGIN_M, VEHICLE_STAGING_OFFSET_M, VEHICLE_STUCK_STAGE2_MS,
};
use crate::models::is_police_bike;

#[derive(Debug, Clone, Default)]
pub struct StuckTracker {
    pub stuck_since: i64,
    pub last_check_time: i64,
    pub last_pos: WorldPos,
    pub last_intervention_time: i64,
    pub last_dir_x: f32,
    pub last_dir_y: f32,
    pub spin_count: i32,
}

impl StuckTracker {
    pub fn reset_intervention(&mut self, now_ms: i64) {
        self.stuck_since = 0;
        self.last_intervention_time = now_ms;
        self.spin_count = 0;
        self.last_dir_x = 0.0;
        self.last_dir_y = 0.0;
    }
}

#[derive(Debug, Clone)]
pub struct TemporaryRoadClosure {
    pub center: WorldPos,
    pub radius: f32,
    pub reopen_time_ms: i64,
}

pub fn reset_stuck_tracker(tracker: &mut StuckTracker, now_ms: i64) {
    tracker.stuck_since = 0;
    tracker.last_intervention_time = now_ms;
    tracker.spin_count = 0;
    tracker.last_dir_x = 0.0;
    tracker.last_dir_y = 0.0;
}

pub fn should_trigger_stage2_warp(
    stuck_duration_ms: i64,
    warp_visible: bool,
    dist_to_target_m: f32,
) -> bool {
    if warp_visible {
        return false;
    }
    if stuck_duration_ms < VEHICLE_STUCK_STAGE2_MS {
        return false;
    }
    if dist_to_target_m > VEHICLE_STAGE2_MIN_DIST_M {
        return true;
    }
    stuck_duration_ms >= VEHICLE_STAGE2_LONG_STUCK_MS
}

pub fn get_vehicle_staging_exit_radius(model: u32) -> f32 {
    let offset = if is_police_bike(model) {
        VEHICLE_BIKE_STAGING_OFFSET_M
    } else {
        VEHICLE_STAGING_OFFSET_M
    };
    offset + VEHICLE_STAGING_EXIT_MARGIN_M
}

pub fn anti_spin_should_bulk_exit(dist_to_crime_m: f32, model: u32) -> bool {
    let staging_radius = get_vehicle_staging_exit_radius(model);
    if dist_to_crime_m <= staging_radius + 4.0 {
        return false;
    }
    dist_to_crime_m < ANTI_SPIN_CLOSE_RANGE_M
}

pub fn queue_deduped_temp_closure(
    center: WorldPos,
    existing: &[TemporaryRoadClosure],
    pending: &mut Vec<TemporaryRoadClosure>,
    now_ms: i64,
) -> bool {
    let reopen_time_ms = now_ms + TEMP_CLOSURE_TTL_MS;
    let radius = TEMP_CLOSURE_RADIUS_M;

    for item in existing.iter().chain(pending.iter()) {
        if closure_center_near(item.center, center, TEMP_CLOSURE_DEDUP_M) {
            return false;
        }
    }

    pending.push(TemporaryRoadClosure {
        center,
        radius,
        reopen_time_ms,
    });
    true
}

fn closure_center_near(a: WorldPos, b: WorldPos, max_dist_m: f32) -> bool {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    (dx * dx + dy * dy).sqrt() <= max_dist_m
}