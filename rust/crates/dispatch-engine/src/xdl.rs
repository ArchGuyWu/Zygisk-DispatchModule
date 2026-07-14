use std::ffi::CString;

use dispatch_native::{xdl_close, xdl_open, xdl_sym};

use crate::symbols::SymbolError;

pub struct Library {
    handle: *mut std::ffi::c_void,
}

impl Library {
    pub fn open(name: &str) -> Result<Self, SymbolError> {
        let cname = CString::new(name).map_err(|e| SymbolError::Library(e.to_string()))?;
        let handle = unsafe { xdl_open(cname.as_ptr() as *const i8, XDL_DEFAULT) };
        if handle.is_null() {
            return Err(SymbolError::Library(name.to_string()));
        }
        Ok(Self { handle })
    }

    pub fn sym(&self, name: &str) -> Option<usize> {
        let cname = CString::new(name).ok()?;
        let mut size: usize = 0;
        let ptr = unsafe { xdl_sym(self.handle, cname.as_ptr() as *const i8, &mut size) };
        if ptr.is_null() {
            None
        } else {
            Some(ptr as usize)
        }
    }
}

impl Drop for Library {
    fn drop(&mut self) {
        unsafe { xdl_close(self.handle) };
    }
}

const XDL_DEFAULT: i32 = 0;