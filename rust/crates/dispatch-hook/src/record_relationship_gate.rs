//! `CPlayerRelationshipRecorder::RecordRelationshipWithPlayer` fail-closed gate.
//!
//! Tombstone_05 (2026-07-15): fault addr 0x28 (read) at offset 0xd7aa4 in libUE4.so.
//!   CPlayerRelationshipRecorder object exists (x0 non-null) but its vtable is null,
//!   causing a null deref when the engine tries to load vtable[0x28].
//!
//! Policy: null/unplausible vtable on `this` → skip orig. Never write engine memory.

/// Reject pointer patterns that are clearly not valid user-space addresses
/// after masking the top byte (MTE/PAC tag).
#[inline]
fn is_plausible_ptr(ptr: *const std::ffi::c_void) -> bool {
    const MAX_USER_ADDR: usize = (1u64 << 52) as usize;
    let addr = ptr as usize;
    (addr & 0x00FF_FFFF_FFFF_FFFF) >= 0x1000
        && (addr & 0x00FF_FFFF_FFFF_FFFF) < MAX_USER_ADDR
}

type OrigRecordRelationship =
    unsafe extern "C" fn(*mut std::ffi::c_void, *const std::ffi::c_void);

static mut ORIG_RECORD_RELATIONSHIP: Option<OrigRecordRelationship> = None;

pub fn set_orig_record_relationship(f: OrigRecordRelationship) {
    unsafe { ORIG_RECORD_RELATIONSHIP = Some(f) };
}

/// Gate only: null vtable → skip orig. No memory writes.
pub unsafe extern "C" fn detour_record_relationship(
    self_: *mut std::ffi::c_void,
    ped: *const std::ffi::c_void,
) {
    if self_.is_null() {
        return;
    }
    // Check that the vtable pointer (first word of `this`) is plausible.
    let vtable: *const std::ffi::c_void = *(self_ as *const *const std::ffi::c_void);
    if !is_plausible_ptr(vtable) {
        return;
    }
    // Also verify vtable[0x28] (the Process/type-query slot) is non-null.
    let fn_at_28: *const std::ffi::c_void =
        *((vtable as *const u8).add(0x28) as *const *const std::ffi::c_void);
    if !is_plausible_ptr(fn_at_28) {
        return;
    }
    if let Some(orig) = ORIG_RECORD_RELATIONSHIP {
        orig(self_, ped);
    }
}
