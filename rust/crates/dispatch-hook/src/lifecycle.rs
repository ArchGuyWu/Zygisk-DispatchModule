use dispatch_core::DespawnReason;

use crate::gate::hook_logic_allowed;
use crate::orig_slot::OrigSlot;
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

static ORIG_REGISTER_KILL: OrigSlot<OrigRegisterKill> = OrigSlot::new();
static ORIG_REMOVE_PED: OrigSlot<OrigRemovePed> = OrigSlot::new();
static ORIG_REMOVE_VEHICLE: OrigSlot<OrigRemoveVehicle> = OrigSlot::new();

pub fn set_orig_register_kill(f: OrigRegisterKill) {
    ORIG_REGISTER_KILL.set(f);
}

pub fn set_orig_remove_ped(f: OrigRemovePed) {
    ORIG_REMOVE_PED.set(f);
}

pub fn set_orig_remove_vehicle(f: OrigRemoveVehicle) {
    ORIG_REMOVE_VEHICLE.set(f);
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
    if hook_logic_allowed() {
        with_runtime(|rt| {
            if rt.symbols.validate_pool_ped(dead).is_some() {
                rt.publish_casualty(dead, killer, weapon);
                rt.mark_kill_pending(dead);
            }
        });
    }
    if let Some(orig) = ORIG_REGISTER_KILL.get() {
        orig(this, dead, killer, weapon, unk);
    }
}

/// Engine pool recycle: registry release only.
pub unsafe extern "C" fn detour_population_remove_ped(ped: *mut std::ffi::c_void) {
    if hook_logic_allowed() {
        with_runtime(|rt| {
            if rt.symbols.validate_pool_ped(ped as *const _).is_some() {
                rt.release_ped_from_pool(ped as *const _);
            }
        });
    }
    if let Some(orig) = ORIG_REMOVE_PED.get() {
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
    if let Some(orig) = ORIG_REMOVE_VEHICLE.get() {
        orig(vehicle);
    }
}
