use std::collections::HashMap;

use slotmap::SlotMap;
use tracing::{debug, info};

use dispatch_core::{CausalKind, CausalSignal, DespawnReason, PedId, PedKind, VehicleId};

use crate::incident::{signal_pos, DepartmentSet, IncidentId};
use crate::record::{CaseId, CaseRecord};
use crate::state::DispatchState;
use crate::report_channel::{ReportChannel, ReportClues};
use crate::report_channel::ReportSeverity;
use crate::timing::{
    DEFAULT_DISPATCH_DELAY_MS, HIGH_PRIORITY_DISPATCH_DELAY_MS,
    INTERRUPTED_REPORT_DISPATCH_DELAY_MS,
};

pub struct CaseStore {
    cases: SlotMap<CaseId, CaseRecord>,
    next_serial: u64,
    cases_by_incident: HashMap<IncidentId, Vec<CaseId>>,
    active_case_count: usize,
}

impl CaseStore {
    pub fn new() -> Self {
        Self {
            cases: SlotMap::with_key(),
            next_serial: 1,
            cases_by_incident: HashMap::new(),
            active_case_count: 0,
        }
    }

    pub fn open_from_report(
        &mut self,
        incident_id: IncidentId,
        reporter: PedId,
        channel: ReportChannel,
        clues: ReportClues,
        criminals: &[PedId],
        detect_time_ms: i64,
    ) -> CaseId {
        self.open_case_inner(
            Some(incident_id),
            Some(reporter),
            Some(channel),
            clues,
            criminals.to_vec(),
            detect_time_ms,
            None,
        )
    }

    pub fn update_report_clues(
        &mut self,
        case_id: CaseId,
        clues: ReportClues,
        criminals: &[PedId],
    ) {
        let Some(record) = self.cases.get_mut(case_id) else {
            return;
        };
        record.reported_clues = clues;
        merge_criminals(&mut record.criminals, criminals);
        if record.primary.is_none() {
            record.primary = record.criminals.first().copied();
        }
        debug!(case = record.serial, kinds = record.reported_clues.kinds, "report clues updated");
    }

    pub fn finalize_report(
        &mut self,
        case_id: CaseId,
        clues: ReportClues,
        partial: bool,
        _now_ms: i64,
    ) {
        let Some(record) = self.cases.get_mut(case_id) else {
            return;
        };
        let mut final_clues = clues;
        final_clues.partial = partial || final_clues.partial;
        record.reported_clues = final_clues.clone();
        record.severity = final_clues.severity;
        record.dispatch_delay_ms = dispatch_delay_for(&final_clues);
        record.report_finalized = true;
        if matches!(record.state, DispatchState::Idle | DispatchState::Timing) {
            record.timer_start_ms = _now_ms;
        }
        info!(
            case = record.serial,
            kinds = record.reported_clues.kinds,
            partial = record.reported_clues.partial,
            ?record.severity,
            delay_ms = record.dispatch_delay_ms,
            "report finalized for dispatch"
        );
    }

    fn open_case_inner(
        &mut self,
        incident_id: Option<IncidentId>,
        reporter: Option<PedId>,
        report_channel: Option<ReportChannel>,
        reported_clues: ReportClues,
        criminals: Vec<PedId>,
        detect_time_ms: i64,
        delay_ms: Option<i32>,
    ) -> CaseId {
        let serial = self.next_serial;
        self.next_serial += 1;
        let delay = delay_ms.unwrap_or(dispatch_delay_for(&reported_clues));
        let record = CaseRecord::new(
            serial,
            incident_id,
            reporter,
            report_channel,
            reported_clues,
            criminals,
            detect_time_ms,
            delay,
        );
        let mut record = record;
        record.refresh_department_needs();
        let id = self.cases.insert(record);
        self.active_case_count += 1;
        if let Some(incident_id) = incident_id {
            self.cases_by_incident
                .entry(incident_id)
                .or_default()
                .push(id);
        }
        info!(case_id = serial, ?id, ?incident_id, ?reporter, "case opened");
        id
    }

    pub fn seed_case_departments(&mut self, case_id: CaseId, deps: DepartmentSet) {
        let Some(record) = self.cases.get_mut(case_id) else {
            return;
        };
        record.seed_department_needs(deps);
    }

    pub fn has_case_for_incident(&self, incident_id: IncidentId) -> bool {
        self.cases_by_incident
            .get(&incident_id)
            .into_iter()
            .flatten()
            .any(|&case_id| {
                self.cases
                    .get(case_id)
                    .is_some_and(|case| !case.cancelled)
            })
    }

    /// True while dispatch is already driving a case for this incident (Timing / OnScene).
    pub fn incident_being_handled(&self, incident_id: IncidentId) -> bool {
        self.cases_by_incident
            .get(&incident_id)
            .into_iter()
            .flatten()
            .any(|&case_id| {
                self.cases.get(case_id).is_some_and(|case| {
                    !case.cancelled
                        && matches!(
                            case.state,
                            DispatchState::Timing | DispatchState::OnScene
                        )
                })
            })
    }

    pub fn get(&self, id: CaseId) -> Option<&CaseRecord> {
        self.cases.get(id)
    }

    pub fn get_mut(&mut self, id: CaseId) -> Option<&mut CaseRecord> {
        self.cases.get_mut(id)
    }

    pub fn case_ids(&self) -> impl Iterator<Item = CaseId> + '_ {
        self.cases.keys()
    }

    pub fn active_count(&self) -> usize {
        self.active_case_count
    }

    /// Nearby-cop list is only consumed during police dispatch timing (mobilize / commit).
    pub fn needs_nearby_cop_refresh(&self) -> bool {
        self.cases.values().any(|record| {
            if record.cancelled {
                return false;
            }
            if record.state != DispatchState::Timing {
                return false;
            }
            if !record.police_script_active() {
                return false;
            }
            record.report_finalized || record.report_channel.is_none()
        })
    }

    pub fn on_causal_signal(&mut self, signal: CausalSignal, incident_id: IncidentId) {
        let count_civilian_casualty = matches!(
            signal,
            CausalSignal::PedCasualty { ped_kind, .. }
                if !matches!(ped_kind, PedKind::Player | PedKind::Cop)
        );

        let Some(case_ids) = self.cases_by_incident.get(&incident_id) else {
            return;
        };
        for &case_id in case_ids {
            let Some(record) = self.cases.get_mut(case_id) else {
                continue;
            };
            if record.cancelled {
                continue;
            }
            if count_civilian_casualty {
                record.civilian_casualties += 1;
                debug!(
                    case = record.serial,
                    incident = ?incident_id,
                    total = record.civilian_casualties,
                    "civilian casualty credited via incident"
                );
            }
            if matches!(
                signal.kind(),
                CausalKind::WeaponDischarge | CausalKind::Explosion
            ) {
                record.is_firearm = true;
            }
            record.mark_live_kind(signal.kind());
            record.refresh_department_needs();
            if matches!(
                signal.kind(),
                CausalKind::FireOutbreak
                    | CausalKind::VehicleBurning
                    | CausalKind::Explosion
                    | CausalKind::PedCasualty
                    | CausalKind::PedInjury
            ) {
                record.last_emergency_eval_ms = 0;
            }
            let pos = signal_pos(signal);
            if pos != dispatch_core::WorldPos::default() {
                record.dispatch_anchor = pos;
            }
        }
    }

    pub fn on_ped_despawned(&mut self, id: PedId, reason: DespawnReason) {
        let case_ids: Vec<CaseId> = self.cases.keys().collect();
        for case_id in case_ids {
            let Some(record) = self.cases.get_mut(case_id) else {
                continue;
            };
            if record.cancelled {
                continue;
            }
            if !record.criminals.contains(&id) {
                continue;
            }
            debug!(case = record.serial, ?id, ?reason, "criminal removed");
            record.remove_criminal(id);
            if record.state == DispatchState::Cleanup {
                info!(case = record.serial, "no criminals left -> cleanup");
            }
        }
    }

    pub fn tick(&mut self, now_ms: i64, ped_live: impl Fn(PedId) -> bool) {
        for record in self.cases.values_mut() {
            if record.cancelled {
                continue;
            }
            record.criminals.retain(|criminal| ped_live(*criminal));
            if record.primary.is_some_and(|primary| !ped_live(primary)) {
                record.primary = record.criminals.first().copied();
            }
            if record.should_enter_cleanup() {
                record.state = DispatchState::Cleanup;
            }
            match record.state {
                DispatchState::Idle => {
                    if record.report_finalized || record.report_channel.is_none() {
                        if record.timer_start_ms <= 0 {
                            record.timer_start_ms = now_ms;
                        }
                        record.state = DispatchState::Timing;
                        debug!(case = record.serial, "idle -> timing (post-report)");
                    }
                }
                DispatchState::Timing | DispatchState::OnScene => {}
                DispatchState::Cleanup => {
                    record.cancelled = true;
                    info!(case = record.serial, "cleanup");
                }
            }
        }
        self.remove_cancelled();
    }

    /// Post-exec cleanup without re-running Idle→Timing transitions.
    pub fn finalize_after_exec(&mut self, ped_live: impl Fn(PedId) -> bool) {
        for record in self.cases.values_mut() {
            if record.cancelled {
                continue;
            }
            record.criminals.retain(|criminal| ped_live(*criminal));
            if record.primary.is_some_and(|primary| !ped_live(primary)) {
                record.primary = record.criminals.first().copied();
            }
            if record.should_enter_cleanup() {
                record.state = DispatchState::Cleanup;
            }
            if matches!(record.state, DispatchState::Cleanup) {
                record.cancelled = true;
                info!(case = record.serial, "cleanup");
            }
        }
        self.remove_cancelled();
    }

    pub fn on_vehicle_despawned(&mut self, id: VehicleId) {
        for record in self.cases.values_mut() {
            if record.cancelled {
                continue;
            }
            record.note_vehicle_gone(id);
        }
    }

    fn remove_cancelled(&mut self) {
        let stale: Vec<CaseId> = self
            .cases
            .iter()
            .filter_map(|(id, record)| record.cancelled.then_some(id))
            .collect();
        for id in stale {
            if let Some(incident_id) = self.cases.get(id).and_then(|record| record.incident_id) {
                if let Some(bucket) = self.cases_by_incident.get_mut(&incident_id) {
                    bucket.retain(|case_id| *case_id != id);
                    if bucket.is_empty() {
                        self.cases_by_incident.remove(&incident_id);
                    }
                }
            }
            self.cases.remove(id);
            self.active_case_count = self.active_case_count.saturating_sub(1);
        }
    }
}

fn dispatch_delay_for(clues: &ReportClues) -> i32 {
    match clues.severity {
        ReportSeverity::Interrupted => INTERRUPTED_REPORT_DISPATCH_DELAY_MS,
        ReportSeverity::High => HIGH_PRIORITY_DISPATCH_DELAY_MS,
        ReportSeverity::Normal => DEFAULT_DISPATCH_DELAY_MS,
    }
}

fn merge_criminals(roster: &mut Vec<PedId>, incoming: &[PedId]) {
    for id in incoming {
        if !roster.contains(id) {
            roster.push(*id);
        }
    }
}