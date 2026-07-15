use dispatch_core::WorldPos;

use crate::entity::entity_pos;
use crate::pool::{entity_pool_key, open_entity_pool};
use crate::library::Library;
use thiserror::Error;

pub const TARGET_LIB: &str = "libUE4.so";

pub struct GameSymbols {
    pub(crate) find_player_ped: SymbolFindPlayerPed,
    pub(crate) get_pool_ped: SymbolGetPoolPed,
    pub(crate) get_ped_type: SymbolGetPedType,
    pub(crate) is_alive: SymbolIsAlive,
    pub(crate) ms_p_ped_pool: *mut *mut std::ffi::c_void,
    pub(crate) ms_p_vehicle_pool: *mut *mut std::ffi::c_void,
    pub(crate) get_pool_vehicle: SymbolGetPoolVehicle,
    pub(crate) curr_area: *const u8,
    /// `gGameState` — `DoGameState` drives this u32, not `CGameLogic::GameState` (u8).
    pub(crate) game_state: *const u32,
    pub(crate) ms_b_loading: *const u8,
    pub(crate) cutscene_running: *const u8,
    pub(crate) ms_b_warping: *const u8,
    pub(crate) we_are_in_interior_transition: SymbolWeAreInInteriorTransition,
    pub(crate) get_matrix: SymbolGetMatrix,
    pub(crate) create_car_for_script: SymbolCreateCarForScript,
}

pub type SymbolGetMatrix =
    unsafe extern "C" fn(entity: *mut std::ffi::c_void) -> *mut std::ffi::c_void;

pub type SymbolFindPlayerPed = unsafe extern "C" fn(player_id: i32) -> *mut std::ffi::c_void;
pub type SymbolGetPoolPed = unsafe extern "C" fn(handle: i32) -> *mut std::ffi::c_void;
pub type SymbolGetPoolVehicle = unsafe extern "C" fn(handle: i32) -> *mut std::ffi::c_void;
pub type SymbolGetPedType = unsafe extern "C" fn(ped: *const std::ffi::c_void) -> i32;
pub type SymbolIsAlive = unsafe extern "C" fn(ped: *const std::ffi::c_void) -> bool;
pub type SymbolWeAreInInteriorTransition = unsafe extern "C" fn() -> bool;
pub type SymbolCreateCarForScript = unsafe extern "C" fn(
    model: i32,
    pos: crate::ffi_types::CVector,
    created_by: u8,
) -> *mut std::ffi::c_void;

#[derive(Debug, Error)]
pub enum SymbolError {
    #[error("library open failed: {0}")]
    Library(String),
    #[error("symbol missing: {0}")]
    Missing(&'static str),
}

impl GameSymbols {
    pub fn pool_ped(&self, handle: i32) -> *const std::ffi::c_void {
        if handle == 0 {
            return std::ptr::null();
        }
        unsafe { (self.get_pool_ped)(handle) as *const _ }
    }

    /// Pool membership without calling `CPed::IsAlive` (legacy `is_ped_pointer_valid_safe`).
    pub fn ped_in_pool(&self, ped: *const std::ffi::c_void) -> bool {
        if ped.is_null() {
            return false;
        }
        for player_id in [0, -1] {
            let player = self.player_ped(player_id);
            if !player.is_null() && ped == player {
                return true;
            }
        }
        let pool = match open_entity_pool(self.ms_p_ped_pool) {
            Some(pool) => pool,
            None => return false,
        };
        entity_pool_key(&pool, self.get_pool_ped, ped).is_some()
    }

    pub fn ped_alive(&self, ped: *const std::ffi::c_void) -> bool {
        if !self.ped_in_pool(ped) {
            return false;
        }
        unsafe { (self.is_alive)(ped) }
    }

    /// Requires pool membership — never calls `GetPedType` on stray pointers.
    pub fn ped_type_of(&self, ped: *const std::ffi::c_void) -> i32 {
        if !self.ped_in_pool(ped) {
            return 0;
        }
        unsafe { (self.get_ped_type)(ped) }
    }

    pub fn player_ped(&self, player_id: i32) -> *const std::ffi::c_void {
        unsafe { (self.find_player_ped)(player_id) as *const _ }
    }

    pub fn entity_world_pos(&self, entity: *const std::ffi::c_void) -> WorldPos {
        entity_pos(entity, self.get_matrix)
    }

    pub fn spawn_car_for_script(
        &self,
        model: i32,
        pos: crate::ffi_types::CVector,
        created_by: u8,
    ) -> *mut std::ffi::c_void {
        unsafe { (self.create_car_for_script)(model, pos, created_by) }
    }

    pub fn resolve(lib: &Library) -> Result<Self, SymbolError> {
        Ok(Self {
            find_player_ped: resolve(lib, "_Z13FindPlayerPedi")?,
            get_pool_ped: resolve(lib, "_Z10GetPoolPedi")?,
            get_ped_type: resolve(lib, "_ZNK4CPed10GetPedTypeEv")?,
            is_alive: resolve(lib, "_ZNK4CPed7IsAliveEv")?,
            ms_p_ped_pool: resolve_global(lib, "_ZN6CPools11ms_pPedPoolE")?,
            ms_p_vehicle_pool: resolve_global(lib, "_ZN6CPools15ms_pVehiclePoolE")?,
            get_pool_vehicle: resolve(lib, "_Z14GetPoolVehiclei")?,
            curr_area: resolve_global(lib, "_ZN5CGame8currAreaE")?,
            game_state: resolve_global(lib, "gGameState")?,
            ms_b_loading: resolve_global(lib, "_ZN19CGenericGameStorage11ms_bLoadingE")?,
            cutscene_running: resolve_global(lib, "_ZN12CCutsceneMgr10ms_runningE")?,
            ms_b_warping: resolve_global(lib, "_ZN10CEntryExit11ms_bWarpingE")?,
            we_are_in_interior_transition: resolve(
                lib,
                "_ZN17CEntryExitManager25WeAreInInteriorTransitionEv",
            )?,
            get_matrix: resolve(lib, "_ZN10CPlaceable9GetMatrixEv")?,
            create_car_for_script: resolve(
                lib,
                "_ZN8CCarCtrl18CreateCarForScriptEi7CVectorh",
            )?,
        })
    }
}

fn resolve<T>(lib: &Library, name: &'static str) -> Result<T, SymbolError> {
    lib.sym(name)
        .map(|ptr| unsafe { std::mem::transmute_copy(&ptr) })
        .ok_or(SymbolError::Missing(name))
}

fn resolve_global<T>(lib: &Library, name: &'static str) -> Result<T, SymbolError> {
    lib.sym(name)
        .map(|ptr| unsafe { std::mem::transmute_copy(&ptr) })
        .ok_or(SymbolError::Missing(name))
}