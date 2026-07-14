use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};

use dispatch_core::{GateReason, LoaderInputs, LoaderSnapshot};
use dispatch_engine::{probe_globals_changed, probe_loader_inputs};
use tracing::{debug, info};

use crate::config::MOD_LOGIC_ENABLED;
use crate::runtime::DispatchRuntime;

static ZONE_ACTIVE: AtomicBool = AtomicBool::new(false);
static LAST_BLOCK_REASON: AtomicU8 = AtomicU8::new(u8::MAX);

const GATE_PROBE_INTERVAL: u32 = 4;

pub fn set_zone_active(active: bool) {
    ZONE_ACTIVE.store(active, Ordering::Relaxed);
}

pub fn zone_active_cached() -> bool {
    if !MOD_LOGIC_ENABLED {
        return false;
    }
    ZONE_ACTIVE.load(Ordering::Relaxed)
}

/// Hot-path gate for detours: cheap load/fade veto, then refresh loader snapshot.
pub fn hook_logic_allowed() -> bool {
    if !MOD_LOGIC_ENABLED {
        return false;
    }
    crate::runtime::with_runtime(|rt| {
        if rt.symbols.engine_transition_blocks_dispatch() {
            set_zone_active(false);
            return false;
        }
        rt.refresh_zone_gate()
    })
    .unwrap_or(false)
}

/// Probe loader inputs, refresh `zone_active`, and log gate transitions.
pub fn refresh_zone_gate() -> bool {
    crate::runtime::with_runtime(|rt| rt.refresh_zone_gate()).unwrap_or(zone_active_cached())
}

impl DispatchRuntime {
    pub(crate) fn refresh_zone_gate(&mut self) -> bool {
        if !MOD_LOGIC_ENABLED {
            set_zone_active(false);
            return false;
        }
        self.gate_frame = self.gate_frame.wrapping_add(1);
        let inputs = if self.gate_frame & (GATE_PROBE_INTERVAL - 1) == 0
            || probe_globals_changed(&self.symbols, self.last_gate_inputs)
        {
            let inputs = probe_loader_inputs(&self.symbols);
            self.last_gate_inputs = inputs;
            inputs
        } else {
            self.last_gate_inputs
        };
        let snap = self.loader.update(inputs);
        let active = self.loader.zone_active();
        set_zone_active(active);
        trace_gate_change(inputs, snap, active);
        active
    }
}

fn trace_gate_change(inputs: LoaderInputs, snap: LoaderSnapshot, zone_active: bool) {
    let reason_code = gate_reason_code(snap.global_block);
    let prev = LAST_BLOCK_REASON.swap(reason_code, Ordering::Relaxed);
    if prev == reason_code {
        return;
    }
    if zone_active {
        info!(
            curr_area = inputs.curr_area,
            game_state = inputs.game_state,
            "zone_active=true"
        );
        return;
    }
    info!(
        ?snap.global_block,
        player_live = inputs.player_live,
        curr_area = inputs.curr_area,
        cutscene = inputs.cutscene,
        warping = inputs.warping,
        interior_transition = inputs.interior_transition,
        game_state = inputs.game_state,
        loading = inputs.loading,
        "zone_active=false"
    );
    debug!(?inputs, ?snap, "loader gate blocked");
}

fn gate_reason_code(reason: Option<GateReason>) -> u8 {
    match reason {
        None => 0,
        Some(GateReason::PlayerMissing) => 1,
        Some(GateReason::Cutscene) => 2,
        Some(GateReason::InteriorTransition) => 3,
        Some(GateReason::GameState) => 4,
        Some(GateReason::Loading) => 5,
    }
}