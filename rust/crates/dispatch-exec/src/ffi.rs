//! Exec-layer symbols extending `dispatch_engine::GameSymbols` (resolved from libUE4.so).
//! All `unsafe` FFI is confined to this module.

use dispatch_engine::{CVector, GameSymbols, Library, LivePed, PoolPed, SymbolError};
use thiserror::Error;

pub type SymbolFindPlayerCoors = unsafe extern "C" fn(player_id: i32) -> CVector;
pub type SymbolIsDriver =
    unsafe extern "C" fn(vehicle: *const std::ffi::c_void, ped: *const std::ffi::c_void) -> bool;
pub type SymbolIsPassenger =
    unsafe extern "C" fn(vehicle: *const std::ffi::c_void, ped: *const std::ffi::c_void) -> bool;
pub type SymbolTellOccupantsToLeaveCar =
    unsafe extern "C" fn(vehicle: *mut std::ffi::c_void);
pub type SymbolGetCarToGoToCoors = unsafe extern "C" fn(
    vehicle: *mut std::ffi::c_void,
    coors: *mut CVector,
    driving_mode: i32,
    flag: bool,
) -> bool;
pub type SymbolAddCriminalToKill =
    unsafe extern "C" fn(cop: *mut std::ffi::c_void, criminal: *mut std::ffi::c_void);
pub type SymbolTaskNew = unsafe extern "C" fn(size: usize) -> *mut std::ffi::c_void;
pub type SymbolTaskComplexArrestPedCtor =
    unsafe extern "C" fn(task: *mut std::ffi::c_void, criminal: *mut std::ffi::c_void);
pub type SymbolAddTaskPrimaryMaybeInGroup = unsafe extern "C" fn(
    intelligence: *mut std::ffi::c_void,
    task: *mut std::ffi::c_void,
    write_to_event_log: bool,
);
pub type SymbolGetWeaponLockOnTarget =
    unsafe extern "C" fn(ped: *const std::ffi::c_void) -> *mut std::ffi::c_void;
pub type SymbolTaskComplexUseMobilePhoneCtor =
    unsafe extern "C" fn(task: *mut std::ffi::c_void, duration_ms: i32);
pub type SymbolTaskInvestigateDisturbanceCtor = unsafe extern "C" fn(
    task: *mut std::ffi::c_void,
    pos: *const CVector,
    entity: *mut std::ffi::c_void,
);
pub type SymbolTaskSimpleStandStillCtor = unsafe extern "C" fn(
    task: *mut std::ffi::c_void,
    duration_ms: i32,
    use_anim_idle_stance: bool,
    head_has_target: bool,
    target_heading: f32,
);
pub type SymbolFindTaskByType =
    unsafe extern "C" fn(intelligence: *const std::ffi::c_void, task_type: i32) -> *mut std::ffi::c_void;
pub type SymbolFindActiveTaskByType =
    unsafe extern "C" fn(task_manager: *const std::ffi::c_void, task_type: i32) -> *mut std::ffi::c_void;
pub type SymbolPedIntelligenceClearTasks =
    unsafe extern "C" fn(intelligence: *mut std::ffi::c_void, primary: bool, secondary: bool);
pub type SymbolTaskComplexUseMobilePhoneMakeAbortable = unsafe extern "C" fn(
    task: *mut std::ffi::c_void,
    ped: *mut std::ffi::c_void,
    priority: i32,
    event: *const std::ffi::c_void,
) -> bool;
pub type SymbolAddPoliceCarOccupants =
    unsafe extern "C" fn(vehicle: *mut std::ffi::c_void, siren_or_alarm: bool);
pub type SymbolAddAmbulanceOccupants = unsafe extern "C" fn(vehicle: *mut std::ffi::c_void);
pub type SymbolAddFiretruckOccupants = unsafe extern "C" fn(vehicle: *mut std::ffi::c_void);

#[derive(Debug, Error)]
pub enum ExecSymbolError {
    #[error("base symbols: {0}")]
    Base(#[from] SymbolError),
    #[error("exec symbol missing: {0}")]
    Missing(&'static str),
}

pub struct ExecSymbols {
    pub base: GameSymbols,
    pub find_player_coors: SymbolFindPlayerCoors,
    pub is_driver: SymbolIsDriver,
    pub is_passenger: SymbolIsPassenger,
    pub tell_occupants_to_leave_car: SymbolTellOccupantsToLeaveCar,
    pub get_car_to_go_to_coors: SymbolGetCarToGoToCoors,
    pub add_criminal_to_kill: SymbolAddCriminalToKill,
    pub task_new: SymbolTaskNew,
    pub task_complex_arrest_ped_ctor: SymbolTaskComplexArrestPedCtor,
    pub add_task_primary_maybe_in_group: SymbolAddTaskPrimaryMaybeInGroup,
    pub get_weapon_lock_on_target: SymbolGetWeaponLockOnTarget,
    pub task_complex_use_mobile_phone_ctor: SymbolTaskComplexUseMobilePhoneCtor,
    pub task_investigate_disturbance_ctor: SymbolTaskInvestigateDisturbanceCtor,
    pub task_simple_stand_still_ctor: SymbolTaskSimpleStandStillCtor,
    pub find_task_by_type: SymbolFindTaskByType,
    pub find_active_task_by_type: SymbolFindActiveTaskByType,
    pub ped_intelligence_clear_tasks: SymbolPedIntelligenceClearTasks,
    pub task_complex_use_mobile_phone_make_abortable: SymbolTaskComplexUseMobilePhoneMakeAbortable,
    pub add_police_car_occupants: SymbolAddPoliceCarOccupants,
    pub add_ambulance_occupants: SymbolAddAmbulanceOccupants,
    pub add_firetruck_occupants: SymbolAddFiretruckOccupants,
}

impl std::ops::Deref for ExecSymbols {
    type Target = GameSymbols;

    fn deref(&self) -> &GameSymbols {
        &self.base
    }
}

impl ExecSymbols {
    pub fn resolve(lib: &Library) -> Result<Self, ExecSymbolError> {
        let base = GameSymbols::resolve(lib).map_err(ExecSymbolError::Base)?;
        Ok(Self {
            base,
            find_player_coors: resolve_sym(lib, "_Z15FindPlayerCoorsi")?,
            is_driver: resolve_sym(lib, "_ZNK8CVehicle8IsDriverEPK4CPed")?,
            is_passenger: resolve_sym(lib, "_ZNK8CVehicle11IsPassengerEPK4CPed")?,
            tell_occupants_to_leave_car: resolve_sym(
                lib,
                "_ZN6CCarAI23TellOccupantsToLeaveCarEP8CVehicle",
            )?,
            get_car_to_go_to_coors: resolve_sym(
                lib,
                "_ZN6CCarAI17GetCarToGoToCoorsEP8CVehicleP7CVectorib",
            )?,
            add_criminal_to_kill: resolve_sym(lib, "_ZN7CCopPed17AddCriminalToKillEP4CPed")?,
            task_new: resolve_sym(lib, "_ZN5CTasknwEm")?,
            task_complex_arrest_ped_ctor: resolve_sym(
                lib,
                "_ZN21CTaskComplexArrestPedC2EP4CPed",
            )?,
            add_task_primary_maybe_in_group: resolve_sym(
                lib,
                "_ZN16CPedIntelligence26AddTaskPrimaryMaybeInGroupEP5CTaskb",
            )?,
            get_weapon_lock_on_target: resolve_sym(
                lib,
                "_ZNK4CPed21GetWeaponLockOnTargetEv",
            )?,
            task_complex_use_mobile_phone_ctor: resolve_sym(
                lib,
                "_ZN26CTaskComplexUseMobilePhoneC2Ei",
            )?,
            task_investigate_disturbance_ctor: resolve_sym(
                lib,
                "_ZN34CTaskComplexInvestigateDisturbanceC2ERK7CVectorP7CEntity",
            )?,
            task_simple_stand_still_ctor: resolve_sym(
                lib,
                "_ZN21CTaskSimpleStandStillC2Eibbf",
            )?,
            find_task_by_type: resolve_sym(
                lib,
                "_ZNK16CPedIntelligence14FindTaskByTypeEi",
            )?,
            find_active_task_by_type: resolve_sym(
                lib,
                "_ZNK12CTaskManager20FindActiveTaskByTypeEi",
            )?,
            ped_intelligence_clear_tasks: resolve_sym(
                lib,
                "_ZN16CPedIntelligence10ClearTasksEbb",
            )?,
            task_complex_use_mobile_phone_make_abortable: resolve_sym(
                lib,
                "_ZN26CTaskComplexUseMobilePhone13MakeAbortableEP4CPediPK6CEvent",
            )?,
            add_police_car_occupants: resolve_sym(
                lib,
                "_ZN6CCarAI21AddPoliceCarOccupantsEP8CVehicleb",
            )?,
            add_ambulance_occupants: resolve_sym(
                lib,
                "_ZN6CCarAI21AddAmbulanceOccupantsEP8CVehicle",
            )?,
            add_firetruck_occupants: resolve_sym(
                lib,
                "_ZN6CCarAI21AddFiretruckOccupantsEP8CVehicle",
            )?,
        })
    }

    pub fn try_get_car_to_go_to_coors(
        &self,
        vehicle: *mut std::ffi::c_void,
        target: CVector,
        driving_style: i32,
        stop_after: bool,
    ) -> bool {
        if vehicle.is_null() {
            return false;
        }
        unsafe { (self.get_car_to_go_to_coors)(vehicle, &target as *const _ as *mut _, driving_style, stop_after) }
    }

    pub fn tell_occupants_to_leave_car(&self, vehicle: *mut std::ffi::c_void) {
        if vehicle.is_null() {
            return;
        }
        unsafe {
            (self.tell_occupants_to_leave_car)(vehicle);
        }
    }

    pub fn player_coors(&self, player_id: i32) -> dispatch_core::WorldPos {
        let pos = unsafe { (self.find_player_coors)(player_id) };
        dispatch_core::WorldPos {
            x: pos.x,
            y: pos.y,
            z: pos.z,
        }
    }

    pub fn add_criminal_to_kill(&self, cop: *mut std::ffi::c_void, criminal: *mut std::ffi::c_void) {
        let Some(cop) = self.validate_live_ped(cop as *const _) else {
            return;
        };
        let Some(criminal) = self.validate_live_ped(criminal as *const _) else {
            return;
        };
        self.add_criminal_to_kill_live(cop, criminal);
    }

    pub fn add_criminal_to_kill_live(&self, cop: LivePed, criminal: LivePed) {
        unsafe {
            (self.add_criminal_to_kill)(cop.as_mut_ptr(), criminal.as_mut_ptr());
        }
    }

    pub fn is_driver_of(&self, vehicle: *const std::ffi::c_void, cop: PoolPed) -> bool {
        if vehicle.is_null() {
            return false;
        }
        unsafe {
            (self.is_driver)(vehicle as *mut _, cop.as_ptr())
        }
    }

    pub fn is_passenger_of(&self, vehicle: *const std::ffi::c_void, cop: PoolPed) -> bool {
        if vehicle.is_null() {
            return false;
        }
        unsafe {
            (self.is_passenger)(vehicle as *mut _, cop.as_ptr())
        }
    }

    pub fn weapon_lock_on_target(&self, cop: LivePed) -> Option<PoolPed> {
        let target = unsafe { (self.get_weapon_lock_on_target)(cop.as_ptr()) };
        self.validate_pool_ped(target as *const _)
    }
}

fn resolve_sym<T>(lib: &Library, name: &'static str) -> Result<T, ExecSymbolError> {
    lib.sym(name)
        .map(|ptr| unsafe { std::mem::transmute_copy(&ptr) })
        .ok_or(ExecSymbolError::Missing(name))
}