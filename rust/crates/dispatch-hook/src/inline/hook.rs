use std::sync::Mutex;

use libc::{
    close, ftruncate, mmap, mprotect, munmap, MAP_FAILED, MAP_SHARED, PROT_EXEC, PROT_READ,
    PROT_WRITE, _SC_PAGESIZE,
};

use super::read::read_target_bytes;
use super::relocate::{build_trampoline, patch_abs_jump, PATCH_BYTES};
const TRAMP_MAP_SIZE: usize = 4096;
const MFD_CLOEXEC: u32 = 0x0001;

static HOOKS: Mutex<Vec<HookRec>> = Mutex::new(Vec::new());

struct TrampMapping {
    rw: *mut std::ffi::c_void,
    exec: *mut std::ffi::c_void,
}

struct HookRec {
    target: usize,
    tramp_rw: usize,
    tramp_exec: usize,
    map_size: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HookError {
    Inval,
    Align,
    Mprot,
    Alloc,
    Pcrel,
    Write,
    Read,
}

impl HookError {
    pub fn message(self) -> &'static str {
        match self {
            Self::Inval => "invalid argument",
            Self::Align => "target not 4-byte aligned",
            Self::Mprot => "mprotect failed",
            Self::Alloc => "mmap trampoline failed",
            Self::Pcrel => "failed to relocate PC-relative insn",
            Self::Write => "failed to write patch",
            Self::Read => "failed to read target bytes",
        }
    }
}

extern "C" {
    fn dispatch_android_log(priority: i32, msg: *const std::ffi::c_char);
    fn memfd_create(name: *const libc::c_char, flags: u32) -> libc::c_int;
}

fn flush_icache(start: usize, len: usize) {
    if len == 0 {
        return;
    }
    let end = start.saturating_add(len);
    let mut p = start & !0x3F;
    while p < end {
        unsafe {
            std::arch::asm!("dc cvau, {0}", in(reg) p, options(nostack, preserves_flags));
        }
        p += 64;
    }
    unsafe {
        std::arch::asm!("dsb ish", options(nostack, preserves_flags));
    }
    p = start & !0x3F;
    while p < end {
        unsafe {
            std::arch::asm!("ic ivau, {0}", in(reg) p, options(nostack, preserves_flags));
        }
        p += 64;
    }
    unsafe {
        std::arch::asm!("dsb ish", options(nostack, preserves_flags));
        std::arch::asm!("isb", options(nostack, preserves_flags));
    }
}

fn log_step(msg: &str) {
    let Ok(cstr) = std::ffi::CString::new(msg) else {
        return;
    };
    unsafe { dispatch_android_log(4, cstr.as_ptr()) };
}

fn page_size() -> usize {
    let n = unsafe { libc::sysconf(_SC_PAGESIZE) };
    if n <= 0 {
        4096
    } else {
        n as usize
    }
}

fn unmap_trampoline(map: &TrampMapping) {
    unsafe {
        munmap(map.rw, TRAMP_MAP_SIZE);
        munmap(map.exec, TRAMP_MAP_SIZE);
    }
}

/// memfd dual-view: write via RW mapping, execute via RX mapping (same backing pages).
fn map_trampoline() -> Result<TrampMapping, HookError> {
    let name = std::ffi::CString::new("dt").map_err(|_| HookError::Alloc)?;
    let fd = unsafe { memfd_create(name.as_ptr(), MFD_CLOEXEC) };
    if fd < 0 {
        return Err(HookError::Alloc);
    }

    if unsafe { ftruncate(fd, TRAMP_MAP_SIZE as libc::off_t) } != 0 {
        unsafe { close(fd) };
        return Err(HookError::Alloc);
    }

    let rw = unsafe {
        mmap(
            std::ptr::null_mut(),
            TRAMP_MAP_SIZE,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            0,
        )
    };
    if rw == MAP_FAILED {
        unsafe { close(fd) };
        return Err(HookError::Alloc);
    }

    let exec = unsafe {
        mmap(
            std::ptr::null_mut(),
            TRAMP_MAP_SIZE,
            PROT_READ | PROT_EXEC,
            MAP_SHARED,
            fd,
            0,
        )
    };
    unsafe { close(fd) };
    if exec == MAP_FAILED {
        unsafe { munmap(rw, TRAMP_MAP_SIZE) };
        return Err(HookError::Alloc);
    }

    log_step("ih: tramp map memfd");
    Ok(TrampMapping { rw, exec })
}

pub fn install(target: usize, detour: usize) -> Result<usize, HookError> {
    if target == 0 || detour == 0 {
        return Err(HookError::Inval);
    }
    if target & 3 != 0 {
        return Err(HookError::Align);
    }

    log_step("ih: read");
    let backup = read_target_bytes(target).ok_or(HookError::Read)?;

    let tramp = map_trampoline()?;

    let tramp_slice =
        unsafe { std::slice::from_raw_parts_mut(tramp.rw as *mut u8, TRAMP_MAP_SIZE) };
    log_step("ih: tramp build");
    let tramp_len = match build_trampoline(
        tramp_slice,
        &backup,
        target as u64,
        (target + PATCH_BYTES) as u64,
        tramp.exec as u64,
    ) {
        Some(len) => len,
        None => {
            unmap_trampoline(&tramp);
            return Err(HookError::Pcrel);
        }
    };
    log_step("ih: tramp build ok");

    // memfd dual-view: code is written via RW VA but executed via RX VA — flush both.
    flush_icache(tramp.exec as usize, tramp_len);
    flush_icache(tramp.rw as usize, tramp_len);
    log_step("ih: tramp cache ok");

    log_step("ih: patch write");
    let patch = patch_abs_jump(detour as u64);
    if !mem_write(target, &patch) {
        unmap_trampoline(&tramp);
        return Err(HookError::Write);
    }

    if let Ok(mut hooks) = HOOKS.lock() {
        hooks.push(HookRec {
            target,
            tramp_rw: tramp.rw as usize,
            tramp_exec: tramp.exec as usize,
            map_size: TRAMP_MAP_SIZE,
        });
    }

    Ok(tramp.exec as usize)
}

fn mem_write(addr: usize, data: &[u8]) -> bool {
    if data.is_empty() {
        return false;
    }
    let ps = page_size();
    let start = addr & !(ps - 1);
    let len = {
        let end = (addr + data.len() + ps - 1) & !(ps - 1);
        end - start
    };
    log_step("ih: patch mprot");
    if unsafe { mprotect(start as *mut _, len, PROT_READ | PROT_WRITE | PROT_EXEC) } != 0 {
        return false;
    }
    log_step("ih: patch memcpy");
    unsafe {
        std::ptr::copy_nonoverlapping(data.as_ptr(), addr as *mut u8, data.len());
    }
    flush_icache(addr, data.len());
    let _ = unsafe { mprotect(start as *mut _, len, PROT_READ | PROT_EXEC) };
    log_step("ih: patch ok");
    true
}