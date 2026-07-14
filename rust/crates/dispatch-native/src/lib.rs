#![allow(non_camel_case_types)]

pub const XDL_DEFAULT: i32 = 0;

#[repr(C)]
pub struct Elf64_Phdr {
    pub p_type: u32,
    pub p_flags: u32,
    pub p_offset: u64,
    pub p_vaddr: u64,
    pub p_paddr: u64,
    pub p_filesz: u64,
    pub p_memsz: u64,
    pub p_align: u64,
}

pub const PT_LOAD: u32 = 1;

#[repr(C)]
pub struct XdlInfo {
    pub dli_fname: *const i8,
    pub dli_fbase: *mut std::ffi::c_void,
    pub dli_sname: *const i8,
    pub dli_saddr: *mut std::ffi::c_void,
    pub dli_ssize: usize,
    pub dlpi_phdr: *const Elf64_Phdr,
    pub dlpi_phnum: usize,
}

extern "C" {
    pub fn xdl_open(filename: *const i8, flags: i32) -> *mut std::ffi::c_void;
    pub fn xdl_close(handle: *mut std::ffi::c_void) -> i32;
    pub fn xdl_sym(
        handle: *mut std::ffi::c_void,
        symbol: *const i8,
        symbol_size: *mut usize,
    ) -> *mut std::ffi::c_void;
    pub fn xdl_addr4(
        addr: *mut std::ffi::c_void,
        info: *mut XdlInfo,
        cache: *mut *mut std::ffi::c_void,
        flags: i32,
    ) -> i32;
    pub fn xdl_addr_clean(cache: *mut *mut std::ffi::c_void);
}