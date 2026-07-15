//! Thin adapter: map `dispatch_model::Effect` → engine-facing [`EffectSink`].
//!
//! MODEL.md §1/§6: model produces intent only; engine `apply` consumes Effects.
//! This module is pure dispatch over a sink so host unit tests never need libUE4.
//! Full wiring into [`crate::game::ExecEnv`] / spawn / attack / arrest is deferred
//! (PR4 hooks + runtime); stubs here keep the surface stable and fail-closed.

use dispatch_model::{CaseId, Effect};

/// Case ids assigned by the model start at 1; zero is never valid.
#[inline]
pub fn is_valid_case_id(id: CaseId) -> bool {
    id.0 != 0
}

/// Engine / mock surface for model Effects. One method per [`Effect`] variant.
///
/// Implement this for real apply (ExecEnv + symbols) or for tests (recording sink).
/// Callers must not panic on missing engine state — skip fail-closed instead.
pub trait EffectSink {
    /// Case opened at Connected — bookkeeping / presentation if any.
    fn open_case(&mut self, case_id: CaseId);

    /// Case finished — stand-down / GC path.
    fn close_case(&mut self, case_id: CaseId);

    /// Task existing nearby units toward the scene (cap from ResponseSize).
    fn mobilize_nearby(&mut self, case_id: CaseId, cap: u8);

    /// Spawn police patrol response vehicle.
    fn spawn_patrol(&mut self, case_id: CaseId);

    /// Spawn SWAT when ResponseSize says so.
    fn spawn_swat(&mut self, case_id: CaseId);

    /// Spawn FBI when ResponseSize says so.
    fn spawn_fbi(&mut self, case_id: CaseId);

    /// Spawn EMS ambulance (view-cap enforced by model before emit).
    fn spawn_ambulance(&mut self, case_id: CaseId);

    /// Spawn firetruck (view-cap enforced by model before emit).
    fn spawn_firetruck(&mut self, case_id: CaseId);

    /// Reinforce wanted suppression while police cases are active.
    fn suppress_wanted(&mut self);

    /// OnScene police attack evaluation / tasking.
    fn attack_pass(&mut self, case_id: CaseId);

    /// OnScene police arrest evaluation / tasking.
    fn arrest_pass(&mut self, case_id: CaseId);
}

/// No-op sink: host tests and pre-wire paths that must not touch the game.
#[derive(Debug, Default, Clone, Copy)]
pub struct NoopEffectSink;

impl EffectSink for NoopEffectSink {
    fn open_case(&mut self, _case_id: CaseId) {}
    fn close_case(&mut self, _case_id: CaseId) {}
    fn mobilize_nearby(&mut self, _case_id: CaseId, _cap: u8) {}
    fn spawn_patrol(&mut self, _case_id: CaseId) {}
    fn spawn_swat(&mut self, _case_id: CaseId) {}
    fn spawn_fbi(&mut self, _case_id: CaseId) {}
    fn spawn_ambulance(&mut self, _case_id: CaseId) {}
    fn spawn_firetruck(&mut self, _case_id: CaseId) {}
    fn suppress_wanted(&mut self) {}
    fn attack_pass(&mut self, _case_id: CaseId) {}
    fn arrest_pass(&mut self, _case_id: CaseId) {}
}

/// Apply a slice of model Effects to `sink`.
///
/// - Case-scoped effects with [`CaseId`]`(0)` are **skipped** (fail-closed).
/// - Empty slices are a no-op.
/// - Order is preserved (model commit order).
pub fn apply_effects(sink: &mut impl EffectSink, effects: &[Effect]) {
    for effect in effects {
        apply_one(sink, effect);
    }
}

fn apply_one(sink: &mut impl EffectSink, effect: &Effect) {
    match effect {
        Effect::OpenCase { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip OpenCase: invalid case id");
                return;
            }
            sink.open_case(*case_id);
        }
        Effect::CloseCase { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip CloseCase: invalid case id");
                return;
            }
            sink.close_case(*case_id);
        }
        Effect::MobilizeNearby { case_id, cap } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip MobilizeNearby: invalid case id");
                return;
            }
            sink.mobilize_nearby(*case_id, *cap);
        }
        Effect::SpawnPatrol { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip SpawnPatrol: invalid case id");
                return;
            }
            sink.spawn_patrol(*case_id);
        }
        Effect::SpawnSwat { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip SpawnSwat: invalid case id");
                return;
            }
            sink.spawn_swat(*case_id);
        }
        Effect::SpawnFbi { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip SpawnFbi: invalid case id");
                return;
            }
            sink.spawn_fbi(*case_id);
        }
        Effect::SpawnAmbulance { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip SpawnAmbulance: invalid case id");
                return;
            }
            sink.spawn_ambulance(*case_id);
        }
        Effect::SpawnFiretruck { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip SpawnFiretruck: invalid case id");
                return;
            }
            sink.spawn_firetruck(*case_id);
        }
        Effect::SuppressWanted => {
            sink.suppress_wanted();
        }
        Effect::AttackPass { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip AttackPass: invalid case id");
                return;
            }
            sink.attack_pass(*case_id);
        }
        Effect::ArrestPass { case_id } => {
            if !is_valid_case_id(*case_id) {
                tracing::debug!(?case_id, "skip ArrestPass: invalid case id");
                return;
            }
            sink.arrest_pass(*case_id);
        }
    }
}

/// Best-effort placeholder sink that logs intent only.
///
/// Real spawn/attack/arrest/wanted apply stays in existing exec modules and will
/// be bound from runtime (PR4) once case-id mapping (model ↔ slotmap) is ready.
#[derive(Debug, Default, Clone, Copy)]
pub struct TracingEffectSink;

impl EffectSink for TracingEffectSink {
    fn open_case(&mut self, case_id: CaseId) {
        // TODO(PR4): map model CaseId → exec case bookkeeping if needed.
        tracing::debug!(?case_id, "effect OpenCase (stub)");
    }

    fn close_case(&mut self, case_id: CaseId) {
        // TODO(PR4): stand-down bound units / GC via coordinator.
        tracing::debug!(?case_id, "effect CloseCase (stub)");
    }

    fn mobilize_nearby(&mut self, case_id: CaseId, cap: u8) {
        // TODO(PR4): dispatch_nearby_available_cops_* with model case anchor.
        tracing::debug!(?case_id, cap, "effect MobilizeNearby (stub)");
    }

    fn spawn_patrol(&mut self, case_id: CaseId) {
        // TODO(PR4): schedule_police_vehicle_spawns / dispatch_spawn_emergency_vehicle patrol.
        tracing::debug!(?case_id, "effect SpawnPatrol (stub)");
    }

    fn spawn_swat(&mut self, case_id: CaseId) {
        // TODO(PR4): SWAT unit via PoliceSpawnUnit plan.
        tracing::debug!(?case_id, "effect SpawnSwat (stub)");
    }

    fn spawn_fbi(&mut self, case_id: CaseId) {
        // TODO(PR4): FBI unit via PoliceSpawnUnit plan.
        tracing::debug!(?case_id, "effect SpawnFbi (stub)");
    }

    fn spawn_ambulance(&mut self, case_id: CaseId) {
        // TODO(PR4): dispatch_spawn_emergency_vehicle ambulance + occupants.
        tracing::debug!(?case_id, "effect SpawnAmbulance (stub)");
    }

    fn spawn_firetruck(&mut self, case_id: CaseId) {
        // TODO(PR4): dispatch_spawn_emergency_vehicle firetruck + occupants.
        tracing::debug!(?case_id, "effect SpawnFiretruck (stub)");
    }

    fn suppress_wanted(&mut self) {
        // TODO(PR4): reinforce wanted-population suppress flag (or policy hook owns fully).
        tracing::debug!("effect SuppressWanted (stub)");
    }

    fn attack_pass(&mut self, case_id: CaseId) {
        // TODO(PR4): make_cops_attack_criminal / attack_foot|vehicle for case.
        tracing::debug!(?case_id, "effect AttackPass (stub)");
    }

    fn arrest_pass(&mut self, case_id: CaseId) {
        // TODO(PR4): try_dispatch_arrest for case criminals.
        tracing::debug!(?case_id, "effect ArrestPass (stub)");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_model::Effect;

    /// Records every successful sink call for assertions.
    #[derive(Debug, Default)]
    struct RecordingSink {
        log: Vec<&'static str>,
        cases: Vec<CaseId>,
        last_cap: Option<u8>,
    }

    impl EffectSink for RecordingSink {
        fn open_case(&mut self, case_id: CaseId) {
            self.log.push("open_case");
            self.cases.push(case_id);
        }
        fn close_case(&mut self, case_id: CaseId) {
            self.log.push("close_case");
            self.cases.push(case_id);
        }
        fn mobilize_nearby(&mut self, case_id: CaseId, cap: u8) {
            self.log.push("mobilize_nearby");
            self.cases.push(case_id);
            self.last_cap = Some(cap);
        }
        fn spawn_patrol(&mut self, case_id: CaseId) {
            self.log.push("spawn_patrol");
            self.cases.push(case_id);
        }
        fn spawn_swat(&mut self, case_id: CaseId) {
            self.log.push("spawn_swat");
            self.cases.push(case_id);
        }
        fn spawn_fbi(&mut self, case_id: CaseId) {
            self.log.push("spawn_fbi");
            self.cases.push(case_id);
        }
        fn spawn_ambulance(&mut self, case_id: CaseId) {
            self.log.push("spawn_ambulance");
            self.cases.push(case_id);
        }
        fn spawn_firetruck(&mut self, case_id: CaseId) {
            self.log.push("spawn_firetruck");
            self.cases.push(case_id);
        }
        fn suppress_wanted(&mut self) {
            self.log.push("suppress_wanted");
        }
        fn attack_pass(&mut self, case_id: CaseId) {
            self.log.push("attack_pass");
            self.cases.push(case_id);
        }
        fn arrest_pass(&mut self, case_id: CaseId) {
            self.log.push("arrest_pass");
            self.cases.push(case_id);
        }
    }

    #[test]
    fn apply_empty_effects_is_noop() {
        let mut sink = RecordingSink::default();
        apply_effects(&mut sink, &[]);
        assert!(sink.log.is_empty());
        apply_effects(&mut NoopEffectSink, &[]);
    }

    #[test]
    fn mock_sink_receives_spawn_ambulance_and_peers() {
        let case = CaseId(7);
        let effects = [
            Effect::OpenCase { case_id: case },
            Effect::MobilizeNearby {
                case_id: case,
                cap: 3,
            },
            Effect::SpawnPatrol { case_id: case },
            Effect::SpawnAmbulance { case_id: case },
            Effect::SpawnFiretruck { case_id: case },
            Effect::SpawnSwat { case_id: case },
            Effect::SpawnFbi { case_id: case },
            Effect::SuppressWanted,
            Effect::AttackPass { case_id: case },
            Effect::ArrestPass { case_id: case },
            Effect::CloseCase { case_id: case },
        ];

        let mut sink = RecordingSink::default();
        apply_effects(&mut sink, &effects);

        assert_eq!(
            sink.log,
            [
                "open_case",
                "mobilize_nearby",
                "spawn_patrol",
                "spawn_ambulance",
                "spawn_firetruck",
                "spawn_swat",
                "spawn_fbi",
                "suppress_wanted",
                "attack_pass",
                "arrest_pass",
                "close_case",
            ]
        );
        assert_eq!(sink.last_cap, Some(3));
        assert!(sink.cases.iter().all(|c| *c == case));
        assert!(sink.log.contains(&"spawn_ambulance"));
    }

    #[test]
    fn invalid_case_id_is_skipped_fail_closed() {
        let bad = CaseId(0);
        let good = CaseId(1);
        let effects = [
            Effect::SpawnAmbulance { case_id: bad },
            Effect::OpenCase { case_id: bad },
            Effect::AttackPass { case_id: bad },
            Effect::SuppressWanted, // no case id — still applied
            Effect::SpawnPatrol { case_id: good },
        ];

        let mut sink = RecordingSink::default();
        apply_effects(&mut sink, &effects);

        assert_eq!(sink.log, ["suppress_wanted", "spawn_patrol"]);
        assert_eq!(sink.cases, [good]);
    }

    #[test]
    fn tracing_stub_sink_does_not_panic() {
        let effects = [
            Effect::SpawnAmbulance { case_id: CaseId(2) },
            Effect::SuppressWanted,
            Effect::ArrestPass {
                case_id: CaseId(2),
            },
        ];
        let mut sink = TracingEffectSink;
        apply_effects(&mut sink, &effects);
    }
}
