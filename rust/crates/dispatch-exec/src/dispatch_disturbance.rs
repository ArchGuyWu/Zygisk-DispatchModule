//! Assign `CTaskComplexInvestigateDisturbance` once — native engine goes to the scene.

use dispatch_case::CaseRecord;
use dispatch_core::{PedId, VehicleId, WorldPos};

use crate::game::ExecEnv;
use crate::response::setup_dispatched_cops;
use crate::tasks::assign_investigate_disturbance_task;

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
    tracing::info!(?ped, ?anchor, case = record.serial, "ped dispatched via investigate disturbance");
    true
}

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

    let occupants: Vec<PedId> = env
        .globals
        .cop_bindings
        .iter()
        .filter(|b| b.vehicle == vehicle)
        .map(|b| b.cop)
        .collect();

    let mut assigned = 0;
    for ped in occupants {
        if dispatch_ped_to_disturbance(env, record, ped, anchor) {
            assigned += 1;
        }
    }

    if assigned > 0 {
        env.globals.add_vehicle_ordered_to_scene(vehicle);
        tracing::info!(
            ?vehicle,
            assigned,
            ?anchor,
            case = record.serial,
            "vehicle occupants dispatched via investigate disturbance"
        );
    }
    assigned
}