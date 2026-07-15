//! Global execution state and game-world bridge (no business-logic `unsafe`).

use std::collections::{HashMap, HashSet};

use dispatch_core::{PedId, ResourceRegistry, VehicleId, WorldPos};
use dispatch_engine::{dist_sq, CVector};

use dispatch_case::CaseId;

use crate::ffi::ExecSymbols;
use crate::models::{is_police_dispatch_model, is_swat_model, PoliceSpawnUnit};
use crate::models::SpawnTask;
use crate::staging::StagingState;
use crate::timing::driving_style_for_case;

#[derive(Debug, Clone)]
pub struct CopVehicleBinding {
    pub cop: PedId,
    pub vehicle: VehicleId,
    pub as_driver: bool,
}

#[derive(Debug, Clone)]
pub struct CopExitRecord {
    pub cop: PedId,
    pub exit_time_ms: i64,
}

/// Per-vehicle custody transport — excluded from crime-scene dispatch; native AI handles attack/carjack.
#[derive(Debug, Clone, Default)]
pub struct TransportVehicleState {
    pub prisoners: Vec<PedId>,
    pub departure_ordered: bool,
    pub depart_ms: i64,
}

pub const MAX_TRANSPORT_PRISONERS: usize = 2;

#[derive(Debug, Clone)]
pub struct PoliceSpawnChain {
    pub case_serial: u64,
    pub target_pos: WorldPos,
    pub loc: WorldPos,
    pub criminal: Option<PedId>,
    pub units: Vec<PoliceSpawnUnit>,
    pub reason: String,
    pub units_completed: usize,
    pub released: bool,
    pub release_timeout_scheduled: bool,
    pub held_vehicles: Vec<VehicleId>,
    pub held_staging_centers: Vec<WorldPos>,
}

#[derive(Debug, Default)]
pub struct ExecGlobals {
    pub paused: bool,
    /// Occupants have left on their own (observed via engine occupancy, not forced).
    pub vehicles_emptied: HashSet<VehicleId>,
    pub vehicles_ordered_to_scene: HashSet<VehicleId>,
    pub vehicles_siren_awakened: HashSet<VehicleId>,

    pub spawned_swats: HashSet<VehicleId>,
    pub cop_bindings: Vec<CopVehicleBinding>,
    pub cop_exits: Vec<CopExitRecord>,
    pub staging: StagingState,
    pub spawn_chains: HashMap<u64, PoliceSpawnChain>,
    pub next_chain_id: u64,
    pub pending_spawn: Vec<(CaseId, i64, SpawnTask)>,
    /// Criminals with `CTaskComplexArrestPed` assigned this frame.
    pub arrest_dispatched_criminals: HashSet<PedId>,
    /// Apprehended criminals (synced each tick) — combat blocked, still in custody pipeline.
    pub custody_criminals: HashSet<PedId>,
    /// Police vehicles carrying prisoners — skip dispatch scheduling; occupants respond natively.
    pub transport_vehicles: HashMap<VehicleId, TransportVehicleState>,
    pub last_reroute_eval_ms: i64,
}

impl ExecGlobals {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn is_vehicle_emptied(&self, id: VehicleId) -> bool {
        self.vehicles_emptied.contains(&id)
    }

    pub fn add_vehicle_emptied(&mut self, id: VehicleId) {
        self.vehicles_emptied.insert(id);
    }

    pub fn is_vehicle_ordered_to_scene(&self, id: VehicleId) -> bool {
        self.vehicles_ordered_to_scene.contains(&id)
    }

    pub fn add_vehicle_ordered_to_scene(&mut self, id: VehicleId) {
        self.vehicles_ordered_to_scene.insert(id);
    }

    pub fn is_transport_vehicle(&self, id: VehicleId) -> bool {
        self.transport_vehicles.contains_key(&id)
    }

    pub fn transport_prisoner_count(&self, vehicle: VehicleId) -> usize {
        self.transport_vehicles
            .get(&vehicle)
            .map(|s| s.prisoners.len())
            .unwrap_or(0)
    }

    pub fn register_transport_prisoner(&mut self, vehicle: VehicleId, criminal: PedId) {
        let state = self.transport_vehicles.entry(vehicle).or_default();
        if !state.prisoners.contains(&criminal) {
            state.prisoners.push(criminal);
        }
        self.vehicles_ordered_to_scene.remove(&vehicle);
        self.vehicles_emptied.remove(&vehicle);
    }

    pub fn release_transport_prisoner(&mut self, vehicle: VehicleId, criminal: PedId) {
        let Some(state) = self.transport_vehicles.get_mut(&vehicle) else {
            return;
        };
        state.prisoners.retain(|&id| id != criminal);
        if state.prisoners.is_empty() {
            self.transport_vehicles.remove(&vehicle);
        }
    }

    pub fn is_cop_exiting(&self, cop: PedId, now_ms: i64) -> bool {
        self.cop_exits.iter().any(|r| r.cop == cop && now_ms - r.exit_time_ms < 8_000)
    }

    pub fn record_exit_start(&mut self, cop: PedId, now_ms: i64) {
        if let Some(rec) = self.cop_exits.iter_mut().find(|r| r.cop == cop) {
            rec.exit_time_ms = now_ms;
        } else {
            self.cop_exits.push(CopExitRecord {
                cop,
                exit_time_ms: now_ms,
            });
        }
    }
}

pub struct ExecEnv<'a> {
    pub symbols: &'a ExecSymbols,
    pub registry: &'a mut ResourceRegistry,
    pub globals: &'a mut ExecGlobals,
}

impl<'a> ExecEnv<'a> {
    pub fn ped_ptr(&self, id: PedId) -> *const std::ffi::c_void {
        self.registry
            .ped(id)
            .and_then(|r| self.symbols.entity_from_ped_key(r.pool_key))
            .unwrap_or(std::ptr::null())
    }

    pub fn live_ped(&self, id: PedId) -> Option<dispatch_engine::LivePed> {
        self.symbols.validate_live_ped(self.ped_ptr(id))
    }

    pub fn vehicle_ptr(&self, id: VehicleId) -> *mut std::ffi::c_void {
        self.registry
            .vehicle(id)
            .and_then(|r| self.symbols.entity_from_vehicle_key(r.pool_key))
            .map(|p| p as *mut _)
            .unwrap_or(std::ptr::null_mut())
    }

    pub fn ped_pos(&self, id: PedId) -> WorldPos {
        self.symbols.entity_world_pos(self.ped_ptr(id))
    }

    pub fn vehicle_pos(&self, id: VehicleId) -> WorldPos {
        self.symbols.entity_world_pos(self.vehicle_ptr(id) as *const _)
    }

    pub fn vehicle_model(&self, id: VehicleId) -> u32 {
        self.registry.vehicle(id).map(|v| v.model).unwrap_or(0)
    }

    pub fn vehicle_has_driver(&self, id: VehicleId) -> bool {
        let ptr = self.vehicle_ptr(id);
        let Some(vehicle) = self.symbols.validate_pool_vehicle(ptr) else {
            return false;
        };
        self.symbols.take_vehicle_driver(vehicle).is_some()
    }

    /// Driver is in pool AND alive — required before passing vehicle to engine
    /// pathfinding / siren commands whose internal call graph may walk peds.
    pub fn vehicle_has_live_driver(&self, id: VehicleId) -> bool {
        let ptr = self.vehicle_ptr(id);
        let Some(vehicle) = self.symbols.validate_pool_vehicle(ptr) else {
            return false;
        };
        let Some(driver) = self.symbols.take_vehicle_driver(vehicle) else {
            return false;
        };
        self.symbols.validate_live_ped(driver.as_ptr()).is_some()
    }

    pub fn arm_vehicle_siren(&self, vehicle: VehicleId) -> bool {
        if !self.vehicle_has_live_driver(vehicle) {
            return false;
        }
        let model = self.vehicle_model(vehicle);
        let ptr = self.vehicle_ptr(vehicle);
        crate::vehicle_spawn::arm_emergency_vehicle_siren(self.symbols, ptr, model)
    }

    pub fn cop_is_in_vehicle(&self, cop: PedId, vehicle: VehicleId) -> bool {
        let veh_ptr = self.vehicle_ptr(vehicle);
        let cop_ptr = self.ped_ptr(cop);
        let Some(cop) = self.symbols.validate_pool_ped(cop_ptr) else {
            return false;
        };
        self.symbols.is_driver_of(veh_ptr, cop)
            || self.symbols.is_passenger_of(veh_ptr, cop)
    }

    /// Last bound vehicle only when the cop is still physically inside it.
    pub fn find_vehicle_of_cop(&self, cop: PedId) -> Option<VehicleId> {
        let binding = self.globals.cop_bindings.iter().find(|b| b.cop == cop)?;
        if self.cop_is_in_vehicle(cop, binding.vehicle) {
            Some(binding.vehicle)
        } else {
            None
        }
    }

    pub fn vehicle_has_cop_occupants(&self, vehicle: VehicleId) -> bool {
        self.globals
            .cop_bindings
            .iter()
            .filter(|b| b.vehicle == vehicle)
            .any(|b| self.registry.contains_ped(b.cop) && self.cop_is_in_vehicle(b.cop, vehicle))
    }

    /// Observe engine state — never force exit; mark vehicles once occupants leave on their own.
    pub fn sync_natural_vehicle_exits(&mut self, now_ms: i64) {
        let bindings: Vec<(PedId, VehicleId)> = self
            .globals
            .cop_bindings
            .iter()
            .map(|b| (b.cop, b.vehicle))
            .collect();

        let mut vehicles: HashSet<VehicleId> = HashSet::new();
        for (cop, vehicle) in bindings {
            vehicles.insert(vehicle);
            if !self.registry.contains_ped(cop) {
                continue;
            }
            let departed = !self.cop_is_in_vehicle(cop, vehicle);
            if departed && !self.globals.is_cop_exiting(cop, now_ms) {
                self.globals.record_exit_start(cop, now_ms);
            }
        }

        for vehicle in vehicles {
            if self.globals.is_transport_vehicle(vehicle) {
                continue;
            }
            if !self.registry.contains_vehicle(vehicle) {
                self.globals.add_vehicle_emptied(vehicle);
                continue;
            }
            if !self.vehicle_has_cop_occupants(vehicle) {
                self.globals.add_vehicle_emptied(vehicle);
            }
        }
    }

    /// `CCarAI::GetCarToGoToCoors` — 4th arg is **set cruise speed**, not a dwell timer.
    /// Near the destination the autopilot mission ends / slows; there is no separate
    /// "park and wait N ms" mission API in SA CarAI (see GetCarToParkAtCoors — flag only).
    pub fn try_get_car_to_go_to_coors(
        &self,
        vehicle: VehicleId,
        target: WorldPos,
        driving_style: i32,
        set_cruise_speed: bool,
    ) -> bool {
        if !self.vehicle_has_live_driver(vehicle) {
            return false;
        }
        let ptr = self.vehicle_ptr(vehicle);
        self.symbols.try_get_car_to_go_to_coors(
            ptr,
            vector_from_pos(target),
            driving_style,
            set_cruise_speed,
        )
    }

    /// One-shot **drive** to scene coords (police / EMS / fire). Autopilot holds approach;
    /// when close, mission clears → vehicle stops (engine-side, not a timed dwell task).
    pub fn command_vehicle_to_scene(
        &self,
        vehicle: VehicleId,
        target_loc: WorldPos,
        driving_style: i32,
    ) -> bool {
        if self.globals.paused {
            return false;
        }
        if !self.vehicle_has_live_driver(vehicle) {
            tracing::warn!(?vehicle, "vehicle has no live driver — blocked drive command");
            return false;
        }
        let ok = self.try_get_car_to_go_to_coors(vehicle, target_loc, driving_style, true);
        if ok {
            tracing::info!(?vehicle, driving_style, "vehicle CarAI go-to scene");
        } else {
            tracing::warn!(?vehicle, "GetCarToGoToCoors failed");
        }
        ok
    }

    /// Compatibility alias.
    pub fn command_cop_vehicle_to_scene(
        &self,
        vehicle: VehicleId,
        target_loc: WorldPos,
        is_firearm: bool,
    ) {
        let style = driving_style_for_case(is_firearm);
        let _ = self.command_vehicle_to_scene(vehicle, target_loc, style);
    }

    pub fn bind_vehicle_occupants_from_vehicle(&mut self, vehicle: VehicleId) {
        let ptr = self.vehicle_ptr(vehicle);
        if ptr.is_null() {
            return;
        }
        let Some(pool) = self.symbols.open_ped_pool() else {
            return;
        };
        for slot in 0..pool.size as usize {
            let flag = pool.byte_map[slot];
            if flag < 0 {
                continue;
            }
            let key = dispatch_core::PoolKey::from_slot_flag(slot as u16, flag as u8);
            let Some(ped_ptr) = self.symbols.entity_from_ped_key(key) else {
                continue;
            };
            let Some(pool_ped) = self.symbols.validate_pool_ped(ped_ptr) else {
                continue;
            };
            let as_driver = self.symbols.is_driver_of(ptr, pool_ped);
            let is_passenger = self.symbols.is_passenger_of(ptr, pool_ped);
            if !as_driver && !is_passenger {
                continue;
            }
            let cop_id = self
                .registry
                .ped_by_pool(key)
                .unwrap_or_else(|| {
                    self.registry
                        .adopt_ped(key, self.symbols.ped_type_of_pool(pool_ped))
                });
            self.bind_vehicle_occupants(vehicle, cop_id, as_driver);
        }
    }

    pub fn bind_vehicle_occupants(&mut self, vehicle: VehicleId, cop: PedId, as_driver: bool) {
        self.globals.vehicles_emptied.remove(&vehicle);
        if let Some(binding) = self.globals.cop_bindings.iter_mut().find(|b| b.cop == cop) {
            binding.vehicle = vehicle;
            binding.as_driver = as_driver;
        } else {
            self.globals.cop_bindings.push(CopVehicleBinding {
                cop,
                vehicle,
                as_driver,
            });
        }
    }

    pub fn register_spawned_swat(&mut self, vehicle: VehicleId) {
        self.globals.spawned_swats.insert(vehicle);
    }

    pub fn is_swat_van_nearby(&self, pos: WorldPos, radius: f32) -> bool {
        let radius_sq = radius * radius;
        self.globals.spawned_swats.iter().any(|&id| {
            if !self.registry.contains_vehicle(id) {
                return false;
            }
            if !is_swat_model(self.vehicle_model(id)) {
                return false;
            }
            dist_sq(self.vehicle_pos(id), pos) <= radius_sq
        })
    }

    pub fn is_mod_police_vehicle(&self, id: VehicleId) -> bool {
        is_police_dispatch_model(self.vehicle_model(id))
    }

}

fn vector_from_pos(pos: WorldPos) -> CVector {
    CVector {
        x: pos.x,
        y: pos.y,
        z: pos.z,
    }
}

pub fn crime_dispatch_position(case: &dispatch_case::CaseRecord) -> WorldPos {
    if case.dispatch_anchor.x != 0.0 || case.dispatch_anchor.y != 0.0 {
        case.dispatch_anchor
    } else {
        case.reported_clues
            .suspect_pos
            .or(case.reported_clues.reporter_pos)
            .unwrap_or_default()
    }
}

