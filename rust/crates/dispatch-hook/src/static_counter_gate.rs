//! `CPedIntelligence::ProcessStaticCounter` fail-closed gate (not a sanitizer).
//!
//! Tombstone_01 (2026-07-14): fault addr 0x18 at `ProcessStaticCounter+…` (0x55de5f4)
//!   `this` = CPedIntelligence*
//!   scans first non-null among [intel+0x8 .. +0x28]
//!   task non-null, vtable null → `ldr x8, [x8, #0x18]` / `blr x8`
//!
//! Policy: unwalkable task graph → skip orig. Never write task slots.

use dispatch_exec::intelligence_has_unwalkable_task;

type OrigProcessStaticCounter = unsafe extern "C" fn(*mut std::ffi::c_void);

static mut ORIG_PROCESS_STATIC_COUNTER: Option<OrigProcessStaticCounter> = None;

pub fn set_orig_process_static_counter(f: OrigProcessStaticCounter) {
    unsafe { ORIG_PROCESS_STATIC_COUNTER = Some(f) };
}

/// Gate only: dangerous intelligence task graph → skip orig. No memory writes.
///
/// No `zone_active` bypass (same rationale as buoyancy / ScanForEvents gates).
pub unsafe extern "C" fn detour_process_static_counter(self_: *mut std::ffi::c_void) {
    if self_.is_null() {
        return;
    }
    // `self` is CPedIntelligence*, not CPed.
    if intelligence_has_unwalkable_task(self_ as *const _) {
        return;
    }
    if let Some(orig) = ORIG_PROCESS_STATIC_COUNTER {
        orig(self_);
    }
}
