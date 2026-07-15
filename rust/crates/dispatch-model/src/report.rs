//! Logical report phases (MODEL.md §3–§4.1). No phone/radio anim gates.

/// Civilian ladder; open case only at [`ReportPhase::Connected`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum ReportPhase {
    Seeking,
    Calling,
    Connected,
    Ended,
}

impl ReportPhase {
    pub fn next(self) -> Self {
        match self {
            Self::Seeking => Self::Calling,
            Self::Calling => Self::Connected,
            Self::Connected => Self::Ended,
            Self::Ended => Self::Ended,
        }
    }

    /// Case opens only at Connected (binary; no Partial quality).
    pub fn opens_case(self) -> bool {
        matches!(self, Self::Connected)
    }
}

/// Default civilian phase dwell times (ms). Tunable via [`crate::world::ModelConfig`].
pub const DEFAULT_SEEKING_MS: u64 = 1_500;
pub const DEFAULT_CALLING_MS: u64 = 2_000;

/// In-flight logical report before / around case open.
#[derive(Debug, Clone, PartialEq)]
pub struct PendingReport {
    pub phase: ReportPhase,
    pub kinds: Vec<crate::signal::SignalKind>,
    pub pos: crate::signal::WorldPos,
    pub reporter: crate::signal::ReporterKind,
    pub criminal_count: u32,
    /// When current phase started (ms).
    pub phase_started_ms: u64,
    /// True when Connected and not yet consumed into a case.
    pub ready_to_open: bool,
}

impl PendingReport {
    pub fn from_signal(sig: &crate::signal::Signal, now_ms: u64) -> Self {
        use crate::signal::ReporterKind;
        match sig.reporter {
            ReporterKind::Police => Self {
                phase: ReportPhase::Connected,
                kinds: vec![sig.kind],
                pos: sig.pos,
                reporter: sig.reporter,
                criminal_count: sig.criminal_count,
                phase_started_ms: now_ms,
                ready_to_open: true,
            },
            ReporterKind::Civilian => Self {
                phase: ReportPhase::Seeking,
                kinds: vec![sig.kind],
                pos: sig.pos,
                reporter: sig.reporter,
                criminal_count: sig.criminal_count,
                phase_started_ms: now_ms,
                ready_to_open: false,
            },
        }
    }

    pub fn merge_signal(&mut self, sig: &crate::signal::Signal) {
        if !self.kinds.contains(&sig.kind) {
            self.kinds.push(sig.kind);
        }
        self.criminal_count = self.criminal_count.max(sig.criminal_count);
        // Prefer later position as scene anchor.
        self.pos = sig.pos;
    }

    /// Advance civilian delays; police stays Connected.
    pub fn advance_clock(
        &mut self,
        now_ms: u64,
        seeking_ms: u64,
        calling_ms: u64,
    ) {
        if matches!(self.reporter, crate::signal::ReporterKind::Police) {
            self.phase = ReportPhase::Connected;
            self.ready_to_open = true;
            return;
        }
        if self.phase == ReportPhase::Ended || self.phase == ReportPhase::Connected {
            if self.phase == ReportPhase::Connected {
                self.ready_to_open = true;
            }
            return;
        }
        let dwell = match self.phase {
            ReportPhase::Seeking => seeking_ms,
            ReportPhase::Calling => calling_ms,
            _ => return,
        };
        if now_ms.saturating_sub(self.phase_started_ms) >= dwell {
            self.phase = self.phase.next();
            self.phase_started_ms = now_ms;
            if self.phase == ReportPhase::Connected {
                self.ready_to_open = true;
            }
        }
    }
}
