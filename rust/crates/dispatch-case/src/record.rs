use slotmap::new_key_type;

use dispatch_core::{EntityRef, PedId, VehicleId, WorldPos};

use crate::incident::{DepartmentSet, IncidentId};
use crate::playbook::DepartmentNeeds;
use crate::state::DispatchState;
use crate::report_channel::ReportChannel;
use crate::report_channel::{ReportClues, ReportSeverity};

new_key_type! { pub struct CaseId; }

#[derive(Debug, Clone)]
pub struct CaseRecord {
    pub serial: u64,
    pub incident_id: Option<IncidentId>,
    pub reporter: Option<dispatch_core::PedId>,
    pub report_channel: Option<ReportChannel>,
    pub reported_clues: ReportClues,
    pub severity: ReportSeverity,
    pub criminals: Vec<dispatch_core::PedId>,
    pub primary: Option<dispatch_core::PedId>,
    pub state: DispatchState,
    pub detect_time_ms: i64,
    pub timer_start_ms: i64,
    pub dispatch_delay_ms: i32,
    pub on_scene_start_ms: i64,
    pub cancelled: bool,
    /// Execution-layer fields (ported from C++ `CrimeEvent`).
    pub dispatch_sent: bool,
    pub vehicle_spawn_pending: bool,
    pub case_vehicles: Vec<VehicleId>,
    /// Cops dispatched to this case — used for on-foot idle wait when no abnormality.
    pub scene_responders: Vec<PedId>,
    pub spawned_vehicle: Option<VehicleId>,
    pub cops_killed: i32,
    pub is_firearm: bool,
    pub dispatch_anchor: WorldPos,
    pub reinforcements_sent: i32,
    pub last_reinforcement_eval_ms: i64,
    /// True once the reporter hung up / radio report closed — post-report dispatch timer runs.
    pub report_finalized: bool,
    pub spawn_time_ms: i64,
    pub last_on_scene_dispatch_ms: i64,
    pub last_nearby_dispatch_attempt_ms: i64,
    pub civilian_casualties: i32,
    pub mod_ambulance_dispatched: bool,
    pub mod_firetruck_dispatched: bool,
    pub last_emergency_eval_ms: i64,
    /// Live incident kinds merged after the 911 call (fire, casualties, …).
    pub live_kinds: u8,
    /// Incident routing at case open; merged into `department_needs` on refresh.
    pub department_needs_seed: Option<DepartmentSet>,
    /// Playbook: which department scripts are active (recomputed from clues + live state).
    pub department_needs: DepartmentNeeds,
}

impl CaseRecord {
    pub fn new(
        serial: u64,
        incident_id: Option<IncidentId>,
        reporter: Option<dispatch_core::PedId>,
        report_channel: Option<ReportChannel>,
        reported_clues: ReportClues,
        criminals: Vec<dispatch_core::PedId>,
        detect_time_ms: i64,
        delay_ms: i32,
    ) -> Self {
        let primary = criminals.first().copied();
        let severity = reported_clues.severity;
        let is_firearm = reported_clues.has_kind(dispatch_core::CausalKind::WeaponDischarge)
            || reported_clues.has_kind(dispatch_core::CausalKind::Explosion);
        let dispatch_anchor = reported_clues
            .suspect_pos
            .or(reported_clues.reporter_pos)
            .unwrap_or_default();
        Self {
            serial,
            incident_id,
            reporter,
            report_channel,
            reported_clues,
            severity,
            criminals,
            primary,
            state: DispatchState::Idle,
            detect_time_ms,
            timer_start_ms: 0,
            dispatch_delay_ms: delay_ms,
            on_scene_start_ms: 0,
            cancelled: false,
            dispatch_sent: false,
            vehicle_spawn_pending: false,
            case_vehicles: Vec::new(),
            scene_responders: Vec::new(),
            spawned_vehicle: None,
            cops_killed: 0,
            is_firearm,
            dispatch_anchor,
            reinforcements_sent: 0,
            last_reinforcement_eval_ms: 0,
            report_finalized: false,
            spawn_time_ms: 0,
            last_on_scene_dispatch_ms: 0,
            last_nearby_dispatch_attempt_ms: 0,
            civilian_casualties: 0,
            mod_ambulance_dispatched: false,
            mod_firetruck_dispatched: false,
            last_emergency_eval_ms: 0,
            live_kinds: 0,
            department_needs_seed: None,
            department_needs: DepartmentNeeds::default(),
        }
    }

    pub fn seed_department_needs(&mut self, deps: DepartmentSet) {
        self.department_needs_seed = Some(deps);
        self.refresh_department_needs();
    }

    pub fn has_live_kind(&self, kind: dispatch_core::CausalKind) -> bool {
        (self.live_kinds & (1 << (kind as u8))) != 0
    }

    pub fn mark_live_kind(&mut self, kind: dispatch_core::CausalKind) {
        self.live_kinds |= 1 << (kind as u8);
    }

    pub fn reported_entities(&self) -> &[EntityRef] {
        &self.reported_clues.entities
    }

    /// Shared criminal removal path (`on_ped_despawned`, arrest transport release, …).
    pub fn register_scene_responder(&mut self, cop: PedId) {
        if !self.scene_responders.contains(&cop) {
            self.scene_responders.push(cop);
        }
    }

    pub fn remove_criminal(&mut self, criminal: PedId) {
        if !self.criminals.contains(&criminal) {
            return;
        }
        self.criminals.retain(|id| *id != criminal);
        if self.primary == Some(criminal) {
            self.primary = self.criminals.first().copied();
        }
        if self.criminals.is_empty() {
            self.state = DispatchState::Cleanup;
        }
    }
}