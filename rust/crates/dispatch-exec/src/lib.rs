//! Police dispatch execution layer — Rust port of C++ `dispatch_*` exec modules.
//!
//! Business logic is idiomatic Rust without `unsafe`; FFI is confined to [`ffi`].

mod arrest;
mod attack;
mod attack_foot;
mod attack_vehicle;
mod case_inputs;
mod coordinator;
mod emergency_services;
mod ffi;
mod game;
mod models;
mod dispatch_disturbance;
mod reroute;
mod response;
mod snapshot;
mod spawn;
mod spawn_gate;
mod staging;
mod tasks;
mod threat;
pub mod timing;
mod vehicle_spawn;

pub use arrest::{criminal_combat_blocked, criminal_in_custody, try_dispatch_arrest, CriminalExecState};
pub use case_inputs::{CaseExecInputs, FrameInputs};
pub use coordinator::{ExecCaseView, ExecCoordinator, PendingTask};
pub use emergency_services::EmergencyCoordinator;
pub use dispatch_engine::TARGET_LIB;
pub use ffi::{ExecSymbolError, ExecSymbols};
pub use game::{
    crime_dispatch_position, ExecEnv, ExecGlobals, TransportVehicleState, MAX_TRANSPORT_PRISONERS,
};
pub use dispatch_disturbance::{dispatch_ped_to_disturbance, dispatch_vehicle_to_disturbance};
pub use models::{
    is_ems_dispatch_model, is_native_ems_emergency_model, is_police_dispatch_model, is_swat_model,
    local_patrol_model, DispatchVehicleKind, MapRegion, PoliceSpawnUnit, ResponseCategory,
    MODEL_AMBULANCE, MODEL_FBI_RANCHER, MODEL_FIRETRUCK, MODEL_POLICE_CAR, MODEL_SWAT_VAN,
};
pub use reroute::{apply_enroute_vehicle_reroutes, find_higher_threat_case_in_av, try_reroute_foot_cop};
pub use response::{
    commit_police_response, compute_nearby_cop_quota, compute_nearby_cop_search_radius,
    dispatch_nearby_available_cops_for_crime_auto, dispatch_nearby_available_cops_to_crime,
    make_cops_attack_criminal, make_single_cop_attack_criminal, setup_dispatched_cops,
    CommitContext, NearbyCopCandidate, PerceptionChannel,
};
pub use models::SpawnTask;
pub use spawn::{
    build_initial_plan_for_case, schedule_police_vehicle_spawns, spawn_stagger_ms_for_loc,
};
pub use spawn_gate::{is_custom_spawn_active, set_custom_spawn_active};
pub use snapshot::{
    read_reroute_snapshots, read_reroute_snapshots_for_eval, read_reroute_snapshots_mut,
    CaseRerouteSnap,
};
pub use tasks::{
    abort_phone_tasks, assign_mobile_phone_task, clear_ped_tasks, ped_has_phone_task,
    poll_report_task_phase, ReportTaskPhase, TASK_COMPLEX_USE_MOBILE_PHONE,
};
pub use staging::{compute_vehicle_staging_decoy, is_police_engagement_at, StagingState};
pub use threat::{
    build_category_spawn_plan, build_initial_spawn_plan, build_on_scene_topup_plan,
    classify_response_category, compute_case_threat_score, compute_dispatch_anchor,
    compute_nearby_dispatch_quota, compute_response_quota, count_active_threats, count_high_threats,
    get_case_max_threat_level, get_case_threat_tier, is_firearm_case, on_scene_needs_reinforcement,
    pick_criminal_target_for_cop, CriminalThreatLevel, ResponseQuota,
};
pub use timing::*;
pub use vehicle_spawn::{
    arm_emergency_vehicle_siren, dispatch_spawn_emergency_vehicle, try_add_emergency_vehicle_occupants,
};
