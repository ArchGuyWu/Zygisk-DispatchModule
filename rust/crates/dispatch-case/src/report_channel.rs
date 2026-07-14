use dispatch_core::{CausalKind, EntityRef, WorldPos};

/// Dispatch urgency derived from what the caller actually conveyed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
pub enum ReportSeverity {
    #[default]
    Normal = 0,
    /// Gunfire, explosion, or armed assault — shorter post-report dispatch wait only.
    High = 1,
    /// Call dropped before completion; partial info still actionable.
    Interrupted = 2,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ReportClues {
    /// Always stated first: where the caller is.
    pub reporter_pos: Option<WorldPos>,
    /// Only if the caller saw the suspect before the line dropped.
    pub suspect_pos: Option<WorldPos>,
    pub kinds: u8,
    pub entities: Vec<EntityRef>,
    /// Ped perpetrators the caller identified — not victims or scenery props.
    pub perpetrators: Vec<EntityRef>,
    pub partial: bool,
    pub severity: ReportSeverity,
}

impl ReportClues {
    pub fn has_kind(&self, kind: CausalKind) -> bool {
        (self.kinds & (1 << kind as u8)) != 0
    }

    pub fn seed_caller_location(&mut self, reporter_pos: WorldPos) {
        self.reporter_pos = Some(reporter_pos);
    }

    pub fn note_suspect_location(&mut self, suspect_pos: WorldPos) {
        self.suspect_pos = Some(suspect_pos);
    }

    pub fn mark_interrupted(&mut self) {
        self.partial = true;
        self.severity = ReportSeverity::Interrupted;
    }

    /// Recompute urgency from collected clues (does not downgrade Interrupted).
    pub fn refresh_severity(&mut self) {
        if self.severity == ReportSeverity::Interrupted {
            return;
        }
        let high = self.has_kind(CausalKind::WeaponDischarge)
            || self.has_kind(CausalKind::Explosion)
            || (self.has_kind(CausalKind::PedCasualty) && !self.perpetrators.is_empty());
        self.severity = if high {
            ReportSeverity::High
        } else {
            ReportSeverity::Normal
        };
    }

    /// Only clues the caller had perceived, not the full latent incident.
    pub fn absorb_perceived(&mut self, perceived_kinds: u8, perceived_entities: &[EntityRef]) {
        self.kinds |= perceived_kinds;
        self.merge_entities(perceived_entities);
    }

    pub fn merge_perpetrators(&mut self, perpetrators: &[EntityRef]) {
        for entity in perpetrators {
            if self.perpetrators.len() >= 8 {
                break;
            }
            if self.perpetrators.contains(entity) {
                continue;
            }
            self.perpetrators.push(*entity);
        }
    }

    fn merge_entities(&mut self, entities: &[EntityRef]) {
        for entity in entities {
            if self.entities.len() >= 16 {
                break;
            }
            if self.entities.contains(entity) {
                continue;
            }
            self.entities.push(*entity);
        }
    }

    pub fn actionable(&self) -> bool {
        self.reporter_pos.is_some() || self.kinds != 0 || self.suspect_pos.is_some()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_core::CausalKind;

    #[test]
    fn gunfire_clues_mark_high_priority() {
        let mut clues = ReportClues::default();
        clues.kinds = 1 << CausalKind::WeaponDischarge as u8;
        clues.refresh_severity();
        assert_eq!(clues.severity, ReportSeverity::High);
    }

    #[test]
    fn interrupted_severity_is_not_downgraded() {
        let mut clues = ReportClues::default();
        clues.mark_interrupted();
        clues.kinds = 1 << CausalKind::WeaponDischarge as u8;
        clues.refresh_severity();
        assert_eq!(clues.severity, ReportSeverity::Interrupted);
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReportChannel {
    Cellphone,
}

/// Driven by engine tasks (`phone_in` / `phone_talk` / `phone_out`), not timers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReportPhase {
    /// Elected reporter; engine should start dial task.
    Approaching,
    /// `phone_in` animation started.
    Dialing,
    /// `phone_talk` — clues accumulate and case is open.
    Active,
    /// Task finished (`phone_out` / hang up).
    Ended,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReportEndReason {
    TaskCompleted,
    ReporterLost,
}