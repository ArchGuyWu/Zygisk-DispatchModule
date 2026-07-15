//! Pure dispatch product model (MODEL.md).
//!
//! SNAPSHOT → COMPUTE → COMMIT. Zero FFI: no engine pointers, no libc/android.
//! Runtime owns `World` and calls [`light_step_tx`] / [`full_dispatch_tx`];
//! engine consumes [`Effect`]s only.

#![forbid(unsafe_code)]

mod case;
mod effects;
mod report;
mod signal;
mod threat;
mod tick;
mod world;

pub use case::{Case, CaseId, Phase};
pub use effects::Effect;
pub use report::{PendingReport, ReportPhase, DEFAULT_CALLING_MS, DEFAULT_SEEKING_MS};
pub use signal::{Despawn, DespawnKind, ReporterKind, Signal, SignalKind, WorldPos};
pub use threat::{
    ems_fire_spawn_budget, needs_from_kinds, needs_from_signal_kind, response_size,
    scene_is_severe, threat_from_kinds, DeptNeeds, ResponseSize, Threat, EMS_FIRE_DEFAULT_PER_CASE,
    EMS_FIRE_VIEW_CAP,
};
pub use tick::{full_dispatch_tx, light_step_tx, should_full_dispatch};
pub use world::{Commit, Inputs, ModelConfig, ViewCounts, World};

#[cfg(test)]
mod tests;
