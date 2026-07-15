use crate::config::MOD_LOGIC_ENABLED;
use crate::orig_slot::OrigSlot;
use crate::runtime::with_runtime;

type OrigScriptsProcess = unsafe extern "C" fn();

static ORIG_SCRIPTS_PROCESS: OrigSlot<OrigScriptsProcess> = OrigSlot::new();

pub fn set_orig_scripts_process(orig: OrigScriptsProcess) {
    ORIG_SCRIPTS_PROCESS.set(orig);
}

/// Frame host: always call orig first; then (when MOD_LOGIC) one `frame_tick`
/// which runs model Light/Full + legacy engine path. Never skip-orig here.
pub unsafe extern "C" fn detour_the_scripts_process() {
    if let Some(orig) = ORIG_SCRIPTS_PROCESS.get() {
        orig();
    }
    if MOD_LOGIC_ENABLED {
        with_runtime(|rt| rt.frame_tick());
    }
}