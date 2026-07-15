//! LightStep / FullDispatch transactions (MODEL.md §5).

use crate::case::{Case, Phase};
use crate::effects::Effect;
use crate::report::ReportPhase;
use crate::signal::{ReporterKind, Signal};
use crate::threat::scene_is_severe;
use crate::world::{Commit, Inputs, World};

/// Whether this frame should run FullDispatch instead of LightStep.
///
/// Typical OR triggers (may be tightened later):
/// - Queue has signals that need full handling
/// - A pending report is Connected and ready to open
/// - A case’s time since last Full ≥ full_interval_ms
/// - A case is at a phase boundary (ready_for_full)
pub fn should_full_dispatch(world: &World, inputs: &Inputs) -> bool {
    if !inputs.signals.is_empty() {
        // First shot / kill / any new clue → full handling.
        return true;
    }
    if world
        .pending_reports
        .iter()
        .any(|r| r.ready_to_open && r.phase.opens_case())
    {
        return true;
    }
    for c in &world.cases {
        if matches!(c.phase, Phase::Done) {
            continue;
        }
        if c.ready_for_full || c.should_close {
            return true;
        }
        if inputs
            .now_ms
            .saturating_sub(c.last_full_ms)
            >= world.config.full_interval_ms
        {
            return true;
        }
    }
    false
}

/// Default most frames: account inputs, advance clocks, no geometry / spawn.
///
/// MODEL §5: **no** threat/needs recompute, **no** spawn/attack decisions.
/// Effects default to `[]`.
pub fn light_step_tx(world: World, inputs: Inputs) -> Commit {
    let mut world = world;
    // Light: book clues without recompute; Full owns merge + needs/threat/size.
    account_signals_light(&mut world, &inputs.signals, inputs.now_ms);
    // TODO(runtime): detach despawn handles / GC bindings on Case or World when
    // unit ownership lands. MODEL §5 drains despawns each frame; model has no
    // pool bindings yet so we only acknowledge the field for future wiring.
    let _despawns = &inputs.despawns;

    advance_report_clocks(&mut world, inputs.now_ms);
    advance_case_clocks_light(&mut world, inputs.now_ms);

    Commit {
        world,
        effects: Vec::new(),
    }
}

/// Full read/compute/commit: open Connected cases, needs/threat/size,
/// mobilize/spawn, OnScene passes, EMS/Fire with severe view-cap.
pub fn full_dispatch_tx(world: World, inputs: Inputs) -> Commit {
    let mut world = world;
    let mut effects = Vec::new();
    let now = inputs.now_ms;

    account_signals_full(&mut world, &inputs.signals, now);
    // Same despawn gap as Light — see TODO on light_step_tx.
    let _despawns = &inputs.despawns;
    advance_report_clocks(&mut world, now);

    // Open cases only at Connected.
    open_connected_reports(&mut world, now, &mut effects);

    // Recompute + response for each live case.
    let view = inputs.view_counts;
    let case_count = world.cases.len();
    for i in 0..case_count {
        if matches!(world.cases[i].phase, Phase::Done) {
            continue;
        }
        world.cases[i].recompute_size_and_needs();
        world.cases[i].last_full_ms = now;
        world.cases[i].ready_for_full = false;

        dispatch_case(&mut world.cases[i], now, view, world.config, &mut effects);
    }

    if world.active_police_cases() {
        if !effects.iter().any(|e| matches!(e, Effect::SuppressWanted)) {
            effects.push(Effect::SuppressWanted);
        }
    }

    // Close cases marked should_close.
    for c in &mut world.cases {
        if c.should_close && !matches!(c.phase, Phase::Done) {
            c.phase = Phase::Done;
            effects.push(Effect::CloseCase { case_id: c.id });
        }
    }

    world.last_full_ms = now;
    Commit { world, effects }
}

/// Light: absorb clues into cases **without** needs/threat recompute; book pending reports.
fn account_signals_light(world: &mut World, signals: &[Signal], now_ms: u64) {
    for sig in signals {
        if let Some(case) = world.cases.iter_mut().find(|c| {
            !matches!(c.phase, Phase::Done) && roughly_same_scene(c.pos, sig.pos)
        }) {
            case.absorb_clues_no_recompute(&[sig.kind], sig.criminal_count);
            continue;
        }
        book_pending_report(world, sig, now_ms);
    }
}

/// Full: merge into cases **with** recompute; book pending reports.
fn account_signals_full(world: &mut World, signals: &[Signal], now_ms: u64) {
    for sig in signals {
        if let Some(case) = world.cases.iter_mut().find(|c| {
            !matches!(c.phase, Phase::Done) && roughly_same_scene(c.pos, sig.pos)
        }) {
            case.merge_kinds(&[sig.kind], sig.criminal_count);
            continue;
        }
        book_pending_report(world, sig, now_ms);
    }
}

fn book_pending_report(world: &mut World, sig: &Signal, now_ms: u64) {
    if let Some(rep) = world.pending_reports.iter_mut().find(|r| {
        r.phase != ReportPhase::Ended && roughly_same_scene(r.pos, sig.pos)
    }) {
        rep.merge_signal(sig);
        if matches!(sig.reporter, ReporterKind::Police) {
            rep.phase = ReportPhase::Connected;
            rep.reporter = ReporterKind::Police;
            rep.ready_to_open = true;
            rep.phase_started_ms = now_ms;
        }
        return;
    }

    world
        .pending_reports
        .push(crate::report::PendingReport::from_signal(sig, now_ms));
}

fn roughly_same_scene(a: crate::signal::WorldPos, b: crate::signal::WorldPos) -> bool {
    // Coarse correlation only (no full geometry). ~40m XY.
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    dx * dx + dy * dy < 40.0_f32 * 40.0_f32
}

fn advance_report_clocks(world: &mut World, now_ms: u64) {
    let seek = world.config.seeking_ms;
    let call = world.config.calling_ms;
    for r in &mut world.pending_reports {
        r.advance_clock(now_ms, seek, call);
    }
}

fn advance_case_clocks_light(world: &mut World, now_ms: u64) {
    let cfg = world.config;
    for c in &mut world.cases {
        if matches!(c.phase, Phase::Done) {
            continue;
        }
        if matches!(c.phase, Phase::Open)
            && now_ms.saturating_sub(c.opened_ms) >= cfg.open_delay_ms
        {
            c.ready_for_full = true;
        }
        if matches!(c.phase, Phase::OnScene) {
            if now_ms.saturating_sub(c.last_attack_ms) >= cfg.attack_interval_ms
                || now_ms.saturating_sub(c.last_arrest_ms) >= cfg.arrest_interval_ms
            {
                c.ready_for_full = true;
            }
        }
        if now_ms.saturating_sub(c.last_full_ms) >= cfg.full_interval_ms {
            c.ready_for_full = true;
        }
    }
}

fn open_connected_reports(world: &mut World, now_ms: u64, effects: &mut Vec<Effect>) {
    let mut opened: Vec<usize> = world
        .pending_reports
        .iter()
        .enumerate()
        .filter(|(_, rep)| rep.ready_to_open && rep.phase.opens_case())
        .map(|(idx, _)| idx)
        .collect();

    for &idx in &opened {
        let rep = world.pending_reports[idx].clone();
        let id = world.alloc_case_id();
        let case = Case::new(id, rep.kinds, rep.pos, rep.criminal_count, now_ms);
        effects.push(Effect::OpenCase { case_id: id });
        world.cases.push(case);
    }

    // Remove opened reports (highest index first).
    opened.sort_unstable();
    for idx in opened.into_iter().rev() {
        world.pending_reports.remove(idx);
    }
}

fn dispatch_case(
    case: &mut Case,
    now_ms: u64,
    view: crate::world::ViewCounts,
    cfg: crate::world::ModelConfig,
    effects: &mut Vec<Effect>,
) {
    // Phase advance Open → Responding after open delay.
    // Response Effects are gated on phase != Open so delay is real.
    if matches!(case.phase, Phase::Open)
        && now_ms.saturating_sub(case.opened_ms) >= cfg.open_delay_ms
    {
        case.phase = Phase::Responding;
    }

    if matches!(case.phase, Phase::Open | Phase::Done) {
        // Still in open delay (or done): no mobilize/spawn/EMS/Fire yet.
        return;
    }

    // Police response: nearby first, then spawn.
    if case.needs.police {
        let want_cap = case.response_size.nearby_slots;
        if want_cap > case.mobilized_cap {
            effects.push(Effect::MobilizeNearby {
                case_id: case.id,
                cap: want_cap,
            });
            case.mobilized_cap = want_cap;
        }
        while case.patrol_spawned < case.response_size.patrol_count {
            effects.push(Effect::SpawnPatrol { case_id: case.id });
            case.patrol_spawned = case.patrol_spawned.saturating_add(1);
        }
        if case.response_size.swat && !case.swat_spawned {
            effects.push(Effect::SpawnSwat { case_id: case.id });
            case.swat_spawned = true;
        }
        if case.response_size.fbi && !case.fbi_spawned {
            effects.push(Effect::SpawnFbi { case_id: case.id });
            case.fbi_spawned = true;
        }
    }

    // EMS / Fire: default 1/case/dept; severe may add a second on a later Full (≤2 view).
    let severe = scene_is_severe(&case.kinds, case.threat);

    if case.needs.ems {
        let budget = crate::threat::ems_fire_spawn_budget(case.ems_spawned, severe, view.ems);
        for _ in 0..budget {
            effects.push(Effect::SpawnAmbulance { case_id: case.id });
            case.ems_spawned = case.ems_spawned.saturating_add(1);
        }
    }
    if case.needs.fire {
        let budget = crate::threat::ems_fire_spawn_budget(case.fire_spawned, severe, view.fire);
        for _ in 0..budget {
            effects.push(Effect::SpawnFiretruck { case_id: case.id });
            case.fire_spawned = case.fire_spawned.saturating_add(1);
        }
    }

    // OnScene police attack/arrest on intervals.
    if matches!(case.phase, Phase::OnScene) && case.needs.police {
        if case.last_attack_ms == 0
            || now_ms.saturating_sub(case.last_attack_ms) >= cfg.attack_interval_ms
        {
            effects.push(Effect::AttackPass { case_id: case.id });
            case.last_attack_ms = now_ms;
        }
        if case.last_arrest_ms == 0
            || now_ms.saturating_sub(case.last_arrest_ms) >= cfg.arrest_interval_ms
        {
            effects.push(Effect::ArrestPass { case_id: case.id });
            case.last_arrest_ms = now_ms;
        }
    }

    // Auto-promote Responding → OnScene once we have ordered response (simple pure rule).
    if matches!(case.phase, Phase::Responding)
        && (case.mobilized_cap > 0 || case.ems_spawned > 0 || case.fire_spawned > 0)
    {
        case.phase = Phase::OnScene;
    }
}
