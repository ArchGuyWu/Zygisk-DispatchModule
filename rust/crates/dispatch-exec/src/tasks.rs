//! Ped task assignment helpers (offsets for intel/task-manager layout).
//!
//! Assign / phone / clear helpers only. No task-manager slot scans and no
//! engine-wide “skip ManageTasks” crash gates (baseline: docs/BASELINE.md).

use dispatch_core::PedId;

use crate::ffi::ExecSymbols;

pub const TASK_COMPLEX_INVESTIGATE_DISTURBANCE: i32 = 935;
pub const TASK_COMPLEX_USE_MOBILE_PHONE: i32 = 1600;
pub const TASK_SIMPLE_PHONE_TALK: i32 = 1601;
pub const TASK_SIMPLE_PHONE_IN: i32 = 1602;
pub const TASK_SIMPLE_PHONE_OUT: i32 = 1603;

const PED_INTEL_OFFSET: usize = 0x5e8;
const TASK_MANAGER_OFFSET: usize = 8;
const TASK_ALLOC_SIZE: usize = 512;
/// `GetSubTask` / walk slot used by buoyancy-style task walks (`ldr x8, [x8, #0x18]`).
const TASK_VT_WALK_OFFSET: usize = 0x18;
/// `Process` / type-query style vtable slot used by FindActiveTaskByType.
const TASK_VT_PROCESS_OFFSET: usize = 0x28;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReportTaskPhase {
    None,
    Dialing,
    Active,
    Ending,
}

#[inline]
fn read_ptr(base: *const std::ffi::c_void, offset: usize) -> *const std::ffi::c_void {
    if base.is_null() {
        return std::ptr::null();
    }
    unsafe {
        let slot = (base as *const u8).add(offset) as *const *const std::ffi::c_void;
        *slot
    }
}

/// Reject pointer patterns that are clearly not valid heap/stack addresses.
/// On ARM64 Android (39/48-bit VA), valid user-space addresses, after masking
/// the top byte (MTE/PAC tag), have bits 48-55 as 0x00.  Sentinels like
/// `0x00FFFFFFFFFFFFFE` (uninitialised task slot during loading) pass `is_null()`
/// but are not valid addresses — dereferencing them faults.
#[inline]
fn is_plausible_ptr(ptr: *const std::ffi::c_void) -> bool {
    const MAX_USER_ADDR: usize = (1u64 << 52) as usize;
    let addr = ptr as usize;
    (addr & 0x00FF_FFFF_FFFF_FFFF) < MAX_USER_ADDR && addr >= 0x1000
}

/// Fail-closed: null task, null vtable, or null fn at `vt_off` → not usable.
#[inline]
fn task_vtable_fn_ok(task: *const std::ffi::c_void, vt_off: usize) -> bool {
    if !is_plausible_ptr(task) {
        return false;
    }
    if task.is_null() {
        return false;
    }
    let vtable = read_ptr(task, 0);
    if vtable.is_null() {
        return false;
    }
    !read_ptr(vtable, vt_off).is_null()
}

/// Accept a task pointer only if it is safe to walk / call at the given vtable offset.
/// Dangerous tasks are discarded (not written back into the engine).
#[inline]
fn take_task_if_safe(task: *mut std::ffi::c_void, vt_off: usize) -> *mut std::ffi::c_void {
    if task_vtable_fn_ok(task as *const _, vt_off) {
        task
    } else {
        std::ptr::null_mut()
    }
}

/// Intelligence pointer for a ped. Null ped / null intel → null (do not pass further).
pub fn ped_intelligence(ped: *const std::ffi::c_void) -> *mut std::ffi::c_void {
    if ped.is_null() {
        return std::ptr::null_mut();
    }
    let intel = read_ptr(ped, PED_INTEL_OFFSET);
    if intel.is_null() {
        return std::ptr::null_mut();
    }
    intel as *mut _
}

pub fn ped_task_manager(ped: *const std::ffi::c_void) -> *mut std::ffi::c_void {
    let intel = ped_intelligence(ped);
    if intel.is_null() {
        return std::ptr::null_mut();
    }
    unsafe { (intel as *mut u8).add(TASK_MANAGER_OFFSET) as *mut std::ffi::c_void }
}

/// Engine `FindTaskByType` result: drop unsafe task pointers instead of forwarding them.
fn find_task_by_type_safe(
    symbols: &ExecSymbols,
    intel: *mut std::ffi::c_void,
    task_type: i32,
) -> *mut std::ffi::c_void {
    if intel.is_null() {
        return std::ptr::null_mut();
    }
    let task = unsafe { (symbols.find_task_by_type)(intel, task_type) };
    // Complex tasks are walked via +0x18 (GetSubTask) in several engine paths.
    take_task_if_safe(task, TASK_VT_WALK_OFFSET)
}

/// Engine `FindActiveTaskByType` result: drop unsafe task pointers.
fn find_active_task_by_type_safe(
    symbols: &ExecSymbols,
    task_mgr: *mut std::ffi::c_void,
    task_type: i32,
) -> *mut std::ffi::c_void {
    if task_mgr.is_null() {
        return std::ptr::null_mut();
    }
    let task = unsafe { (symbols.find_active_task_by_type)(task_mgr, task_type) };
    take_task_if_safe(task, TASK_VT_PROCESS_OFFSET)
}

/// Only assign tasks to live pool peds with a real intelligence object.
fn live_intel_for_assign(
    symbols: &ExecSymbols,
    ped: *mut std::ffi::c_void,
) -> Option<*mut std::ffi::c_void> {
    if ped.is_null() {
        return None;
    }
    // Must be in pool + alive before we hand anything to task ctor / AddTask.
    if symbols.validate_live_ped(ped as *const _).is_none() {
        return None;
    }
    let intel = ped_intelligence(ped);
    if intel.is_null() {
        return None;
    }
    Some(intel)
}

/// Re-check live+intel after ctor work; only then hand task to engine (ownership transfer).
fn commit_primary_task(
    symbols: &ExecSymbols,
    ped: *mut std::ffi::c_void,
    task: *mut std::ffi::c_void,
) -> bool {
    if task.is_null() {
        return false;
    }
    // Ped may have died between alloc and commit — do not attach task to dead intel.
    let Some(intel) = live_intel_for_assign(symbols, ped) else {
        // Engine `operator new` for CTask — if we never AddTask, leak is preferable to
        // wrong free ABI; rare path (ped dies mid-assign).
        tracing::warn!("task alloc discarded: ped not live at commit");
        return false;
    };
    unsafe {
        (symbols.add_task_primary_maybe_in_group)(intel, task, true);
    }
    true
}

pub fn assign_investigate_disturbance_task(
    symbols: &ExecSymbols,
    cop: *mut std::ffi::c_void,
    target: dispatch_core::WorldPos,
) -> bool {
    let Some(intel) = live_intel_for_assign(symbols, cop) else {
        return false;
    };
    let already = !find_task_by_type_safe(symbols, intel, TASK_COMPLEX_INVESTIGATE_DISTURBANCE)
        .is_null();
    if already {
        return true;
    }
    let task = unsafe { (symbols.task_new)(TASK_ALLOC_SIZE) };
    if task.is_null() {
        return false;
    }
    let pos = dispatch_engine::CVector {
        x: target.x,
        y: target.y,
        z: target.z,
    };
    unsafe {
        (symbols.task_investigate_disturbance_ctor)(task, &pos as *const _, std::ptr::null_mut());
    }
    if !commit_primary_task(symbols, cop, task) {
        return false;
    }
    tracing::debug!(?cop, ?target, "CTaskComplexInvestigateDisturbance assigned");
    true
}

pub fn assign_arrest_task(
    symbols: &ExecSymbols,
    cop: *mut std::ffi::c_void,
    criminal: *mut std::ffi::c_void,
) -> bool {
    let Some(_intel) = live_intel_for_assign(symbols, cop) else {
        return false;
    };
    // Do not pass a non-live criminal into the arrest task ctor.
    if symbols.validate_live_ped(criminal as *const _).is_none() {
        return false;
    }
    let task = unsafe { (symbols.task_new)(TASK_ALLOC_SIZE) };
    if task.is_null() {
        return false;
    }
    unsafe {
        (symbols.task_complex_arrest_ped_ctor)(task, criminal);
    }
    if !commit_primary_task(symbols, cop, task) {
        return false;
    }
    tracing::debug!(?cop, ?criminal, "CTaskComplexArrestPed assigned");
    true
}

pub fn assign_mobile_phone_task(symbols: &ExecSymbols, ped: *mut std::ffi::c_void) -> bool {
    if live_intel_for_assign(symbols, ped).is_none() {
        return false;
    }
    let task = unsafe { (symbols.task_new)(TASK_ALLOC_SIZE) };
    if task.is_null() {
        return false;
    }
    unsafe {
        (symbols.task_complex_use_mobile_phone_ctor)(task, 30_000);
    }
    if !commit_primary_task(symbols, ped, task) {
        return false;
    }
    tracing::debug!(?ped, "CTaskComplexUseMobilePhone assigned");
    true
}

pub fn ped_has_phone_task(symbols: &ExecSymbols, ped: *const std::ffi::c_void) -> bool {
    if ped.is_null() {
        return false;
    }
    // Dead / non-pool peds: do not probe task manager.
    if symbols.validate_pool_ped(ped).is_none() {
        return false;
    }
    let intel = ped_intelligence(ped);
    let task_mgr = ped_task_manager(ped);
    if intel.is_null() || task_mgr.is_null() {
        return false;
    }
    if !find_task_by_type_safe(symbols, intel, TASK_COMPLEX_USE_MOBILE_PHONE).is_null() {
        return true;
    }
    if !find_active_task_by_type_safe(symbols, task_mgr, TASK_SIMPLE_PHONE_TALK).is_null() {
        return true;
    }
    if !find_active_task_by_type_safe(symbols, task_mgr, TASK_SIMPLE_PHONE_IN).is_null() {
        return true;
    }
    if !find_active_task_by_type_safe(symbols, task_mgr, TASK_SIMPLE_PHONE_OUT).is_null() {
        return true;
    }
    false
}

/// Engine `CPedIntelligence::ClearTasks(primary, secondary)` — legitimate teardown.
///
/// Only validates pool membership / intel presence. Does **not** scan task-manager
/// slots (deepseek 11/32-slot "unwalkable" scans removed — they false-positive).
pub fn clear_ped_tasks(symbols: &ExecSymbols, ped: *mut std::ffi::c_void) -> bool {
    if ped.is_null() {
        return false;
    }
    if symbols.validate_pool_ped(ped as *const _).is_none() {
        return false;
    }
    let intel = ped_intelligence(ped);
    if intel.is_null() {
        return false;
    }
    unsafe {
        (symbols.ped_intelligence_clear_tasks)(intel, true, true);
    }
    true
}

/// Break phone/radio task chains so the ped can take damage and die mid-action.
/// Uses engine `ClearTasks` only when a live phone task was actually observed — never
/// hand-nulls task slots.
pub fn abort_phone_tasks(symbols: &ExecSymbols, ped: *mut std::ffi::c_void) -> bool {
    if ped.is_null() || !ped_has_phone_task(symbols, ped) {
        return false;
    }
    // ClearTasks / MakeAbortable require a live ped; otherwise skip (do not touch engine).
    if symbols.validate_live_ped(ped as *const _).is_none() {
        return false;
    }
    let intel = ped_intelligence(ped);
    if intel.is_null() {
        return false;
    }

    let mut aborted = false;
    unsafe {
        let complex = find_task_by_type_safe(symbols, intel, TASK_COMPLEX_USE_MOBILE_PHONE);
        if !complex.is_null() {
            aborted |= (symbols.task_complex_use_mobile_phone_make_abortable)(
                complex,
                ped,
                0,
                std::ptr::null(),
            );
        }
        (symbols.ped_intelligence_clear_tasks)(intel, true, true);
    }
    tracing::debug!(?ped, aborted, "phone/radio task aborted");
    true
}

pub fn poll_report_task_phase(symbols: &ExecSymbols, ped: *const std::ffi::c_void) -> ReportTaskPhase {
    if ped.is_null() || symbols.validate_pool_ped(ped).is_none() {
        return ReportTaskPhase::None;
    }
    let intel = ped_intelligence(ped);
    let task_mgr = ped_task_manager(ped);
    if intel.is_null() || task_mgr.is_null() {
        return ReportTaskPhase::None;
    }

    if !find_active_task_by_type_safe(symbols, task_mgr, TASK_SIMPLE_PHONE_TALK).is_null() {
        return ReportTaskPhase::Active;
    }
    if !find_active_task_by_type_safe(symbols, task_mgr, TASK_SIMPLE_PHONE_IN).is_null() {
        return ReportTaskPhase::Dialing;
    }
    if !find_task_by_type_safe(symbols, intel, TASK_COMPLEX_USE_MOBILE_PHONE).is_null() {
        return ReportTaskPhase::Dialing;
    }
    if !find_active_task_by_type_safe(symbols, task_mgr, TASK_SIMPLE_PHONE_OUT).is_null() {
        return ReportTaskPhase::Ending;
    }
    ReportTaskPhase::None
}


