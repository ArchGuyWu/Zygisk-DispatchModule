//! Scene approach: foot uses InvestigateDisturbance; vehicles use CarAI go-to.

use dispatch_case::CaseRecord;
use dispatch_core::{PedId, VehicleId, WorldPos};

use crate::game::ExecEnv;
use crate::response::setup_dispatched_cops;
use crate::tasks::assign_investigate_disturbance_task;
use crate::timing::{driving_style_for_case, DRIVE_STYLE_RESPONSE_EMERGENCY};

/// On-foot: `CTaskComplexInvestigateDisturbance` to anchor (walk → stand → look).
pub fn dispatch_ped_to_disturbance(
    env: &mut ExecEnv<'_>,
    record: &mut CaseRecord,
    ped: PedId,
    anchor: WorldPos,
) -> bool {
    if !env.registry.contains_ped(ped) {
        return false;
    }
    let ptr = env.ped_ptr(ped) as *mut std::ffi::c_void;
    if !assign_investigate_disturbance_task(env.symbols, ptr, anchor) {
        return false;
    }
    record.register_scene_responder(ped);
    tracing::info!(
        ?ped,
        ?anchor,
        case = record.serial,
        "ped: InvestigateDisturbance to scene"
    );
    true
}

/// In-vehicle response: **drive** via `CCarAI::GetCarToGoToCoors` to the anchor.
///
/// Does **not** force InvestigateDisturbance on seated occupants (that fights autopilot
/// and turns long approaches into walk-outs). Occupants are registered as responders only.
///
/// Engine behaviour near the point: goto mission completes / clears → vehicle stops.
/// There is no separate "park and idle for N ms" CarAI entry used here; `GetCarToParkAtCoors`
/// only sets `bCanPark` + low cruise and is not sufficient alone.
pub fn dispatch_vehicle_to_disturbance(
    env: &mut ExecEnv<'_>,
    record: &mut CaseRecord,
    vehicle: VehicleId,
    anchor: WorldPos,
    arm_police_siren: bool,
) -> i32 {
    if !env.registry.contains_vehicle(vehicle) || env.globals.is_transport_vehicle(vehicle) {
        return 0;
    }

    if !record.case_vehicles.contains(&vehicle) {
        record.case_vehicles.push(vehicle);
    }
    if record.spawned_vehicle.is_none() {
        record.spawned_vehicle = Some(vehicle);
    }

    env.bind_vehicle_occupants_from_vehicle(vehicle);
    if arm_police_siren {
        setup_dispatched_cops(env, vehicle, record.primary);
    } else {
        env.arm_vehicle_siren(vehicle);
    }

    let style = if arm_police_siren {
        driving_style_for_case(record.is_firearm)
    } else {
        // Ambulance / firetruck: emergency response style.
        DRIVE_STYLE_RESPONSE_EMERGENCY
    };

    if !env.command_vehicle_to_scene(vehicle, anchor, style) {
        return 0;
    }

    let occupants: Vec<PedId> = env
        .globals
        .cop_bindings
        .iter()
        .filter(|b| b.vehicle == vehicle)
        .map(|b| b.cop)
        .collect();

    let mut assigned = 0i32;
    for ped in occupants {
        record.register_scene_responder(ped);
        assigned += 1;
    }
    // Count the vehicle as dispatched even if occupant bind missed (driver still drives).
    if assigned == 0 {
        assigned = 1;
    }

    env.globals.add_vehicle_ordered_to_scene(vehicle);
    tracing::info!(
        ?vehicle,
        assigned,
        ?anchor,
        style,
        case = record.serial,
        "vehicle: CarAI go-to scene (no foot InvestigateDisturbance on crew)"
    );
    assigned
}
