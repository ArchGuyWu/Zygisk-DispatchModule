//! Department needs, threat, and police ResponseSize (MODEL.md §3–§4).

use crate::signal::SignalKind;

/// Department need flags (not a score API).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DeptNeeds {
    pub police: bool,
    pub ems: bool,
    pub fire: bool,
}

impl DeptNeeds {
    pub fn any(self) -> bool {
        self.police || self.ems || self.fire
    }

    pub fn merge(self, other: Self) -> Self {
        Self {
            police: self.police || other.police,
            ems: self.ems || other.ems,
            fire: self.fire || other.fire,
        }
    }
}

/// Needs from a single signal kind (MODEL.md §4.2).
///
/// | Dept   | Triggered by                          |
/// |--------|----------------------------------------|
/// | Fire   | Fire / explosion                       |
/// | Ems    | Casualty / injury                      |
/// | Police | Violent / illegal (default for most)   |
pub fn needs_from_signal_kind(kind: SignalKind) -> DeptNeeds {
    match kind {
        SignalKind::Fire | SignalKind::Explosion => DeptNeeds {
            police: false,
            ems: matches!(kind, SignalKind::Explosion), // explosion often injures
            fire: true,
        },
        SignalKind::Casualty | SignalKind::Injury => DeptNeeds {
            police: matches!(kind, SignalKind::Casualty), // lethal → police too
            ems: true,
            fire: false,
        },
        SignalKind::Gunfire => DeptNeeds {
            police: true,
            ems: false,
            fire: false,
        },
        SignalKind::PropertyDamage => DeptNeeds {
            police: true,
            ems: false,
            fire: false,
        },
    }
}

/// Aggregate needs over all clue kinds on a case/report.
pub fn needs_from_kinds(kinds: &[SignalKind]) -> DeptNeeds {
    kinds
        .iter()
        .fold(DeptNeeds::default(), |acc, k| acc.merge(needs_from_signal_kind(*k)))
}

/// Coarse police threat ladder (EMS/Fire do **not** use this).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Threat {
    Low,
    Armed,
    ActiveFire,
}

impl Default for Threat {
    fn default() -> Self {
        Self::Low
    }
}

/// Derive threat from accumulated signal kinds.
pub fn threat_from_kinds(kinds: &[SignalKind]) -> Threat {
    let mut threat = Threat::Low;
    for k in kinds {
        let t = match k {
            SignalKind::Gunfire => Threat::ActiveFire,
            SignalKind::Casualty => Threat::Armed,
            SignalKind::Explosion => Threat::Armed,
            SignalKind::Fire => Threat::Low,
            SignalKind::Injury | SignalKind::PropertyDamage => Threat::Low,
        };
        if t > threat {
            threat = t;
        }
    }
    threat
}

/// Scene is "severe" for EMS/Fire reinforcement (view-cap path).
pub fn scene_is_severe(kinds: &[SignalKind], threat: Threat) -> bool {
    if matches!(threat, Threat::ActiveFire | Threat::Armed) {
        return true;
    }
    kinds.iter().any(|k| {
        matches!(
            k,
            SignalKind::Fire | SignalKind::Explosion | SignalKind::Casualty
        )
    })
}

/// Single function of Threat + criminal count (MODEL.md §3 ResponseSize).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResponseSize {
    /// Nearby on-map unit slots to mobilize first.
    pub nearby_slots: u8,
    /// Offscreen patrol vehicles to spawn if nearby insufficient.
    pub patrol_count: u8,
    pub swat: bool,
    pub fbi: bool,
}

impl Default for ResponseSize {
    fn default() -> Self {
        Self {
            nearby_slots: 1,
            patrol_count: 1,
            swat: false,
            fbi: false,
        }
    }
}

/// Multi-offender bar for SWAT/FBI eligibility.
const GANG_MIN: u32 = 3;

/// Pure mapping: Threat + criminal count → nearby slots, patrols, special units.
pub fn response_size(threat: Threat, criminal_count: u32) -> ResponseSize {
    let n = criminal_count;
    let multi = n >= GANG_MIN;
    let pair = n >= 2;

    match threat {
        Threat::ActiveFire => ResponseSize {
            nearby_slots: if pair || multi { 2 } else { 2 },
            patrol_count: if pair { 3 } else { 2 },
            swat: multi || pair,
            fbi: multi || (pair && n >= 2),
        },
        Threat::Armed => ResponseSize {
            nearby_slots: 2,
            patrol_count: if multi { 2 } else { 2 },
            swat: multi,
            fbi: multi,
        },
        Threat::Low => ResponseSize {
            nearby_slots: if multi { 2 } else { 1 },
            patrol_count: if multi { 2 } else { 1 },
            swat: false,
            fbi: false,
        },
    }
}

/// EMS/Fire hard cap: ≤2 of the **same** department in player view/stream.
pub const EMS_FIRE_VIEW_CAP: u8 = 2;

/// Default vehicles per case per EMS/Fire department.
pub const EMS_FIRE_DEFAULT_PER_CASE: u8 = 1;

/// How many EMS or Fire vehicles this case still wants to spawn.
///
/// - Default: 1 per case per dept.
/// - Severe: may go to 2, but never past `view_count` room under [`EMS_FIRE_VIEW_CAP`].
pub fn ems_fire_spawn_budget(
    already_spawned: u8,
    severe: bool,
    view_count: u8,
) -> u8 {
    let target = if severe {
        EMS_FIRE_VIEW_CAP
    } else {
        EMS_FIRE_DEFAULT_PER_CASE
    };
    let remaining_for_case = target.saturating_sub(already_spawned);
    let room_in_view = EMS_FIRE_VIEW_CAP.saturating_sub(view_count);
    remaining_for_case.min(room_in_view)
}
