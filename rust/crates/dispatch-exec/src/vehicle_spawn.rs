//! Emergency vehicle spawn (ported from `dispatch_tick_main.cpp::dispatch_spawn_emergency_vehicle`).

use std::f32::consts::PI;

use dispatch_core::{VehicleId, WorldPos};
use dispatch_engine::{entity_model_index, CVector};

use crate::ffi::ExecSymbols;
use crate::game::ExecEnv;
use crate::models::{is_police_dispatch_model, MODEL_AMBULANCE, MODEL_FIRETRUCK};
use crate::spawn_gate::set_custom_spawn_active;

const SPAWN_RING_DIST_M: f32 = 85.0;
/// Maximum distance from player for emergency vehicle spawn.  Beyond this range
/// the engine's navmesh / CCarCtrl state isn't loaded, causing a null write at
/// offset 0x919 inside `GenerateEmergencyServicesCar`.
const STREAMING_MAX_DIST_M: f32 = 75.0;
const STREAMING_TARGET_DIST_M: f32 = 60.0;

pub fn resolve_road_spawn_pos(anchor: WorldPos, unit_index: usize) -> WorldPos {
    let angle = (unit_index as f32) * PI / 2.0;
    WorldPos {
        x: anchor.x + angle.cos() * SPAWN_RING_DIST_M,
        y: anchor.y + angle.sin() * SPAWN_RING_DIST_M,
        z: anchor.z,
    }
}

/// Clamp a spawn position to within streaming range of the player so that
/// CCarCtrl navmesh/generator state is loaded.  Spawning beyond this range
/// hits a null write at offset 0x919 in GenerateEmergencyServicesCar.
pub fn clamp_spawn_to_streaming_range(player_pos: WorldPos, proposed: WorldPos) -> WorldPos {
    let dx = proposed.x - player_pos.x;
    let dy = proposed.y - player_pos.y;
    let dist_xy = (dx * dx + dy * dy).sqrt();
    if dist_xy <= STREAMING_MAX_DIST_M {
        return proposed;
    }
    let scale = STREAMING_TARGET_DIST_M / dist_xy;
    WorldPos {
        x: player_pos.x + dx * scale,
        y: player_pos.y + dy * scale,
        z: proposed.z,
    }
}

pub fn dispatch_spawn_emergency_vehicle(
    env: &mut ExecEnv<'_>,
    model: u32,
    pos: WorldPos,
    incident_anchor: WorldPos,
) -> Option<VehicleId> {
    // Clamp ambulance/firetruck spawns to streaming range to prevent the engine's
    // GenerateEmergencyServicesCar from writing to a null CCarCtrl member at
    // offset 0x919 when navmesh data isn't loaded at the target position.
    let pos = if model == MODEL_AMBULANCE || model == MODEL_FIRETRUCK {
        let player_pos = env.symbols.entity_world_pos(env.symbols.player_ped(0));
        clamp_spawn_to_streaming_range(player_pos, pos)
    } else {
        pos
    };

    let cvec = CVector {
        x: pos.x,
        y: pos.y,
        z: pos.z,
    };

    set_custom_spawn_active(true);
    let veh_ptr = env.symbols.spawn_car_for_script(model as i32, cvec, 1);
    set_custom_spawn_active(false);

    if veh_ptr.is_null() {
        tracing::warn!(model, x = pos.x, y = pos.y, "CreateCarForScript failed");
        return None;
    }

    let spawned_model = entity_model_index(veh_ptr as *const _);
    if spawned_model as u32 != model {
        tracing::warn!(
            model,
            spawned_model,
            "spawn model mismatch (streaming?)"
        );
        return None;
    }

    if !try_add_emergency_vehicle_occupants(env.symbols, veh_ptr, model) {
        tracing::warn!(model, "spawn: no emergency occupants");
        return None;
    }

    let key = env.symbols.lookup_vehicle_key(veh_ptr as *const _)?;
    let vehicle_id = env.registry.adopt_vehicle(key, model);
    env.globals
        .cop_bindings
        .retain(|b| env.registry.contains_ped(b.cop));

    tracing::info!(model, ?vehicle_id, x = pos.x, y = pos.y, "spawn ok");
    Some(vehicle_id)
}

pub fn try_add_police_car_occupants(
    symbols: &ExecSymbols,
    vehicle: *mut std::ffi::c_void,
) -> bool {
    if vehicle.is_null() {
        return false;
    }
    unsafe {
        (symbols.add_police_car_occupants)(vehicle, true);
    }
    vehicle_has_driver(symbols, vehicle)
}

pub fn try_add_emergency_vehicle_occupants(
    symbols: &ExecSymbols,
    vehicle: *mut std::ffi::c_void,
    model: u32,
) -> bool {
    if vehicle.is_null() {
        return false;
    }
    match model {
        m if is_police_dispatch_model(m) => try_add_police_car_occupants(symbols, vehicle),
        MODEL_AMBULANCE => {
            unsafe {
                (symbols.add_ambulance_occupants)(vehicle);
            }
            vehicle_has_driver(symbols, vehicle)
        }
        MODEL_FIRETRUCK => {
            unsafe {
                (symbols.add_firetruck_occupants)(vehicle);
            }
            vehicle_has_driver(symbols, vehicle)
        }
        _ => true,
    }
}

/// Native siren/alarm setup — `vehicles_siren_awakened` alone does not touch the engine.
pub fn arm_emergency_vehicle_siren(
    symbols: &ExecSymbols,
    vehicle: *mut std::ffi::c_void,
    model: u32,
) -> bool {
    if vehicle.is_null() {
        return false;
    }
    match model {
        m if is_police_dispatch_model(m) => {
            unsafe {
                (symbols.add_police_car_occupants)(vehicle, true);
            }
            true
        }
        MODEL_AMBULANCE => {
            unsafe {
                (symbols.add_ambulance_occupants)(vehicle);
            }
            true
        }
        MODEL_FIRETRUCK => {
            unsafe {
                (symbols.add_firetruck_occupants)(vehicle);
            }
            true
        }
        _ => false,
    }
}

fn vehicle_has_driver(symbols: &ExecSymbols, vehicle: *mut std::ffi::c_void) -> bool {
    if vehicle.is_null() {
        return false;
    }
    let Some(pool) = symbols.open_ped_pool() else {
        return false;
    };
    for slot in 0..pool.size as usize {
        let flag = pool.byte_map[slot];
        if flag < 0 {
            continue;
        }
        let key = dispatch_core::PoolKey::from_slot_flag(slot as u16, flag as u8);
        let Some(ped_ptr) = symbols.entity_from_ped_key(key) else {
            continue;
        };
        let Some(pool_ped) = symbols.validate_pool_ped(ped_ptr) else {
            continue;
        };
        if symbols.is_driver_of(vehicle as *const _, pool_ped) {
            return true;
        }
    }
    false
}