//! World snapshot + frame inputs (MODEL.md §5).

use crate::case::{Case, CaseId};
use crate::effects::Effect;
use crate::report::{PendingReport, DEFAULT_CALLING_MS, DEFAULT_SEEKING_MS};
use crate::signal::{Despawn, Signal};

/// Tunables that are product timing, not engine constants.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ModelConfig {
    pub seeking_ms: u64,
    pub calling_ms: u64,
    /// Minimum spacing between FullDispatch commits for a case (ms).
    pub full_interval_ms: u64,
    /// OnScene attack evaluation interval (ms).
    pub attack_interval_ms: u64,
    /// OnScene arrest evaluation interval (ms).
    pub arrest_interval_ms: u64,
    /// Open → Responding delay before first mobilize/spawn (ms). 0 = immediate.
    pub open_delay_ms: u64,
}

impl Default for ModelConfig {
    fn default() -> Self {
        Self {
            seeking_ms: DEFAULT_SEEKING_MS,
            calling_ms: DEFAULT_CALLING_MS,
            full_interval_ms: 500,
            attack_interval_ms: 2_000,
            arrest_interval_ms: 3_000,
            open_delay_ms: 0,
        }
    }
}

/// Counts of same-dept emergency vehicles currently in player view / stream.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ViewCounts {
    pub ems: u8,
    pub fire: u8,
}

/// Pure model state. Holds ids only — never raw engine pointers.
#[derive(Debug, Clone, PartialEq)]
pub struct World {
    pub cases: Vec<Case>,
    pub pending_reports: Vec<PendingReport>,
    pub next_case_id: u64,
    pub config: ModelConfig,
    /// Wall-clock of last FullDispatch commit (any case).
    pub last_full_ms: u64,
}

impl Default for World {
    fn default() -> Self {
        Self::new(ModelConfig::default())
    }
}

impl World {
    pub fn new(config: ModelConfig) -> Self {
        Self {
            cases: Vec::new(),
            pending_reports: Vec::new(),
            next_case_id: 1,
            config,
            last_full_ms: 0,
        }
    }

    pub fn alloc_case_id(&mut self) -> CaseId {
        let id = CaseId(self.next_case_id);
        self.next_case_id = self.next_case_id.saturating_add(1);
        id
    }

    pub fn active_police_cases(&self) -> bool {
        self.cases.iter().any(|c| {
            c.needs.police && !matches!(c.phase, crate::case::Phase::Done)
        })
    }
}

/// Frame inputs drained from runtime queues.
#[derive(Debug, Clone, PartialEq)]
pub struct Inputs {
    pub signals: Vec<Signal>,
    pub despawns: Vec<Despawn>,
    pub now_ms: u64,
    pub view_counts: ViewCounts,
}

impl Inputs {
    pub fn empty(now_ms: u64) -> Self {
        Self {
            signals: Vec::new(),
            despawns: Vec::new(),
            now_ms,
            view_counts: ViewCounts::default(),
        }
    }
}

/// Single commit: World' + Effects (coherence over per-frame freshness).
#[derive(Debug, Clone, PartialEq)]
pub struct Commit {
    pub world: World,
    pub effects: Vec<Effect>,
}
