const GAME_STATE_FADE_OR_INIT: u32 = 0;
const GAME_STATE_FRONTEND_IDLE: u32 = 7;
const GAME_STATE_LOADING_STARTED: u32 = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GateReason {
    PlayerMissing,
    Cutscene,
    InteriorTransition,
    GameState,
    Loading,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoaderInputs {
    pub player_live: bool,
    pub curr_area: u8,
    pub cutscene: bool,
    pub warping: bool,
    pub interior_transition: bool,
    pub game_state: u32,
    pub loading: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoaderSnapshot {
    pub global_block: Option<GateReason>,
    pub outdoor_active: bool,
    pub interior_active: bool,
    pub curr_area: u8,
}

pub struct Loader {
    snapshot: LoaderSnapshot,
}

impl Loader {
    pub fn new() -> Self {
        Self {
            snapshot: LoaderSnapshot {
                global_block: Some(GateReason::PlayerMissing),
                outdoor_active: false,
                interior_active: false,
                curr_area: 0,
            },
        }
    }

    pub fn update(&mut self, inputs: LoaderInputs) -> LoaderSnapshot {
        self.snapshot = evaluate(inputs);
        self.snapshot
    }

    pub fn snapshot(&self) -> LoaderSnapshot {
        self.snapshot
    }

    pub fn global_block(&self) -> Option<GateReason> {
        self.snapshot.global_block
    }

    pub fn outdoor_active(&self) -> bool {
        self.snapshot.global_block.is_none() && self.snapshot.outdoor_active
    }

    pub fn interior_active(&self) -> bool {
        self.snapshot.global_block.is_none() && self.snapshot.interior_active
    }

    pub fn zone_active(&self) -> bool {
        self.outdoor_active() || self.interior_active()
    }
}

fn evaluate(inputs: LoaderInputs) -> LoaderSnapshot {
    let curr_area = inputs.curr_area;

    if !inputs.player_live {
        return blocked(curr_area, GateReason::PlayerMissing);
    }
    if inputs.cutscene {
        return blocked(curr_area, GateReason::Cutscene);
    }
    if inputs.warping || inputs.interior_transition {
        return blocked(curr_area, GateReason::InteriorTransition);
    }
    if game_state_blocks(inputs.game_state) {
        return blocked(curr_area, GateReason::GameState);
    }
    if inputs.loading {
        return blocked(curr_area, GateReason::Loading);
    }

    if curr_area == 0 {
        LoaderSnapshot {
            global_block: None,
            outdoor_active: true,
            interior_active: false,
            curr_area,
        }
    } else {
        LoaderSnapshot {
            global_block: None,
            outdoor_active: false,
            interior_active: true,
            curr_area,
        }
    }
}

fn blocked(curr_area: u8, reason: GateReason) -> LoaderSnapshot {
    LoaderSnapshot {
        global_block: Some(reason),
        outdoor_active: false,
        interior_active: false,
        curr_area,
    }
}

pub fn game_state_blocks(game_state: u32) -> bool {
    game_state == GAME_STATE_FADE_OR_INIT
        || game_state == GAME_STATE_FRONTEND_IDLE
        || game_state == GAME_STATE_LOADING_STARTED
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inputs(
        player_live: bool,
        curr_area: u8,
        cutscene: bool,
        warping: bool,
        interior_transition: bool,
        game_state: u32,
        loading: bool,
    ) -> LoaderInputs {
        LoaderInputs {
            player_live,
            curr_area,
            cutscene,
            warping,
            interior_transition,
            game_state,
            loading,
        }
    }

    #[test]
    fn outdoor_playable_zone_active() {
        let snap = evaluate(inputs(true, 0, false, false, false, 9, false));
        assert!(snap.global_block.is_none());
        assert!(snap.outdoor_active);
        let mut loader = Loader::new();
        assert!(loader.update(inputs(true, 0, false, false, false, 9, false)).outdoor_active);
    }

    #[test]
    fn interior_playable_zone_active() {
        let snap = evaluate(inputs(true, 3, false, false, false, 9, false));
        assert!(snap.global_block.is_none());
        assert!(snap.interior_active);
    }

    #[test]
    fn fade_game_state_blocks() {
        let snap = evaluate(inputs(true, 0, false, false, false, 0, false));
        assert_eq!(snap.global_block, Some(GateReason::GameState));
    }

    #[test]
    fn frontend_game_state_blocks() {
        let snap = evaluate(inputs(true, 0, false, false, false, 7, false));
        assert_eq!(snap.global_block, Some(GateReason::GameState));
    }

    #[test]
    fn loading_started_blocks() {
        let snap = evaluate(inputs(true, 0, false, false, false, 8, false));
        assert_eq!(snap.global_block, Some(GateReason::GameState));
    }

    #[test]
    fn ms_b_loading_always_blocks() {
        let snap = evaluate(inputs(true, 0, false, false, false, 9, true));
        assert_eq!(snap.global_block, Some(GateReason::Loading));
    }

    #[test]
    fn interior_transition_blocks() {
        let snap = evaluate(inputs(true, 0, false, false, true, 9, false));
        assert_eq!(snap.global_block, Some(GateReason::InteriorTransition));
    }

    #[test]
    fn in_game_state_four_is_playable() {
        let snap = evaluate(inputs(true, 0, false, false, false, 4, false));
        assert!(snap.global_block.is_none());
        assert!(snap.outdoor_active);
    }
}