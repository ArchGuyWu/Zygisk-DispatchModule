use dispatch_core::DespawnReason;

use crate::gate::hook_logic_allowed;
use crate::runtime::with_runtime;

type OrigRegisterKill = unsafe extern "C" fn(
    *mut std::ffi::c_void,
    *const std::ffi::c_void,
    *const std::ffi::c_void,
    i32,
    bool,
);
type OrigRemovePed = unsafe extern "C" fn(*mut std::ffi::c_void);
type OrigRemoveVehicle = unsafe extern "C" fn(*mut std::ffi::c_void);

static mut ORIG_REGISTER_KILL: Option<OrigRegisterKill> = None;
static mut ORIG_REMOVE_PED: Option<OrigRemovePed> = None;
static mut ORIG_REMOVE_VEHICLE: Option<OrigRemoveVehicle> = None;

pub fn set_orig_register_kill(f: OrigRegisterKill) {
    unsafe { ORIG_REGISTER_KILL = Some(f) };
}

pub fn set_orig_remove_ped(f: OrigRemovePed) {
    unsafe { ORIG_REMOVE_PED = Some(f) };
}

pub fn set_orig_remove_vehicle(f: OrigRemoveVehicle) {
    unsafe { ORIG_REMOVE_VEHICLE = Some(f) };
}

/// Engine death registration.
///
/// Policy: do not clean up / destroy engine task objects here — the engine
/// owns ped death teardown and will clear tasks during its own death sequence.
/// Premature ClearTasks from our hook creates dangling CTaskManager slots
/// (non-null pointer, null vtable) that crash subsequent task-graph walkers.
/// We limit ourselves to causal ingest and ped tracking.
pub unsafe extern "C" fn detour_register_kill(
    this: *mut std::ffi::c_void,
    dead: *const std::ffi::c_void,
    killer: *const std::ffi::c_void,
    weapon: i32,
    unk: bool,
) {
    // Do not call engine ClearTasks here — engine owns ped death task
    // teardown and will clean up tasks during its own death sequence.
    // Premature cleanup creates dangling task slots (non-null pointer, null
    // vtable) that crash subsequent task-graph walkers (ManageTasks,
    // FindActiveTaskByType, ProcessBuoyancy, etc.).
    if hook_logic_allowed() {
        with_runtime(|rt| {
            if rt.symbols.validate_pool_ped(dead).is_some() {
                rt.publish_casualty(dead, killer, weapon);
                rt.mark_kill_pending(dead);
            }
        });
    }
    if let Some(orig) = ORIG_REGISTER_KILL {
        orig(this, dead, killer, weapon, unk);
    }
}

/// Engine pool recycle: registry release only. Do not call engine ClearTasks
/// — the engine owns ped death teardown and will clean up tasks during its
/// own recycling sequence. Premature cleanup creates dangling task slots.
/// Fail-closed: validate pool membership before releasing from our registry.
pub unsafe extern "C" fn detour_population_remove_ped(ped: *mut std::ffi::c_void) {
    if hook_logic_allowed() {
        with_runtime(|rt| {
            if rt.symbols.validate_pool_ped(ped as *const _).is_some() {
                rt.release_ped_from_pool(ped as *const _);
            }
        });
    }
    if let Some(orig) = ORIG_REMOVE_PED {
        orig(ped);
    }
}

pub unsafe extern "C" fn detour_possibly_remove_vehicle(vehicle: *mut std::ffi::c_void) {
    if hook_logic_allowed() {
        with_runtime(|rt| {
            if rt.symbols.validate_pool_vehicle(vehicle as *const _).is_some() {
                rt.despawn_vehicle(vehicle as *const _, DespawnReason::PoolRecycle);
            }
        });
    }
    if let Some(orig) = ORIG_REMOVE_VEHICLE {
        orig(vehicle);
    }
}