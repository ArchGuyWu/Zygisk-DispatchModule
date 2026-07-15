//! Bridge: hook signals / despawns → `dispatch_model` queues + Light/Full `frame_tick`.
//!
//! MODEL.md §1: runtime owns World + input queues; only the script-frame host calls
//! `frame_tick`. Effects apply through `dispatch_exec::EffectSink` (Tracing/Noop until
//! real ExecEnv mapping lands).

use dispatch_core::{CausalKind, CausalSignal, WorldPos as CorePos};
use dispatch_exec::{apply_effects, EffectSink, NoopEffectSink, TracingEffectSink};
use dispatch_model::{
    full_dispatch_tx, light_step_tx, should_full_dispatch, Despawn, DespawnKind, Inputs,
    ModelConfig, ReporterKind, Signal, SignalKind, ViewCounts, World, WorldPos,
};

/// Model-side runtime state held by [`crate::runtime::DispatchRuntime`].
#[derive(Debug, Clone)]
pub struct ModelRuntime {
    pub world: World,
    signal_queue: Vec<Signal>,
    despawn_queue: Vec<Despawn>,
}

impl Default for ModelRuntime {
    fn default() -> Self {
        Self::new(ModelConfig::default())
    }
}

impl ModelRuntime {
    pub fn new(config: ModelConfig) -> Self {
        Self {
            world: World::new(config),
            signal_queue: Vec::new(),
            despawn_queue: Vec::new(),
        }
    }

    #[inline]
    pub fn enqueue_signal(&mut self, signal: Signal) {
        self.signal_queue.push(signal);
    }

    #[inline]
    pub fn enqueue_despawn(&mut self, despawn: Despawn) {
        self.despawn_queue.push(despawn);
    }

    /// Drop queued inputs without ticking (zone off / mission gate / soft pause).
    /// Avoids backlog burst when `dispatch_enabled` rises again.
    pub fn drain_discard(&mut self) {
        self.signal_queue.clear();
        self.despawn_queue.clear();
    }

    pub fn pending_signal_len(&self) -> usize {
        self.signal_queue.len()
    }

    pub fn pending_despawn_len(&self) -> usize {
        self.despawn_queue.len()
    }

    /// One script-frame model step: drain → Light or Full → commit World → apply Effects.
    ///
    /// Early-out when World is idle and queues empty (no clock work needed).
    pub fn frame_tick(&mut self, now_ms: u64, view_counts: ViewCounts, sink: &mut impl EffectSink) {
        if self.signal_queue.is_empty()
            && self.despawn_queue.is_empty()
            && self.world.cases.is_empty()
            && self.world.pending_reports.is_empty()
        {
            return;
        }

        let inputs = Inputs {
            signals: std::mem::take(&mut self.signal_queue),
            despawns: std::mem::take(&mut self.despawn_queue),
            now_ms,
            view_counts,
        };

        // World by value into model tx (SNAPSHOT → COMPUTE → COMMIT).
        let world = std::mem::take(&mut self.world);
        let commit = if should_full_dispatch(&world, &inputs) {
            full_dispatch_tx(world, inputs)
        } else {
            light_step_tx(world, inputs)
        };
        self.world = commit.world;

        apply_effects(sink, &commit.effects);
    }

    /// Convenience for device path: TracingEffectSink (no engine FFI).
    pub fn frame_tick_traced(&mut self, now_ms: u64, view_counts: ViewCounts) {
        let mut sink = TracingEffectSink;
        self.frame_tick(now_ms, view_counts, &mut sink);
    }

    /// Host / offline path with silent apply.
    pub fn frame_tick_noop(&mut self, now_ms: u64, view_counts: ViewCounts) {
        let mut sink = NoopEffectSink;
        self.frame_tick(now_ms, view_counts, &mut sink);
    }
}

/// Map engine-facing causal ingest → pure model [`Signal`] (no pointers).
///
/// Report progression is **logical** only (MODEL §3): caller may use
/// [`ReporterKind::Civilian`] timers or [`ReporterKind::Police`] → Connected
/// without phone/radio animations.
pub fn model_signal_from_causal(
    signal: CausalSignal,
    reporter: ReporterKind,
    criminal_count: u32,
) -> Option<Signal> {
    let kind = match signal.kind() {
        CausalKind::PedCasualty => SignalKind::Casualty,
        CausalKind::PedInjury => SignalKind::Injury,
        CausalKind::WeaponDischarge => SignalKind::Gunfire,
        CausalKind::VehiclePropertyDamage => SignalKind::PropertyDamage,
        CausalKind::FireOutbreak | CausalKind::VehicleBurning => SignalKind::Fire,
        CausalKind::Explosion => SignalKind::Explosion,
        // Vanilla ReportCrime path — mod swallows; never enqueued into model.
        CausalKind::CrimeReported => return None,
    };
    let pos = causal_pos(signal);
    Some(
        Signal::new(kind, map_pos(pos), reporter).with_criminals(criminal_count),
    )
}

#[inline]
pub fn map_pos(p: CorePos) -> WorldPos {
    WorldPos {
        x: p.x,
        y: p.y,
        z: p.z,
    }
}

fn causal_pos(signal: CausalSignal) -> CorePos {
    match signal {
        CausalSignal::PedCasualty { pos, .. }
        | CausalSignal::PedInjury { pos, .. }
        | CausalSignal::VehicleBurning { pos, .. }
        | CausalSignal::FireOutbreak { pos, .. }
        | CausalSignal::Explosion { pos, .. }
        | CausalSignal::WeaponDischarge { pos, .. }
        | CausalSignal::VehiclePropertyDamage { pos, .. }
        | CausalSignal::CrimeReported { pos, .. } => pos,
    }
}

/// Opaque despawn handle from pool slot+generation (not a raw engine pointer).
#[inline]
pub fn pool_key_handle(slot: u16, generation: u8) -> u64 {
    (slot as u64) | ((generation as u64) << 16)
}

#[inline]
pub fn ped_despawn(handle: u64) -> Despawn {
    Despawn {
        kind: DespawnKind::Ped,
        handle,
    }
}

#[inline]
pub fn vehicle_despawn(handle: u64) -> Despawn {
    Despawn {
        kind: DespawnKind::Vehicle,
        handle,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_core::{CausalSource, PedKind};
    use dispatch_model::{CaseId, Effect, Phase, ReportPhase};

    /// Records sink calls for assertions without engine.
    #[derive(Debug, Default)]
    struct RecordingSink {
        log: Vec<&'static str>,
    }

    impl EffectSink for RecordingSink {
        fn open_case(&mut self, _case_id: CaseId) {
            self.log.push("open_case");
        }
        fn close_case(&mut self, _case_id: CaseId) {
            self.log.push("close_case");
        }
        fn mobilize_nearby(&mut self, _case_id: CaseId, _cap: u8) {
            self.log.push("mobilize_nearby");
        }
        fn spawn_patrol(&mut self, _case_id: CaseId) {
            self.log.push("spawn_patrol");
        }
        fn spawn_swat(&mut self, _case_id: CaseId) {
            self.log.push("spawn_swat");
        }
        fn spawn_fbi(&mut self, _case_id: CaseId) {
            self.log.push("spawn_fbi");
        }
        fn spawn_ambulance(&mut self, _case_id: CaseId) {
            self.log.push("spawn_ambulance");
        }
        fn spawn_firetruck(&mut self, _case_id: CaseId) {
            self.log.push("spawn_firetruck");
        }
        fn suppress_wanted(&mut self) {
            self.log.push("suppress_wanted");
        }
        fn attack_pass(&mut self, _case_id: CaseId) {
            self.log.push("attack_pass");
        }
        fn arrest_pass(&mut self, _case_id: CaseId) {
            self.log.push("arrest_pass");
        }
    }

    fn gunfire_at(x: f32, y: f32) -> CausalSignal {
        CausalSignal::WeaponDischarge {
            shooter: std::ptr::null(),
            pos: CorePos { x, y, z: 0.0 },
            weapon: 22,
            source: CausalSource::WeaponDamage,
        }
    }

    #[test]
    fn maps_gunfire_and_skips_crime_reported() {
        let sig = model_signal_from_causal(
            gunfire_at(1.0, 2.0),
            ReporterKind::Civilian,
            1,
        )
        .expect("gunfire maps");
        assert_eq!(sig.kind, SignalKind::Gunfire);
        assert_eq!(sig.pos.x, 1.0);
        assert_eq!(sig.criminal_count, 1);

        let crime = CausalSignal::CrimeReported {
            perpetrator: std::ptr::null(),
            victim: std::ptr::null(),
            crime_type: 1,
            pos: CorePos::default(),
            source: CausalSource::ReportCrime,
        };
        assert!(model_signal_from_causal(crime, ReporterKind::Civilian, 0).is_none());
    }

    #[test]
    fn idle_frame_is_noop() {
        let mut rt = ModelRuntime::default();
        let mut sink = RecordingSink::default();
        rt.frame_tick(0, ViewCounts::default(), &mut sink);
        assert!(sink.log.is_empty());
        assert!(rt.world.cases.is_empty());
    }

    #[test]
    fn police_reporter_opens_case_on_full_without_phone_anim() {
        // Police → Connected immediately; open_delay 0 → Responding + effects same Full.
        let mut rt = ModelRuntime::new(ModelConfig {
            open_delay_ms: 0,
            full_interval_ms: 500,
            ..ModelConfig::default()
        });
        let sig = model_signal_from_causal(
            gunfire_at(10.0, 20.0),
            ReporterKind::Police,
            1,
        )
        .unwrap();
        rt.enqueue_signal(sig);

        let mut sink = RecordingSink::default();
        rt.frame_tick(1_000, ViewCounts::default(), &mut sink);

        assert!(
            sink.log.contains(&"open_case"),
            "expected OpenCase from police Connected path, got {:?}",
            sink.log
        );
        assert!(
            sink.log.contains(&"mobilize_nearby") || sink.log.contains(&"spawn_patrol"),
            "expected police response effects, got {:?}",
            sink.log
        );
        assert_eq!(rt.world.cases.len(), 1);
        assert!(!matches!(rt.world.cases[0].phase, Phase::Done));
        // No phone/radio task required — report phase is logical only.
        assert!(rt.world.pending_reports.is_empty());
    }

    #[test]
    fn civilian_report_uses_logical_timers_not_anim() {
        let mut rt = ModelRuntime::new(ModelConfig {
            seeking_ms: 100,
            calling_ms: 100,
            open_delay_ms: 0,
            full_interval_ms: 50,
            ..ModelConfig::default()
        });
        let sig = model_signal_from_causal(
            gunfire_at(0.0, 0.0),
            ReporterKind::Civilian,
            0,
        )
        .unwrap();
        rt.enqueue_signal(sig);

        // t=0 book report Seeking
        let mut sink = RecordingSink::default();
        rt.frame_tick(0, ViewCounts::default(), &mut sink);
        assert!(sink.log.is_empty(), "no open before Connected");
        assert_eq!(rt.world.pending_reports.len(), 1);
        assert_eq!(rt.world.pending_reports[0].phase, ReportPhase::Seeking);

        // Advance through Seeking → Calling → Connected via Light clocks only.
        rt.frame_tick(150, ViewCounts::default(), &mut sink);
        assert_eq!(rt.world.pending_reports[0].phase, ReportPhase::Calling);

        // Calling dwell elapsed → Connected + ready_to_open (still Light this frame;
        // should_full sees pre-advance snapshot, so open is next frame).
        rt.frame_tick(300, ViewCounts::default(), &mut sink);
        assert!(
            rt.world.cases.is_empty(),
            "open deferred to Full after Connected flag"
        );
        assert_eq!(rt.world.pending_reports[0].phase, ReportPhase::Connected);
        assert!(rt.world.pending_reports[0].ready_to_open);

        rt.frame_tick(350, ViewCounts::default(), &mut sink);
        assert_eq!(rt.world.cases.len(), 1);
        assert!(sink.log.contains(&"open_case"));
    }

    #[test]
    fn drain_discard_clears_queues_without_world_write() {
        let mut rt = ModelRuntime::default();
        rt.enqueue_signal(
            model_signal_from_causal(gunfire_at(0.0, 0.0), ReporterKind::Civilian, 0).unwrap(),
        );
        rt.enqueue_despawn(ped_despawn(42));
        assert_eq!(rt.pending_signal_len(), 1);
        rt.drain_discard();
        assert_eq!(rt.pending_signal_len(), 0);
        assert_eq!(rt.pending_despawn_len(), 0);
        assert!(rt.world.cases.is_empty());
    }

    #[test]
    fn casualty_maps_and_noop_sink_runs() {
        let cas = CausalSignal::PedCasualty {
            dead: std::ptr::null(),
            killer: std::ptr::null(),
            weapon: 0,
            ped_kind: PedKind::Civilian,
            pos: CorePos {
                x: 3.0,
                y: 4.0,
                z: 1.0,
            },
            source: CausalSource::RegisterKill,
        };
        let sig = model_signal_from_causal(cas, ReporterKind::Police, 1).unwrap();
        assert_eq!(sig.kind, SignalKind::Casualty);

        let mut rt = ModelRuntime::default();
        rt.enqueue_signal(sig);
        rt.frame_tick_noop(1, ViewCounts::default());
        assert!(!rt.world.cases.is_empty() || !rt.world.pending_reports.is_empty());
    }

    #[test]
    fn light_step_default_when_only_clocks() {
        // After a case exists with no new signals and not due for full → Light (no new Effects).
        let mut rt = ModelRuntime::new(ModelConfig {
            open_delay_ms: 0,
            full_interval_ms: 10_000,
            attack_interval_ms: 10_000,
            arrest_interval_ms: 10_000,
            ..ModelConfig::default()
        });
        rt.enqueue_signal(
            model_signal_from_causal(gunfire_at(5.0, 5.0), ReporterKind::Police, 1).unwrap(),
        );
        let mut sink = RecordingSink::default();
        rt.frame_tick(100, ViewCounts::default(), &mut sink);
        assert!(sink.log.contains(&"open_case"));
        let after_full = sink.log.len();

        // Next frame soon after: should_full false (interval 10s) → Light, no extra effects.
        sink.log.clear();
        rt.frame_tick(200, ViewCounts::default(), &mut sink);
        assert!(
            sink.log.is_empty(),
            "LightStep should emit no effects, got {:?}",
            sink.log
        );
        assert!(after_full > 0);
        // Suppress phantom unused Effect import concern
        let _ = Effect::SuppressWanted;
    }
}
