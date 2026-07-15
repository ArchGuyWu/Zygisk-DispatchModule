use std::collections::HashSet;

use tracing::{debug, info, warn};

use dispatch_core::{CausalKind, CausalSignal, EntityRef, PedId, WorldPos};

use crate::incident::{
    perpetrator_entity_refs, signal_entity_refs, Incident, IncidentId, IncidentStore,
};
use crate::record::CaseId;
use crate::report_channel::{
    ReportChannel, ReportClues, ReportEndReason, ReportPhase, ReportSeverity,
};
use crate::store::CaseStore;

pub const MAX_WITNESS_ENTITIES: usize = 8;
pub const MAX_WITNESS_PERPETRATORS: usize = 4;

#[derive(Debug, Clone, Copy)]
pub struct WitnessObservation {
    pub ped: PedId,
    /// Caller location — always reported first.
    pub pos: WorldPos,
    pub panicked: bool,
    pub heard: bool,
    pub saw: bool,
    pub perceived_entities: [EntityRef; MAX_WITNESS_ENTITIES],
    pub perceived_entity_count: u8,
    pub perceived_kinds: u8,
    /// Peds the witness identified as attackers (engine perception).
    pub perceived_perpetrators: [EntityRef; MAX_WITNESS_PERPETRATORS],
    pub perceived_perpetrator_count: u8,
    /// Set only when the caller saw the suspect.
    pub suspect_pos: Option<WorldPos>,
}

impl WitnessObservation {
    pub fn can_report(&self) -> bool {
        !self.panicked && (self.heard || self.saw)
    }

    pub fn perceived_entities(&self) -> &[EntityRef] {
        &self.perceived_entities[..self.perceived_entity_count as usize]
    }

    pub fn perceived_perpetrators(&self) -> &[EntityRef] {
        &self.perceived_perpetrators[..self.perceived_perpetrator_count as usize]
    }

    pub fn new_perceived(
        ped: PedId,
        pos: WorldPos,
        panicked: bool,
        heard: bool,
        saw: bool,
        entities: &[EntityRef],
        kinds: u8,
        perpetrators: &[EntityRef],
        suspect_pos: Option<WorldPos>,
    ) -> Self {
        let (perceived_entities, perceived_entity_count) = pack_entity_refs(entities);
        let (perceived_perpetrators, perceived_perpetrator_count) = pack_entity_refs(perpetrators);
        Self {
            ped,
            pos,
            panicked,
            heard,
            saw,
            perceived_entities,
            perceived_entity_count,
            perceived_kinds: kinds,
            perceived_perpetrators,
            perceived_perpetrator_count,
            suspect_pos,
        }
    }
}

pub fn pack_entity_refs<const N: usize>(src: &[EntityRef]) -> ([EntityRef; N], u8) {
    let mut out = [EntityRef(std::ptr::null()); N];
    let count = src.len().min(N) as u8;
    for (i, &entity) in src.iter().take(N).enumerate() {
        out[i] = entity;
    }
    (out, count)
}

#[derive(Debug, Clone)]
pub struct ReportSession {
    pub incident_id: IncidentId,
    pub reporter: PedId,
    pub channel: ReportChannel,
    pub phase: ReportPhase,
    pub started_ms: i64,
    pub case_id: Option<CaseId>,
    pub collected: ReportClues,
    pub end_reason: Option<ReportEndReason>,
    reporter_pos: WorldPos,
    perceived_kinds: u8,
    perceived_entities: Vec<EntityRef>,
    perceived_perpetrators: Vec<EntityRef>,
    suspect_pos: Option<WorldPos>,
}

pub struct WitnessReportCoordinator {
    sessions: Vec<ReportSession>,
    observations: Vec<WitnessObservation>,
}

impl WitnessReportCoordinator {
    pub fn new() -> Self {
        Self {
            sessions: Vec::new(),
            observations: Vec::new(),
        }
    }

    pub fn clear_observations(&mut self) {
        self.observations.clear();
    }

    pub fn observe(&mut self, observation: WitnessObservation) {
        if observation.can_report() {
            self.observations.push(observation);
        }
    }

    /// Bare causal signals merge only while a reporter is actively on the line.
    pub fn active_incident(&self) -> Option<IncidentId> {
        self.sessions
            .iter()
            .find(|session| session.phase == ReportPhase::Active)
            .map(|session| session.incident_id)
    }

    pub fn session_for_reporter(&self, reporter: PedId) -> Option<&ReportSession> {
        self.sessions.iter().find(|session| session.reporter == reporter)
    }

    pub fn session_for_incident(&self, incident_id: IncidentId) -> Option<&ReportSession> {
        self.sessions
            .iter()
            .find(|session| session.incident_id == incident_id)
    }

    pub fn has_open_report(&self, incident_id: IncidentId) -> bool {
        self.session_for_incident(incident_id)
            .is_some_and(|session| session.phase != ReportPhase::Ended)
    }

    pub fn pending_sessions(&self) -> impl Iterator<Item = &ReportSession> {
        self.sessions
            .iter()
            .filter(|session| session.phase == ReportPhase::Approaching)
    }

    /// All sessions still driven by engine tasks (not yet ended).
    pub fn open_sessions(&self) -> impl Iterator<Item = &ReportSession> {
        self.sessions
            .iter()
            .filter(|session| session.phase != ReportPhase::Ended)
    }

    /// Elect reporter and create session; engine must drive tasks from here.
    pub fn try_start_reports(
        &mut self,
        incidents: &mut IncidentStore,
        cases: &CaseStore,
        now_ms: i64,
    ) -> usize {
        let mut started = 0usize;
        let incident_ids: Vec<IncidentId> = incidents.iter_ids().collect();
        for incident_id in incident_ids {
            if self.has_open_report(incident_id) || cases.has_case_for_incident(incident_id) {
                continue;
            }
            let Some(incident) = incidents.get(incident_id) else {
                continue;
            };
            if incident.reporting_exhausted {
                continue;
            }
            if !incident_warrants_civilian_report(incident) {
                continue;
            }
            let Some((reporter, obs)) = elect_reporter(incident, &self.observations) else {
                continue;
            };
            let incident_serial = incident.serial;
            let session = ReportSession {
                incident_id,
                reporter,
                channel: ReportChannel::Cellphone,
                phase: ReportPhase::Approaching,
                started_ms: now_ms,
                case_id: None,
                collected: ReportClues::default(),
                end_reason: None,
                reporter_pos: obs.pos,
                perceived_kinds: obs.perceived_kinds,
                perceived_entities: obs.perceived_entities().to_vec(),
                perceived_perpetrators: obs.perceived_perpetrators().to_vec(),
                suspect_pos: obs.suspect_pos,
            };
            incidents.mark_reporting_exhausted(incident_id);
            info!(
                incident = incident_serial,
                ?reporter,
                "civilian report queued for engine task"
            );
            self.sessions.push(session);
            started += 1;
        }
        started
    }

    pub fn on_report_task_dialing(&mut self, reporter: PedId) {
        let Some(session) = self.session_mut_reporter(reporter) else {
            return;
        };
        if session.phase == ReportPhase::Approaching {
            session.phase = ReportPhase::Dialing;
            debug!(?reporter, "report task dialing");
        }
    }

    pub fn on_report_task_active(
        &mut self,
        reporter: PedId,
        incidents: &IncidentStore,
        cases: &mut CaseStore,
        mut criminals_for: impl FnMut(&ReportClues, &mut Vec<PedId>),
        now_ms: i64,
    ) -> bool {
        let Some(incident_id) = self
            .session_mut_reporter(reporter)
            .map(|session| session.incident_id)
        else {
            return false;
        };
        let Some(incident) = incidents.get(incident_id) else {
            return false;
        };
        let Some(session) = self.session_mut_reporter(reporter) else {
            return false;
        };
        if matches!(session.phase, ReportPhase::Ended) {
            return false;
        }
        session.phase = ReportPhase::Active;
        session.collected.seed_caller_location(session.reporter_pos);
        if let Some(suspect_pos) = session.suspect_pos {
            session.collected.note_suspect_location(suspect_pos);
        }
        session.collected.absorb_perceived(
            session.perceived_kinds,
            &session.perceived_entities,
        );
        session
            .collected
            .merge_perpetrators(&session.perceived_perpetrators);
        let mut criminals = Vec::with_capacity(4);
        criminals_for(&session.collected, &mut criminals);
        let case_id = cases.open_from_report(
            incident_id,
            reporter,
            session.channel,
            session.collected.clone(),
            &criminals,
            now_ms,
        );
        cases.seed_case_departments(case_id, incident.departments);
        session.case_id = Some(case_id);
        info!(
            incident = incident.serial,
            ?reporter,
            kinds = session.collected.kinds,
            needs = ?cases.get(case_id).map(|c| c.department_needs),
            "report call active, case opened"
        );
        true
    }

    pub fn on_report_task_ended(&mut self, reporter: PedId, cases: &mut CaseStore, now_ms: i64) {
        self.finish_session(reporter, ReportEndReason::TaskCompleted, cases, now_ms);
    }

    pub fn on_reporter_interrupted(
        &mut self,
        reporter: PedId,
        incidents: &IncidentStore,
        cases: &mut CaseStore,
        mut criminals_for: impl FnMut(&ReportClues, &mut Vec<PedId>),
        now_ms: i64,
    ) {
        let Some(session) = self.session_for_reporter(reporter) else {
            return;
        };
        if session.phase == ReportPhase::Ended {
            return;
        }
        let Some(session) = self.session_mut_reporter(reporter) else {
            return;
        };
        seed_collected_from_session(session);
        session.collected.mark_interrupted();
        session.collected.refresh_severity();
        open_case_if_needed(session, incidents, cases, &mut criminals_for, now_ms);
        warn!(
            ?reporter,
            ?session.phase,
            kinds = session.collected.kinds,
            "report interrupted, partial clues escalated"
        );
        self.finish_session(reporter, ReportEndReason::ReporterLost, cases, now_ms);
    }

    /// Engine: caller panicked mid-call — same as any other interruption.
    pub fn on_reporter_panic(
        &mut self,
        reporter: PedId,
        incidents: &IncidentStore,
        cases: &mut CaseStore,
        mut criminals_for: impl FnMut(&ReportClues, &mut Vec<PedId>),
        now_ms: i64,
    ) {
        self.on_reporter_interrupted(reporter, incidents, cases, criminals_for, now_ms);
    }

    pub fn on_causal_signal(
        &mut self,
        signal: CausalSignal,
        incident_id: IncidentId,
        incidents: &IncidentStore,
        cases: &mut CaseStore,
        mut criminals_for: impl FnMut(&ReportClues, &mut Vec<PedId>),
    ) {
        let Some(session) = self
            .sessions
            .iter_mut()
            .find(|session| session.incident_id == incident_id)
        else {
            return;
        };
        if session.phase != ReportPhase::Active {
            return;
        };
        let Some(incident) = incidents.get(incident_id) else {
            return;
        };
        let kind_bit = 1 << signal.kind() as u8;
        if session.perceived_kinds & kind_bit == 0 {
            return;
        }
        let entity_set: HashSet<EntityRef> =
            session.perceived_entities.iter().copied().collect();
        let perp_set: HashSet<EntityRef> =
            session.perceived_perpetrators.iter().copied().collect();
        let perceived_entities: Vec<_> = signal_entity_refs(signal)
            .into_iter()
            .filter(|entity| {
                entity.ptr() != std::ptr::null() && entity_set.contains(entity)
            })
            .collect();
        let perpetrators: Vec<_> = perpetrator_entity_refs(signal)
            .into_iter()
            .filter(|entity| {
                entity.ptr() != std::ptr::null()
                    && (perp_set.contains(entity) || entity_set.contains(entity))
            })
            .collect();
        let before = session.collected.kinds;
        session
            .collected
            .absorb_perceived(kind_bit, &perceived_entities);
        session.collected.merge_perpetrators(&perpetrators);
        if session.collected.kinds == before && perpetrators.is_empty() {
            return;
        };
        session.collected.refresh_severity();
        if let Some(case_id) = session.case_id {
            let mut criminals = Vec::with_capacity(4);
            criminals_for(&session.collected, &mut criminals);
            cases.update_report_clues(case_id, session.collected.clone(), &criminals);
            debug!(
                incident = incident.serial,
                kinds = session.collected.kinds,
                "report collected new perceived clues"
            );
        }
    }

    pub fn tick(
        &mut self,
        reporter_live: impl Fn(PedId) -> bool,
        incidents: &IncidentStore,
        cases: &mut CaseStore,
        mut criminals_for: impl FnMut(&ReportClues, &mut Vec<PedId>),
        now_ms: i64,
    ) {
        let interrupted: Vec<PedId> = self
            .sessions
            .iter()
            .filter(|session| {
                session.phase != ReportPhase::Ended && !reporter_live(session.reporter)
            })
            .map(|session| session.reporter)
            .collect();
        for reporter in interrupted {
            self.on_reporter_interrupted(reporter, incidents, cases, &mut criminals_for, now_ms);
        }
        self.sessions
            .retain(|session| session.phase != ReportPhase::Ended);
    }

    fn finish_session(
        &mut self,
        reporter: PedId,
        reason: ReportEndReason,
        cases: &mut CaseStore,
        now_ms: i64,
    ) {
        let Some(session) = self.session_mut_reporter(reporter) else {
            return;
        };
        if session.phase == ReportPhase::Ended {
            return;
        };
        session.phase = ReportPhase::Ended;
        session.end_reason = Some(reason);
        if let Some(case_id) = session.case_id {
            cases.finalize_report(case_id, session.collected.clone(), session.collected.partial, now_ms);
        }
        info!(?reporter, ?reason, partial = session.collected.partial, "report session ended");
    }

    fn session_mut_reporter(&mut self, reporter: PedId) -> Option<&mut ReportSession> {
        self.sessions
            .iter_mut()
            .find(|session| session.reporter == reporter)
    }

}

fn open_case_if_needed(
    session: &mut ReportSession,
    incidents: &IncidentStore,
    cases: &mut CaseStore,
    criminals_for: &mut impl FnMut(&ReportClues, &mut Vec<PedId>),
    now_ms: i64,
) {
    if session.case_id.is_some() || !session.collected.actionable() {
        return;
    }
    let mut criminals = Vec::with_capacity(4);
    criminals_for(&session.collected, &mut criminals);
    let case_id = cases.open_from_report(
        session.incident_id,
        session.reporter,
        session.channel,
        session.collected.clone(),
        &criminals,
        now_ms,
    );
    if let Some(incident) = incidents.get(session.incident_id) {
        cases.seed_case_departments(case_id, incident.departments);
    }
    session.case_id = Some(case_id);
    info!(
        ?session.reporter,
        kinds = session.collected.kinds,
        partial = session.collected.partial,
        "case opened from interrupted report"
    );
}

fn seed_collected_from_session(session: &mut ReportSession) {
    if session.collected.reporter_pos.is_none() {
        session.collected.seed_caller_location(session.reporter_pos);
    }
    if session.collected.suspect_pos.is_none() {
        if let Some(suspect_pos) = session.suspect_pos {
            session.collected.note_suspect_location(suspect_pos);
        }
    }
    if session.phase != ReportPhase::Active {
        session.collected.absorb_perceived(
            session.perceived_kinds,
            &session.perceived_entities,
        );
        session
            .collected
            .merge_perpetrators(&session.perceived_perpetrators);
    }
    session.collected.refresh_severity();
}

impl Default for WitnessReportCoordinator {
    fn default() -> Self {
        Self::new()
    }
}

/// Any incident with perceived clues may trigger a civilian 911 / cellphone report.
/// Department routing (police / EMS / fire) happens after the call opens a case.
fn incident_warrants_civilian_report(incident: &Incident) -> bool {
    incident.kinds != 0
}

fn elect_reporter(
    incident: &Incident,
    observations: &[WitnessObservation],
) -> Option<(PedId, WitnessObservation)> {
    let mut best: Option<(WitnessObservation, i32, f32)> = None;
    for obs in observations {
        if !obs.can_report() {
            continue;
        }
        let score = clue_score(incident, obs);
        if score <= 0 {
            continue;
        }
        let dist_sq = dist_sq(obs.pos, incident.anchor);
        match best {
            Some((_, best_score, _)) if score < best_score => {}
            Some((_, best_score, best_dist)) if score == best_score && dist_sq >= best_dist => {}
            _ => best = Some((obs.clone(), score, dist_sq)),
        }
    }
    best.map(|(obs, _, _)| (obs.ped, obs))
}

/// Prefer witnesses who share any incident kind and who saw (not multi-weight theater).
fn clue_score(incident: &Incident, obs: &WitnessObservation) -> i32 {
    if obs.perceived_kinds & incident.kinds == 0
        && !obs
            .perceived_entities()
            .iter()
            .any(|e| incident.has_entity(*e))
    {
        return 0;
    }
    let mut score = 1i32;
    if obs.saw {
        score += 2;
    } else if obs.heard {
        score += 1;
    }
    score
}

fn dist_sq(a: WorldPos, b: WorldPos) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    let dz = a.z - b.z;
    dx * dx + dy * dy + dz * dz
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::timing::INTERRUPTED_REPORT_DISPATCH_DELAY_MS;
    use dispatch_core::CausalSource;

    fn ptr(id: usize) -> *const std::ffi::c_void {
        id as *const std::ffi::c_void
    }

    #[test]
    fn panicked_witnesses_are_filtered() {
        let witness = WitnessObservation::new_perceived(
            PedId::default(),
            WorldPos::default(),
            true,
            true,
            true,
            &[],
            0,
            &[],
            None,
        );
        assert!(!witness.can_report());
    }

    #[test]
    fn fire_only_incident_starts_civilian_report() {
        let mut incidents = IncidentStore::new();
        let cases = CaseStore::new();
        let mut coord = WitnessReportCoordinator::new();
        let reporter = PedId::default();

        let _incident_id = incidents.ingest(
            CausalSignal::FireOutbreak {
                pos: WorldPos {
                    x: 100.0,
                    y: 200.0,
                    z: 10.0,
                },
                source: CausalSource::FireManager,
            },
            1_000,
            None,
        );

        coord.observe(WitnessObservation::new_perceived(
            reporter,
            WorldPos {
                x: 100.0,
                y: 200.0,
                z: 10.0,
            },
            false,
            true,
            true,
            &[],
            1 << CausalKind::FireOutbreak as u8,
            &[],
            None,
        ));

        assert_eq!(coord.try_start_reports(&mut incidents, &cases, 2_000), 1);
    }

    #[test]
    fn case_opens_only_when_task_active() {
        let attacker = ptr(0x1000);
        let vehicle = ptr(0x2000);
        let mut incidents = IncidentStore::new();
        let mut cases = CaseStore::new();
        let mut coord = WitnessReportCoordinator::new();
        let reporter = PedId::default();
        let mut criminals_for = |_: &ReportClues, out: &mut Vec<PedId>| out.clear();

        let incident_id = incidents.ingest(
            CausalSignal::VehiclePropertyDamage {
                vehicle,
                attacker,
                pos: WorldPos {
                    x: 2166.0,
                    y: -1155.0,
                    z: 24.0,
                },
                damage: 120.0,
                source: CausalSource::VehicleInflictDamage,
            },
            1_000,
            None,
        );

        coord.observe(WitnessObservation::new_perceived(
            reporter,
            WorldPos {
                x: 2166.0,
                y: -1155.0,
                z: 24.0,
            },
            false,
            true,
            true,
            &[
                EntityRef::new(attacker).unwrap(),
                EntityRef::new(vehicle).unwrap(),
            ],
            1 << CausalKind::VehiclePropertyDamage as u8,
            &[EntityRef::new(attacker).unwrap()],
            Some(WorldPos {
                x: 2170.0,
                y: -1150.0,
                z: 24.0,
            }),
        ));

        assert_eq!(coord.try_start_reports(&mut incidents, &cases, 2_000), 1);
        assert!(!cases.has_case_for_incident(incident_id));

        coord.on_report_task_active(reporter, &incidents, &mut cases, |_, out| { out.clear(); }, 3_000);
        assert!(cases.has_case_for_incident(incident_id));
    }

    #[test]
    fn interrupted_report_keeps_partial_clues_and_escalates() {
        let attacker = ptr(0x1000);
        let vehicle = ptr(0x2000);
        let mut incidents = IncidentStore::new();
        let mut cases = CaseStore::new();
        let mut coord = WitnessReportCoordinator::new();
        let reporter = PedId::default();
        let mut criminals_for = |_: &ReportClues, out: &mut Vec<PedId>| out.clear();

        let incident_id = incidents.ingest(
            CausalSignal::VehiclePropertyDamage {
                vehicle,
                attacker,
                pos: WorldPos::default(),
                damage: 50.0,
                source: CausalSource::VehicleInflictDamage,
            },
            1_000,
            None,
        );
        coord.observe(WitnessObservation::new_perceived(
            reporter,
            WorldPos::default(),
            false,
            true,
            true,
            &[EntityRef::new(attacker).unwrap()],
            1 << CausalKind::VehiclePropertyDamage as u8,
            &[EntityRef::new(attacker).unwrap()],
            None,
        ));
        coord.try_start_reports(&mut incidents, &cases, 1_500);
        coord.on_report_task_active(reporter, &incidents, &mut cases, |_, out| { out.clear(); }, 2_000);
        let case_id = coord
            .session_for_reporter(reporter)
            .expect("session")
            .case_id
            .expect("case");

        incidents.ingest(
            CausalSignal::Explosion {
                pos: WorldPos::default(),
                weapon: 51,
                attacker,
                vehicle,
                source: CausalSource::Explosion,
            },
            2_500,
            None,
        );

        coord.on_reporter_interrupted(
            reporter,
            &incidents,
            &mut cases,
            criminals_for,
            2_600,
        );

        let case = cases.get(case_id).expect("case");
        assert!(case.reported_clues.partial);
        assert_eq!(case.severity, ReportSeverity::Interrupted);
        assert!(case.reported_clues.has_kind(CausalKind::VehiclePropertyDamage));
        assert!(!case.reported_clues.has_kind(CausalKind::Explosion));
        assert!(cases.has_case_for_incident(incident_id));
        assert_eq!(coord.try_start_reports(&mut incidents, &cases, 3_000), 0);
        assert!(incidents.get(incident_id).unwrap().reporting_exhausted);
    }

    #[test]
    fn interrupted_before_active_still_opens_case() {
        let attacker = ptr(0x1000);
        let vehicle = ptr(0x2000);
        let mut incidents = IncidentStore::new();
        let mut cases = CaseStore::new();
        let mut coord = WitnessReportCoordinator::new();
        let reporter = PedId::default();
        let mut criminals_for = |_: &ReportClues, out: &mut Vec<PedId>| out.clear();

        let incident_id = incidents.ingest(
            CausalSignal::VehiclePropertyDamage {
                vehicle,
                attacker,
                pos: WorldPos::default(),
                damage: 50.0,
                source: CausalSource::VehicleInflictDamage,
            },
            1_000,
            None,
        );
        coord.observe(WitnessObservation::new_perceived(
            reporter,
            WorldPos {
                x: 10.0,
                y: 0.0,
                z: 0.0,
            },
            false,
            true,
            false,
            &[EntityRef::new(vehicle).unwrap()],
            1 << CausalKind::VehiclePropertyDamage as u8,
            &[],
            None,
        ));
        coord.try_start_reports(&mut incidents, &cases, 1_500);
        assert!(!cases.has_case_for_incident(incident_id));

        coord.on_reporter_interrupted(
            reporter,
            &incidents,
            &mut cases,
            criminals_for,
            2_000,
        );

        assert!(cases.has_case_for_incident(incident_id));
        let case = cases
            .get(
                coord
                    .session_for_reporter(reporter)
                    .expect("session")
                    .case_id
                    .expect("case"),
            )
            .expect("case");
        assert!(case.reported_clues.partial);
        assert_eq!(case.severity, ReportSeverity::Interrupted);
        assert_eq!(case.dispatch_delay_ms, INTERRUPTED_REPORT_DISPATCH_DELAY_MS);
        assert!(case.reported_clues.reporter_pos.is_some());
    }

    #[test]
    fn active_call_only_absorbs_perceived_signal_kinds() {
        let attacker = ptr(0x1000);
        let vehicle = ptr(0x2000);
        let mut incidents = IncidentStore::new();
        let mut cases = CaseStore::new();
        let mut coord = WitnessReportCoordinator::new();
        let reporter = PedId::default();

        let incident_id = incidents.ingest(
            CausalSignal::VehiclePropertyDamage {
                vehicle,
                attacker,
                pos: WorldPos::default(),
                damage: 50.0,
                source: CausalSource::VehicleInflictDamage,
            },
            1_000,
            None,
        );
        coord.observe(WitnessObservation::new_perceived(
            reporter,
            WorldPos::default(),
            false,
            true,
            true,
            &[
                EntityRef::new(attacker).unwrap(),
                EntityRef::new(vehicle).unwrap(),
            ],
            1 << CausalKind::VehiclePropertyDamage as u8,
            &[EntityRef::new(attacker).unwrap()],
            None,
        ));
        coord.try_start_reports(&mut incidents, &cases, 1_500);
        coord.on_report_task_active(reporter, &incidents, &mut cases, |_, out| { out.clear(); }, 2_000);

        incidents.ingest(
            CausalSignal::Explosion {
                pos: WorldPos::default(),
                weapon: 51,
                attacker,
                vehicle,
                source: CausalSource::Explosion,
            },
            2_500,
            Some(incident_id),
        );

        coord.on_causal_signal(
            CausalSignal::Explosion {
                pos: WorldPos::default(),
                weapon: 51,
                attacker,
                vehicle,
                source: CausalSource::Explosion,
            },
            incident_id,
            &incidents,
            &mut cases,
            |_, out| { out.clear(); },
        );

        let case_id = coord
            .session_for_reporter(reporter)
            .expect("session")
            .case_id
            .expect("case");
        let case = cases.get(case_id).expect("case");
        assert!(case.reported_clues.has_kind(CausalKind::VehiclePropertyDamage));
        assert!(!case.reported_clues.has_kind(CausalKind::Explosion));
    }

    #[test]
    fn report_clues_state_position_before_suspect() {
        let mut clues = ReportClues::default();
        clues.seed_caller_location(WorldPos {
            x: 1.0,
            y: 2.0,
            z: 3.0,
        });
        assert!(clues.reporter_pos.is_some());
        assert!(clues.suspect_pos.is_none());
        clues.note_suspect_location(WorldPos {
            x: 9.0,
            y: 8.0,
            z: 7.0,
        });
        assert!(clues.suspect_pos.is_some());
    }
}