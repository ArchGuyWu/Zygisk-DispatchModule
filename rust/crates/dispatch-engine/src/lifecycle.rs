//! Engine pointer lifecycle — every `libUE4.so` ped/entity touch must pass through here.
//!
//! Policy: fail-closed. Dangerous pointers are not taken and not passed into engine APIs.
//! Never sanitize engine memory by writing nulls into task/entity slots.

use dispatch_core::{game_state_blocks, PoolKey};

use crate::pool::{entity_pool_key, open_entity_pool};
use crate::symbols::GameSymbols;

/// Ped confirmed present in `ms_pPedPool` (no `IsAlive` call).
#[repr(transparent)]
#[derive(Clone, Copy, Debug)]
pub struct PoolPed(*const std::ffi::c_void);

/// Ped in pool and `CPed::IsAlive` — safe to pass into engine APIs that query ped state.
#[repr(transparent)]
#[derive(Clone, Copy, Debug)]
pub struct LivePed(*const std::ffi::c_void);

/// Vehicle confirmed present in `ms_pVehiclePool`.
#[repr(transparent)]
#[derive(Clone, Copy, Debug)]
pub struct PoolVehicle(*const std::ffi::c_void);

impl PoolPed {
    #[inline]
    pub fn as_ptr(self) -> *const std::ffi::c_void {
        self.0
    }
}

impl LivePed {
    #[inline]
    pub fn as_ptr(self) -> *const std::ffi::c_void {
        self.0
    }

    #[inline]
    pub fn as_mut_ptr(self) -> *mut std::ffi::c_void {
        self.0 as *mut _
    }
}

impl PoolVehicle {
    #[inline]
    pub fn as_ptr(self) -> *const std::ffi::c_void {
        self.0
    }
}

impl GameSymbols {
    /// Hook/FFI ingress: reject null and non-pool pointers before safe business logic.
    pub fn validate_pool_ped(&self, ped: *const std::ffi::c_void) -> Option<PoolPed> {
        if self.ped_in_pool(ped) {
            Some(PoolPed(ped))
        } else {
            None
        }
    }

    /// Before any engine call that may invoke `CPed::IsAlive` internally.
    pub fn validate_live_ped(&self, ped: *const std::ffi::c_void) -> Option<LivePed> {
        if self.ped_alive(ped) {
            Some(LivePed(ped))
        } else {
            None
        }
    }

    /// Vehicle pool membership only — never read `+0x460` driver until this succeeds.
    pub fn validate_pool_vehicle(&self, vehicle: *const std::ffi::c_void) -> Option<PoolVehicle> {
        if vehicle.is_null() {
            return None;
        }
        let pool = self.open_vehicle_pool()?;
        if entity_pool_key(&pool, self.get_pool_vehicle, vehicle).is_some() {
            Some(PoolVehicle(vehicle))
        } else {
            None
        }
    }

    /// Driver of a pool vehicle, only if that driver is itself a pool ped. Else None.
    pub fn take_vehicle_driver(&self, vehicle: PoolVehicle) -> Option<PoolPed> {
        let driver = crate::entity::vehicle_driver(vehicle.as_ptr());
        self.validate_pool_ped(driver)
    }

    pub fn ped_type_of_pool(&self, ped: PoolPed) -> i32 {
        self.ped_type_of(ped.as_ptr())
    }

    pub fn lookup_ped_key(&self, ped: *const std::ffi::c_void) -> Option<PoolKey> {
        self.validate_pool_ped(ped)
            .and_then(|p| self.ped_pool_key(p))
    }

    pub fn lookup_vehicle_key(&self, vehicle: *const std::ffi::c_void) -> Option<PoolKey> {
        if vehicle.is_null() {
            return None;
        }
        let pool = self.open_vehicle_pool()?;
        entity_pool_key(&pool, self.get_pool_vehicle, vehicle)
    }

    pub fn ped_pool_key(&self, ped: PoolPed) -> Option<PoolKey> {
        let pool = self.open_ped_pool()?;
        entity_pool_key(&pool, self.get_pool_ped, ped.as_ptr())
    }

    pub fn entity_from_ped_key(&self, key: PoolKey) -> Option<*const std::ffi::c_void> {
        self.pool_entity_ptr(key, false)
    }

    pub fn entity_from_vehicle_key(&self, key: PoolKey) -> Option<*const std::ffi::c_void> {
        self.pool_entity_ptr(key, true)
    }

    pub fn open_ped_pool(&self) -> Option<crate::EntityPoolView<'_>> {
        open_entity_pool(self.ms_p_ped_pool)
    }

    pub fn open_vehicle_pool(&self) -> Option<crate::EntityPoolView<'_>> {
        open_entity_pool(self.ms_p_vehicle_pool)
    }

    pub fn pool_entity_ptr(&self, key: PoolKey, vehicle: bool) -> Option<*const std::ffi::c_void> {
        let pool = if vehicle {
            self.open_vehicle_pool()?
        } else {
            self.open_ped_pool()?
        };
        let get = if vehicle {
            self.get_pool_vehicle
        } else {
            self.get_pool_ped
        };
        let ptr = crate::entity_from_pool_key(&pool, get, key)?;
        if ptr.is_null() {
            None
        } else {
            Some(ptr as *const _)
        }
    }

    /// Load/fade veto without probing player ped (hook fast path).
    pub fn engine_transition_blocks_dispatch(&self) -> bool {
        if !self.ms_b_loading.is_null() && unsafe { *self.ms_b_loading != 0 } {
            return true;
        }
        if self.game_state.is_null() {
            return true;
        }
        game_state_blocks(unsafe { *self.game_state })
    }
}