//! Emergency staging cordon (ported from `dispatch_emergency_staging.cpp`).

use dispatch_case::CaseRecord;
use dispatch_core::WorldPos;

use crate::timing::{
    STAGING_AREA_CLOSURE_DEDUP_M, STAGING_AREA_CLOSURE_RADIUS_M, VEHICLE_STAGING_EXIT_MARGIN_M,
    VEHICLE_STAGING_OFFSET_M,
};
use crate::threat::count_active_threats;

#[derive(Debug, Clone)]
struct StagingAreaClosure {
    center: WorldPos,
    ref_count: i32,
}

#[derive(Debug, Default)]
pub struct StagingState {
    closures: Vec<StagingAreaClosure>,
}

impl StagingState {
    pub fn compute_staging_point(incident_anchor: WorldPos, vehicle_pos: WorldPos) -> WorldPos {
        compute_vehicle_staging_decoy(incident_anchor, vehicle_pos)
    }

    pub fn is_vehicle_at_staging(&self, veh_pos: WorldPos, incident_anchor: WorldPos) -> bool {
        let staging = Self::compute_staging_point(incident_anchor, veh_pos);
        distance_3d(veh_pos, staging) <= staging_arrival_radius_m()
    }

    pub fn begin_staging_area_closure(&mut self, staging_center: WorldPos) -> bool {
        if let Some(existing) = find_staging_closure_near(&mut self.closures, staging_center) {
            existing.ref_count += 1;
            return false;
        }
        self.closures.push(StagingAreaClosure {
            center: staging_center,
            ref_count: 1,
        });
        tracing::info!(
            x = staging_center.x,
            y = staging_center.y,
            radius = STAGING_AREA_CLOSURE_RADIUS_M,
            "staging cordon closed"
        );
        true
    }

    pub fn end_staging_area_closure(&mut self, staging_center: WorldPos) -> bool {
        let Some(idx) = self
            .closures
            .iter()
            .position(|c| staging_center_near(c.center, staging_center, STAGING_AREA_CLOSURE_DEDUP_M))
        else {
            return false;
        };
        self.closures[idx].ref_count -= 1;
        if self.closures[idx].ref_count > 0 {
            return false;
        }
        let center = self.closures.remove(idx).center;
        tracing::info!(x = center.x, y = center.y, "staging cordon reopened");
        true
    }

    pub fn clear_all(&mut self) {
        self.closures.clear();
    }
}

pub fn is_police_engagement_at(case: &CaseRecord) -> bool {
    if case.cancelled {
        return false;
    }
    count_active_threats(case) > 0 || case.is_firearm
}

pub fn compute_vehicle_staging_decoy(crime_pos: WorldPos, vehicle_pos: WorldPos) -> WorldPos {
    let dx = vehicle_pos.x - crime_pos.x;
    let dy = vehicle_pos.y - crime_pos.y;
    let dist_xy = (dx * dx + dy * dy).sqrt();
    let offset = VEHICLE_STAGING_OFFSET_M;

    if dist_xy < 1.0 {
        return WorldPos {
            x: crime_pos.x + offset,
            y: crime_pos.y,
            z: crime_pos.z,
        };
    }
    if dist_xy <= offset + 4.0 {
        return vehicle_pos;
    }

    let nx = dx / dist_xy;
    let ny = dy / dist_xy;
    WorldPos {
        x: crime_pos.x + nx * offset,
        y: crime_pos.y + ny * offset,
        z: crime_pos.z,
    }
}

fn staging_arrival_radius_m() -> f32 {
    VEHICLE_STAGING_OFFSET_M + VEHICLE_STAGING_EXIT_MARGIN_M
}

fn distance_3d(a: WorldPos, b: WorldPos) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    let dz = a.z - b.z;
    (dx * dx + dy * dy + dz * dz).sqrt()
}

fn staging_center_near(a: WorldPos, b: WorldPos, max_dist_m: f32) -> bool {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    (dx * dx + dy * dy).sqrt() <= max_dist_m
}

fn find_staging_closure_near<'a>(
    closures: &'a mut [StagingAreaClosure],
    center: WorldPos,
) -> Option<&'a mut StagingAreaClosure> {
    closures
        .iter_mut()
        .find(|c| staging_center_near(c.center, center, STAGING_AREA_CLOSURE_DEDUP_M))
}