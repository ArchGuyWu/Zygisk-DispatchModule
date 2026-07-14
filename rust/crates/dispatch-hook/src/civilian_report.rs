//! Drive `CTaskComplexUseMobilePhone` for queued witness report sessions.

use std::collections::{HashMap, HashSet};

use dispatch_case::{ped_kind_from_type, ReportPhase};
use dispatch_core::{PedId, PedKind};
use dispatch_exec::{assign_mobile_phone_task, poll_report_task_phase, ReportTaskPhase};

use crate::runtime::DispatchRuntime;

pub struct CivilianReportDriver {
    task_dispatched: HashSet<PedId>,
    /// Last observed engine task phase — callbacks fire only on transitions.
    last_task_phase: HashMap<PedId, ReportTaskPhase>,
}

impl CivilianReportDriver {
    pub fn new() -> Self {
        Self {
            task_dispatched: HashSet::new(),
            last_task_phase: HashMap::new(),
        }
    }

    pub fn tick(&mut self, runtime: &mut DispatchRuntime) {
        self.dispatch_pending_tasks(runtime);
        self.track_open_sessions(runtime);
        self.task_dispatched.retain(|reporter| runtime.ped_id_live(*reporter));
        self.last_task_phase
            .retain(|reporter, _| runtime.ped_id_live(*reporter));
    }

    fn dispatch_pending_tasks(&mut self, runtime: &mut DispatchRuntime) {
        let pending: Vec<PedId> = runtime
            .witness_reports
            .pending_sessions()
            .map(|session| session.reporter)
            .collect();

        for reporter in pending {
            if self.task_dispatched.contains(&reporter) {
                continue;
            }
            let Some(ped_ptr) = runtime.ped_ptr(reporter) else {
                continue;
            };
            if reporter_is_cop(runtime, reporter) {
                self.task_dispatched.insert(reporter);
                runtime.on_report_task_dialing(reporter);
                runtime.on_report_task_active(reporter);
                tracing::debug!(?reporter, "cop witness report -> case opened");
                continue;
            }
            if assign_mobile_phone_task(&runtime.symbols, ped_ptr as *mut _) {
                self.task_dispatched.insert(reporter);
                tracing::debug!(?reporter, "civilian report mobile-phone task dispatched");
            }
        }
    }

    fn track_open_sessions(&mut self, runtime: &mut DispatchRuntime) {
        let reporters: Vec<PedId> = runtime
            .witness_reports
            .open_sessions()
            .map(|session| session.reporter)
            .collect();

        for reporter in reporters {
            let Some(ped_ptr) = runtime.ped_ptr(reporter) else {
                runtime.on_reporter_panic(reporter);
                self.task_dispatched.remove(&reporter);
                self.last_task_phase.remove(&reporter);
                continue;
            };

            let phase = poll_report_task_phase(&runtime.symbols, ped_ptr);
            let prev = self
                .last_task_phase
                .insert(reporter, phase)
                .unwrap_or(ReportTaskPhase::None);
            if phase == prev {
                continue;
            }

            if reporter_is_cop(runtime, reporter) {
                continue;
            }

            match phase {
                ReportTaskPhase::Dialing => runtime.on_report_task_dialing(reporter),
                ReportTaskPhase::Active => runtime.on_report_task_active(reporter),
                ReportTaskPhase::Ending => runtime.on_report_task_ended(reporter),
                ReportTaskPhase::None => {
                    if self.task_dispatched.contains(&reporter)
                        && prev != ReportTaskPhase::None
                        && runtime
                            .witness_reports
                            .session_for_reporter(reporter)
                            .is_some_and(|s| s.phase != ReportPhase::Ended)
                    {
                        runtime.on_report_task_ended(reporter);
                    }
                    self.task_dispatched.remove(&reporter);
                }
            }
        }
    }
}

fn reporter_is_cop(runtime: &DispatchRuntime, reporter: PedId) -> bool {
    runtime
        .registry
        .ped(reporter)
        .is_some_and(|entry| ped_kind_from_type(entry.ped_type) == PedKind::Cop)
}

impl Default for CivilianReportDriver {
    fn default() -> Self {
        Self::new()
    }
}