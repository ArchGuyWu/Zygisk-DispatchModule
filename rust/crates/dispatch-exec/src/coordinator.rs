//! `ExecCoordinator` — per-case execution tick (Timing / OnScene).

use std::collections::HashMap;

use dispatch_case::{CaseId, CaseRecord, CaseStore, DispatchState, WitnessReportCoordinator};
use dispatch_core::{PedId, ResourceRegistry, WorldPos};

use crate::arrest::{sync_custody_criminals, tick_case_arrests, CriminalExecState};
use crate::case_inputs::{CaseExecInputs, FrameInputs};
use crate::game::{crime_dispatch_position, ExecEnv, ExecGlobals};
use crate::ffi::ExecSymbols;

use crate::response::{
    commit_police_response, try_mobilize_nearby_cops, CommitContext, NearbyCopCandidate,
    PerceptionChannel,
};
use crate::reroute::apply_enroute_vehicle_reroutes;
use crate::snapshot::read_reroute_snapshots_for_eval;
use crate::models::SpawnTask;
use crate::emergency_services::EmergencyCoordinator;

use crate::spawn::{begin_police_spawn_chain, execute_spawn_unit, handle_batch_release_timeout};
use crate::threat::{compute_dispatch_anchor, on_scene_needs_reinforcement};
use crate::timing::{
    elapsed_since_dispatch_timer, ON_SCENE_DISPATCH_INTERVAL_MS, REINFORCEMENT_EVAL_INTERVAL_MS,
    REINFORCE_NEARBY_ATTEMPT_MS, REROUTE_EVAL_INTERVAL_MS,
};

/// Read-only + mutable exec view over `CaseRecord` exec fields.
pub trait ExecCaseView {
    fn dispatch_sent(&self) -> bool;
    fn dispatch_anchor(&self) -> WorldPos;
    fn is_firearm(&self) -> bool;
    fn cops_killed(&self) -> i32;
    fn vehicle_spawn_pending(&self) -> bool;
    fn case_vehicles(&self) -> &[dispatch_core::VehicleId];
}

impl ExecCaseView for CaseRecord {
    fn dispatch_sent(&self) -> bool {
        self.dispatch_sent
    }
    fn dispatch_anchor(&self) -> WorldPos {
        self.dispatch_anchor
    }
    fn is_firearm(&self) -> bool {
        self.is_firearm
    }
    fn cops_killed(&self) -> i32 {
        self.cops_killed
    }
    fn vehicle_spawn_pending(&self) -> bool {
        self.vehicle_spawn_pending
    }
    fn case_vehicles(&self) -> &[dispatch_core::VehicleId] {
        &self.case_vehicles
    }
}

#[derive(Debug, Clone)]
pub enum PendingTask {
    Spawn(SpawnTask),
    ReinforcementNearby { wave: i32 },
}

pub struct ExecCoordinator {
    pub globals: ExecGlobals,
    pub arrest_states: HashMap<CaseId, HashMap<PedId, CriminalExecState>>,
    pub emergency: EmergencyCoordinator,
    pending: Vec<(CaseId, i64, PendingTask)>,
    /// Reused each tick to iterate cases without allocating.
    exec_case_ids: Vec<CaseId>,
}

impl ExecCoordinator {
    pub fn new() -> Self {
        Self {
            globals: ExecGlobals::new(),
            arrest_states: HashMap::new(),
            emergency: EmergencyCoordinator::default(),
            pending: Vec::new(),
            exec_case_ids: Vec::new(),
        }
    }

    pub fn tick(
        &mut self,
        symbols: &ExecSymbols,
        registry: &mut ResourceRegistry,
        store: &mut CaseStore,
        now_ms: i64,
        frame: &FrameInputs<'_>,
        witness: &WitnessReportCoordinator,
    ) {
        self.flush_globals_pending(now_ms);
        sync_custody_criminals(&self.arrest_states, &mut self.globals);

        self.drain_pending_tasks(symbols, registry, store, now_ms);

        {
            let mut env = ExecEnv {
                symbols,
                registry,
                globals: &mut self.globals,
            };
            self.emergency.tick(&mut env, store, now_ms);
        }

        if !self.globals.vehicles_ordered_to_scene.is_empty()
            && now_ms.saturating_sub(self.globals.last_reroute_eval_ms) >= REROUTE_EVAL_INTERVAL_MS
        {
            self.globals.last_reroute_eval_ms = now_ms;
            let reroute_snaps =
                read_reroute_snapshots_for_eval(store, &self.globals.vehicles_ordered_to_scene);
            let mut env = ExecEnv {
                symbols,
                registry,
                globals: &mut self.globals,
            };
            apply_enroute_vehicle_reroutes(&mut env, store, &reroute_snaps, now_ms);
        }

        let mut case_ids = std::mem::take(&mut self.exec_case_ids);
        case_ids.clear();
        case_ids.extend(store.case_ids());
        for &case_id in &case_ids {
            let Some(record) = store.get_mut(case_id) else {
                continue;
            };
            if record.cancelled {
                continue;
            }
            let case_input = CaseExecInputs::build(record, frame, witness, registry);
            self.tick_case(
                symbols,
                registry,
                case_id,
                record,
                now_ms,
                frame,
                case_input,
            );
        }
        self.exec_case_ids = case_ids;
    }

    fn tick_case(
        &mut self,
        symbols: &ExecSymbols,
        registry: &mut ResourceRegistry,
        case_id: CaseId,
        record: &mut CaseRecord,
        now_ms: i64,
        frame: &FrameInputs<'_>,
        case_input: CaseExecInputs,
    ) {
        match record.state {
            DispatchState::Timing => {
                self.tick_timing(symbols, registry, case_id, record, now_ms, frame, case_input)
            }
            DispatchState::OnScene => {
                self.tick_on_scene(symbols, registry, case_id, record, now_ms, frame, case_input)
            }
            _ => {}
        }
    }

    fn tick_timing(
        &mut self,
        symbols: &ExecSymbols,
        registry: &mut ResourceRegistry,
        case_id: CaseId,
        record: &mut CaseRecord,
        now_ms: i64,
        frame: &FrameInputs<'_>,
        case_input: CaseExecInputs,
    ) {
        record.refresh_department_needs();
        if !record.police_script_active() {
            return;
        }

        let post_report = record.report_finalized || record.report_channel.is_none();

        if post_report
            && (case_input.perception_best >= PerceptionChannel::Seen as i32
                || case_input.suspect_confirmed)
        {
            let mut env = ExecEnv {
                symbols,
                registry,
                globals: &mut self.globals,
            };
            let mut ctx = CommitContext {
                env,
                road_reachable: case_input.road_reachable,
                offscreen_allowed: case_input.offscreen_allowed,
                witness_backing: case_input.witness_backing,
                is_player_case: case_input.is_player_case,
                density: frame.density,
                swat_already: frame.swat_already,
            };
            commit_police_response(
                &mut ctx,
                record,
                case_id,
                frame.nearby_cops,
                now_ms,
                "PerceptionUpgrade",
            );
            return;
        }

        if !post_report {
            return;
        }

        let elapsed = elapsed_since_dispatch_timer(record.timer_start_ms, now_ms);
        if elapsed < record.dispatch_delay_ms as i64 {
            let mut env = ExecEnv {
                symbols,
                registry,
                globals: &mut self.globals,
            };
            if try_mobilize_nearby_cops(
                &mut env,
                record,
                frame.nearby_cops,
                frame.density,
                now_ms,
                "NearbyRetry",
            ) {
                return;
            }
            return;
        }

        let mut env = ExecEnv {
            symbols,
            registry,
            globals: &mut self.globals,
        };
        let mut ctx = CommitContext {
            env,
            road_reachable: case_input.road_reachable,
            offscreen_allowed: case_input.offscreen_allowed,
            witness_backing: case_input.witness_backing,
            is_player_case: case_input.is_player_case,
            density: frame.density,
            swat_already: frame.swat_already,
        };
        commit_police_response(
            &mut ctx,
            record,
            case_id,
            frame.nearby_cops,
            now_ms,
            "TimerExpired",
        );
    }

    fn tick_on_scene(
        &mut self,
        symbols: &ExecSymbols,
        registry: &mut ResourceRegistry,
        case_id: CaseId,
        record: &mut CaseRecord,
        now_ms: i64,
        _frame: &FrameInputs<'_>,
        case_input: CaseExecInputs,
    ) {
        record.refresh_department_needs();

        let dispatch_now = case_input.cop_within_av
            || record.last_on_scene_dispatch_ms <= 0
            || now_ms - record.last_on_scene_dispatch_ms >= ON_SCENE_DISPATCH_INTERVAL_MS;

        {
            let arrest_map = self.arrest_states.entry(case_id).or_default();
            crate::arrest::sync_arrest_flags_from_globals(&self.globals, arrest_map);
        }

        if record.on_scene_start_ms > 0
            && !(record.vehicle_spawn_pending && record.case_vehicles.is_empty())
        {
            let positions: Vec<WorldPos> = {
                let mut env = ExecEnv {
                    symbols,
                    registry,
                    globals: &mut self.globals,
                };
                let arrest_map = self.arrest_states.entry(case_id).or_default();
                tick_case_arrests(&mut env, record, arrest_map, now_ms);
                record
                    .criminals
                    .iter()
                    .filter(|id| env.registry.contains_ped(**id))
                    .map(|id| env.ped_pos(*id))
                    .collect()
            };
            sync_custody_criminals(&self.arrest_states, &mut self.globals);
            record.dispatch_anchor = compute_dispatch_anchor(record, &positions);
        }

        let combat_blocked = record.primary.map(|p| {
            let state = self
                .arrest_states
                .get(&case_id)
                .and_then(|map| map.get(&p));
            let dispatched = state.map(|s| s.arrest_dispatched).unwrap_or(false);
            let apprehended = state.map(|s| s.apprehended).unwrap_or(false);
            crate::arrest::criminal_combat_blocked(p, dispatched, apprehended)
        });

        {
            let mut env = ExecEnv {
                symbols,
                registry,
                globals: &mut self.globals,
            };

            env.sync_natural_vehicle_exits(now_ms);

            if record.police_script_active()
                && record.police_scene_abnormal()
                && dispatch_now
                && combat_blocked == Some(false)
            {
                if let Some(criminal) = record.primary {
                    crate::response::make_cops_attack_criminal(
                        &mut env,
                        record,
                        criminal,
                        now_ms,
                        record.is_firearm,
                    );
                }
                record.last_on_scene_dispatch_ms = now_ms;
            }

            env.globals.arrest_dispatched_criminals.clear();
        }

        if record.police_script_active()
            && !record.criminals.is_empty()
            && record.reinforcements_sent < 3
            && now_ms.saturating_sub(record.last_reinforcement_eval_ms)
                >= REINFORCEMENT_EVAL_INTERVAL_MS
        {
            record.last_reinforcement_eval_ms = now_ms;
            if on_scene_needs_reinforcement(record, _frame.density) {
                record.reinforcements_sent += 1;
                let wave = record.reinforcements_sent;
                self.pending.push((
                    case_id,
                    now_ms + REINFORCE_NEARBY_ATTEMPT_MS,
                    PendingTask::ReinforcementNearby { wave },
                ));
                tracing::info!(
                    case = record.serial,
                    wave,
                    density = _frame.density,
                    "reinforcement queued (on-scene situation)"
                );
            }
        }

    }

    fn flush_globals_pending(&mut self, now_ms: i64) {
        let due: Vec<_> = self
            .globals
            .pending_spawn
            .iter()
            .filter(|(_, t, _)| *t <= now_ms)
            .cloned()
            .collect();
        self.globals
            .pending_spawn
            .retain(|(_, t, _)| *t > now_ms);
        for (case_id, _, task) in due {
            self.pending.push((case_id, now_ms, PendingTask::Spawn(task)));
        }
    }

    fn drain_pending_tasks(
        &mut self,
        symbols: &ExecSymbols,
        registry: &mut ResourceRegistry,
        store: &mut CaseStore,
        now_ms: i64,
    ) {
        let due: Vec<_> = self
            .pending
            .iter()
            .filter(|(_, t, _)| *t <= now_ms)
            .cloned()
            .collect();
        self.pending.retain(|(_, t, _)| *t > now_ms);
        for (case_id, _, task) in due {
            let Some(record) = store.get_mut(case_id) else {
                continue;
            };
            match task {
                PendingTask::ReinforcementNearby { wave } => {
                    let mut env = ExecEnv {
                        symbols,
                        registry,
                        globals: &mut self.globals,
                    };
                    let _ = crate::response::dispatch_nearby_available_cops_for_crime_auto(
                        &mut env,
                        record,
                        &[],
                        1,
                        now_ms,
                    );
                    tracing::info!(case = record.serial, wave, "reinforcement nearby attempt");
                }
                PendingTask::Spawn(spawn_task) => {
                    self.handle_spawn_task(
                        symbols,
                        registry,
                        case_id,
                        record,
                        spawn_task,
                        now_ms,
                    );
                }
            }
        }
    }

    fn handle_spawn_task(
        &mut self,
        symbols: &ExecSymbols,
        registry: &mut ResourceRegistry,
        case_id: CaseId,
        record: &mut CaseRecord,
        task: SpawnTask,
        now_ms: i64,
    ) {
        let mut env = ExecEnv {
            symbols,
            registry,
            globals: &mut self.globals,
        };
        let follow_up = match task {
            SpawnTask::BeginChain { chain_id } => {
                begin_police_spawn_chain(&mut env, case_id, record, chain_id, now_ms)
            }
            SpawnTask::SpawnUnit {
                chain_id,
                index,
                attempt,
            } => execute_spawn_unit(&mut env, case_id, record, chain_id, index, attempt, now_ms),
            SpawnTask::BatchReleaseTimeout { chain_id } => {
                handle_batch_release_timeout(&mut env, record, chain_id, now_ms);
                Vec::new()
            }
        };
        for (due, next) in follow_up {
            self.globals.pending_spawn.push((case_id, due, next));
        }
    }
}

