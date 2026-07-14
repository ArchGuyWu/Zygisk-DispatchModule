//! `CTaskComplexFacial::ControlSubTask` fail-closed gate (not a sanitizer).
//!
//! Tombstone_01 (2026-07-15): fault addr 0x38 at `ControlSubTask` (vtable+0x38 = CreateNextSubTask)
//!   engine ped update loop walks child tasks of CTaskComplexFacial
//!   child task non-null, vtable null → `ldr x8, [x8, #0x38]` / `blr x8`
//!
//! Policy: unwalkable ped task graph → skip orig. Never write task slots.

use dispatch_exec::ped_has_unwalkable_task;

type OrigControlSubTask = unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void);

static mut ORIG_CONTROL_SUB_TASK: Option<OrigControlSubTask> = None;

pub fn set_orig_control_sub_task(f: OrigControlSubTask) {
    unsafe { ORIG_CONTROL_SUB_TASK = Some(f) };
}

/// Gate only: dangerous ped task graph → skip orig. No memory writes.
/// During loading (zone inactive), forward directly — task memory may be uninitialised.
pub unsafe extern "C" fn detour_control_sub_task(
    self_: *mut std::ffi::c_void,
    ped: *mut std::ffi::c_void,
) {
    if !crate::gate::zone_active_cached() {
        if let Some(orig) = ORIG_CONTROL_SUB_TASK { orig(self_, ped); }
        return;
    }
    if ped.is_null() {
        return;
    }
    // Fail-closed: do not pass this ped into the engine walk.
    if ped_has_unwalkable_task(ped as *const _) {
        return;
    }
    if let Some(orig) = ORIG_CONTROL_SUB_TASK {
        orig(self_, ped);
    }
}
