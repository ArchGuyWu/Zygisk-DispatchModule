use std::thread;
use std::time::Duration;

use anyhow::Context;
use dispatch_engine::{Library, TARGET_LIB};

extern "C" {
    fn zygisk_entry_rust(table: *mut std::ffi::c_void, env: *mut std::ffi::c_void);
    fn dispatch_android_log(priority: i32, msg: *const std::ffi::c_char);
}

#[no_mangle]
pub unsafe extern "C" fn zygisk_module_entry(
    table: *mut std::ffi::c_void,
    env: *mut std::ffi::c_void,
) {
    zygisk_entry_rust(table, env);
}

#[no_mangle]
pub extern "C" fn dispatch_bootstrap_rust() {
    log_android(4, "bootstrap: enter");
    match bootstrap() {
        Ok(()) => {
            use dispatch_hook::{HOOKS_ENABLED, MOD_LOGIC_ENABLED};
            let msg = if !HOOKS_ENABLED {
                "bootstrap: complete — no hooks (control build)"
            } else if MOD_LOGIC_ENABLED {
                "bootstrap: complete — mod logic active"
            } else {
                "bootstrap: complete — hooks only (mod logic OFF)"
            };
            log_android(4, msg);
        }
        Err(err) => log_android(6, &format!("bootstrap failed: {err:#}")),
    }
}

fn log_android(priority: i32, msg: &str) {
    let cstr = std::ffi::CString::new(msg).unwrap_or_default();
    unsafe { dispatch_android_log(priority, cstr.as_ptr()) };
}

fn bootstrap() -> anyhow::Result<()> {
    wait_for_lib()?;
    log_android(4, "bootstrap: installing hooks");
    dispatch_hook::install_lifecycle_hooks().context("install hooks")?;
    Ok(())
}

fn wait_for_lib() -> anyhow::Result<()> {
    log_android(4, &format!("bootstrap: waiting for {TARGET_LIB}"));
    for i in 0..240 {
        if Library::open(TARGET_LIB).is_ok() {
            log_android(
                4,
                &format!("bootstrap: {TARGET_LIB} found after ~{} ms", i * 500),
            );
            return Ok(());
        }
        if i > 0 && i % 20 == 0 {
            log_android(
                4,
                &format!("bootstrap: still waiting for {TARGET_LIB} ({} s)", i / 2),
            );
        }
        thread::sleep(Duration::from_millis(500));
    }
    anyhow::bail!("timed out: {TARGET_LIB}")
}