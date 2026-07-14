//! Read-only case snapshots for reroute (avoids borrow conflicts in coordinator tick).

use std::collections::HashSet;

use dispatch_case::{CaseId, CaseRecord, CaseStore};
use dispatch_core::{PedId, VehicleId, WorldPos};

use crate::game::crime_dispatch_position;
use crate::threat::get_case_threat_tier;

#[derive(Debug, Clone)]
pub struct CaseRerouteSnap {
    pub case_id: CaseId,
    pub serial: u64,
    pub cancelled: bool,
    pub is_firearm: bool,
    pub primary: Option<PedId>,
    pub case_vehicles: Vec<VehicleId>,
    pub dispatch_anchor: WorldPos,
    pub threat_tier: i32,
}

fn snap_from_record(case_id: CaseId, record: &CaseRecord) -> CaseRerouteSnap {
    CaseRerouteSnap {
        case_id,
        serial: record.serial,
        cancelled: record.cancelled,
        is_firearm: record.is_firearm,
        primary: record.primary,
        case_vehicles: record.case_vehicles.clone(),
        dispatch_anchor: crime_dispatch_position(record),
        threat_tier: get_case_threat_tier(record),
    }
}

fn snap_for_reroute_eval(
    case_id: CaseId,
    record: &CaseRecord,
    ordered: &HashSet<VehicleId>,
) -> CaseRerouteSnap {
    CaseRerouteSnap {
        case_id,
        serial: record.serial,
        cancelled: false,
        is_firearm: record.is_firearm,
        primary: record.primary,
        case_vehicles: record
            .case_vehicles
            .iter()
            .filter(|vehicle| ordered.contains(vehicle))
            .copied()
            .collect(),
        dispatch_anchor: crime_dispatch_position(record),
        threat_tier: get_case_threat_tier(record),
    }
}

pub fn read_reroute_snapshots(cases: &[(CaseId, &CaseRecord)]) -> Vec<CaseRerouteSnap> {
    cases
        .iter()
        .map(|(case_id, record)| snap_from_record(*case_id, record))
        .collect()
}

pub fn read_reroute_snapshots_mut(cases: &[(CaseId, &mut CaseRecord)]) -> Vec<CaseRerouteSnap> {
    cases
        .iter()
        .map(|(case_id, record)| snap_from_record(*case_id, record))
        .collect()
}

/// All active cases for reroute threat comparison; `case_vehicles` only lists en-route units.
pub fn read_reroute_snapshots_for_eval(
    store: &CaseStore,
    ordered: &HashSet<VehicleId>,
) -> Vec<CaseRerouteSnap> {
    if ordered.is_empty() {
        return Vec::new();
    }
    let case_ids: Vec<CaseId> = store.case_ids().collect();
    case_ids
        .into_iter()
        .filter_map(|case_id| {
            let record = store.get(case_id)?;
            if record.cancelled {
                return None;
            }
            Some(snap_for_reroute_eval(case_id, record, ordered))
        })
        .collect()
}

impl CaseRerouteSnap {
    pub fn dispatch_pos(&self) -> WorldPos {
        self.dispatch_anchor
    }
}