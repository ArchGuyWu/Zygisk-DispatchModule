use dispatch_case::is_native_ems_emergency_model;
use dispatch_engine::CVector;
use dispatch_exec::is_custom_spawn_active;

use crate::gate::hook_logic_allowed;
use crate::orig_slot::OrigSlot;

type OrigGenerateEmergency = unsafe extern "C" fn(model: u32, pos: CVector);
type OrigScriptGenerateEmergency = unsafe extern "C" fn(model: u32, pos: CVector);
type OrigCreateCarForScript =
    unsafe extern "C" fn(model: i32, pos: CVector, created_by: u8) -> *mut std::ffi::c_void;

static ORIG_GENERATE_EMERGENCY: OrigSlot<OrigGenerateEmergency> = OrigSlot::new();
static ORIG_SCRIPT_GENERATE_EMERGENCY: OrigSlot<OrigScriptGenerateEmergency> = OrigSlot::new();
static ORIG_CREATE_CAR_FOR_SCRIPT: OrigSlot<OrigCreateCarForScript> = OrigSlot::new();

pub fn set_orig_generate_emergency(f: OrigGenerateEmergency) {
    ORIG_GENERATE_EMERGENCY.set(f);
}

pub fn set_orig_script_generate_emergency(f: OrigScriptGenerateEmergency) {
    ORIG_SCRIPT_GENERATE_EMERGENCY.set(f);
}

pub fn set_orig_create_car_for_script(f: OrigCreateCarForScript) {
    ORIG_CREATE_CAR_FOR_SCRIPT.set(f);
}

/// Zone gate: block vanilla ambulance / firetruck while mod dispatch is active.
fn should_block_native_ems_spawn(model: u32) -> bool {
    if !is_native_ems_emergency_model(model) {
        return false;
    }
    if is_custom_spawn_active() {
        return false;
    }
    hook_logic_allowed()
}

unsafe fn forward_vanilla_ems_spawn(model: u32, pos: CVector, script_path: bool) {
    if should_block_native_ems_spawn(model) {
        return;
    }
    if script_path {
        if let Some(orig) = ORIG_SCRIPT_GENERATE_EMERGENCY.get() {
            orig(model, pos);
        }
    } else if let Some(orig) = ORIG_GENERATE_EMERGENCY.get() {
        orig(model, pos);
    }
}

pub unsafe extern "C" fn detour_generate_emergency_car(model: u32, pos: CVector) {
    forward_vanilla_ems_spawn(model, pos, false);
}

pub unsafe extern "C" fn detour_script_generate_emergency_car(model: u32, pos: CVector) {
    forward_vanilla_ems_spawn(model, pos, true);
}

pub unsafe extern "C" fn detour_create_car_for_script(
    model: i32,
    pos: CVector,
    created_by: u8,
) -> *mut std::ffi::c_void {
    if is_custom_spawn_active() {
        if let Some(orig) = ORIG_CREATE_CAR_FOR_SCRIPT.get() {
            return orig(model, pos, created_by);
        }
        return std::ptr::null_mut();
    }
    if should_block_native_ems_spawn(model as u32) {
        return std::ptr::null_mut();
    }
    if let Some(orig) = ORIG_CREATE_CAR_FOR_SCRIPT.get() {
        orig(model, pos, created_by)
    } else {
        std::ptr::null_mut()
    }
}