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

fn estimate_criminal_threat(case: &CaseRecord) -> CriminalThreatLevel {
    if case.is_firearm {
        CriminalThreatLevel::FirearmInactive
    } else if case.reported_clues.has_kind(CausalKind::PedCasualty)
        || case.reported_clues.has_kind(CausalKind::PedInjury)
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
    pub const DENSITY_CAT2_MIN: i32 = 5;
    pub const DENSITY_CAT3_MIN: i32 = 8;
    pub const CONSOLIDATED_CAT2_MIN: i32 = 3;
    pub const ACTIVE_THREATS_CAT2_MIN: i32 = 3;
    pub const HIGH_THREATS_CAT3_MIN: i32 = 2;
    pub const THREAT_SCORE_CAT3_MIN: i32 = 22;
    pub const CAT2_SWAT_DENSITY_MIN: i32 = 7;
}

use response_thresholds::*;

pub fn classify_response_category(case: &CaseRecord, density: i32) -> ResponseCategory {
    let max_threat = threat_level_score(get_case_max_threat_level(case));
    let high_threats = count_high_threats(case);
    let active_threats = count_active_threats(case);
    let threat_score = compute_case_threat_score(case);
    let consolidated = case.criminals.len() as i32;
    let firearm_active = threat_level_score(CriminalThreatLevel::FirearmActive);
    let firearm_air = threat_level_score(CriminalThreatLevel::FirearmAirShoot);
    let melee_active = threat_level_score(CriminalThreatLevel::MeleeActive);
    let firearm = is_firearm_case(case);

    if max_threat >= firearm_active && density >= 6 {
        return ResponseCategory::Three;
    }
    if high_threats >= HIGH_THREATS_CAT3_MIN && density >= 4 {
        return ResponseCategory::Three;
    }
    if density >= DENSITY_CAT3_MIN && firearm && max_threat >= firearm_air {
        return ResponseCategory::Three;
    }
    if threat_score >= THREAT_SCORE_CAT3_MIN {
        return ResponseCategory::Three;
    }

    if density >= DENSITY_CAT2_MIN && density < DENSITY_CAT3_MIN {
        return ResponseCategory::Two;
    }
    if consolidated >= CONSOLIDATED_CAT2_MIN {
        return ResponseCategory::Two;
    }
    if active_threats >= ACTIVE_THREATS_CAT2_MIN {
        return ResponseCategory::Two;
    }
    if max_threat >= firearm_air && density >= 4 {
        return ResponseCategory::Two;
    }
    if consolidated >= 2 && max_threat >= melee_active && density >= 3 {
        return ResponseCategory::Two;
    }

    ResponseCategory::One
}

fn pick_category_three_variant(case: &CaseRecord, density: i32) -> i32 {
    let max_threat = threat_level_score(get_case_max_threat_level(case));
    let firearm_active = threat_level_score(CriminalThreatLevel::FirearmActive);

    if count_high_threats(case) >= 2 && density >= 7 {
        return 2;
    }
    if count_high_threats(case) >= 2 && density >= 6 {
        return 1;
    }
    0
}

pub fn build_category_spawn_plan(
    category: ResponseCategory,
    case: &CaseRecord,
    loc: WorldPos,
    density: i32,
    swat_already: bool,
) -> Vec<crate::models::PoliceSpawnUnit> {
    let mut plan = Vec::new();

    match category {
        ResponseCategory::One => push_patrol(&mut plan, loc),
        ResponseCategory::Two => {
            let max_threat = threat_level_score(get_case_max_threat_level(case));
            let firearm_active = threat_level_score(CriminalThreatLevel::FirearmActive);
            let prefer_swat = max_threat >= firearm_active
                || is_firearm_case(case)
                || density >= CAT2_SWAT_DENSITY_MIN;
            if prefer_swat && !swat_already {
                push_patrol(&mut plan, loc);
                push_swat(&mut plan, swat_already);
            } else {
                push_patrol(&mut plan, loc);
                push_patrol(&mut plan, loc);
            }
        }
        ResponseCategory::Three => {
            let variant = pick_category_three_variant(case, density);
            match variant {
                2 => {
                    push_swat(&mut plan, swat_already);
                    push_fbi(&mut plan);
                    push_fbi(&mut plan);
                    if plan.is_empty() {
                        push_patrol(&mut plan, loc);
                        push_fbi(&mut plan);
                        push_fbi(&mut plan);
                    }
                }
                1 => {
                    push_patrol(&mut plan, loc);
                    push_swat(&mut plan, swat_already);
                    push_fbi(&mut plan);
                }
                _ => {
                    push_patrol(&mut plan, loc);
                    push_patrol(&mut plan, loc);
                    push_fbi(&mut plan);
                }
            }
        }
    }

    plan
}

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

pub fn build_reinforcement_spawn_plan(
    case: &CaseRecord,
    loc: WorldPos,
    reinforcement_wave: i32,
    density: i32,
    swat_already: bool,
) -> Vec<crate::models::PoliceSpawnUnit> {
    let category = classify_response_category(case, density);
    let composition = build_category_spawn_plan(category, case, loc, density, swat_already);
    if composition.is_empty() {
        return composition;
    }
    let pick = (reinforcement_wave.saturating_sub(1).max(0) as usize) % composition.len();
    tracing::info!(
        category = category.name(),
        pick,
        total = composition.len(),
        wave = reinforcement_wave,
        "reinforcement spawn unit"
    );
    vec![composition[pick].clone()]
}

/// On-scene reinforcement when the situation still feels under-resourced — not tied to cop deaths.
pub fn on_scene_needs_reinforcement(case: &CaseRecord, density: i32) -> bool {
    if case.criminals.is_empty() || !case.police_script_active() {
        return false;
    }

    let max_threat = threat_level_score(get_case_max_threat_level(case));
    let active_threats = count_active_threats(case);
    let high_threats = count_high_threats(case);
    let threat_score = compute_case_threat_score(case);
    let firearm_active = threat_level_score(CriminalThreatLevel::FirearmActive);
    let firearm_air = threat_level_score(CriminalThreatLevel::FirearmAirShoot);
    let ongoing_gunfire = case.has_live_kind(CausalKind::WeaponDischarge);

    if ongoing_gunfire && active_threats >= 1 {
        return true;
    }
    if is_firearm_case(case) && case.criminals.len() >= 2 && density >= 3 {
        return true;
    }
    if max_threat >= firearm_active && active_threats >= 2 && density >= 4 {
        return true;
    }
    if high_threats >= 1 && threat_score >= 12 && density >= 5 {
        return true;
    }
    if classify_response_category(case, density) >= ResponseCategory::Two
        && active_threats >= 3
        && max_threat >= firearm_air
    {
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
    fn firearm_case_escalates_with_density() {
        let case = sample_case(true);
        assert_eq!(classify_response_category(&case, 8), ResponseCategory::Three);
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
        case.mark_live_kind(CausalKind::WeaponDischarge);
        case.criminals = vec![dispatch_core::PedId::default()];
        assert!(on_scene_needs_reinforcement(&case, 2));
        assert_eq!(case.cops_killed, 0);
    }
}