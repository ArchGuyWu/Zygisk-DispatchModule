//! Police response commit, nearby mobilization, and vehicle commands
//! (ported from `dispatch_cop_response.cpp` + `dispatch_tick_state_timing.cpp`).

use dispatch_case::{CaseId, CaseRecord, DispatchState};
use dispatch_core::{PedId, VehicleId, WorldPos};
use dispatch_engine::dist_sq;

use crate::dispatch_disturbance::{dispatch_ped_to_disturbance, dispatch_vehicle_to_disturbance};
use crate::game::{crime_dispatch_position, ExecEnv};
use crate::spawn::{build_initial_plan_for_case, schedule_police_vehicle_spawns};
use crate::threat::compute_nearby_dispatch_quota;
use crate::timing::{
    should_attempt_nearby_dispatch, NEARBY_SEARCH_FIREARM_M, NEARBY_SEARCH_MELEE_M,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PerceptionChannel {
    None = 0,
    Heard = 1,
    Seen = 2,
}

#[derive(Debug, Clone, Copy)]
pub struct NearbyCopCandidate {
    pub cop: PedId,
    /// World position cached at scan time — avoids repeated `ped_ptr` + matrix reads.
    pub cop_pos: WorldPos,
    pub dist_sq: f32,
    pub vehicle: Option<VehicleId>,
}

pub struct CommitContext<'a> {
    pub env: ExecEnv<'a>,
    pub road_reachable: bool,
    pub offscreen_allowed: bool,
    pub witness_backing: bool,
    pub is_player_case: bool,
    pub density: i32,
    pub swat_already: bool,
}

pub fn compute_nearby_cop_search_radius(case: &CaseRecord) -> f32 {
    if case.is_firearm {
        NEARBY_SEARCH_FIREARM_M
    } else {
        NEARBY_SEARCH_MELEE_M
    }
}

pub fn compute_nearby_cop_quota(case: &CaseRecord, density: i32) -> i32 {
    compute_nearby_dispatch_quota(case, density)
}

pub fn setup_dispatched_cops(
    env: &mut ExecEnv<'_>,
    vehicle: VehicleId,
    criminal: Option<PedId>,
) {
    if !env.registry.contains_vehicle(vehicle) {
        return;
    }
    if let Some(cop) = criminal {
        env.bind_vehicle_occupants(vehicle, cop, true);
    }
    if !env.vehicle_has_driver(vehicle) {
        tracing::warn!(?vehicle, "setup_dispatched_cops: no driver");
        return;
    }
    if env.arm_vehicle_siren(vehicle) {
        env.globals.vehicles_siren_awakened.insert(vehicle);
        tracing::info!(?vehicle, ?criminal, "native siren armed");
    } else {
        tracing::warn!(?vehicle, "siren arm failed");
    }
}

pub fn dispatch_nearby_available_cops_to_crime(
    env: &mut ExecEnv<'_>,
    case: &mut CaseRecord,
    max_cops: i32,
    search_radius: f32,
    mobilize_center: WorldPos,
    candidates: &[NearbyCopCandidate],
    now_ms: i64,
) -> i32 {
    if env.globals.paused || case.cancelled || max_cops <= 0 {
        return 0;
    }

    let radius_sq = search_radius * search_radius;
    let mut perceived: Vec<(NearbyCopCandidate, PerceptionChannel)> = Vec::new();

    for &candidate in candidates {
        if !env.registry.contains_ped(candidate.cop) {
            continue;
        }
        let dist_sq = dist_sq(candidate.cop_pos, mobilize_center);
        if dist_sq > radius_sq {
            continue;
        }
        let mut entry = candidate;
        entry.dist_sq = dist_sq;
        perceived.push((entry, PerceptionChannel::Seen));
    }

    perceived.sort_by(|a, b| {
        (b.1 as i32)
            .cmp(&(a.1 as i32))
            .then_with(|| a.0.dist_sq.partial_cmp(&b.0.dist_sq).unwrap_or(std::cmp::Ordering::Equal))
    });

    let mut dispatched_cops = std::collections::HashSet::new();
    let mut dispatched_vehicles = std::collections::HashSet::new();
    let mut mobilized = 0;

    for (candidate, channel) in perceived {
        if mobilized >= max_cops {
            break;
        }
        if dispatched_cops.contains(&candidate.cop) {
            continue;
        }
        let route_pos = mobilize_center;

        if let Some(vehicle) = candidate.vehicle.filter(|veh| {
            env.registry.contains_vehicle(*veh)
                && !env.globals.is_transport_vehicle(*veh)
                && !env.globals.is_vehicle_emptied(*veh)
                && env.cop_is_in_vehicle(candidate.cop, *veh)
        }) {
            if dispatched_vehicles.contains(&vehicle) {
                continue;
            }
            let assigned =
                dispatch_vehicle_to_disturbance(env, case, vehicle, route_pos, true);
            if assigned > 0 {
                dispatched_vehicles.insert(vehicle);
                dispatched_cops.insert(candidate.cop);
                mobilized += 1;
            }
            continue;
        }

        if dispatch_ped_to_disturbance(env, case, candidate.cop, route_pos) {
            dispatched_cops.insert(candidate.cop);
            mobilized += 1;
        }
    }

    if mobilized > 0 {
        tracing::info!(case = case.serial, mobilized, max_cops, search_radius, "nearby cops mobilized");
    }
    mobilized
}

pub fn dispatch_nearby_available_cops_for_crime_auto(
    env: &mut ExecEnv<'_>,
    case: &mut CaseRecord,
    candidates: &[NearbyCopCandidate],
    density: i32,
    now_ms: i64,
) -> i32 {
    let quota = compute_nearby_cop_quota(case, density);
    let radius = compute_nearby_cop_search_radius(case);
    let center = crime_dispatch_position(case);
    dispatch_nearby_available_cops_to_crime(env, case, quota, radius, center, candidates, now_ms)
}

pub fn commit_police_response(
    ctx: &mut CommitContext<'_>,
    case: &mut CaseRecord,
    case_id: CaseId,
    nearby_cops: &[NearbyCopCandidate],
    now_ms: i64,
    reason: &str,
) -> bool {
    if case.dispatch_sent || !case.police_script_active() {
        return false;
    }

    let intel_pos = crime_dispatch_position(case);
    let dispatch_pos = intel_pos;

    let mobilized = dispatch_nearby_available_cops_for_crime_auto(
        &mut ctx.env,
        case,
        nearby_cops,
        ctx.density,
        now_ms,
    );

    if mobilized > 0 {
        tracing::info!(case = case.serial, mobilized, %reason, "commit -> ON_SCENE via nearby");
        case.dispatch_sent = true;
        case.spawn_time_ms = now_ms;
        case.on_scene_start_ms = now_ms;
        case.state = DispatchState::OnScene;
        return true;
    }

    if !ctx.offscreen_allowed {
        tracing::warn!(case = case.serial, %reason, "offscreen blocked");
        return false;
    }

    if !ctx.road_reachable {
        tracing::warn!(case = case.serial, %reason, "no road path");
        return false;
    }

    if ctx.is_player_case && !ctx.witness_backing {
        tracing::warn!(case = case.serial, %reason, "player case lacks witness backing");
        return false;
    }

    let density = ctx.density.max(if ctx.is_player_case { 1 } else { 0 });
    let swat = if density >= 6 {
        ctx.env.is_swat_van_nearby(dispatch_pos, 150.0)
    } else {
        ctx.swat_already
    };

    tracing::info!(case = case.serial, density, %reason, "commit -> spawn pending");
    case.dispatch_sent = true;
    case.vehicle_spawn_pending = true;
    case.spawn_time_ms = now_ms;
    case.on_scene_start_ms = 0;
    case.state = DispatchState::OnScene;

    let plan = build_initial_plan_for_case(case, intel_pos, density, swat);
    schedule_police_vehicle_spawns(
        ctx.env.globals,
        case,
        case_id,
        intel_pos,
        intel_pos,
        case.primary,
        plan,
        reason,
        now_ms,
    );
    true
}

pub fn try_mobilize_nearby_cops(
    env: &mut ExecEnv<'_>,
    case: &mut CaseRecord,
    candidates: &[NearbyCopCandidate],
    density: i32,
    now_ms: i64,
    reason: &str,
) -> bool {
    if !should_attempt_nearby_dispatch(case.last_nearby_dispatch_attempt_ms, now_ms) {
        return false;
    }
    case.last_nearby_dispatch_attempt_ms = now_ms;
    let mobilized = dispatch_nearby_available_cops_for_crime_auto(env, case, candidates, density, now_ms);
    if mobilized <= 0 {
        return false;
    }
    tracing::info!(case = case.serial, mobilized, %reason, "nearby retry -> ON_SCENE");
    case.dispatch_sent = true;
    case.spawn_time_ms = now_ms;
    case.on_scene_start_ms = now_ms;
    case.state = DispatchState::OnScene;
    true
}

pub fn make_cops_attack_criminal(
    env: &mut ExecEnv<'_>,
    case: &mut CaseRecord,
    criminal: PedId,
    now_ms: i64,
    is_firearm: bool,
) {
    if env.globals.paused {
        return;
    }
    crate::attack::run_attack_pass(env, case, criminal, now_ms, is_firearm);
}

pub fn make_single_cop_attack_criminal(
    env: &mut ExecEnv<'_>,
    cop: PedId,
    criminal: PedId,
    force_redispatch: bool,
    now_ms: i64,
) {
    if env.globals.paused {
        return;
    }
    if !env.registry.contains_ped(cop) || !env.registry.contains_ped(criminal) {
        return;
    }

    let arrest_dispatched = env.globals.arrest_dispatched_criminals.contains(&criminal);
    let apprehended = env.globals.custody_criminals.contains(&criminal);
    if crate::arrest::criminal_combat_blocked(criminal, arrest_dispatched, apprehended) {
        return;
    }

    if !force_redispatch && cop_is_already_pursuing(env, cop, criminal) {
        return;
    }

    if crate::arrest::try_dispatch_arrest(env, cop, criminal) {
        return;
    }

    let Some(cop_live) = env.live_ped(cop) else {
        return;
    };
    let Some(crim_live) = env.live_ped(criminal) else {
        return;
    };
    env.symbols.add_criminal_to_kill_live(cop_live, crim_live);
    tracing::info!(?cop, ?criminal, "AddCriminalToKill dispatched");
}

pub fn cop_is_already_pursuing(env: &ExecEnv<'_>, cop: PedId, criminal: PedId) -> bool {
    let Some(cop_live) = env.live_ped(cop) else {
        return false;
    };
    let Some(crim) = env.symbols.validate_pool_ped(env.ped_ptr(criminal)) else {
        return false;
    };
    env.symbols
        .weapon_lock_on_target(cop_live)
        .is_some_and(|target| target.as_ptr() == crim.as_ptr())
}

