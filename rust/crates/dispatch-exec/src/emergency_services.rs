//! EMS / Fire department scripts (dispatch module — not police-only).
//!
//! Flow: department needs → delayed spawn (CreateCarForScript) → drive to scene.
//! Native medic/fire AI takes over on scene; we do not micro-manage tasks beyond route.

use dispatch_case::{CaseId, CaseRecord, CaseStore};
use dispatch_core::{VehicleId, WorldPos};

use crate::game::{crime_dispatch_position, ExecEnv};
use crate::models::{MODEL_AMBULANCE, MODEL_FIRETRUCK};
use crate::timing::{
    AMBULANCE_SPAWN_DELAY_MAX_MS, AMBULANCE_SPAWN_DELAY_MIN_MS, EMERGENCY_EVAL_INTERVAL_MS,
    FIRETRUCK_SPAWN_DELAY_MAX_MS, FIRETRUCK_SPAWN_DELAY_MIN_MS,
};
use crate::vehicle_spawn::{dispatch_spawn_emergency_vehicle, resolve_road_spawn_pos};

#[derive(Debug, Clone)]
enum EmsTask {
    /// Spawn then immediately route — no second full-pool “identify” pass.
    Dispatch {
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
    pub fn tick(&mut self, env: &mut ExecEnv<'_>, store: &mut CaseStore, now_ms: i64) {
        if now_ms - self.last_eval_ms >= EMERGENCY_EVAL_INTERVAL_MS {
            self.last_eval_ms = now_ms;
            self.evaluate_needs(store, now_ms);
        }
        self.drain_due(env, store, now_ms);
    }

    fn evaluate_needs(&mut self, store: &mut CaseStore, now_ms: i64) {
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

            // Fire: one truck per case when fire department is needed.
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
                    EmsTask::Dispatch {
                        case_id,
                        model: MODEL_FIRETRUCK,
                        spawn_pos,
                        anchor,
                    },
                ));
                tracing::info!(case = record.serial, delay, "fire: schedule firetruck");
            }

            // EMS: one ambulance when casualties / injury kinds need medic.
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
                    EmsTask::Dispatch {
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
                    "ems: schedule ambulance"
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
            let EmsTask::Dispatch {
                case_id,
                model,
                spawn_pos,
                anchor,
            } = task;
            let Some(record) = store.get_mut(case_id) else {
                continue;
            };
            if record.cancelled {
                reset_dispatch_flag(record, model);
                continue;
            }
            // Re-check department still wants this unit (needs may have cleared).
            let still_needed = match model {
                MODEL_FIRETRUCK => record.fire_script_active(),
                MODEL_AMBULANCE => record.ems_script_active(),
                _ => false,
            };
            if !still_needed {
                reset_dispatch_flag(record, model);
                continue;
            }

            let Some(vehicle) = dispatch_spawn_emergency_vehicle(env, model, spawn_pos, anchor)
            else {
                tracing::warn!(case = record.serial, model, "ems/fire spawn failed");
                reset_dispatch_flag(record, model);
                continue;
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
                "ems/fire: routed to scene"
            );
            let _ = now_ms;
        }
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spawn_delay_stays_in_range() {
        for seed in 0..32u64 {
            let d = spawn_delay_ms(5_000, 8_000, seed);
            assert!((5_000..=8_000).contains(&d));
            let f = spawn_delay_ms(3_000, 5_000, seed);
            assert!((3_000..=5_000).contains(&f));
        }
    }
}
