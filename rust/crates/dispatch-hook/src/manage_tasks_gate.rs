//! `CTaskManager::ManageTasks` fail-closed gate (not a sanitizer).
//!
//! ManageTasks is the engine's per-frame task dispatcher.  It iterates every
//! active task slot and calls multiple virtual functions through the vtable,
//! so a single dangling slot produces crashes at many different vtable offsets
//! (0x18, 0x20, 0x28, 0x30, 0x38, …).  Gating the entry point covers all of
//! them in one hook.
//!
//! Policy: if the CTaskManager has any unwalkable slot (non-null pointer, null
//! vtable), do not enter the engine walk.  Never write task slots.

use dispatch_exec::task_manager_has_unwalkable_task;

type OrigManageTasks = unsafe extern "C" fn(*mut std::ffi::c_void);

static mut ORIG_MANAGE_TASKS: Option<OrigManageTasks> = None;

pub fn set_orig_manage_tasks(f: OrigManageTasks) {
    unsafe { ORIG_MANAGE_TASKS = Some(f) };
}

/// Gate only: unwalkable task manager slot → skip orig. No memory writes.
pub unsafe extern "C" fn detour_manage_tasks(self_: *mut std::ffi::c_void) {
    if task_manager_has_unwalkable_task(self_ as *const _) {
        return;
    }
    if let Some(orig) = ORIG_MANAGE_TASKS {
        orig(self_);
    }
}
