//! EMS / Fire playbook — spawn asset, one drive command, native on-scene AI completes the rest.

use dispatch_case::{CaseId, CaseRecord, CaseStore};
use dispatch_core::{VehicleId, WorldPos};
use dispatch_engine::entity_model_index;

use crate::game::{crime_dispatch_position, ExecEnv};
use crate::models::{MODEL_AMBULANCE, MODEL_FIRETRUCK};
use crate::vehicle_spawn::resolve_road_spawn_pos;
use crate::timing::{
    AMBULANCE_SPAWN_DELAY_MAX_MS, AMBULANCE_SPAWN_DELAY_MIN_MS, EMERGENCY_EVAL_INTERVAL_MS,
    FIRETRUCK_SPAWN_DELAY_MIN_MS, FIRETRUCK_SPAWN_DELAY_MAX_MS, VEHICLE_IDENTIFY_DELAY_MS,
    VEHICLE_IDENTIFY_RADIUS_M,
};
use crate::vehicle_spawn::dispatch_spawn_emergency_vehicle;

#[derive(Debug, Clone)]
enum EmsTask {
    Spawn {
        case_id: CaseId,
        model: u32,
        spawn_pos: WorldPos,
        anchor: WorldPos,
    },
    Finalize {
        case_id: CaseId,
        model: u32,
        spawn_pos: WorldPos,
        anchor: WorldPos,
    },
}

#[derive(Default)]
pub struct EmergencyCoordinator {
    pending: Vec<(i64, EmsTask)>,
    last_eval_ms: i64,
    case_ids_scratch: Vec<CaseId>,
}

impl EmergencyCoordinator {
    pub fn tick(
        &mut self,
        env: &mut ExecEnv<'_>,
        store: &mut CaseStore,
        now_ms: i64,
    ) {
        if now_ms - self.last_eval_ms >= EMERGENCY_EVAL_INTERVAL_MS {
            self.last_eval_ms = now_ms;
            self.evaluate_needs(store, now_ms);
        }
        self.drain_due(env, store, now_ms);
    }

    fn evaluate_needs(
        &mut self,
        store: &mut CaseStore,
        now_ms: i64,
    ) {
        self.case_ids_scratch.clear();
        self.case_ids_scratch.extend(store.case_ids());
        for &case_id in &self.case_ids_scratch {
            let Some(record) = store.get_mut(case_id) else {
                continue;
            };
            if record.cancelled {
                continue;
            }
            record.refresh_department_needs();
            record.last_emergency_eval_ms = now_ms;
            let anchor = crime_dispatch_position(record);

            if record.fire_script_active() && !record.mod_firetruck_dispatched {
                let delay = spawn_delay_ms(
                    FIRETRUCK_SPAWN_DELAY_MIN_MS,
                    FIRETRUCK_SPAWN_DELAY_MAX_MS,
                    record.serial,
                );
                record.mod_firetruck_dispatched = true;
                let spawn_pos = resolve_road_spawn_pos(anchor, 0);
                self.pending.push((
                    now_ms + delay,
                    EmsTask::Spawn {
                        case_id,
                        model: MODEL_FIRETRUCK,
                        spawn_pos,
                        anchor,
                    },
                ));
                tracing::info!(case = record.serial, delay, "fire script: scheduling firetruck");
            }

            if record.ems_script_active() && !record.mod_ambulance_dispatched {
                let delay = spawn_delay_ms(
                    AMBULANCE_SPAWN_DELAY_MIN_MS,
                    AMBULANCE_SPAWN_DELAY_MAX_MS,
                    record.serial.wrapping_add(1),
                );
                record.mod_ambulance_dispatched = true;
                let spawn_pos = resolve_road_spawn_pos(anchor, 1);
                self.pending.push((
                    now_ms + delay,
                    EmsTask::Spawn {
                        case_id,
                        model: MODEL_AMBULANCE,
                        spawn_pos,
                        anchor,
                    },
                ));
                tracing::info!(
                    case = record.serial,
                    delay,
                    casualties = record.civilian_casualties,
                    "ems script: scheduling ambulance"
                );
            }
        }
    }

    fn drain_due(&mut self, env: &mut ExecEnv<'_>, store: &mut CaseStore, now_ms: i64) {
        let due: Vec<_> = self
            .pending
            .iter()
            .filter(|(t, _)| *t <= now_ms)
            .cloned()
            .collect();
        self.pending.retain(|(t, _)| *t > now_ms);

        for (_, task) in due {
            match task {
                EmsTask::Spawn {
                    case_id,
                    model,
                    spawn_pos,
                    anchor,
                } => {
                    let Some(record) = store.get_mut(case_id) else {
                        continue;
                    };
                    if record.cancelled {
                        reset_dispatch_flag(record, model);
                        continue;
                    }
                    if let Some(vehicle) =
                        dispatch_spawn_emergency_vehicle(env, model, spawn_pos, anchor)
                    {
                        tracing::info!(case = record.serial, ?vehicle, model, "EMS spawn ok");
                    } else {
                        tracing::warn!(case = record.serial, model, "EMS spawn failed");
                        reset_dispatch_flag(record, model);
                        continue;
                    }
                    self.pending.push((
                        now_ms + VEHICLE_IDENTIFY_DELAY_MS,
                        EmsTask::Finalize {
                            case_id,
                            model,
                            spawn_pos,
                            anchor,
                        },
                    ));
                }
                EmsTask::Finalize {
                    case_id,
                    model,
                    spawn_pos,
                    anchor,
                } => {
                    let Some(record) = store.get_mut(case_id) else {
                        continue;
                    };
                    if record.cancelled {
                        reset_dispatch_flag(record, model);
                        continue;
                    }
                    finalize_spawn(env, record, model, spawn_pos, anchor);
                }
            }
        }
    }
}

fn finalize_spawn(
    env: &mut ExecEnv<'_>,
    record: &mut CaseRecord,
    model: u32,
    spawn_pos: WorldPos,
    anchor: WorldPos,
) {
    let Some(vehicle) = find_closest_vehicle_near(env, spawn_pos, model, VEHICLE_IDENTIFY_RADIUS_M) else {
        tracing::warn!(case = record.serial, model, "EMS finalize: vehicle not found");
        reset_dispatch_flag(record, model);
        return;
    };
    if !record.case_vehicles.contains(&vehicle) {
        record.case_vehicles.push(vehicle);
    }
    let assigned = crate::dispatch_disturbance::dispatch_vehicle_to_disturbance(
        env, record, vehicle, anchor, false,
    );
    tracing::info!(
        case = record.serial,
        ?vehicle,
        model,
        assigned,
        "EMS script: investigate disturbance to scene"
    );
}

fn find_closest_vehicle_near(
    env: &ExecEnv<'_>,
    pos: WorldPos,
    model: u32,
    radius_m: f32,
) -> Option<VehicleId> {
    let pool = env.symbols.open_vehicle_pool()?;
    let radius_sq = radius_m * radius_m;
    let mut best: Option<(VehicleId, f32)> = None;

    for slot in 0..pool.size as usize {
        let flag = pool.byte_map[slot];
        if flag < 0 {
            continue;
        }
        let key = dispatch_core::PoolKey::from_slot_flag(slot as u16, flag as u8);
        let Some(veh_ptr) = env.symbols.entity_from_vehicle_key(key) else {
            continue;
        };
        if veh_ptr.is_null() || entity_model_index(veh_ptr) as u32 != model {
            continue;
        }
        let veh_pos = env.symbols.entity_world_pos(veh_ptr);
        let dx = veh_pos.x - pos.x;
        let dy = veh_pos.y - pos.y;
        let dz = veh_pos.z - pos.z;
        let dist_sq = dx * dx + dy * dy + dz * dz;
        if dist_sq > radius_sq {
            continue;
        }
        let Some(id) = env.registry.vehicle_by_pool(dispatch_core::PoolKey::from_slot_flag(
            slot as u16,
            flag as u8,
        )) else {
            continue;
        };
        if best.map(|(_, d)| dist_sq < d).unwrap_or(true) {
            best = Some((id, dist_sq));
        }
    }
    best.map(|(id, _)| id)
}

fn spawn_delay_ms(min_ms: i64, max_ms: i64, seed: u64) -> i64 {
    let span = (max_ms - min_ms).max(0);
    min_ms + (seed % (span as u64 + 1)) as i64
}

fn reset_dispatch_flag(record: &mut CaseRecord, model: u32) {
    if model == MODEL_AMBULANCE {
        record.mod_ambulance_dispatched = false;
    } else if model == MODEL_FIRETRUCK {
        record.mod_firetruck_dispatched = false;
    }
}