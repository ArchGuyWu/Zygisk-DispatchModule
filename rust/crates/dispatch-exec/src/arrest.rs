//! Arrest pipeline (ported from `dispatch_cop_arrest.cpp`).

use std::collections::HashMap;

use dispatch_case::CaseRecord;
use dispatch_core::{PedId, VehicleId, WorldPos};

use crate::game::{ExecEnv, ExecGlobals, MAX_TRANSPORT_PRISONERS};
use crate::models::is_police_dispatch_model;
use crate::tasks::assign_arrest_task;
use crate::timing::DRIVE_STYLE_RESPONSE_EMERGENCY;

const TRANSPORT_DEPART_DISTANCE_M: f32 = 260.0;
const TRANSPORT_RELEASE_PLAYER_DIST_M: f32 = 180.0;
const TRANSPORT_RELEASE_MIN_MS: i64 = 12_000;

#[derive(Debug, Clone, Default)]
pub struct CriminalExecState {
    pub arrest_dispatched: bool,
    pub apprehended: bool,
    pub transport_vehicle: Option<VehicleId>,
    pub apprehended_ms: i64,
}

pub fn criminal_in_custody(state: Option<&CriminalExecState>) -> bool {
    state.map(|s| s.apprehended).unwrap_or(false)
}

pub fn criminal_combat_blocked(
    _criminal: PedId,
    arrest_dispatched: bool,
    apprehended: bool,
) -> bool {
    arrest_dispatched || apprehended
}

pub fn criminal_eligible_for_arrest(apprehended: bool) -> bool {
    !apprehended
}

pub fn sync_custody_criminals(
    arrest_states: &HashMap<dispatch_case::CaseId, HashMap<PedId, CriminalExecState>>,
    globals: &mut ExecGlobals,
) {
    globals.custody_criminals.clear();
    for map in arrest_states.values() {
        for (&criminal, state) in map {
            if state.apprehended {
                globals.custody_criminals.insert(criminal);
            }
        }
    }
}

pub fn try_dispatch_arrest(env: &mut ExecEnv<'_>, cop: PedId, criminal: PedId) -> bool {
    if env.globals.custody_criminals.contains(&criminal) {
        return false;
    }
    if !env.registry.contains_ped(cop) || !env.registry.contains_ped(criminal) {
        return false;
    }
    let cop_ptr = env.ped_ptr(cop) as *mut std::ffi::c_void;
    let crim_ptr = env.ped_ptr(criminal) as *mut std::ffi::c_void;
    if cop_ptr.is_null() || crim_ptr.is_null() {
        return false;
    }
    if !assign_arrest_task(env.symbols, cop_ptr, crim_ptr) {
        return false;
    }
    env.globals.arrest_dispatched_criminals.insert(criminal);
    true
}

pub fn sync_arrest_flags_from_globals(
    globals: &crate::game::ExecGlobals,
    states: &mut HashMap<PedId, CriminalExecState>,
) {
    for &criminal in &globals.arrest_dispatched_criminals {
        states.entry(criminal).or_default().arrest_dispatched = true;
    }
}

pub fn tick_case_arrests(
    env: &mut ExecEnv<'_>,
    case: &mut CaseRecord,
    states: &mut HashMap<PedId, CriminalExecState>,
    now_ms: i64,
) {
    if case.cancelled {
        return;
    }

    for &criminal in case.criminals.clone().as_slice() {
        let state = states.entry(criminal).or_default();

        if !state.apprehended {
            if let Some(vehicle) = find_police_custody_vehicle(env, case, criminal) {
                state.apprehended = true;
                state.transport_vehicle = Some(vehicle);
                state.apprehended_ms = now_ms;
                state.arrest_dispatched = false;
                env.globals.register_transport_prisoner(vehicle, criminal);
                tracing::info!(?criminal, ?vehicle, case = case.serial, "criminal apprehended");
            }
        }

        if state.apprehended {
            if let Some(vehicle) = state.transport_vehicle {
                maybe_order_transport_departure(env, case, vehicle, now_ms);
            }
        }

        if state.apprehended
            && transport_ready_for_release(env, state.transport_vehicle, now_ms)
        {
            if let Some(vehicle) = state.transport_vehicle {
                env.globals.release_transport_prisoner(vehicle, criminal);
            }
            case.remove_criminal(criminal);
            states.remove(&criminal);
        }
    }
}

fn find_police_custody_vehicle(
    env: &ExecEnv<'_>,
    case: &CaseRecord,
    criminal: PedId,
) -> Option<VehicleId> {
    let crim_ptr = env.ped_ptr(criminal);
    if crim_ptr.is_null() {
        return None;
    }

    let mut candidates = case.case_vehicles.clone();
    for binding in &env.globals.cop_bindings {
        if !candidates.contains(&binding.vehicle) {
            candidates.push(binding.vehicle);
        }
    }

    for vehicle in candidates {
        if !env.registry.contains_vehicle(vehicle) {
            continue;
        }
        if !is_police_dispatch_model(env.vehicle_model(vehicle)) {
            continue;
        }
        if !transport_vehicle_has_capacity(env, vehicle) {
            continue;
        }
        let veh_ptr = env.vehicle_ptr(vehicle);
        if veh_ptr.is_null() {
            continue;
        }
        let Some(crim) = env.symbols.validate_pool_ped(crim_ptr) else {
            continue;
        };
        let in_vehicle = env.symbols.is_driver_of(veh_ptr, crim)
            || env.symbols.is_passenger_of(veh_ptr, crim);
        if in_vehicle {
            return Some(vehicle);
        }
    }
    None
}

fn transport_vehicle_has_capacity(env: &ExecEnv<'_>, vehicle: VehicleId) -> bool {
    env.globals.transport_prisoner_count(vehicle) < MAX_TRANSPORT_PRISONERS
}

fn maybe_order_transport_departure(
    env: &mut ExecEnv<'_>,
    case: &CaseRecord,
    vehicle: VehicleId,
    now_ms: i64,
) {
    if !env.registry.contains_vehicle(vehicle) {
        return;
    }
    let Some(state) = env.globals.transport_vehicles.get(&vehicle) else {
        return;
    };
    if state.departure_ordered {
        return;
    }
    if state.prisoners.is_empty() {
        return;
    }

    let veh_pos = env.vehicle_pos(vehicle);
    let depart = compute_transport_departure_target(veh_pos, env);
    env.try_get_car_to_go_to_coors(vehicle, depart, DRIVE_STYLE_RESPONSE_EMERGENCY, true);

    let transport = env.globals.transport_vehicles.get_mut(&vehicle).expect("transport");
    transport.departure_ordered = true;
    transport.depart_ms = now_ms;

    tracing::info!(?vehicle, case = case.serial, "transport departure");
}

fn compute_transport_departure_target(anchor: WorldPos, env: &ExecEnv<'_>) -> WorldPos {
    let player_pos = env.symbols.player_coors(0);

    let away_x = anchor.x - player_pos.x;
    let away_y = anchor.y - player_pos.y;
    let away_len = (away_x * away_x + away_y * away_y).sqrt().max(1.0);
    WorldPos {
        x: anchor.x + (away_x / away_len) * TRANSPORT_DEPART_DISTANCE_M,
        y: anchor.y + (away_y / away_len) * TRANSPORT_DEPART_DISTANCE_M,
        z: anchor.z,
    }
}

fn transport_ready_for_release(
    env: &ExecEnv<'_>,
    vehicle: Option<VehicleId>,
    now_ms: i64,
) -> bool {
    let Some(vehicle) = vehicle else {
        return true;
    };
    if !env.registry.contains_vehicle(vehicle) {
        return true;
    }
    let Some(state) = env.globals.transport_vehicles.get(&vehicle) else {
        return true;
    };
    if !state.departure_ordered {
        return false;
    }
    let veh_pos = env.vehicle_pos(vehicle);
    let player_pos = env.symbols.player_coors(0);
    let dist_player = dist_xy(veh_pos, player_pos);
    dist_player >= TRANSPORT_RELEASE_PLAYER_DIST_M
        && (now_ms - state.depart_ms) >= TRANSPORT_RELEASE_MIN_MS
}

fn dist_xy(a: WorldPos, b: WorldPos) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    (dx * dx + dy * dy).sqrt()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn combat_blocked_when_apprehended() {
        let criminal = PedId::default();
        assert!(criminal_combat_blocked(criminal, false, true));
        assert!(!criminal_combat_blocked(criminal, false, false));
        assert!(criminal_combat_blocked(criminal, true, false));
    }
}