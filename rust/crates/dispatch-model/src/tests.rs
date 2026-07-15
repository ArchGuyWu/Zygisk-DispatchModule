//! Table-driven unit tests encoding MODEL.md §3–§6.

use crate::*;

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
    // Calling → Connected
    let commit = light_step_tx(
        world,
        Inputs::empty(1_000 + DEFAULT_SEEKING_MS + DEFAULT_CALLING_MS),
    );
    world = commit.world;
    assert_eq!(world.pending_reports[0].phase, ReportPhase::Connected);
    assert!(world.pending_reports[0].ready_to_open);
    assert!(should_full_dispatch(&world, &Inputs::empty(1_000 + DEFAULT_SEEKING_MS + DEFAULT_CALLING_MS)));
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
}

// ---------------------------------------------------------------------------
// EMS/Fire: default 1 spawn; severe view-cap ≤2 same dept
// ---------------------------------------------------------------------------

#[test]
fn ems_fire_default_one_and_severe_view_cap() {
    // Budget helper table
    let budget_table: &[(u8, bool, u8, u8)] = &[
        // already, severe, view, expected_budget
        (0, false, 0, 1), // default 1
        (1, false, 0, 0), // already at default
        (0, true, 0, 2),  // severe, empty view → 2
        (0, true, 1, 1),  // severe, 1 in view → room 1
        (0, true, 2, 0),  // view full at cap
        (1, true, 1, 1),  // case has 1, view 1 → can order 1 more
        (2, true, 1, 0),  // case already at 2
        (0, false, 2, 0), // default but view full — still no room past cap
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

    // Non-severe injury → 1 ambulance
    let world = World::default();
    let mut inputs = Inputs::empty(100);
    inputs.signals.push(
        Signal::new(SignalKind::Injury, pos, ReporterKind::Police),
    );
    let commit = full_dispatch_tx(world, inputs);
    let ambs: Vec<_> = commit
        .effects
        .iter()
        .filter(|e| matches!(e, Effect::SpawnAmbulance { .. }))
        .collect();
    assert_eq!(ambs.len(), 1, "default EMS 1: {:?}", commit.effects);
    assert!(
        !commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnFiretruck { .. }))
    );

    // Fire signal → 1 firetruck (Fire is severe for reinforcement path; first full still
    // can request up to 2 if view empty — Fire/Explosion are severe).
    // MODEL: default 1; severe *may* dispatch more. Fire kind is severe.
    let world = World::default();
    let mut inputs = Inputs::empty(200);
    inputs.view_counts = ViewCounts { ems: 0, fire: 0 };
    inputs
        .signals
        .push(Signal::new(SignalKind::Fire, pos, ReporterKind::Police));
    let commit = full_dispatch_tx(world, inputs);
    let trucks: Vec<_> = commit
        .effects
        .iter()
        .filter(|e| matches!(e, Effect::SpawnFiretruck { .. }))
        .collect();
    // Severe fire with empty view → up to 2
    assert!(
        trucks.len() <= EMS_FIRE_VIEW_CAP as usize,
        "firetruck count {} exceeds cap",
        trucks.len()
    );
    assert!(!trucks.is_empty(), "expected firetruck: {:?}", commit.effects);

    // View already at 2 → no more same-dept spawns
    let world = World::default();
    let mut inputs = Inputs::empty(300);
    inputs.view_counts = ViewCounts { ems: 0, fire: 2 };
    inputs
        .signals
        .push(Signal::new(SignalKind::Fire, pos, ReporterKind::Police));
    let commit = full_dispatch_tx(world, inputs);
    let trucks: Vec<_> = commit
        .effects
        .iter()
        .filter(|e| matches!(e, Effect::SpawnFiretruck { .. }))
        .collect();
    assert!(
        trucks.is_empty(),
        "view cap blocks firetruck: {:?}",
        commit.effects
    );

    // Severe EMS with view=0 → 2 ambulances max
    let world = World::default();
    let mut inputs = Inputs::empty(400);
    inputs.view_counts = ViewCounts { ems: 0, fire: 0 };
    inputs.signals.push(
        Signal::new(SignalKind::Casualty, pos, ReporterKind::Police).with_criminals(1),
    );
    let commit = full_dispatch_tx(world, inputs);
    let ambs: Vec<_> = commit
        .effects
        .iter()
        .filter(|e| matches!(e, Effect::SpawnAmbulance { .. }))
        .collect();
    assert!(
        ambs.len() <= 2 && !ambs.is_empty(),
        "severe EMS ≤2: {:?}",
        commit.effects
    );

    // Independent caps: fire view full must not block EMS
    let world = World::default();
    let mut inputs = Inputs::empty(500);
    inputs.view_counts = ViewCounts { ems: 0, fire: 2 };
    inputs.signals.push(Signal::new(
        SignalKind::Explosion,
        pos,
        ReporterKind::Police,
    ));
    let commit = full_dispatch_tx(world, inputs);
    assert!(
        commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnAmbulance { .. })),
        "EMS still allowed when fire view full: {:?}",
        commit.effects
    );
    assert!(
        !commit
            .effects
            .iter()
            .any(|e| matches!(e, Effect::SpawnFiretruck { .. })),
        "fire blocked at view 2: {:?}",
        commit.effects
    );
}

// ---------------------------------------------------------------------------
// LightStep produces empty effects by default
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
    // Even with a civilian signal, Light only books in + clocks — no Effects.
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

    // Light with empty world + empty inputs
    let commit = light_step_tx(World::default(), Inputs::empty(1));
    assert!(commit.effects.is_empty());
    assert!(commit.world.cases.is_empty());
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

    let has_open = commit
        .effects
        .iter()
        .any(|e| matches!(e, Effect::OpenCase { .. }));
    let has_mobilize = commit
        .effects
        .iter()
        .any(|e| matches!(e, Effect::MobilizeNearby { .. }));
    let has_patrol = commit
        .effects
        .iter()
        .any(|e| matches!(e, Effect::SpawnPatrol { .. }));
    let has_swat = commit
        .effects
        .iter()
        .any(|e| matches!(e, Effect::SpawnSwat { .. }));
    let has_amb = commit
        .effects
        .iter()
        .any(|e| matches!(e, Effect::SpawnAmbulance { .. }));
    let has_suppress = commit
        .effects
        .iter()
        .any(|e| matches!(e, Effect::SuppressWanted));

    assert!(has_open, "OpenCase: {:?}", commit.effects);
    assert!(has_mobilize, "MobilizeNearby: {:?}", commit.effects);
    assert!(has_patrol, "SpawnPatrol: {:?}", commit.effects);
    assert!(has_swat, "SpawnSwat for gang gunfire: {:?}", commit.effects);
    assert!(has_amb, "SpawnAmbulance for injury: {:?}", commit.effects);
    assert!(has_suppress, "SuppressWanted: {:?}", commit.effects);
    assert_eq!(commit.world.cases.len(), 1);
    assert!(commit.world.cases[0].needs.police);
    assert!(commit.world.cases[0].needs.ems);
}

#[test]
fn should_full_dispatch_false_when_quiet() {
    let world = World::default();
    let inputs = Inputs::empty(10);
    assert!(!should_full_dispatch(&world, &inputs));
}
