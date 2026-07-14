//! Police vehicle spawn planning and scheduling (ported from `dispatch_police_spawn.cpp`).

use dispatch_case::{CaseId, CaseRecord};
use dispatch_core::{PedId, VehicleId, WorldPos};

use crate::game::{crime_dispatch_position, ExecEnv, ExecGlobals, PoliceSpawnChain};
use crate::models::{detect_map_region, MapRegion, PoliceSpawnUnit, SpawnTask};

use crate::threat::build_initial_spawn_plan;
use crate::timing::{
    REINFORCEMENT_BATCH_RELEASE_TIMEOUT_MS, REINFORCEMENT_SPAWN_STAGGER_RURAL_MS,
    REINFORCEMENT_SPAWN_STAGGER_URBAN_MS,
};
use crate::vehicle_spawn::{dispatch_spawn_emergency_vehicle, resolve_road_spawn_pos};

pub fn spawn_stagger_ms_for_loc(loc: WorldPos) -> i64 {
    if detect_map_region(loc) == MapRegion::Countryside {
        REINFORCEMENT_SPAWN_STAGGER_RURAL_MS
    } else {
        REINFORCEMENT_SPAWN_STAGGER_URBAN_MS
    }
}

pub fn schedule_police_vehicle_spawns(
    globals: &mut ExecGlobals,
    case: &CaseRecord,
    case_id: CaseId,
    target_pos: WorldPos,
    loc: WorldPos,
    criminal: Option<PedId>,
    units: Vec<PoliceSpawnUnit>,
    reason: &str,
    now_ms: i64,
) -> Vec<(i64, SpawnTask)> {
    if units.is_empty() {
        tracing::warn!(case = case.serial, reason, "spawn skip empty plan");
        return Vec::new();
    }

    let chain_id = globals.next_chain_id;
    globals.next_chain_id += 1;

    globals.spawn_chains.insert(
        chain_id,
        PoliceSpawnChain {
            case_serial: case.serial,
            target_pos,
            loc,
            criminal,
            units: units.clone(),
            reason: reason.to_string(),
            units_completed: 0,
            released: false,
            release_timeout_scheduled: false,
            held_vehicles: Vec::new(),
            held_staging_centers: Vec::new(),
        },
    );

    tracing::info!(
        case = case.serial,
        ?case_id,
        units = units.len(),
        reason,
        x = loc.x,
        y = loc.y,
        "spawn scheduled"
    );

    globals
        .pending_spawn
        .push((case_id, now_ms, SpawnTask::BeginChain { chain_id }));
    vec![(now_ms, SpawnTask::BeginChain { chain_id })]
}

pub fn begin_police_spawn_chain(
    env: &mut ExecEnv<'_>,
    case_id: CaseId,
    case: &mut CaseRecord,
    chain_id: u64,
    now_ms: i64,
) -> Vec<(i64, SpawnTask)> {
    let Some(chain) = env.globals.spawn_chains.get(&chain_id) else {
        return Vec::new();
    };
    if chain.units.is_empty() {
        return Vec::new();
    }
    if let Some((due, task)) = schedule_batch_release_timeout(env.globals, chain_id, now_ms) {
        env.globals.pending_spawn.push((case_id, due, task));
    }
    vec![(now_ms, SpawnTask::SpawnUnit {
        chain_id,
        index: 0,
        attempt: 0,
    })]
}

pub fn execute_spawn_unit(
    env: &mut ExecEnv<'_>,
    case_id: CaseId,
    case: &mut CaseRecord,
    chain_id: u64,
    index: usize,
    attempt: u32,
    now_ms: i64,
) -> Vec<(i64, SpawnTask)> {
    let Some(chain) = env.globals.spawn_chains.get(&chain_id).cloned() else {
        return Vec::new();
    };
    if index >= chain.units.len() {
        return Vec::new();
    }

    let unit = chain.units[index].clone();
    let road_spawn = resolve_road_spawn_pos(chain.loc, index);
    let vehicle = dispatch_spawn_emergency_vehicle(env, unit.model, road_spawn, chain.loc);

    let mut follow_up = Vec::new();

    if vehicle.is_none() {
        let done_tasks = on_spawn_unit_done(env, chain_id, case, now_ms);
        follow_up.extend(done_tasks);
        if index + 1 < chain.units.len() {
            let stagger = spawn_stagger_ms_for_loc(chain.loc);
            follow_up.push((
                now_ms + stagger,
                SpawnTask::SpawnUnit {
                    chain_id,
                    index: index + 1,
                    attempt: 0,
                },
            ));
        }
        return follow_up;
    }

    let vehicle = vehicle.unwrap();
    register_held_spawn_unit(
        env.globals,
        chain_id,
        vehicle,
        &unit,
        index == 0,
        case,
        env.vehicle_pos(vehicle),
    );
    env.bind_vehicle_occupants_from_vehicle(vehicle);

    let done_tasks = on_spawn_unit_done(env, chain_id, case, now_ms);
    follow_up.extend(done_tasks);
    if index + 1 < chain.units.len() {
        let stagger = spawn_stagger_ms_for_loc(chain.loc);
        follow_up.push((
            now_ms + stagger,
            SpawnTask::SpawnUnit {
                chain_id,
                index: index + 1,
                attempt: 0,
            },
        ));
    }

    let _ = case_id;
    follow_up
}

pub fn handle_batch_release_timeout(
    env: &mut ExecEnv<'_>,
    case: &mut CaseRecord,
    chain_id: u64,
    now_ms: i64,
) {
    let Some(chain) = env.globals.spawn_chains.get(&chain_id) else {
        return;
    };
    if chain.released || chain.units_completed >= chain.units.len() {
        return;
    }
    tracing::warn!(
        case = case.serial,
        chain_id,
        held = chain.held_vehicles.len(),
        "spawn batch timeout — early release"
    );
    if let Some(chain_mut) = env.globals.spawn_chains.get_mut(&chain_id) {
        chain_mut.units_completed = chain_mut.units.len();
    }
    release_police_spawn_batch(env, chain_id, case, now_ms);
}

pub fn on_spawn_unit_done(
    env: &mut ExecEnv<'_>,
    chain_id: u64,
    case: &mut CaseRecord,
    now_ms: i64,
) -> Vec<(i64, SpawnTask)> {
    let Some(chain) = env.globals.spawn_chains.get_mut(&chain_id) else {
        return Vec::new();
    };
    chain.units_completed += 1;
    let should_release = chain.units_completed >= chain.units.len();
    if should_release {
        release_police_spawn_batch(env, chain_id, case, now_ms);
    }
    Vec::new()
}

pub fn release_police_spawn_batch(
    env: &mut ExecEnv<'_>,
    chain_id: u64,
    case: &mut CaseRecord,
    now_ms: i64,
) {
    let Some(chain) = env.globals.spawn_chains.get_mut(&chain_id) else {
        return;
    };
    if chain.released {
        return;
    }
    chain.released = true;

    let loc = crime_dispatch_position(case);
    let reason = chain.reason.clone();
    let held: Vec<_> = chain.held_vehicles.clone();
    let centers: Vec<_> = chain.held_staging_centers.clone();

    if held.is_empty() {
        case.vehicle_spawn_pending = false;
        tracing::info!(case = case.serial, %reason, "spawn batch empty");
        return;
    }

    tracing::info!(case = case.serial, count = held.len(), %reason, "releasing spawn batch");

    for (i, vehicle) in held.iter().enumerate() {
        if i < centers.len() {
            env.globals.staging.end_staging_area_closure(centers[i]);
        }
        if !env.registry.contains_vehicle(*vehicle) {
            continue;
        }
        crate::dispatch_disturbance::dispatch_vehicle_to_disturbance(
            env, case, *vehicle, loc, true,
        );
    }

    case.vehicle_spawn_pending = false;
    case.on_scene_start_ms = now_ms;
    case.spawn_time_ms = now_ms;
}

pub fn register_held_spawn_unit(
    globals: &mut ExecGlobals,
    chain_id: u64,
    vehicle: VehicleId,
    unit: &PoliceSpawnUnit,
    set_primary: bool,
    case: &mut CaseRecord,
    hold_pos: WorldPos,
) {
    if let Some(chain) = globals.spawn_chains.get_mut(&chain_id) {
        chain.held_vehicles.push(vehicle);
        chain.held_staging_centers.push(hold_pos);
    }
    if set_primary || case.spawned_vehicle.is_none() {
        case.spawned_vehicle = Some(vehicle);
    }
    if !case.case_vehicles.contains(&vehicle) {
        case.case_vehicles.push(vehicle);
    }
    if unit.register_swat {
        globals.spawned_swats.insert(vehicle);
    }
    globals.staging.begin_staging_area_closure(hold_pos);
}

pub fn build_initial_plan_for_case(
    case: &CaseRecord,
    loc: WorldPos,
    density: i32,
    swat_already: bool,
) -> Vec<PoliceSpawnUnit> {
    build_initial_spawn_plan(case, loc, density, swat_already)
}

pub fn schedule_batch_release_timeout(
    globals: &mut ExecGlobals,
    chain_id: u64,
    now_ms: i64,
) -> Option<(i64, SpawnTask)> {
    let chain = globals.spawn_chains.get_mut(&chain_id)?;
    if chain.release_timeout_scheduled {
        return None;
    }
    chain.release_timeout_scheduled = true;
    Some((
        now_ms + REINFORCEMENT_BATCH_RELEASE_TIMEOUT_MS,
        SpawnTask::BatchReleaseTimeout { chain_id },
    ))
}