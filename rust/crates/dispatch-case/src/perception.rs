use dispatch_core::{CausalKind, CausalSignal, WorldPos};

use crate::engine_event_map::NATIVE_EVENT_KINDS;
use crate::weapon::{classify_weapon, is_audible_weapon};

pub const WITNESS_HEARD_RANGE_M: f32 = 80.0;
pub const WITNESS_SAW_RANGE_M: f32 = 45.0;

/// Causal kinds the engine already delivers to peds via `CEventGroup::Add`.
pub fn native_perception_covers(kind: CausalKind) -> bool {
    NATIVE_EVENT_KINDS.iter().any(|entry| entry.kind == kind)
}

/// Geometric scan only fills gaps native AV events do not cover.
/// `CrimeReported` is vanilla player-only — never ingested or supplemented.
pub fn needs_geometric_supplement(kind: CausalKind) -> bool {
    matches!(
        kind,
        CausalKind::Explosion | CausalKind::VehiclePropertyDamage
    )
}

pub fn kind_is_visual(kind: CausalKind) -> bool {
    matches!(
        kind,
        CausalKind::VehiclePropertyDamage
            | CausalKind::VehicleBurning
            | CausalKind::PedInjury
            | CausalKind::PedCasualty
            | CausalKind::Explosion
            | CausalKind::FireOutbreak
    )
}

pub fn witness_ranges_for(signal: CausalSignal) -> (f32, f32) {
    let kind = signal.kind();
    let weapon = match signal {
        CausalSignal::WeaponDischarge { weapon, .. }
        | CausalSignal::PedInjury { weapon, .. }
        | CausalSignal::PedCasualty { weapon, .. }
        | CausalSignal::Explosion { weapon, .. } => weapon,
        _ => 0,
    };
    let class = classify_weapon(weapon);
    let heard = if is_audible_weapon(class) || kind_is_visual(kind) {
        WITNESS_HEARD_RANGE_M
    } else {
        WITNESS_HEARD_RANGE_M * 0.5
    };
    let saw = if kind_is_visual(kind) {
        WITNESS_SAW_RANGE_M
    } else {
        WITNESS_SAW_RANGE_M * 0.6
    };
    (heard, saw)
}

pub fn anchor_for_signal(signal: CausalSignal) -> WorldPos {
    crate::incident::signal_pos(signal)
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_core::CausalKind;

    #[test]
    fn explosion_needs_geometric_supplement() {
        assert!(needs_geometric_supplement(CausalKind::Explosion));
        assert!(needs_geometric_supplement(CausalKind::VehiclePropertyDamage));
    }

    #[test]
    fn gunshot_is_native_not_geometric() {
        assert!(native_perception_covers(CausalKind::WeaponDischarge));
        assert!(!needs_geometric_supplement(CausalKind::WeaponDischarge));
    }
}