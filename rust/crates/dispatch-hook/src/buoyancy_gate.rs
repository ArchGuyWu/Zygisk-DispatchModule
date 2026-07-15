//! `CPed::ProcessBuoyancy` fail-closed gate (not a sanitizer).
//!
//! Tombstone pattern (fault addr 0x18 / also covered for 0x28):
//!   task = non-null, `*task` (vtable) = null → `ldr x8, [x8, #0x18]`.
//!
//! Uses shared `ped_has_unwalkable_task` (checks vtable +0x18 and +0x28).
//! Policy: never write slots; unwalkable → skip orig.

use dispatch_exec::ped_has_unwalkable_task;

type OrigProcessBuoyancy = unsafe extern "C" fn(*mut std::ffi::c_void);

static mut ORIG_PROCESS_BUOYANCY: Option<OrigProcessBuoyancy> = None;

pub fn set_orig_process_buoyancy(f: OrigProcessBuoyancy) {
    unsafe { ORIG_PROCESS_BUOYANCY = Some(f) };
}

/// Gate only: dangerous task graph → skip orig. No memory writes.
///
/// No `zone_active` bypass: that only disables this check during load/cutscene,
/// when sentinels are most common. `is_plausible_ptr` + 11-slot scan already
/// handles uninitialised slots without needing to call orig blind.
pub unsafe extern "C" fn detour_process_buoyancy(self_: *mut std::ffi::c_void) {
    if self_.is_null() {
        return;
    }
    // Fail-closed: do not pass this ped into the engine walk.
    if ped_has_unwalkable_task(self_ as *const _) {
        return;
    }
    if let Some(orig) = ORIG_PROCESS_BUOYANCY {
        orig(self_);
    }
}
