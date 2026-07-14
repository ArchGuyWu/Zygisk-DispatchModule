use crate::symbols::GameSymbols;
use dispatch_core::LoaderInputs;

/// Cheap gate dirty-check without `find_player_ped`.
pub fn probe_globals_changed(symbols: &GameSymbols, last: LoaderInputs) -> bool {
    read_u8(symbols.curr_area) != last.curr_area
        || read_bool(symbols.cutscene_running) != last.cutscene
        || read_bool(symbols.ms_b_warping) != last.warping
        || read_u32(symbols.game_state) != last.game_state
        || read_bool(symbols.ms_b_loading) != last.loading
}

pub fn probe_loader_inputs(symbols: &GameSymbols) -> LoaderInputs {
    LoaderInputs {
        player_live: player_live(symbols),
        curr_area: read_u8(symbols.curr_area),
        cutscene: read_bool(symbols.cutscene_running),
        warping: read_bool(symbols.ms_b_warping),
        interior_transition: unsafe { (symbols.we_are_in_interior_transition)() },
        game_state: read_u32(symbols.game_state),
        loading: read_bool(symbols.ms_b_loading),
    }
}

fn player_live(symbols: &GameSymbols) -> bool {
    let player = symbols.player_ped(0);
    symbols.ped_alive(player)
}

fn read_u8(ptr: *const u8) -> u8 {
    unsafe { *ptr }
}

fn read_u32(ptr: *const u32) -> u32 {
    unsafe { *ptr }
}

fn read_bool(ptr: *const u8) -> bool {
    unsafe { *ptr != 0 }
}