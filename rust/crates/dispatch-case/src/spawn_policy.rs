//! Vanilla EMS model IDs and spawn-hook symbols.
//!
//! Interception lives in `dispatch-hook/spawn.rs` (zone gate + custom-spawn bypass).
//! Mission/script compatibility is handled by separate gate logic, not per-case business rules.

pub const MODEL_AMBULANCE: u32 = 416;
pub const MODEL_FIRETRUCK: u32 = 407;

/// Vanilla EMS only; police spawns are suppressed by wanted hooks, not spawn hooks.
pub const NATIVE_SPAWN_BLOCK_MODELS: &[&str] = &["ambulance", "firetruck"];

pub const SPAWN_HOOKS: &[&str] = &[
    "_ZN8CCarCtrl31GenerateOneEmergencyServicesCarEj7CVector",
    "_ZN8CCarCtrl37ScriptGenerateOneEmergencyServicesCarEj7CVector",
    "_ZN8CCarCtrl18CreateCarForScriptEi7CVectorh",
];

pub fn is_native_ems_emergency_model(model: u32) -> bool {
    model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK
}