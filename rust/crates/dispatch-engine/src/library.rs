use std::ffi::CString;

use crate::symbols::SymbolError;

extern "C" {
    fn dlopen(filename: *const i8, flags: i32) -> *mut std::ffi::c_void;
    fn dlclose(handle: *mut std::ffi::c_void) -> i32;
    fn dlsym(handle: *mut std::ffi::c_void, symbol: *const i8) -> *mut std::ffi::c_void;
}

/// Return an existing handle without creating a new mapping (Bionic extension).
const RTLD_NOLOAD: i32 = 0x4;

/// Reinterpret `*const u8` (CString::as_ptr return) as `*const i8` for dlopen/dlsym.
#[inline]
fn as_c_ptr(p: *const u8) -> *const i8 {
    p as *const i8
}

pub struct Library {
    handle: *mut std::ffi::c_void,
}

impl Library {
    pub fn open(name: &str) -> Result<Self, SymbolError> {
        let cname = CString::new(name).map_err(|e| SymbolError::Library(e.to_string()))?;
        let handle = unsafe { dlopen(as_c_ptr(cname.as_ptr()), RTLD_NOLOAD) };
        if handle.is_null() {
            return Err(SymbolError::Library(name.to_string()));
        }
        Ok(Self { handle })
    }

    pub fn sym(&self, name: &str) -> Option<usize> {
        let cname = CString::new(name).ok()?;
        let ptr = unsafe { dlsym(self.handle, as_c_ptr(cname.as_ptr())) };
        if ptr.is_null() {
            None
        } else {
            Some(ptr as usize)
        }
    }
}

impl Drop for Library {
    fn drop(&mut self) {
        unsafe { dlclose(self.handle); }
    }
}