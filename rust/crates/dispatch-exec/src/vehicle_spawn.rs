//! Emergency vehicle spawn (ported from `dispatch_tick_main.cpp::dispatch_spawn_emergency_vehicle`).

use std::f32::consts::PI;

use dispatch_core::{VehicleId, WorldPos};
use dispatch_engine::{entity_model_index, CVector};

use crate::ffi::ExecSymbols;
use crate::game::ExecEnv;
use crate::models::{is_police_dispatch_model, MODEL_AMBULANCE, MODEL_FIRETRUCK};
use crate::spawn_gate::set_custom_spawn_active;

const SPAWN_RING_DIST_M: f32 = 85.0;

pub fn resolve_road_spawn_pos(anchor: WorldPos, unit_index: usize) -> WorldPos {
    let angle = (unit_index as f32) * PI / 2.0;
    WorldPos {
        x: anchor.x + angle.cos() * SPAWN_RING_DIST_M,
        y: anchor.y + angle.sin() * SPAWN_RING_DIST_M,
        z: anchor.z,
    }
}

pub fn dispatch_spawn_emergency_vehicle(
    env: &mut ExecEnv<'_>,
    model: u32,
    pos: WorldPos,
    _incident_anchor: WorldPos,
) -> Option<VehicleId> {
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

#[cfg(test)]
mod spawn_pos_tests {
    use super::*;

    /// Drive the real shipped `resolve_road_spawn_pos` (not a reimplementation).
    #[test]
    fn unit_zero_is_east_of_anchor_at_ring_radius() {
        let anchor = WorldPos {
            x: 100.0,
            y: 200.0,
            z: 10.0,
        };
        let p = resolve_road_spawn_pos(anchor, 0);
        let dx = p.x - anchor.x;
        let dy = p.y - anchor.y;
        let dist = (dx * dx + dy * dy).sqrt();
        assert!((dist - SPAWN_RING_DIST_M).abs() < 0.01);
        assert!((dx - SPAWN_RING_DIST_M).abs() < 0.01);
        assert!(dy.abs() < 0.01);
        assert_eq!(p.z, anchor.z);
    }

    #[test]
    fn successive_indices_are_quarter_turns_apart() {
        let anchor = WorldPos {
            x: 0.0,
            y: 0.0,
            z: 0.0,
        };
        let p0 = resolve_road_spawn_pos(anchor, 0);
        let p1 = resolve_road_spawn_pos(anchor, 1);
        // index 1 → angle π/2 → north (+Y)
        assert!(p0.x > 0.0 && p0.y.abs() < 0.01);
        assert!(p1.y > 0.0 && p1.x.abs() < 0.01);
        let d0 = (p0.x * p0.x + p0.y * p0.y).sqrt();
        let d1 = (p1.x * p1.x + p1.y * p1.y).sqrt();
        assert!((d0 - SPAWN_RING_DIST_M).abs() < 0.01);
        assert!((d1 - SPAWN_RING_DIST_M).abs() < 0.01);
    }
}