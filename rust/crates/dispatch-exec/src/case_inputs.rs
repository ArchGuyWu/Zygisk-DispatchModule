//! Per-case execution inputs derived from `CaseRecord` and frame context.

use dispatch_case::{ped_kind_from_type, CaseRecord, ReportClues, WitnessReportCoordinator};
use dispatch_core::{PedId, PedKind, ResourceRegistry, WorldPos};
use dispatch_engine::dist_sq;

use crate::response::PerceptionChannel;
use crate::timing::{av_range_for_firearm, OFFSCREEN_SPAWN_MIN_PLAYER_DIST_M};

#[derive(Debug, Clone)]
pub struct FrameInputs<'a> {
    pub nearby_cops: &'a [crate::response::NearbyCopCandidate],
    pub density: i32,
    pub swat_already: bool,
    pub player_pos: WorldPos,
}

#[derive(Debug, Clone, Copy)]
pub struct CaseExecInputs {
    pub road_reachable: bool,
    pub offscreen_allowed: bool,
    pub witness_backing: bool,
    pub is_player_case: bool,
    pub player_dist: f32,
    pub perception_best: i32,
    pub suspect_confirmed: bool,
    pub cop_within_av: bool,
}

impl CaseExecInputs {
    pub fn build(
        record: &CaseRecord,
        frame: &FrameInputs<'_>,
        witness: &WitnessReportCoordinator,
        registry: &ResourceRegistry,
    ) -> Self {
        let crime_pos = crime_anchor(record);
        let player_dist = dist_xy(frame.player_pos, crime_pos);
        let is_player_case = record
            .criminals
            .iter()
            .copied()
            .chain(record.primary)
            .any(|id| {
                registry
                    .ped(id)
                    .is_some_and(|entry| ped_kind_from_type(entry.ped_type) == PedKind::Player)
            });
        let perception_best = perception_from_clues(&record.reported_clues);
        let suspect_confirmed = record.primary.is_some()
            || record.reported_clues.suspect_pos.is_some()
            || !record.reported_clues.perpetrators.is_empty();
        let witness_backing = witness_backing_for(record, witness);
        let offscreen_allowed =
            player_dist >= OFFSCREEN_SPAWN_MIN_PLAYER_DIST_M || !is_player_case;
        let road_reachable = crime_pos != WorldPos::default();
        let cop_within_av = any_cop_within_av(frame.nearby_cops, crime_pos, record.is_firearm);

        Self {
            road_reachable,
            offscreen_allowed,
            witness_backing,
            is_player_case,
            player_dist,
            perception_best,
            suspect_confirmed,
            cop_within_av,
        }
    }
}

fn crime_anchor(record: &CaseRecord) -> WorldPos {
    if record.dispatch_anchor != WorldPos::default() {
        return record.dispatch_anchor;
    }
    record
        .reported_clues
        .suspect_pos
        .or(record.reported_clues.reporter_pos)
        .unwrap_or_default()
}

fn dist_xy(a: WorldPos, b: WorldPos) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    (dx * dx + dy * dy).sqrt()
}

fn perception_from_clues(clues: &ReportClues) -> i32 {
    if clues.suspect_pos.is_some() || clues.perpetrators.iter().any(|e| !e.ptr().is_null()) {
        return PerceptionChannel::Seen as i32;
    }
    if clues.kinds != 0 {
        return PerceptionChannel::Heard as i32;
    }
    PerceptionChannel::None as i32
}

fn witness_backing_for(record: &CaseRecord, witness: &WitnessReportCoordinator) -> bool {
    if record.reported_clues.actionable() {
        return true;
    }
    if let Some(incident_id) = record.incident_id {
        if witness.has_open_report(incident_id) {
            return true;
        }
    }
    record.reporter.is_some() && record.report_channel.is_some()
}

fn any_cop_within_av(
    nearby: &[crate::response::NearbyCopCandidate],
    crime_pos: WorldPos,
    is_firearm: bool,
) -> bool {
    let range = av_range_for_firearm(is_firearm);
    let radius_sq = range * range;
    nearby
        .iter()
        .any(|candidate| dist_sq(candidate.cop_pos, crime_pos) <= radius_sq)
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_case::ReportClues;

    #[test]
    fn player_case_detected_from_criminal_ped() {
        let mut registry = ResourceRegistry::new();
        let player = registry.adopt_ped(dispatch_core::PoolKey::from_slot_flag(1, 1), 0);
        let record = CaseRecord::new(
            1,
            None,
            None,
            None,
            ReportClues::default(),
            vec![player],
            0,
            1000,
        );
        let frame = FrameInputs {
            nearby_cops: &[],
            density: 1,
            swat_already: false,
            player_pos: WorldPos::default(),
        };
        let witness = WitnessReportCoordinator::new();
        let inputs = CaseExecInputs::build(&record, &frame, &witness, &registry);
        assert!(inputs.is_player_case);
    }
}