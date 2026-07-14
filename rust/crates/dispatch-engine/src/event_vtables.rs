use dispatch_case::{NativeEventKind, NATIVE_EVENT_KINDS};

use crate::xdl::Library;

/// Address stored in `CEvent` vptr (Itanium: skip typeinfo + offset slots).
pub fn event_vptr_identity(vtable_array: *const std::ffi::c_void) -> *const std::ffi::c_void {
    if vtable_array.is_null() {
        return std::ptr::null();
    }
    unsafe { (vtable_array as *const u8).add(16) as *const std::ffi::c_void }
}

#[derive(Debug, Clone, Copy)]
pub struct ResolvedNativeEvent {
    pub def: NativeEventKind,
    pub vptr_id: *const std::ffi::c_void,
}

pub struct NativeEventRegistry {
    entries: Vec<ResolvedNativeEvent>,
}

impl NativeEventRegistry {
    pub fn resolve(lib: &Library) -> Self {
        let mut entries = Vec::new();
        for def in NATIVE_EVENT_KINDS {
            let Some(sym) = lib.sym(def.vtable_sym) else {
                tracing::warn!(event = def.name, sym = def.vtable_sym, "native event vtable missing");
                continue;
            };
            entries.push(ResolvedNativeEvent {
                def: *def,
                vptr_id: event_vptr_identity(sym as *const std::ffi::c_void),
            });
        }
        Self { entries }
    }

    pub fn tracked_entries(&self) -> impl Iterator<Item = ResolvedNativeEvent> + '_ {
        self.entries.iter().copied()
    }

    pub fn read_entity(
        &self,
        event: *const std::ffi::c_void,
        offset: usize,
    ) -> *const std::ffi::c_void {
        if event.is_null() {
            return std::ptr::null();
        }
        unsafe {
            let base = event as *const u8;
            let slot = base.add(offset) as *const *const std::ffi::c_void;
            if slot.is_null() {
                return std::ptr::null();
            }
            *slot
        }
    }
}