use crate::config::MOD_LOGIC_ENABLED;
use crate::runtime::with_runtime;

type OrigScriptsProcess = unsafe extern "C" fn();

static mut ORIG_SCRIPTS_PROCESS: Option<OrigScriptsProcess> = None;

pub fn set_orig_scripts_process(orig: OrigScriptsProcess) {
    unsafe { ORIG_SCRIPTS_PROCESS = Some(orig) };
}

pub unsafe extern "C" fn detour_the_scripts_process() {
    if let Some(orig) = ORIG_SCRIPTS_PROCESS {
        orig();
    }
    if MOD_LOGIC_ENABLED {
        with_runtime(|rt| rt.frame_tick());
    }
}