//! Threat scoring and response quotas (ported from `dispatch_threat.cpp`).

use dispatch_case::CaseRecord;
use dispatch_core::{CausalKind, PedId, WorldPos};

use crate::models::{
    PoliceSpawnUnit, ResponseCategory, MODEL_FBI_RANCHER, MODEL_SWAT_VAN,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum CriminalThreatLevel {
    UnarmedInactive = 0,
    UnarmedActive = 1,
    MeleeInactive = 2,
    MeleeActive = 3,
    FirearmInactive = 4,
    FirearmAirShoot = 5,
    FirearmActive = 6,
}

pub fn threat_level_score(level: CriminalThreatLevel) -> i32 {
    level as i32
}

#[derive(Debug, Clone, Copy, Default)]
pub struct ResponseQuota {
    pub max_vehicles: i32,
    pub max_foot_cops: i32,
}

impl ResponseQuota {
    pub fn new(max_vehicles: i32, max_foot_cops: i32) -> Self {
        Self {
            max_vehicles,
            max_foot_cops,
        }
    }
}

pub fn get_case_max_threat_level(case: &CaseRecord) -> CriminalThreatLevel {
    let mut max_level = CriminalThreatLevel::UnarmedInactive;
    for _ in &case.criminals {
        let level = estimate_criminal_threat(case);
        if threat_level_score(level) > threat_level_score(max_level) {
            max_level = level;
        }
    }
    if case.is_firearm
        && threat_level_score(max_level) < threat_level_score(CriminalThreatLevel::FirearmInactive)
    {
        max_level = CriminalThreatLevel::FirearmInactive;
    }
    max_level
}

/// Case-level threat estimate from clues + live signals (pursuit config input).
fn estimate_criminal_threat(case: &CaseRecord) -> CriminalThreatLevel {
    let firearm = case.is_firearm
        || case.reported_clues.has_kind(CausalKind::WeaponDischarge)
        || case.has_live_kind(CausalKind::WeaponDischarge);
    if firearm {
        // Ongoing discharge is active firearm threat; otherwise armed but not currently firing.
        if case.has_live_kind(CausalKind::WeaponDischarge) {
            CriminalThreatLevel::FirearmActive
        } else {
            CriminalThreatLevel::FirearmInactive
        }
    } else if case.reported_clues.has_kind(CausalKind::PedCasualty)
        || case.reported_clues.has_kind(CausalKind::PedInjury)
        || case.has_live_kind(CausalKind::PedCasualty)
        || case.has_live_kind(CausalKind::PedInjury)
    {
        CriminalThreatLevel::MeleeActive
    } else {
        CriminalThreatLevel::MeleeInactive
    }
}

pub fn compute_case_threat_score(case: &CaseRecord) -> i32 {
    let mut total = 0;
    for _ in &case.criminals {
        total += threat_level_score(estimate_criminal_threat(case));
    }
    if case.is_firearm && total == 0 {
        total += threat_level_score(CriminalThreatLevel::FirearmInactive);
    }
    total
}

pub fn count_active_threats(case: &CaseRecord) -> i32 {
    let level = get_case_max_threat_level(case);
    if threat_level_score(level) >= threat_level_score(CriminalThreatLevel::MeleeActive) {
        case.criminals.len() as i32
    } else {
        0
    }
}

pub fn count_high_threats(case: &CaseRecord) -> i32 {
    let level = get_case_max_threat_level(case);
    if threat_level_score(level) >= threat_level_score(CriminalThreatLevel::FirearmAirShoot) {
        case.criminals.len() as i32
    } else {
        0
    }
}

pub fn is_firearm_case(case: &CaseRecord) -> bool {
    if case.is_firearm {
        return true;
    }
    threat_level_score(get_case_max_threat_level(case))
        >= threat_level_score(CriminalThreatLevel::FirearmInactive)
}

pub fn compute_dispatch_anchor(case: &CaseRecord, criminal_positions: &[WorldPos]) -> WorldPos {
    if criminal_positions.is_empty() {
        return case.dispatch_anchor;
    }
    let weight = threat_level_score(get_case_max_threat_level(case)).max(1) as f32;
    let n = criminal_positions.len() as f32;
    let sum = criminal_positions.iter().fold(WorldPos::default(), |acc, p| WorldPos {
        x: acc.x + p.x,
        y: acc.y + p.y,
        z: acc.z + p.z,
    });
    WorldPos {
        x: sum.x / n,
        y: sum.y / n,
        z: sum.z / n,
    }
}

pub fn compute_response_quota(case: &CaseRecord) -> ResponseQuota {
    let max_threat = threat_level_score(get_case_max_threat_level(case));
    let threat_score = compute_case_threat_score(case);
    let active_threats = count_active_threats(case);
    let high_threats = count_high_threats(case);
    let total = case.criminals.len() as i32;

    let mut quota = ResponseQuota::new(1, 1);

    if max_threat >= threat_level_score(CriminalThreatLevel::FirearmActive) || high_threats >= 2 {
        quota = ResponseQuota::new(3, 4);
    } else if max_threat >= threat_level_score(CriminalThreatLevel::FirearmAirShoot) {
        quota = ResponseQuota::new(3, 3);
    } else if max_threat >= threat_level_score(CriminalThreatLevel::FirearmInactive) {
        quota = ResponseQuota::new(2, if active_threats >= 2 { 3 } else { 2 });
    } else if max_threat >= threat_level_score(CriminalThreatLevel::MeleeActive) {
        quota = ResponseQuota::new(
            if total <= 1 { 1 } else { 2 },
            if total <= 1 { 1 } else { 2 },
        );
    }

    if threat_score >= 20 {
        quota.max_foot_cops = quota.max_foot_cops.max(4);
        quota.max_vehicles = quota.max_vehicles.max(3);
    } else if threat_score >= 12 {
        quota.max_foot_cops = quota.max_foot_cops.max(3);
        quota.max_vehicles = quota.max_vehicles.max(2);
    }

    quota
}

pub fn compute_nearby_dispatch_quota(case: &CaseRecord, density: i32) -> i32 {
    let threat_score = compute_case_threat_score(case);
    let active_threats = count_active_threats(case);

    if threat_score >= 20 || active_threats >= 4 || density >= 6 {
        4
    } else if threat_score >= 12 || active_threats >= 2 || density >= 3 {
        2
    } else {
        1
    }
}

pub fn pick_criminal_target_for_cop(
    case: &CaseRecord,
    cop_pos: WorldPos,
    criminal_positions: &[(PedId, WorldPos)],
) -> Option<PedId> {
    const THREAT_WEIGHT: f32 = 45.0;
    const NEAR_PREFER_RADIUS: f32 = 15.0;

    let threat = threat_level_score(get_case_max_threat_level(case)) as f32;
    let mut best: Option<(PedId, f32)> = None;

    for &(id, crim_pos) in criminal_positions {
        let dx = cop_pos.x - crim_pos.x;
        let dy = cop_pos.y - crim_pos.y;
        let dz = cop_pos.z - crim_pos.z;
        let dist = (dx * dx + dy * dy + dz * dz).sqrt();
        let mut score = threat * THREAT_WEIGHT - dist;
        if dist <= NEAR_PREFER_RADIUS {
            score += threat * 10.0;
        }
        if best.map(|(_, s)| score > s).unwrap_or(true) {
            best = Some((id, score));
        }
    }

    best.map(|(id, _)| id).or(case.primary)
}

mod response_thresholds {
    /// Density only *boosts* multi-offender / firearm scenes — threat level is primary.
    pub const DENSITY_BOOST_CAT2: i32 = 5;
    pub const DENSITY_BOOST_CAT3: i32 = 7;
    pub const CONSOLIDATED_CAT2_MIN: i32 = 3;
    pub const THREAT_SCORE_CAT3_MIN: i32 = 22;
}

use response_thresholds::*;

/// Pursuit / offscreen composition from **criminal threat first**, density/count as boost only.
pub fn classify_response_category(case: &CaseRecord, density: i32) -> ResponseCategory {
    let level = get_case_max_threat_level(case);
    let n = case.criminals.len() as i32;
    let threat_score = compute_case_threat_score(case);

    match level {
        CriminalThreatLevel::FirearmActive => {
            if n >= 2 || threat_score >= 12 || density >= DENSITY_BOOST_CAT3 {
                ResponseCategory::Three
            } else {
                ResponseCategory::Two
            }
        }
        CriminalThreatLevel::FirearmAirShoot | CriminalThreatLevel::FirearmInactive => {
            if n >= 3 || threat_score >= THREAT_SCORE_CAT3_MIN || density >= DENSITY_BOOST_CAT3 {
                ResponseCategory::Three
            } else {
                // Any firearm pursuit is at least Cat2 configuration.
                ResponseCategory::Two
            }
        }
        CriminalThreatLevel::MeleeActive => {
            if n >= CONSOLIDATED_CAT2_MIN || density >= DENSITY_BOOST_CAT2 {
                ResponseCategory::Two
            } else {
                ResponseCategory::One
            }
        }
        CriminalThreatLevel::MeleeInactive
        | CriminalThreatLevel::UnarmedActive
        | CriminalThreatLevel::UnarmedInactive => {
            if n >= CONSOLIDATED_CAT2_MIN || density >= DENSITY_BOOST_CAT2 {
                ResponseCategory::Two
            } else {
                ResponseCategory::One
            }
        }
    }
}

/// Multi-criminal "gang / consolidated" bar for special units (SWAT/FBI).
const GANG_CONSOLIDATED_MIN: i32 = 3;

/// SWAT is rare: multi-offender consolidation **and** other severity, or extreme active threat.
pub fn swat_warranted(case: &CaseRecord, density: i32) -> bool {
    let n = case.criminals.len() as i32;
    let level = get_case_max_threat_level(case);
    let score = compute_case_threat_score(case);
    let gang = n >= GANG_CONSOLIDATED_MIN;
    let severe = is_firearm_case(case)
        || matches!(level, CriminalThreatLevel::FirearmActive)
        || score >= 12
        || density >= DENSITY_BOOST_CAT3;

    // Typical path: multi-criminal case + severity.
    if gang && severe {
        return true;
    }
    // Exception: very high individual threat without full gang size.
    if matches!(level, CriminalThreatLevel::FirearmActive) && (n >= 2 || score >= 16) {
        return true;
    }
    false
}

/// FBI is rarer than SWAT: gang-scale + high threat / Cat3 pressure.
pub fn fbi_warranted(case: &CaseRecord, density: i32) -> bool {
    let n = case.criminals.len() as i32;
    let level = get_case_max_threat_level(case);
    let score = compute_case_threat_score(case);
    if n < GANG_CONSOLIDATED_MIN {
        // Only extreme active multi-shooter without waiting for 3 IDs.
        return matches!(level, CriminalThreatLevel::FirearmActive) && n >= 2 && score >= 16;
    }
    let high = matches!(
        level,
        CriminalThreatLevel::FirearmActive | CriminalThreatLevel::FirearmAirShoot
    ) || score >= THREAT_SCORE_CAT3_MIN
        || density >= DENSITY_BOOST_CAT3;
    high && (is_firearm_case(case) || matches!(level, CriminalThreatLevel::FirearmActive))
}

pub fn build_category_spawn_plan(
    category: ResponseCategory,
    case: &CaseRecord,
    loc: WorldPos,
    density: i32,
    swat_already: bool,
) -> Vec<crate::models::PoliceSpawnUnit> {
    let mut plan = Vec::new();
    let allow_swat = swat_warranted(case, density) && !swat_already;
    let allow_fbi = fbi_warranted(case, density);

    match category {
        // Default response: regional two-person patrol car(s). Never bike.
        ResponseCategory::One => push_patrol(&mut plan, loc),
        ResponseCategory::Two => {
            // Ordinary escalate: second patrol unit. SWAT only if gang+severity bar met.
            push_patrol(&mut plan, loc);
            if allow_swat {
                push_swat(&mut plan, swat_already);
            } else {
                push_patrol(&mut plan, loc);
            }
        }
        ResponseCategory::Three => {
            // Heavy scene still starts with patrols; special units only if warranted.
            push_patrol(&mut plan, loc);
            push_patrol(&mut plan, loc);
            if allow_swat {
                push_swat(&mut plan, swat_already);
            }
            if allow_fbi {
                push_fbi(&mut plan);
            }
            // If neither special unit unlocked, a third patrol for mass response.
            if !allow_swat && !allow_fbi {
                push_patrol(&mut plan, loc);
            }
        }
    }

    plan
}

/// Two-person regional patrol car only — never police bike (unsuitable for reinforce/spawn plan).
fn push_patrol(plan: &mut Vec<PoliceSpawnUnit>, loc: WorldPos) {
    plan.push(PoliceSpawnUnit {
        model: crate::models::local_patrol_model(loc),
        register_swat: false,
    });
}

fn push_swat(plan: &mut Vec<PoliceSpawnUnit>, swat_already: bool) {
    if !swat_already {
        append_unique_unit(
            plan,
            PoliceSpawnUnit {
                model: MODEL_SWAT_VAN,
                register_swat: true,
            },
        );
    }
}

fn push_fbi(plan: &mut Vec<PoliceSpawnUnit>) {
    append_unique_unit(
        plan,
        PoliceSpawnUnit {
            model: MODEL_FBI_RANCHER,
            register_swat: false,
        },
    );
}

fn append_unique_unit(plan: &mut Vec<PoliceSpawnUnit>, unit: PoliceSpawnUnit) {
    if plan.iter().any(|u| u.model == unit.model) {
        return;
    }
    plan.push(unit);
}

pub fn build_initial_spawn_plan(
    case: &CaseRecord,
    loc: WorldPos,
    density: i32,
    swat_already: bool,
) -> Vec<crate::models::PoliceSpawnUnit> {
    let category = classify_response_category(case, density);
    tracing::info!(
        category = category.name(),
        density,
        firearm = is_firearm_case(case),
        "initial spawn plan"
    );
    build_category_spawn_plan(category, case, loc, density, swat_already)
}

/// On-scene top-up: default **one patrol car** (two officers). SWAT/FBI only if warranted.
/// Never police bike.
pub fn build_on_scene_topup_plan(
    case: &CaseRecord,
    loc: WorldPos,
    density: i32,
    swat_already: bool,
) -> Vec<crate::models::PoliceSpawnUnit> {
    let level = get_case_max_threat_level(case);
    let category = classify_response_category(case, density);
    let mut plan = Vec::new();

    // Special units only when gang/threat bars are met — not for ordinary firearm calls.
    if swat_warranted(case, density) && !swat_already {
        push_swat(&mut plan, swat_already);
    } else if fbi_warranted(case, density) && category >= ResponseCategory::Three {
        push_fbi(&mut plan);
    } else {
        push_patrol(&mut plan, loc);
    }

    tracing::info!(
        category = category.name(),
        threat = ?level,
        swat = swat_warranted(case, density),
        fbi = fbi_warranted(case, density),
        units = plan.len(),
        "on-scene top-up (patrol default; special units gated)"
    );
    plan
}

/// Scene still under-resourced (ongoing threat) — not cop deaths, not pursuit "waves".
pub fn on_scene_needs_reinforcement(case: &CaseRecord, density: i32) -> bool {
    if case.criminals.is_empty() || !case.police_script_active() {
        return false;
    }

    let level = get_case_max_threat_level(case);
    let n = case.criminals.len() as i32;
    let ongoing_gunfire = case.has_live_kind(CausalKind::WeaponDischarge);

    if ongoing_gunfire {
        return true;
    }
    if is_firearm_case(case) && n >= 2 {
        return true;
    }
    if matches!(level, CriminalThreatLevel::FirearmActive) {
        return true;
    }
    if matches!(level, CriminalThreatLevel::MeleeActive) && n >= 2 && density >= 3 {
        return true;
    }
    if classify_response_category(case, density) >= ResponseCategory::Two && n >= 3 {
        return true;
    }

    false
}

pub fn get_case_threat_tier(case: &CaseRecord) -> i32 {
    let score = threat_level_score(get_case_max_threat_level(case));
    if score >= threat_level_score(CriminalThreatLevel::FirearmInactive) {
        2
    } else if score >= threat_level_score(CriminalThreatLevel::MeleeActive) {
        1
    } else if case.is_firearm {
        2
    } else {
        1
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_case::ReportClues;
    use dispatch_core::CausalKind;

    fn sample_case(firearm: bool) -> CaseRecord {
        let mut clues = ReportClues::default();
        if firearm {
            clues.kinds |= 1 << CausalKind::WeaponDischarge as u8;
        }
        CaseRecord::new(1, None, None, None, clues, vec![], 0, 8_000)
    }

    #[test]
    fn classify_light_case_as_category_one() {
        let case = sample_case(false);
        assert_eq!(classify_response_category(&case, 1), ResponseCategory::One);
    }

    #[test]
    fn firearm_threat_sets_pursuit_config() {
        // Firearm (not live) → at least Cat2 regardless of low density.
        let case = sample_case(true);
        assert_eq!(classify_response_category(&case, 1), ResponseCategory::Two);

        // Six FirearmInactive criminals → high threat_score → Cat3.
        let mut dense = sample_case(true);
        dense.criminals = vec![dispatch_core::PedId::default(); 6];
        assert_eq!(
            classify_response_category(&dense, 1),
            ResponseCategory::Three
        );

        // Live discharge elevates to FirearmActive.
        let mut live = sample_case(true);
        live.criminals = vec![dispatch_core::PedId::default()];
        live.mark_live_kind(CausalKind::WeaponDischarge);
        assert_eq!(
            get_case_max_threat_level(&live),
            CriminalThreatLevel::FirearmActive
        );
    }

    #[test]
    fn on_scene_topup_defaults_to_patrol_not_swat() {
        let mut case = sample_case(true);
        case.criminals = vec![dispatch_core::PedId::default()];
        let plan = build_on_scene_topup_plan(&case, WorldPos::default(), 2, false);
        assert_eq!(plan.len(), 1);
        assert!(!plan[0].register_swat);
        assert!(!crate::models::is_swat_model(plan[0].model));
        // Patrol car models only — never bike for top-up.
        assert_ne!(plan[0].model, crate::models::MODEL_POLICE_BIKE);
    }

    #[test]
    fn swat_requires_gang_scale_not_lone_firearm() {
        let mut lone = sample_case(true);
        lone.criminals = vec![dispatch_core::PedId::default()];
        assert!(!swat_warranted(&lone, 2));

        let mut gang = sample_case(true);
        gang.criminals = vec![dispatch_core::PedId::default(); 3];
        assert!(swat_warranted(&gang, 2));

        let plan = build_category_spawn_plan(
            ResponseCategory::Two,
            &gang,
            WorldPos::default(),
            2,
            false,
        );
        assert!(plan.iter().any(|u| u.register_swat));
    }

    #[test]
    fn initial_spawn_plan_includes_patrol_for_cat_one() {
        let case = sample_case(false);
        let plan = build_initial_spawn_plan(&case, WorldPos::default(), 1, false);
        assert_eq!(plan.len(), 1);
        assert!(!plan[0].register_swat);
    }

    #[test]
    fn reinforcement_triggers_on_ongoing_gunfire_not_cop_death() {
        let mut case = sample_case(true);
        case.criminals = vec![dispatch_core::PedId::default()];
        case.mark_live_kind(CausalKind::WeaponDischarge);
        case.refresh_department_needs();
        assert!(case.police_script_active());
        assert!(on_scene_needs_reinforcement(&case, 2));
        assert_eq!(case.cops_killed, 0);
    }
}