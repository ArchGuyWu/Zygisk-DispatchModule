//! `CEventScanner::ScanForEvents` fail-closed gate (not a sanitizer).
//!
//! Tombstone_00 (2026-07-14): fault addr 0x28 at `ScanForEvents+…`
//!   intel = ped+0x5e8
//!   task  = [intel+0x28] non-null, vtable null
//!   `ldr x8, [x8, #0x28]` / `blr x8`
//!
//! Policy: if task graph is unwalkable, do not enter the engine scanner.
//! Never write/null engine task slots.

use dispatch_exec::ped_has_unwalkable_task;

type OrigScanForEvents =
    unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void);

static mut ORIG_SCAN_FOR_EVENTS: Option<OrigScanForEvents> = None;

pub fn set_orig_scan_for_events(f: OrigScanForEvents) {
    unsafe { ORIG_SCAN_FOR_EVENTS = Some(f) };
}

/// Gate only: dangerous ped task graph → skip orig. No memory writes.
///
/// No `zone_active` bypass (same rationale as buoyancy / static_counter gates).
pub unsafe extern "C" fn detour_scan_for_events(
    self_: *mut std::ffi::c_void,
    ped: *mut std::ffi::c_void,
) {
    if ped.is_null() {
        return;
    }
    if ped_has_unwalkable_task(ped as *const _) {
        return;
    }
    if let Some(orig) = ORIG_SCAN_FOR_EVENTS {
        orig(self_, ped);
    }
}
