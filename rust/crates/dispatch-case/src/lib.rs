mod department;
mod playbook;
mod engine_event_map;
mod incident;
mod ingress;
mod perception;
mod report_channel;
mod weapon;
mod record;
mod spawn_policy;
mod state;
mod store;
mod timing;
mod witness_report;

pub use department::{departments_for, is_explosive_weapon, is_fire_related_weapon, kind_label, ped_kind_from_type, Department};
pub use engine_event_map::{
    NativeEventKind, SenseChannel, GROUP_ADD_HOOK, NATIVE_EVENT_KINDS, PERCEPTION_TAPS,
};
pub use incident::{
    perpetrator_entity_refs, signal_entity_refs, signal_pos, DepartmentSet, Incident, IncidentId,
    IncidentStore,
};
pub use ingress::{signals_from_vehicle_hit, signals_from_weapon_hit};
pub use perception::{
    anchor_for_signal, kind_is_visual, native_perception_covers, needs_geometric_supplement,
    witness_ranges_for, WITNESS_HEARD_RANGE_M, WITNESS_SAW_RANGE_M,
};
pub use weapon::{classify_weapon, WeaponClass, MIN_PED_DAMAGE, MIN_VEHICLE_DAMAGE};
pub use spawn_policy::{
    is_native_ems_emergency_model, NATIVE_SPAWN_BLOCK_MODELS, MODEL_AMBULANCE, MODEL_FIRETRUCK,
    SPAWN_HOOKS,
};
pub use playbook::DepartmentNeeds;
pub use record::{CaseId, CaseRecord};
pub use state::DispatchState;
pub use store::CaseStore;
pub use report_channel::{
    ReportChannel, ReportClues, ReportEndReason, ReportPhase, ReportSeverity,
};
pub use timing::INTERRUPTED_REPORT_DISPATCH_DELAY_MS;
pub use witness_report::{
    pack_entity_refs, ReportSession, WitnessObservation, WitnessReportCoordinator,
    MAX_WITNESS_ENTITIES, MAX_WITNESS_PERPETRATORS,
};
pub use timing::{
    DEFAULT_DISPATCH_DELAY_MS, HIGH_PRIORITY_DISPATCH_DELAY_MS, NPC_ON_SCENE_TIMEOUT_MS,
    ON_SCENE_IDLE_WAIT_MS, SPAWN_ARRIVED_SCENE_TIMEOUT_MS,
};