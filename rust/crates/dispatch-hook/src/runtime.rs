use std::cell::UnsafeCell;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Instant;

use dispatch_case::{
    native_perception_covers, needs_geometric_supplement, ped_kind_from_type,
    signals_from_vehicle_hit, signals_from_weapon_hit, signal_entity_refs, CaseStore,
    IncidentStore, WitnessReportCoordinator,
};
use dispatch_core::{
    CausalSignal, CausalSource, DespawnReason, EntityRef, Loader, PedId, PedKind, PoolKey,
    ResourceRegistry, VehicleId, WorldPos,
};
use dispatch_engine::{
    pool_key_live, vehicle_is_burning, NativeEventRegistry,
};
use dispatch_case::ReportPhase;
use dispatch_exec::{
    abort_phone_tasks, ped_has_phone_task, ExecCoordinator, ExecSymbols,
    FrameInputs, NearbyCopCandidate,
};
use tracing::debug;

use crate::civilian_report::CivilianReportDriver;
use crate::gate::zone_active_cached;
use crate::model_bridge::{
    model_signal_from_causal, ped_despawn, pool_key_handle, vehicle_despawn, ModelRuntime,
};
use crate::witness_scan;
use dispatch_core::LoaderInputs;
use dispatch_model::{ReporterKind, ViewCounts};

const PED_INTEL_OFFSET: usize = 0x5e8;
const EVENT_GROUP_OFFSET: usize = 0xc8;
const NEARBY_COPS_REFRESH_MS: i64 = 3_000;
const GEOMETRIC_SCANS_PER_TICK_MAX: usize = 1;

pub struct DispatchRuntime {
    pub registry: ResourceRegistry,
    pub loader: Loader,
    pub cases: CaseStore,
    pub incidents: IncidentStore,
    pub witness_reports: WitnessReportCoordinator,
    pub native_events: NativeEventRegistry,
    pub symbols: ExecSymbols,
    pub exec: ExecCoordinator,
    pub civilian_reports: CivilianReportDriver,
    /// Pool slots marked by `RegisterKill`; consumed on the matching `RemovePed`.
    kill_pending: HashSet<PoolKey>,
    /// `CEventGroup` ptr → owning `CPed` (avoids O(pool) scan per native event).
    event_group_ped_cache: HashMap<usize, *const std::ffi::c_void>,
    /// Entity ptr → pool key; avoids repeated O(pool) reverse lookups on hot hooks.
    ped_ptr_cache: UnsafeCell<HashMap<usize, PoolKey>>,
    vehicle_ptr_cache: UnsafeCell<HashMap<usize, PoolKey>>,
    /// Geometric witness supplements deferred off damage/event hooks.
    pending_geometry: Vec<(CausalSignal, dispatch_case::IncidentId)>,
    /// Case anchor / severity updates batched off the hot signal path.
    pending_case_signals: Vec<(CausalSignal, dispatch_case::IncidentId)>,
    nearby_cops: Vec<NearbyCopCandidate>,
    nearby_cops_at_ms: i64,
    last_cop_bindings_len: usize,
    pub(crate) gate_frame: u32,
    pub(crate) last_gate_inputs: LoaderInputs,
    criminal_scratch: Vec<PedId>,
    cop_vehicle_scratch: HashMap<PedId, VehicleId>,
    seen_cops_scratch: HashSet<PedId>,
    boot: Instant,
    /// Pure-logic product model (MODEL.md): World + input queues; driven only from
    /// script-frame `frame_tick`. Engine apply via TracingEffectSink until case-id map lands.
    pub model: ModelRuntime,
}

struct RuntimeSlot(UnsafeCell<Option<DispatchRuntime>>);

unsafe impl Sync for RuntimeSlot {}

static RUNTIME: RuntimeSlot = RuntimeSlot(UnsafeCell::new(None));
/// `with_runtime` is not re-entrant: engine callbacks from `frame_tick` must not
/// take a second `&mut DispatchRuntime` (aliasing corrupts `CaseStore` / slotmap).
static RUNTIME_DEPTH: AtomicU32 = AtomicU32::new(0);

struct RuntimeDepthGuard;

impl RuntimeDepthGuard {
    fn enter() -> Option<Self> {
        let prev = RUNTIME_DEPTH.fetch_add(1, Ordering::Acquire);
        if prev > 0 {
            RUNTIME_DEPTH.fetch_sub(1, Ordering::Release);
            return None;
        }
        Some(Self)
    }
}

impl Drop for RuntimeDepthGuard {
    fn drop(&mut self) {
        RUNTIME_DEPTH.fetch_sub(1, Ordering::Release);
    }
}

pub fn runtime_reentrant() -> bool {
    RUNTIME_DEPTH.load(Ordering::Relaxed) > 0
}

pub fn init(symbols: ExecSymbols, native_events: NativeEventRegistry) {
    unsafe {
        *RUNTIME.0.get() = Some(DispatchRuntime {
        registry: ResourceRegistry::new(),
        loader: Loader::new(),
        cases: CaseStore::new(),
        incidents: IncidentStore::new(),
        witness_reports: WitnessReportCoordinator::new(),
        native_events,
        symbols,
        exec: ExecCoordinator::new(),
        civilian_reports: CivilianReportDriver::new(),
        kill_pending: HashSet::new(),
        event_group_ped_cache: HashMap::new(),
        ped_ptr_cache: UnsafeCell::new(HashMap::new()),
        vehicle_ptr_cache: UnsafeCell::new(HashMap::new()),
        pending_geometry: Vec::new(),
        pending_case_signals: Vec::new(),
        nearby_cops: Vec::new(),
        nearby_cops_at_ms: 0,
        last_cop_bindings_len: 0,
        gate_frame: 0,
        last_gate_inputs: LoaderInputs {
            player_live: false,
            curr_area: 0,
            cutscene: false,
            warping: false,
            interior_transition: false,
            game_state: 0,
            loading: false,
        },
        criminal_scratch: Vec::with_capacity(8),
        cop_vehicle_scratch: HashMap::new(),
        seen_cops_scratch: HashSet::new(),
        boot: Instant::now(),
        model: ModelRuntime::default(),
        });
    }
}

/// Game hooks and `TheScripts::Process` run on one thread but re-enter during `tick`.
pub fn with_runtime<R>(f: impl FnOnce(&mut DispatchRuntime) -> R) -> Option<R> {
    let _guard = RuntimeDepthGuard::enter()?;
    unsafe {
        let slot = &mut *RUNTIME.0.get();
        slot.as_mut().map(f)
    }
}

impl DispatchRuntime {
    pub fn now_ms(&self) -> i64 {
        self.boot.elapsed().as_millis() as i64
    }

    pub fn frame_tick(&mut self) {
        // `dispatch_enabled` ≈ zone_active && MOD_LOGIC (see gate + scripts detour).
        if self.symbols.engine_transition_blocks_dispatch() {
            crate::gate::set_zone_active(false);
            // Soft-pause: drop backlog so re-enable does not burst spawn from model queue.
            self.model.drain_discard();
            return;
        }
        self.refresh_zone_gate();
        if !zone_active_cached() {
            self.model.drain_discard();
            return;
        }
        // Model Light/Full first (product state machine); legacy path still drives engine.
        self.model_frame_tick();
        self.tick_active();
    }

    /// Drain model queues → should_full? Full : Light → apply_effects (Tracing sink).
    fn model_frame_tick(&mut self) {
        let now_ms = self.now_ms().max(0) as u64;
        // ViewCounts stub: real EMS/fire in-view counts need pool scan; model enforces
        // budget with zeros → default 1/case until engine sink is wired.
        let view = ViewCounts::default();
        self.model.frame_tick_traced(now_ms, view);
    }

    /// Enqueue a pure-model signal (no phone/radio anim required for progression).
    pub fn enqueue_model_signal_from_causal(
        &mut self,
        signal: CausalSignal,
        reporter: ReporterKind,
        criminal_count: u32,
    ) {
        if let Some(sig) = model_signal_from_causal(signal, reporter, criminal_count) {
            self.model.enqueue_signal(sig);
        }
    }

    fn tick_active(&mut self) {
        self.drain_pending_native_events();
        let now = self.now_ms();
        let registry = &self.registry;
        let symbols = &self.symbols;
        let ped_cache_ptr = self.ped_ptr_cache.get();
        let live = |id| ped_id_live(registry, symbols, id);
        let mut criminals_for = |clues: &dispatch_case::ReportClues, out: &mut Vec<PedId>| {
            unsafe {
                criminals_into_clues(registry, symbols, &mut *ped_cache_ptr, clues, out);
            }
        };
        self.witness_reports.tick(
            live,
            &self.incidents,
            &mut self.cases,
            &mut criminals_for,
            now,
        );
        self.cases.tick(now, live);
        self.flush_pending_case_signals();
        self.drain_pending_geometry();
        if self.cases.active_count() > 0 {
            self.tick_execution(now);
        }
        self.try_civilian_reports(now);
        let mut report_driver = std::mem::take(&mut self.civilian_reports);
        report_driver.tick(self);
        self.civilian_reports = report_driver;
    }

    fn tick_execution(&mut self, now_ms: i64) {
        self.refresh_nearby_cops(now_ms);
        let player_pos = self.symbols.player_coors(0);
        let frame = FrameInputs {
            nearby_cops: &self.nearby_cops,
            density: self.cases.active_count() as i32,
            swat_already: !self.exec.globals.spawned_swats.is_empty(),
            player_pos,
        };
        let symbols = &self.symbols;
        let witness = &self.witness_reports;
        {
            let registry = &mut self.registry;
            let cases = &mut self.cases;
            self.exec
                .tick(symbols, registry, cases, now_ms, &frame, witness);
        }
        let live = |id| ped_id_live(&self.registry, &self.symbols, id);
        self.cases.finalize_after_exec(live);
    }

    fn refresh_nearby_cops(&mut self, now_ms: i64) {
        if !self.cases.needs_nearby_cop_refresh() {
            self.nearby_cops.clear();
            return;
        }
        if now_ms - self.nearby_cops_at_ms < NEARBY_COPS_REFRESH_MS {
            return;
        }
        self.scan_nearby_cops();
        self.nearby_cops_at_ms = now_ms;
    }

    fn flush_pending_case_signals(&mut self) {
        if self.pending_case_signals.is_empty() {
            return;
        }
        let pending = std::mem::take(&mut self.pending_case_signals);
        for (signal, incident_id) in pending {
            self.cases.on_causal_signal(signal, incident_id);
        }
    }

    fn drain_pending_geometry(&mut self) {
        if self.pending_geometry.is_empty() {
            return;
        }
        let mut pending = std::mem::take(&mut self.pending_geometry);
        let batch_len = pending.len().min(GEOMETRIC_SCANS_PER_TICK_MAX);
        let remainder = pending.split_off(batch_len);
        for (signal, incident_id) in pending {
            if self.cases.incident_being_handled(incident_id) {
                continue;
            }
            witness_scan::geometric_scan(self, signal);
        }
        self.pending_geometry = remainder;
    }

    /// Skip civilian/cop witness buffering once dispatch already owns the incident.
    pub(crate) fn should_collect_witness_observation(
        &self,
        entities: &[EntityRef],
        entity_count: usize,
    ) -> bool {
        for entity in entities.iter().take(entity_count) {
            if let Some(incident_id) = self.incidents.incident_for_entity(*entity) {
                if self.cases.has_case_for_incident(incident_id) {
                    return false;
                }
            }
        }
        true
    }

    /// Resolve witness ped from `CEventGroup` without scanning the pool on cache hit.
    pub fn ped_from_event_group(
        &mut self,
        event_group: *const std::ffi::c_void,
    ) -> Option<*const std::ffi::c_void> {
        if event_group.is_null() {
            return None;
        }
        let key = event_group as usize;
        if let Some(&ped) = self.event_group_ped_cache.get(&key) {
            if self.ped_ptr_live(ped) {
                return Some(ped);
            }
            self.event_group_ped_cache.remove(&key);
        }
        let ped = self.scan_ped_owning_event_group(event_group)?;
        if !self.symbols.ped_in_pool(ped) {
            return None;
        }
        self.event_group_ped_cache.insert(key, ped);
        Some(ped)
    }

    fn scan_ped_owning_event_group(
        &self,
        event_group: *const std::ffi::c_void,
    ) -> Option<*const std::ffi::c_void> {
        let intel_heap = (event_group as *const u8).wrapping_sub(EVENT_GROUP_OFFSET);
        let pool = self.symbols.open_ped_pool()?;
        for slot in 0..pool.size as usize {
            if pool.byte_map[slot] < 0 {
                continue;
            }
            let handle =
                PoolKey::from_slot_flag(slot as u16, pool.byte_map[slot] as u8).handle();
            let ped = self.symbols.pool_ped(handle);
            if ped.is_null() {
                continue;
            }
            let intel_slot = unsafe { (ped as *const u8).add(PED_INTEL_OFFSET) as *const *const u8 };
            if unsafe { *intel_slot } == intel_heap {
                return Some(ped);
            }
        }
        None
    }

    pub(crate) fn cache_event_group_for_ped(&mut self, ped: *const std::ffi::c_void) {
        if ped.is_null() {
            return;
        }
        let intel = unsafe { *((ped as *const *const u8).add(PED_INTEL_OFFSET)) };
        if intel.is_null() {
            return;
        }
        let event_group = unsafe { intel.add(EVENT_GROUP_OFFSET) } as usize;
        self.event_group_ped_cache.insert(event_group, ped);
    }

    fn uncache_event_group_for_ped(&mut self, ped: *const std::ffi::c_void) {
        if ped.is_null() {
            return;
        }
        let intel = unsafe { *((ped as *const *const u8).add(PED_INTEL_OFFSET)) };
        if intel.is_null() {
            return;
        }
        let event_group = unsafe { intel.add(EVENT_GROUP_OFFSET) } as usize;
        self.event_group_ped_cache.remove(&event_group);
    }

    fn scan_nearby_cops(&mut self) {
        self.cop_vehicle_scratch.clear();
        for binding in &self.exec.globals.cop_bindings {
            self.cop_vehicle_scratch
                .insert(binding.cop, binding.vehicle);
        }
        let bindings_len = self.exec.globals.cop_bindings.len();
        self.nearby_cops.clear();
        self.nearby_cops.reserve(bindings_len + 8);
        self.seen_cops_scratch.clear();

        for binding in &self.exec.globals.cop_bindings {
            if !self.ped_id_live(binding.cop) {
                continue;
            }
            self.seen_cops_scratch.insert(binding.cop);
            let cop_pos = self
                .ped_ptr(binding.cop)
                .map(|ptr| self.symbols.entity_world_pos(ptr))
                .unwrap_or_default();
            self.nearby_cops.push(NearbyCopCandidate {
                cop: binding.cop,
                cop_pos,
                dist_sq: 0.0,
                vehicle: Some(binding.vehicle),
            });
        }

        let bindings_changed = bindings_len != self.last_cop_bindings_len;
        let needs_pool_scan =
            bindings_changed || self.exec.globals.cop_bindings.is_empty() || self.nearby_cops.is_empty();
        self.last_cop_bindings_len = bindings_len;
        if !needs_pool_scan {
            return;
        }

        let pool = match self.symbols.open_ped_pool() {
            Some(p) => p,
            None => return,
        };
        for slot in 0..pool.size as usize {
            let flag = pool.byte_map[slot];
            if flag < 0 {
                continue;
            }
            let key = PoolKey::from_slot_flag(slot as u16, flag as u8);
            let ped = self.symbols.pool_ped(key.handle());
            if !self.symbols.ped_alive(ped) {
                continue;
            }
            let ped_type = self.symbols.ped_type_of(ped);
            if ped_kind_from_type(ped_type) != PedKind::Cop {
                continue;
            }
            self.cache_ptr_key(ped, key, true);
            let cop_id = self
                .registry
                .ped_by_pool(key)
                .unwrap_or_else(|| self.registry.adopt_ped(key, ped_type));
            if self.seen_cops_scratch.contains(&cop_id) {
                continue;
            }
            self.seen_cops_scratch.insert(cop_id);
            let cop_pos = self.symbols.entity_world_pos(ped);
            let vehicle = self.cop_vehicle_scratch.get(&cop_id).copied();
            self.nearby_cops.push(NearbyCopCandidate {
                cop: cop_id,
                cop_pos,
                dist_sq: 0.0,
                vehicle,
            });
        }
    }



    /// Engine: `phone_in` / walk-to-booth task started.
    pub fn on_report_task_dialing(&mut self, reporter: PedId) {
        self.witness_reports.on_report_task_dialing(reporter);
    }

    /// Engine: `phone_talk` task started — opens case with clues known so far.
    pub fn on_report_task_active(&mut self, reporter: PedId) {
        let now = self.now_ms();
        let registry = &self.registry;
        let symbols = &self.symbols;
        let ped_cache_ptr = self.ped_ptr_cache.get();
        let mut criminals_for = |clues: &dispatch_case::ReportClues, out: &mut Vec<PedId>| {
            unsafe {
                criminals_into_clues(registry, symbols, &mut *ped_cache_ptr, clues, out);
            }
        };
        self.witness_reports.on_report_task_active(
            reporter,
            &self.incidents,
            &mut self.cases,
            criminals_for,
            now,
        );
    }

    /// Engine: `phone_out` / hang up task finished.
    pub fn on_report_task_ended(&mut self, reporter: PedId) {
        let now = self.now_ms();
        self.witness_reports
            .on_report_task_ended(reporter, &mut self.cases, now);
    }

    /// Engine: caller panicked during approach / dial / talk.
    pub fn on_reporter_panic(&mut self, reporter: PedId) {
        let now = self.now_ms();
        let registry = &self.registry;
        let symbols = &self.symbols;
        let ped_cache_ptr = self.ped_ptr_cache.get();
        let mut criminals_for = |clues: &dispatch_case::ReportClues, out: &mut Vec<PedId>| {
            unsafe {
                criminals_into_clues(registry, symbols, &mut *ped_cache_ptr, clues, out);
            }
        };
        self.witness_reports.on_reporter_panic(
            reporter,
            &self.incidents,
            &mut self.cases,
            criminals_for,
            now,
        );
    }

    fn try_civilian_reports(&mut self, now_ms: i64) {
        let started = self.witness_reports.try_start_reports(
            &mut self.incidents,
            &self.cases,
            now_ms,
        );
        if started > 0 {
            debug!(started, "civilian reports queued for engine tasks");
        }
        self.witness_reports.clear_observations();
    }

    /// Abort engine phone/radio tasks and escalate any open civilian report session.
    /// Fail-closed: non-pool / non-live peds are not taken into task APIs.
    pub fn interrupt_ped_action(&mut self, ped: *const std::ffi::c_void, reason: &'static str) {
        if self.symbols.validate_pool_ped(ped).is_none() {
            return;
        }
        if !ped_has_phone_task(&self.symbols, ped) {
            return;
        }
        // MakeAbortable / ClearTasks only when still live; otherwise skip engine touch.
        if self.symbols.validate_live_ped(ped).is_some() {
            abort_phone_tasks(&self.symbols, ped as *mut _);
        }
        let Some(reporter) = self.resolve_ped_id(ped) else {
            return;
        };
        if !self
            .witness_reports
            .session_for_reporter(reporter)
            .is_some_and(|session| session.phase != ReportPhase::Ended)
        {
            return;
        }
        let now = self.now_ms();
        let registry = &self.registry;
        let symbols = &self.symbols;
        let ped_cache_ptr = self.ped_ptr_cache.get();
        let mut criminals_for = |clues: &dispatch_case::ReportClues, out: &mut Vec<PedId>| {
            unsafe {
                criminals_into_clues(registry, symbols, &mut *ped_cache_ptr, clues, out);
            }
        };
        self.witness_reports.on_reporter_interrupted(
            reporter,
            &self.incidents,
            &mut self.cases,
            criminals_for,
            now,
        );
        tracing::warn!(?reporter, reason, "reporter action interrupted");
    }

    pub fn publish_casualty(
        &mut self,
        dead: *const std::ffi::c_void,
        killer: *const std::ffi::c_void,
        weapon: i32,
    ) {
        if dead.is_null() {
            return;
        }
        self.interrupt_ped_action(dead, "casualty");
        self.track_ped(dead);
        if !killer.is_null() {
            self.track_ped(self.resolve_attacker(killer));
        }
        let ped_kind = ped_kind_from_type(self.symbols.ped_type_of(dead));
        let pos = self.symbols.entity_world_pos(dead);
        let signal = CausalSignal::PedCasualty {
            dead,
            killer: self.resolve_attacker(killer),
            weapon,
            ped_kind,
            pos,
            source: CausalSource::RegisterKill,
        };
        self.publish_signal(signal);
    }

    pub fn ingest_weapon_damage(
        &mut self,
        victim: *const std::ffi::c_void,
        source: *const std::ffi::c_void,
        weapon: i32,
        damage: f32,
        victim_kind: PedKind,
    ) {
        if self.symbols.validate_pool_ped(victim).is_none() {
            return;
        }
        let attacker = self.resolve_attacker(source);
        if attacker.is_null() || attacker == victim {
            return;
        }
        if self.is_cop(attacker) {
            return;
        }
        self.track_ped(victim);
        self.track_ped(attacker);
        let pos = self.symbols.entity_world_pos(victim);
        for signal in signals_from_weapon_hit(victim, attacker, weapon, damage, victim_kind, pos) {
            if native_perception_covers(signal.kind()) {
                continue;
            }
            self.track_signal_entities(signal);
            self.publish_signal(signal);
        }
    }

    pub fn ingest_vehicle_damage(
        &mut self,
        vehicle: *const std::ffi::c_void,
        source: *const std::ffi::c_void,
        weapon: i32,
        damage: f32,
        impact_pos: [f32; 3],
    ) {
        if vehicle.is_null() || !self.vehicle_ptr_live(vehicle) {
            return;
        }
        let attacker = self.resolve_attacker(source);
        if attacker.is_null() {
            return;
        }
        if self.is_cop(attacker) {
            return;
        }
        self.track_ped(attacker);
        let pos = WorldPos {
            x: impact_pos[0],
            y: impact_pos[1],
            z: impact_pos[2],
        };
        let pos = if pos == WorldPos::default() {
            self.symbols.entity_world_pos(vehicle)
        } else {
            pos
        };
        let burning = vehicle_is_burning(vehicle);
        for signal in signals_from_vehicle_hit(vehicle, attacker, weapon, damage, pos, burning) {
            if native_perception_covers(signal.kind()) {
                continue;
            }
            self.track_signal_entities(signal);
            self.publish_signal(signal);
        }
    }

    pub fn publish_signal(&mut self, signal: CausalSignal) {
        // Model queue only (logical report timers; no phone/radio anim gate).
        // Reporter defaults to Civilian; Full/Light open cases at Connected.
        self.enqueue_model_signal_from_causal(signal, ReporterKind::Civilian, 0);
        let now = self.now_ms();
        let active = self.witness_reports.active_incident();
        let incident_id = self.incidents.ingest(signal, now, active);
        if needs_geometric_supplement(signal.kind())
            && !self.cases.incident_being_handled(incident_id)
        {
            self.pending_geometry.push((signal, incident_id));
        }
        let registry = &self.registry;
        let symbols = &self.symbols;
        let ped_cache_ptr = self.ped_ptr_cache.get();
        let mut criminals_for = |clues: &dispatch_case::ReportClues, out: &mut Vec<PedId>| {
            unsafe {
                criminals_into_clues(registry, symbols, &mut *ped_cache_ptr, clues, out);
            }
        };
        self.witness_reports.on_causal_signal(
            signal,
            incident_id,
            &self.incidents,
            &mut self.cases,
            criminals_for,
        );
        self.pending_case_signals.push((signal, incident_id));
    }

    /// Map a damage/kill `source` entity to a live ped attacker.
    ///
    /// Fail-closed: only take pool-live peds. Vehicle layout (`+0x460` driver) is read
    /// only after pool membership; wild non-null killers are discarded, never passed on.
    fn resolve_attacker(&self, source: *const std::ffi::c_void) -> *const std::ffi::c_void {
        if source.is_null() {
            return std::ptr::null();
        }
        if self.ped_ptr_live(source) {
            return source;
        }
        let Some(vehicle) = self.symbols.validate_pool_vehicle(source) else {
            return std::ptr::null();
        };
        let Some(driver) = self.symbols.take_vehicle_driver(vehicle) else {
            return std::ptr::null();
        };
        if self.ped_ptr_live(driver.as_ptr()) {
            return driver.as_ptr();
        }
        std::ptr::null()
    }

    fn is_cop(&self, ped: *const std::ffi::c_void) -> bool {
        if ped.is_null() {
            return false;
        }
        ped_kind_from_type(self.symbols.ped_type_of(ped)) == PedKind::Cop
    }

    fn track_signal_entities(&mut self, signal: CausalSignal) {
        for entity in signal_entity_refs(signal) {
            let ptr = entity.ptr();
            if ptr.is_null() {
                continue;
            }
            if self.track_ped(ptr).is_some() {
                continue;
            }
            if self.resolve_vehicle_pool_key(ptr).is_some() {
                let _ = self.track_vehicle(ptr, 0);
            }
        }
    }

    pub fn track_ped(&mut self, ped: *const std::ffi::c_void) -> Option<PedId> {
        let key = self.resolve_ped_pool_key(ped)?;
        self.cache_event_group_for_ped(ped);
        Some(self.registry.adopt_ped(key, self.symbols.ped_type_of(ped)))
    }

    pub fn track_vehicle(
        &mut self,
        vehicle: *const std::ffi::c_void,
        model: u32,
    ) -> Option<VehicleId> {
        let key = self.resolve_vehicle_pool_key(vehicle)?;
        Some(self.registry.adopt_vehicle(key, model))
    }

    pub fn ped_ptr_live(&self, ped: *const std::ffi::c_void) -> bool {
        if ped.is_null() {
            return false;
        }
        if !self.symbols.ped_alive(ped) {
            return false;
        }
        self.resolve_ped_pool_key(ped).is_some()
    }

    pub fn vehicle_ptr_live(&self, vehicle: *const std::ffi::c_void) -> bool {
        if vehicle.is_null() {
            return false;
        }
        self.resolve_vehicle_pool_key(vehicle).is_some()
    }

    pub fn ped_id_live(&self, id: PedId) -> bool {
        ped_id_live(&self.registry, &self.symbols, id)
    }

    pub fn vehicle_id_live(&self, id: VehicleId) -> bool {
        let Some(record) = self.registry.vehicle(id) else {
            return false;
        };
        self.symbols
            .entity_from_vehicle_key(record.pool_key)
            .is_some()
    }

    pub fn ped_ptr(&self, id: PedId) -> Option<*const std::ffi::c_void> {
        let record = self.registry.ped(id)?;
        self.symbols.entity_from_ped_key(record.pool_key)
    }

    pub fn vehicle_ptr(&self, id: VehicleId) -> Option<*const std::ffi::c_void> {
        let record = self.registry.vehicle(id)?;
        self.symbols.entity_from_vehicle_key(record.pool_key)
    }

    pub fn resolve_ped_id(&self, ped: *const std::ffi::c_void) -> Option<PedId> {
        let key = self.resolve_ped_pool_key(ped)?;
        self.registry.ped_by_pool(key)
    }

    pub fn resolve_vehicle_id(&self, vehicle: *const std::ffi::c_void) -> Option<VehicleId> {
        let key = self.resolve_vehicle_pool_key(vehicle)?;
        self.registry.vehicle_by_pool(key)
    }

    pub fn mark_kill_pending(&mut self, ped: *const std::ffi::c_void) {
        if let Some(key) = self.resolve_ped_pool_key(ped) {
            self.kill_pending.insert(key);
        }
    }

    /// `RemovePed` hook entry: idempotent registry release keyed by engine pool slot.
    pub fn release_ped_from_pool(&mut self, ped: *const std::ffi::c_void) {
        let Some(key) = self.resolve_ped_pool_key(ped) else {
            return;
        };
        let reason = if self.kill_pending.remove(&key) {
            DespawnReason::Killed
        } else {
            DespawnReason::PoolRecycle
        };
        self.despawn_ped(ped, reason);
    }

    pub fn despawn_ped(&mut self, ped: *const std::ffi::c_void, reason: DespawnReason) {
        let Some(id) = self.resolve_ped_id(ped) else {
            return;
        };
        self.release_ped(id, reason);
    }

    pub fn despawn_vehicle(&mut self, vehicle: *const std::ffi::c_void, reason: DespawnReason) {
        let Some(id) = self.resolve_vehicle_id(vehicle) else {
            return;
        };
        self.release_vehicle(id, reason);
    }

    fn release_ped(&mut self, id: PedId, reason: DespawnReason) {
        let now = self.now_ms();
        // Model despawn queue (opaque pool handle) before registry release.
        if let Some(record) = self.registry.ped(id) {
            let handle = pool_key_handle(record.pool_key.slot, record.pool_key.generation);
            self.model.enqueue_despawn(ped_despawn(handle));
        }
        let registry = &self.registry;
        let symbols = &self.symbols;
        let ped_cache_ptr = self.ped_ptr_cache.get();
        let mut criminals_for = |clues: &dispatch_case::ReportClues, out: &mut Vec<PedId>| {
            unsafe {
                criminals_into_clues(registry, symbols, &mut *ped_cache_ptr, clues, out);
            }
        };
        self.witness_reports.on_reporter_interrupted(
            id,
            &self.incidents,
            &mut self.cases,
            criminals_for,
            now,
        );
        if let Some(ptr) = self.ped_ptr(id) {
            self.uncache_event_group_for_ped(ptr);
            self.uncache_ptr_key(ptr, true);
        }
        if self.registry.release_ped(id).is_none() {
            return;
        }
        debug!(?id, ?reason, "ped despawned");
        self.cases.on_ped_despawned(id, reason);
    }

    fn release_vehicle(&mut self, id: VehicleId, reason: DespawnReason) {
        if let Some(record) = self.registry.vehicle(id) {
            let handle = pool_key_handle(record.pool_key.slot, record.pool_key.generation);
            self.model.enqueue_despawn(vehicle_despawn(handle));
        }
        if self.registry.release_vehicle(id).is_none() {
            return;
        }
        // Drop stale VehicleId from exec globals and open cases (lifecycle hardening).
        self.exec.globals.spawned_swats.remove(&id);
        self.exec.globals.vehicles_ordered_to_scene.remove(&id);
        self.exec.globals.vehicles_emptied.remove(&id);
        self.exec.globals.vehicles_siren_awakened.remove(&id);
        self.exec.globals.transport_vehicles.remove(&id);
        self.exec
            .globals
            .cop_bindings
            .retain(|b| b.vehicle != id);
        self.cases.on_vehicle_despawned(id);
        debug!(?id, ?reason, "vehicle despawned");
    }

    pub(crate) fn cache_ptr_key(&self, entity: *const std::ffi::c_void, key: PoolKey, is_ped: bool) {
        if entity.is_null() {
            return;
        }
        let addr = entity as usize;
        let cache = if is_ped {
            self.ped_ptr_cache_mut()
        } else {
            self.vehicle_ptr_cache_mut()
        };
        cache.insert(addr, key);
    }

    fn uncache_ptr_key(&self, entity: *const std::ffi::c_void, is_ped: bool) {
        if entity.is_null() {
            return;
        }
        let addr = entity as usize;
        let cache = if is_ped {
            self.ped_ptr_cache_mut()
        } else {
            self.vehicle_ptr_cache_mut()
        };
        cache.remove(&addr);
    }

    /// Attach pool generation when the pointer is a live ped/vehicle.
    pub(crate) fn entity_ref_enriched(
        &self,
        ptr: *const std::ffi::c_void,
    ) -> Option<dispatch_core::EntityRef> {
        use dispatch_core::EntityRef;
        if ptr.is_null() {
            return None;
        }
        if let Some(key) = self
            .resolve_ped_pool_key(ptr)
            .or_else(|| self.resolve_vehicle_pool_key(ptr))
        {
            return EntityRef::with_pool(ptr, key);
        }
        EntityRef::new(ptr)
    }

    pub(crate) fn resolve_ped_pool_key(&self, ped: *const std::ffi::c_void) -> Option<PoolKey> {
        resolve_ped_pool_key_cached(&self.symbols, self.ped_ptr_cache_mut(), ped)
    }

    pub(crate) fn resolve_vehicle_pool_key(&self, vehicle: *const std::ffi::c_void) -> Option<PoolKey> {
        if vehicle.is_null() {
            return None;
        }
        let addr = vehicle as usize;
        let cache = self.vehicle_ptr_cache_mut();
        if let Some(key) = cache.get(&addr).copied() {
            let pool = self.symbols.open_vehicle_pool()?;
            if pool_key_live(&pool, key) {
                return Some(key);
            }
            cache.remove(&addr);
        }
        let key = self.symbols.lookup_vehicle_key(vehicle)?;
        self.cache_ptr_key(vehicle, key, false);
        Some(key)
    }

    fn ped_ptr_cache_mut(&self) -> &mut HashMap<usize, PoolKey> {
        unsafe { &mut *self.ped_ptr_cache.get() }
    }

    fn vehicle_ptr_cache_mut(&self) -> &mut HashMap<usize, PoolKey> {
        unsafe { &mut *self.vehicle_ptr_cache.get() }
    }
}

fn criminals_into_clues(
    registry: &ResourceRegistry,
    symbols: &ExecSymbols,
    ped_cache: &mut HashMap<usize, PoolKey>,
    clues: &dispatch_case::ReportClues,
    out: &mut Vec<PedId>,
) {
    out.clear();
    let source = if clues.perpetrators.is_empty() {
        clues.entities.as_slice()
    } else {
        clues.perpetrators.as_slice()
    };
    for entity in source {
        let ped = entity.ptr() as *const std::ffi::c_void;
        if let Some(id) = ped_id_from_ptr(registry, symbols, ped_cache, ped) {
            out.push(id);
        }
    }
}

fn ped_id_from_ptr(
    registry: &ResourceRegistry,
    symbols: &ExecSymbols,
    ped_cache: &mut HashMap<usize, PoolKey>,
    ped: *const std::ffi::c_void,
) -> Option<PedId> {
    let key = resolve_ped_pool_key_cached(symbols, ped_cache, ped)?;
    registry.ped_by_pool(key)
}

fn resolve_ped_pool_key_cached(
    symbols: &ExecSymbols,
    cache: &mut HashMap<usize, PoolKey>,
    ped: *const std::ffi::c_void,
) -> Option<PoolKey> {
    if ped.is_null() {
        return None;
    }
    let addr = ped as usize;
    if let Some(key) = cache.get(&addr).copied() {
        let pool = symbols.open_ped_pool()?;
        if pool_key_live(&pool, key) {
            return Some(key);
        }
        cache.remove(&addr);
    }
    let key = symbols.lookup_ped_key(ped)?;
    cache.insert(addr, key);
    Some(key)
}

fn ped_id_live(registry: &ResourceRegistry, symbols: &ExecSymbols, id: PedId) -> bool {
    let Some(record) = registry.ped(id) else {
        return false;
    };
    let Some(ptr) = symbols.entity_from_ped_key(record.pool_key) else {
        return false;
    };
    symbols.ped_alive(ptr)
}