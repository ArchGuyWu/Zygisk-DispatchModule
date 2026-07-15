//! Case lifecycle (MODEL.md §3 Phase).

use crate::threat::{DeptNeeds, ResponseSize, Threat};

/// Opaque case id assigned by the model World.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct CaseId(pub u64);

/// Case phase: Open → Responding → OnScene → Done.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    Open,
    Responding,
    OnScene,
    Done,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Case {
    pub id: CaseId,
    pub phase: Phase,
    pub needs: DeptNeeds,
    pub threat: Threat,
    pub response_size: ResponseSize,
    pub kinds: Vec<crate::signal::SignalKind>,
    pub pos: crate::signal::WorldPos,
    pub criminal_count: u32,
    pub opened_ms: u64,
    pub last_full_ms: u64,
    /// LightStep may set when open-delay / OnScene interval is due.
    pub ready_for_full: bool,
    pub should_close: bool,
    /// How many ambulances this case has already ordered.
    pub ems_spawned: u8,
    /// How many firetrucks this case has already ordered.
    pub fire_spawned: u8,
    /// Police patrols already ordered (spawn effects).
    pub patrol_spawned: u8,
    pub swat_spawned: bool,
    pub fbi_spawned: bool,
    pub mobilized: bool,
    pub last_attack_ms: u64,
    pub last_arrest_ms: u64,
}

impl Case {
    pub fn new(
        id: CaseId,
        kinds: Vec<crate::signal::SignalKind>,
        pos: crate::signal::WorldPos,
        criminal_count: u32,
        now_ms: u64,
    ) -> Self {
        let needs = crate::threat::needs_from_kinds(&kinds);
        let threat = crate::threat::threat_from_kinds(&kinds);
        let response_size = crate::threat::response_size(threat, criminal_count);
        Self {
            id,
            phase: Phase::Open,
            needs,
            threat,
            response_size,
            kinds,
            pos,
            criminal_count,
            opened_ms: now_ms,
            last_full_ms: now_ms,
            ready_for_full: false,
            should_close: false,
            ems_spawned: 0,
            fire_spawned: 0,
            patrol_spawned: 0,
            swat_spawned: false,
            fbi_spawned: false,
            mobilized: false,
            last_attack_ms: 0,
            last_arrest_ms: 0,
        }
    }

    pub fn recompute_size_and_needs(&mut self) {
        self.needs = crate::threat::needs_from_kinds(&self.kinds);
        self.threat = crate::threat::threat_from_kinds(&self.kinds);
        self.response_size = crate::threat::response_size(self.threat, self.criminal_count);
    }

    pub fn merge_kinds(&mut self, kinds: &[crate::signal::SignalKind], criminal_count: u32) {
        for k in kinds {
            if !self.kinds.contains(k) {
                self.kinds.push(*k);
            }
        }
        self.criminal_count = self.criminal_count.max(criminal_count);
        self.recompute_size_and_needs();
    }
}
