//! Cop attack orchestration (ported from `dispatch_cop_attack.cpp` + pass logic).

use std::collections::HashSet;

use dispatch_core::PedId;

use crate::game::ExecEnv;
use crate::threat::compute_response_quota;
use crate::timing::av_range_for_firearm;

#[derive(Debug, Default)]
pub struct CopAttackContext {
    pub criminal: PedId,
    pub crime_pos: dispatch_core::WorldPos,
    pub is_active_firearm: bool,
    pub active_vehicles_count: i32,
    pub active_foot_cops_count: i32,
    pub max_vehicles: i32,
    pub max_foot_cops: i32,
    pub av_range_sq: f32,
    pub vehicles_emptied: HashSet<dispatch_core::VehicleId>,
    pub vehicles_ordered_to_scene: HashSet<dispatch_core::VehicleId>,
    pub vehicles_siren_awakened: HashSet<dispatch_core::VehicleId>,
    pub counted_vehicles: Vec<dispatch_core::VehicleId>,
}

impl CopAttackContext {
    pub fn reset(&mut self) {
        self.active_vehicles_count = 0;
        self.active_foot_cops_count = 0;
        self.counted_vehicles.clear();
    }

    pub fn snapshot_globals(&mut self, env: &ExecEnv<'_>) {
        self.vehicles_emptied = env.globals.vehicles_emptied.clone();
        self.vehicles_ordered_to_scene = env.globals.vehicles_ordered_to_scene.clone();
        self.vehicles_siren_awakened = env.globals.vehicles_siren_awakened.clone();
    }

    pub fn commit_pending(&self, env: &mut ExecEnv<'_>) {
        for id in &self.vehicles_emptied {
            env.globals.vehicles_emptied.insert(*id);
        }
        for id in &self.vehicles_ordered_to_scene {
            env.globals.vehicles_ordered_to_scene.insert(*id);
        }
        for id in &self.vehicles_siren_awakened {
            env.globals.vehicles_siren_awakened.insert(*id);
        }
    }
}

#[derive(Debug, Clone)]
pub struct CopPoolEntry {
    pub ped: PedId,
    pub cop_pos: dispatch_core::WorldPos,
    pub vehicle: Option<dispatch_core::VehicleId>,
    pub target_criminal: PedId,
    pub target_crime_pos: dispatch_core::WorldPos,
    pub dist_sq: f32,
}

pub fn run_attack_pass(
    env: &mut ExecEnv<'_>,
    case: &mut dispatch_case::CaseRecord,
    criminal: PedId,
    now_ms: i64,
    is_firearm: bool,
) {
    if env.ped_ptr(criminal).is_null() {
        return;
    }
    env.sync_natural_vehicle_exits(now_ms);
    let mut ctx = CopAttackContext {
        criminal,
        crime_pos: env.ped_pos(criminal),
        ..Default::default()
    };
    ctx.is_active_firearm = is_firearm;
    ctx.av_range_sq = {
        let r = av_range_for_firearm(ctx.is_active_firearm);
        r * r
    };
    compute_quotas(&mut ctx);
    let entries = build_cop_pool_entries(env, &ctx);
    ctx.snapshot_globals(env);
    count_active_from_entries(&mut ctx, &entries, &env.globals);
    dispatch_entries(env, &mut ctx, &entries, now_ms);
    for entry in &entries {
        case.register_scene_responder(entry.ped);
    }
    ctx.commit_pending(env);
}

fn compute_quotas(ctx: &mut CopAttackContext) {
    let quota = compute_response_quota(&dispatch_case::CaseRecord::new(
        0,
        None,
        None,
        None,
        Default::default(),
        vec![ctx.criminal],
        0,
        8000,
    ));
    ctx.max_vehicles = quota.max_vehicles;
    ctx.max_foot_cops = quota.max_foot_cops;
}

fn dispatch_entries(
    env: &mut ExecEnv<'_>,
    ctx: &mut CopAttackContext,
    entries: &[CopPoolEntry],
    now_ms: i64,
) {
    for entry in entries {
        if let Some(vehicle) = entry.vehicle {
            if env.registry.contains_vehicle(vehicle) {
                crate::attack_vehicle::dispatch_vehicle_cop(
                    env,
                    ctx,
                    entry.ped,
                    entry.target_criminal,
                    entry.target_crime_pos,
                    vehicle,
                    now_ms,
                );
                continue;
            }
        }
        crate::attack_foot::dispatch_foot_cop(
            env,
            ctx,
            entry.ped,
            entry.target_criminal,
            entry.target_crime_pos,
            entry.dist_sq,
            now_ms,
        );
    }
}

fn build_cop_pool_entries(env: &ExecEnv<'_>, ctx: &CopAttackContext) -> Vec<CopPoolEntry> {
    let mut entries = Vec::new();

    for binding in &env.globals.cop_bindings {
        if !env.registry.contains_ped(binding.cop) {
            continue;
        }
        if binding.cop == ctx.criminal {
            continue;
        }
        let cop_pos = env.ped_pos(binding.cop);
        let target = ctx.criminal;
        let target_pos = ctx.crime_pos;
        let dist_sq = dispatch_engine::dist_sq(cop_pos, target_pos);
        entries.push(CopPoolEntry {
            ped: binding.cop,
            cop_pos,
            vehicle: Some(binding.vehicle),
            target_criminal: target,
            target_crime_pos: target_pos,
            dist_sq,
        });
    }

    entries
}

fn count_active_from_entries(
    ctx: &mut CopAttackContext,
    entries: &[CopPoolEntry],
    globals: &crate::game::ExecGlobals,
) {
    ctx.active_vehicles_count = 0;
    ctx.active_foot_cops_count = 0;
    ctx.counted_vehicles.clear();

    for entry in entries {
        if let Some(vehicle) = entry.vehicle {
            if globals.vehicles_ordered_to_scene.contains(&vehicle)
                || globals.vehicles_siren_awakened.contains(&vehicle)
            {
                if !ctx.counted_vehicles.contains(&vehicle) {
                    ctx.counted_vehicles.push(vehicle);
                    ctx.active_vehicles_count += 1;
                }
            }
        } else {
            ctx.active_foot_cops_count += 1;
        }
    }
}