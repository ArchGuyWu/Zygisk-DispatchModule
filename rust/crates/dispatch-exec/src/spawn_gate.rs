//! Gate for mod-owned `CreateCarForScript` calls.

use std::sync::atomic::{AtomicBool, Ordering};

static CUSTOM_SPAWN_ACTIVE: AtomicBool = AtomicBool::new(false);

pub fn set_custom_spawn_active(active: bool) {
    CUSTOM_SPAWN_ACTIVE.store(active, Ordering::Relaxed);
}

pub fn is_custom_spawn_active() -> bool {
    CUSTOM_SPAWN_ACTIVE.load(Ordering::Relaxed)
}