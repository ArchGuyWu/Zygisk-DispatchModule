//! Emergency vehicle spawn (ported from `dispatch_tick_main.cpp::dispatch_spawn_emergency_vehicle`).

use std::f32::consts::PI;
use std::sync::Mutex;
use std::time::Instant;

use dispatch_core::{VehicleId, WorldPos};
use dispatch_engine::{entity_model_index, CVector};

use crate::ffi::ExecSymbols;
use crate::game::ExecEnv;
use crate::models::{is_police_dispatch_model, MODEL_AMBULANCE, MODEL_FIRETRUCK};
use crate::spawn_gate::set_custom_spawn_active;

const SPAWN_RING_DIST_M: f32 = 85.0;
/// Minimum interval between two script car spawns (ms).  Creating vehicles too
/// rapidly exhausts CCarCtrl internal generator state and triggers a null write
/// at offset 0x919 inside GenerateEmergencyServicesCar.
const SPAWN_COOLDOWN_MS: u64 = 1200;
static LAST_SPAWN: Mutex<Option<Instant>> = Mutex::new(None);

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
    // Throttle script car creation to avoid exhausting internal CCarCtrl generator
    // state (null deref at offset 0x919 in GenerateEmergencyServicesCar).
    {
        let mut last = LAST_SPAWN.lock().unwrap();
        if let Some(t) = *last {
            if t.elapsed().as_millis() < SPAWN_COOLDOWN_MS as u128 {
                tracing::debug!(model, "vehicle spawn throttled (cooldown)");
                return None;
            }
        }
        *last = Some(Instant::now());
    }

    // Reject invalid positions that would confuse the engine pathfinder.
    if pos.x.is_nan() || pos.y.is_nan() || pos.z.is_nan()
        || pos.x.abs() > 100_000.0 || pos.y.abs() > 100_000.0
    {
        tracing::warn!(model, x = pos.x, y = pos.y, "vehicle spawn: invalid position");
        return None;
    }

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