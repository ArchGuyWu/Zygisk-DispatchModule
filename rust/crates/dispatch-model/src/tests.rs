//! Table-driven unit tests encoding MODEL.md §3–§6.

use crate::*;

fn count_effect(effects: &[Effect], pred: impl Fn(&Effect) -> bool) -> usize {
    effects.iter().filter(|e| pred(e)).count()
}

// ---------------------------------------------------------------------------
// Department needs from signal kinds
// ---------------------------------------------------------------------------

#[test]
fn department_needs_from_signal_kinds() {
    let table: &[(SignalKind, bool, bool, bool)] = &[
        // kind, police, ems, fire
        (SignalKind::Fire, false, false, true),
        (SignalKind::Explosion, false, true, true),
        (SignalKind::Casualty, true, true, false),
        (SignalKind::Injury, false, true, false),
        (SignalKind::Gunfire, true, false, false),
        (SignalKind::PropertyDamage, true, false, false),
    ];

    for &(kind, police, ems, fire) in table {
        let n = needs_from_signal_kind(kind);
        assert_eq!(n.police, police, "{kind:?} police");
        assert_eq!(n.ems, ems, "{kind:?} ems");
        assert_eq!(n.fire, fire, "{kind:?} fire");
    }

    // Gunfight + injury → Police + Ems
    let multi = needs_from_kinds(&[SignalKind::Gunfire, SignalKind::Injury]);
    assert!(multi.police && multi.ems && !multi.fire);
}

// ---------------------------------------------------------------------------
// Connected opens case; pre-Connected does not
// ---------------------------------------------------------------------------

#[test]
fn connected_opens_case_pre_connected_does_not() {
    let pos = WorldPos {
        x: 1.0,
        y: 2.0,
        z: 0.0,
    };

    // Police reporter → straight to Connected → open on FullDispatch.
    let world = World::default();
    let mut inputs = Inputs::empty(1_000);
    inputs.signals.push(
        Signal::new(SignalKind::Gunfire, pos, ReporterKind::Police).with_criminals(1),
    );
    assert!(should_full_dispatch(&world, &inputs));
    let commit = full_dispatch_tx(world, inputs);
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::OpenCase { .. })),
        "Connected (police) must OpenCase: {:?}",
        commit.effects
    );
    assert_eq!(commit.world.cases.len(), 1);
    assert!(commit.world.pending_reports.is_empty());

    // Civilian pre-Connected: Seeking — FullDispatch must not open.
    let world = World::default();
    let mut inputs = Inputs::empty(1_000);
    inputs
        .signals
        .push(Signal::new(SignalKind::Injury, pos, ReporterKind::Civilian));
    let commit = full_dispatch_tx(world, inputs);
    assert!(
        !commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::OpenCase { .. })),
        "pre-Connected must not OpenCase: {:?}",
        commit.effects
    );
    assert!(commit.world.cases.is_empty());
    assert_eq!(commit.world.pending_reports.len(), 1);
    assert_eq!(
        commit.world.pending_reports[0].phase,
        ReportPhase::Seeking
    );

    // Advance civilian through delays → Connected → open.
    let mut world = commit.world;
    // Seeking → Calling
    let commit = light_step_tx(
        world,
        Inputs::empty(1_000 + DEFAULT_SEEKING_MS),
    );
    world = commit.world;
    assert_eq!(world.pending_reports[0].phase, ReportPhase::Calling);

    // Calling phase: Full must still not OpenCase.
    let commit = full_dispatch_tx(world, Inputs::empty(1_000 + DEFAULT_SEEKING_MS + 1));
    assert!(
        !commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::OpenCase { .. })),
        "Calling must not OpenCase: {:?}",
        commit.effects
    );
    world = commit.world;
    assert_eq!(world.pending_reports[0].phase, ReportPhase::Calling);

    // Calling → Connected via Light
    let commit = light_step_tx(
        world,
        Inputs::empty(1_000 + DEFAULT_SEEKING_MS + DEFAULT_CALLING_MS),
    );
    world = commit.world;
    assert_eq!(world.pending_reports[0].phase, ReportPhase::Connected);
    assert!(world.pending_reports[0].ready_to_open);
    assert!(should_full_dispatch(
        &world,
        &Inputs::empty(1_000 + DEFAULT_SEEKING_MS + DEFAULT_CALLING_MS)
    ));
    let now = 1_000 + DEFAULT_SEEKING_MS + DEFAULT_CALLING_MS + 1;
    let commit = full_dispatch_tx(world, Inputs::empty(now));
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::OpenCase { .. })),
        "Connected civilian must OpenCase: {:?}",
        commit.effects
    );
    assert_eq!(commit.world.cases.len(), 1);
}

// ---------------------------------------------------------------------------
// response_size from threat + criminal count
// ---------------------------------------------------------------------------

#[test]
fn response_size_from_threat_and_criminal_count() {
    let table: &[(Threat, u32, u8, u8, bool, bool)] = &[
        // threat, criminals, nearby, patrol, swat, fbi
        (Threat::Low, 0, 1, 1, false, false),
        (Threat::Low, 1, 1, 1, false, false),
        (Threat::Low, 3, 2, 2, false, false),
        (Threat::Armed, 1, 2, 2, false, false),
        (Threat::Armed, 3, 2, 2, true, true),
        (Threat::ActiveFire, 1, 2, 2, false, false),
        // ActiveFire: intentional pair threshold for specials (not GANG_MIN).
        (Threat::ActiveFire, 2, 2, 3, true, true),
        (Threat::ActiveFire, 3, 2, 3, true, true),
    ];

    for &(threat, n, nearby, patrol, swat, fbi) in table {
        let s = response_size(threat, n);
        assert_eq!(s.nearby_slots, nearby, "{threat:?} n={n} nearby");
        assert_eq!(s.patrol_count, patrol, "{threat:?} n={n} patrol");
        assert_eq!(s.swat, swat, "{threat:?} n={n} swat");
        assert_eq!(s.fbi, fbi, "{threat:?} n={n} fbi");
    }

    // Gunfire kinds → ActiveFire threat
    assert_eq!(threat_from_kinds(&[SignalKind::Gunfire]), Threat::ActiveFire);
    assert_eq!(threat_from_kinds(&[SignalKind::Injury]), Threat::Low);
    assert_eq!(threat_from_kinds(&[SignalKind::Casualty]), Threat::Armed);

    // Plain Fire is not severe; Explosion/Casualty are.
    assert!(!scene_is_severe(&[SignalKind::Fire], Threat::Low));
    assert!(scene_is_severe(&[SignalKind::Explosion], Threat::Low));
    assert!(scene_is_severe(&[SignalKind::Casualty], Threat::Armed));
}

// ---------------------------------------------------------------------------
// EMS/Fire: default 1 spawn; severe view-cap ≤2 same dept
// ---------------------------------------------------------------------------

#[test]
fn ems_fire_default_one_and_severe_view_cap() {
    // Budget helper: at most one new unit per Full; severe target 2 over time.
    let budget_table: &[(u8, bool, u8, u8)] = &[
        // already, severe, view, expected_budget
        (0, false, 0, 1), // default first tick
        (1, false, 0, 0), // already at default
        (0, true, 0, 1),  // severe first Full → only 1 this tick ("may")
        (1, true, 0, 1),  // severe second Full → may add second
        (0, true, 1, 1),  // severe, 1 in view → room 1
        (0, true, 2, 0),  // view full at cap
        (1, true, 1, 1),  // case has 1, view 1 → can order 1 more
        (2, true, 1, 0),  // case already at 2
        (0, false, 2, 0), // default but view full
    ];
    for &(already, severe, view, expect) in budget_table {
        let b = ems_fire_spawn_budget(already, severe, view);
        assert_eq!(
            b, expect,
            "already={already} severe={severe} view={view}"
        );
    }
    assert_eq!(EMS_FIRE_VIEW_CAP, 2);
    assert_eq!(EMS_FIRE_DEFAULT_PER_CASE, 1);

    let pos = WorldPos::default();

    // Non-severe injury → exactly 1 ambulance
    let world = World::default();
    let mut inputs = Inputs::empty(100);
    inputs
        .signals
        .push(Signal::new(SignalKind::Injury, pos, ReporterKind::Police));
    let commit = full_dispatch_tx(world, inputs);
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnAmbulance { .. })),
        1,
        "default EMS 1: {:?}",
        commit.effects
    );
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnFiretruck { .. })),
        0
    );

    // Plain Fire is not severe → exactly 1 firetruck on first Full
    let world = World::default();
    let mut inputs = Inputs::empty(200);
    inputs.view_counts = ViewCounts { ems: 0, fire: 0 };
    inputs
        .signals
        .push(Signal::new(SignalKind::Fire, pos, ReporterKind::Police));
    let commit = full_dispatch_tx(world, inputs);
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnFiretruck { .. })),
        1,
        "plain Fire default 1: {:?}",
        commit.effects
    );
    // Second Full still non-severe → no second truck
    let commit = full_dispatch_tx(commit.world, Inputs::empty(200 + 500));
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnFiretruck { .. })),
        0,
        "plain Fire must not reinforce: {:?}",
        commit.effects
    );

    // View already at 2 → no firetruck
    let world = World::default();
    let mut inputs = Inputs::empty(300);
    inputs.view_counts = ViewCounts { ems: 0, fire: 2 };
    inputs
        .signals
        .push(Signal::new(SignalKind::Fire, pos, ReporterKind::Police));
    let commit = full_dispatch_tx(world, inputs);
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnFiretruck { .. })),
        0,
        "view cap blocks firetruck: {:?}",
        commit.effects
    );

    // Severe Casualty: first Full → exactly 1 amb; second Full empty view → exactly 1 more
    let world = World::default();
    let mut inputs = Inputs::empty(400);
    inputs.view_counts = ViewCounts { ems: 0, fire: 0 };
    inputs.signals.push(
        Signal::new(SignalKind::Casualty, pos, ReporterKind::Police).with_criminals(1),
    );
    let commit = full_dispatch_tx(world, inputs);
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnAmbulance { .. })),
        1,
        "severe first Full → 1: {:?}",
        commit.effects
    );
    assert_eq!(commit.world.cases[0].ems_spawned, 1);

    let mut inputs = Inputs::empty(400 + 500);
    inputs.view_counts = ViewCounts { ems: 0, fire: 0 };
    let commit = full_dispatch_tx(commit.world, inputs);
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnAmbulance { .. })),
        1,
        "severe second Full → +1: {:?}",
        commit.effects
    );
    assert_eq!(commit.world.cases[0].ems_spawned, 2);

    // Third Full: at case target 2 → no more
    let commit = full_dispatch_tx(commit.world, Inputs::empty(400 + 1000));
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnAmbulance { .. })),
        0
    );

    // Independent caps: fire view full must not block EMS (Explosion)
    let world = World::default();
    let mut inputs = Inputs::empty(500);
    inputs.view_counts = ViewCounts { ems: 0, fire: 2 };
    inputs.signals.push(Signal::new(
        SignalKind::Explosion,
        pos,
        ReporterKind::Police,
    ));
    let commit = full_dispatch_tx(world, inputs);
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnAmbulance { .. })),
        1,
        "EMS still allowed when fire view full: {:?}",
        commit.effects
    );
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::SpawnFiretruck { .. })),
        0,
        "fire blocked at view 2: {:?}",
        commit.effects
    );
}

// ---------------------------------------------------------------------------
// LightStep produces empty effects; no needs recompute on case merge
// ---------------------------------------------------------------------------

#[test]
fn light_step_produces_empty_effects() {
    let pos = WorldPos {
        x: 10.0,
        y: 0.0,
        z: 0.0,
    };
    let world = World::default();
    let mut inputs = Inputs::empty(50);
    inputs
        .signals
        .push(Signal::new(SignalKind::Gunfire, pos, ReporterKind::Civilian));
    let commit = light_step_tx(world, inputs);
    assert!(
        commit.effects.is_empty(),
        "LightStep Effects must be []: {:?}",
        commit.effects
    );
    assert_eq!(commit.world.pending_reports.len(), 1);
    assert!(commit.world.cases.is_empty());

    let commit = light_step_tx(World::default(), Inputs::empty(1));
    assert!(commit.effects.is_empty());
    assert!(commit.world.cases.is_empty());
}

#[test]
fn light_step_does_not_recompute_needs_on_case_merge() {
    let pos = WorldPos::default();
    // Open a police gunfire case first.
    let mut inputs = Inputs::empty(10);
    inputs.signals.push(
        Signal::new(SignalKind::Gunfire, pos, ReporterKind::Police).with_criminals(1),
    );
    let commit = full_dispatch_tx(World::default(), inputs);
    let mut world = commit.world;
    assert_eq!(world.cases.len(), 1);
    assert!(world.cases[0].needs.police);
    assert!(!world.cases[0].needs.ems);
    let threat_before = world.cases[0].threat;
    let needs_before = world.cases[0].needs;

    // Light absorbs injury clue without recompute (MODEL §5).
    let mut inputs = Inputs::empty(20);
    inputs
        .signals
        .push(Signal::new(SignalKind::Injury, pos, ReporterKind::Civilian));
    let commit = light_step_tx(world, inputs);
    world = commit.world;
    assert!(commit.effects.is_empty());
    assert!(
        world.cases[0].kinds.contains(&SignalKind::Injury),
        "clue absorbed: {:?}",
        world.cases[0].kinds
    );
    assert_eq!(
        world.cases[0].needs, needs_before,
        "Light must not recompute needs"
    );
    assert_eq!(
        world.cases[0].threat, threat_before,
        "Light must not recompute threat"
    );

    // Full recompute picks up EMS need.
    let commit = full_dispatch_tx(world, Inputs::empty(20 + 500));
    assert!(commit.world.cases[0].needs.ems);
    assert!(commit.world.cases[0].needs.police);
}

// ---------------------------------------------------------------------------
// open_delay gates response Effects
// ---------------------------------------------------------------------------

#[test]
fn open_delay_blocks_response_effects_until_elapsed() {
    let pos = WorldPos::default();
    let cfg = ModelConfig {
        open_delay_ms: 1_000,
        ..ModelConfig::default()
    };
    let world = World::new(cfg);
    let mut inputs = Inputs::empty(100);
    inputs.signals.push(
        Signal::new(SignalKind::Gunfire, pos, ReporterKind::Police).with_criminals(1),
    );
    let commit = full_dispatch_tx(world, inputs);
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::OpenCase { .. })),
        "still opens: {:?}",
        commit.effects
    );
    assert_eq!(
        count_effect(&commit.effects, |e| matches!(e, Effect::MobilizeNearby { .. })),
        0,
        "no mobilize during open delay: {:?}",
        commit.effects
    );
    assert_eq!(
        count_effect(&commit.effects, |e| {
            matches!(
                e,
                Effect::SpawnPatrol { .. }
                    | Effect::SpawnSwat { .. }
                    | Effect::SpawnFbi { .. }
                    | Effect::SpawnAmbulance { .. }
                    | Effect::SpawnFiretruck { .. }
            )
        }),
        0,
        "no spawns during open delay: {:?}",
        commit.effects
    );
    assert!(matches!(commit.world.cases[0].phase, Phase::Open));

    // After delay: response Effects appear.
    let commit = full_dispatch_tx(commit.world, Inputs::empty(100 + 1_000));
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::MobilizeNearby { .. })),
        "mobilize after delay: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnPatrol { .. })),
        "patrol after delay: {:?}",
        commit.effects
    );
}

// ---------------------------------------------------------------------------
// MobilizeNearby re-emits when nearby_slots increases
// ---------------------------------------------------------------------------

#[test]
fn mobilize_nearby_top_up_when_cap_increases() {
    let pos = WorldPos::default();
    let mut inputs = Inputs::empty(10);
    // Low threat single → nearby_slots = 1
    inputs.signals.push(
        Signal::new(SignalKind::PropertyDamage, pos, ReporterKind::Police).with_criminals(1),
    );
    let commit = full_dispatch_tx(World::default(), inputs);
    let caps: Vec<u8> = commit
        .effects
        .iter()
        .filter_map(|e| match e {
            Effect::MobilizeNearby { cap, .. } => Some(*cap),
            _ => None,
        })
        .collect();
    assert_eq!(caps, vec![1], "initial mobilize cap 1: {:?}", commit.effects);

    // Escalate: more criminals on Low → nearby 2 (gang)
    let mut inputs = Inputs::empty(10 + 500);
    inputs.signals.push(
        Signal::new(SignalKind::PropertyDamage, pos, ReporterKind::Police).with_criminals(3),
    );
    let commit = full_dispatch_tx(commit.world, inputs);
    let caps: Vec<u8> = commit
        .effects
        .iter()
        .filter_map(|e| match e {
            Effect::MobilizeNearby { cap, .. } => Some(*cap),
            _ => None,
        })
        .collect();
    assert_eq!(caps, vec![2], "top-up mobilize cap 2: {:?}", commit.effects);
    assert_eq!(commit.world.cases[0].mobilized_cap, 2);
}

// ---------------------------------------------------------------------------
// FullDispatch can produce OpenCase / Spawn* / etc.
// ---------------------------------------------------------------------------

#[test]
fn full_dispatch_produces_open_and_spawn_effects() {
    let pos = WorldPos {
        x: 5.0,
        y: 5.0,
        z: 0.0,
    };
    let world = World::default();
    let mut inputs = Inputs::empty(1_000);
    inputs.signals.push(
        Signal::new(SignalKind::Gunfire, pos, ReporterKind::Police).with_criminals(3),
    );
    inputs
        .signals
        .push(Signal::new(SignalKind::Injury, pos, ReporterKind::Police));

    assert!(should_full_dispatch(&world, &inputs));
    let commit = full_dispatch_tx(world, inputs);

    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::OpenCase { .. })),
        "OpenCase: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::MobilizeNearby { .. })),
        "MobilizeNearby: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnPatrol { .. })),
        "SpawnPatrol: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnSwat { .. })),
        "SpawnSwat for gang gunfire: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnFbi { .. })),
        "SpawnFbi alongside SWAT: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnAmbulance { .. })),
        "SpawnAmbulance for injury: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SuppressWanted)),
        "SuppressWanted: {:?}",
        commit.effects
    );
    assert_eq!(commit.world.cases.len(), 1);
    assert!(commit.world.cases[0].needs.police);
    assert!(commit.world.cases[0].needs.ems);
}

// ---------------------------------------------------------------------------
// OnScene AttackPass / ArrestPass + CloseCase
// ---------------------------------------------------------------------------

#[test]
fn onscene_attack_arrest_and_close_case() {
    let pos = WorldPos::default();
    let mut inputs = Inputs::empty(1_000);
    inputs.signals.push(
        Signal::new(SignalKind::Gunfire, pos, ReporterKind::Police).with_criminals(1),
    );
    let commit = full_dispatch_tx(World::default(), inputs);
    // First Full promotes to OnScene after response; AttackPass runs only when
    // already OnScene at start of response block — so second Full emits passes.
    assert!(matches!(commit.world.cases[0].phase, Phase::OnScene));

    let commit = full_dispatch_tx(commit.world, Inputs::empty(1_000 + 2_000));
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::AttackPass { .. })),
        "AttackPass: {:?}",
        commit.effects
    );
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::ArrestPass { .. })),
        "ArrestPass: {:?}",
        commit.effects
    );

    let mut world = commit.world;
    world.cases[0].should_close = true;
    let commit = full_dispatch_tx(world, Inputs::empty(1_000 + 3_000));
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::CloseCase { .. })),
        "CloseCase: {:?}",
        commit.effects
    );
    assert!(matches!(commit.world.cases[0].phase, Phase::Done));
}

#[test]
fn should_full_dispatch_false_when_quiet() {
    let world = World::default();
    let inputs = Inputs::empty(10);
    assert!(!should_full_dispatch(&world, &inputs));
}
