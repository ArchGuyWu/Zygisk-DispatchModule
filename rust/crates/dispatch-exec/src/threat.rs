//! Threat → response size → spawn/top-up (coarsened for in-game signal).
//!
//! Player-visible outcomes are few: patrol count, SWAT/FBI or not, nearby cap.
//! Keep three threat buckets only; no dead AirShoot/UnarmedActive tiers.

use dispatch_case::CaseRecord;
use dispatch_core::{CausalKind, PedId, WorldPos};

use crate::models::{
    PoliceSpawnUnit, ResponseCategory, MODEL_FBI_RANCHER, MODEL_SWAT_VAN,
};

/// Case-level threat — only values `case_threat` can produce.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum CriminalThreatLevel {
    /// Unarmed / melee / injury scene (no firearm).
    Melee = 0,
    /// Firearm reported or flagged; not currently live discharge.
    Armed = 1,
    /// Ongoing `WeaponDischarge` live signal.
    ActiveFire = 2,
}

/// Multi-criminal bar for SWAT/FBI (gang / consolidated).
const GANG_MIN: i32 = 3;
/// Single density boost threshold (busy area) — no 5-vs-7 split.
const DENSITY_BUSY: i32 = 6;

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

/// Single case-level estimate (not per-criminal fake loop).
pub fn case_threat(case: &CaseRecord) -> CriminalThreatLevel {
    let firearm = case.is_firearm
        || case.reported_clues.has_kind(CausalKind::WeaponDischarge)
        || case.has_live_kind(CausalKind::WeaponDischarge);
    if firearm {
        if case.has_live_kind(CausalKind::WeaponDischarge) {
            CriminalThreatLevel::ActiveFire
        } else {
            CriminalThreatLevel::Armed
        }
    } else {
        CriminalThreatLevel::Melee
    }
}

/// Compatibility alias used across exec.
pub fn get_case_max_threat_level(case: &CaseRecord) -> CriminalThreatLevel {
    case_threat(case)
}

pub fn is_firearm_case(case: &CaseRecord) -> bool {
    case.is_firearm
        || case.reported_clues.has_kind(CausalKind::WeaponDischarge)
        || case.has_live_kind(CausalKind::WeaponDischarge)
        || matches!(
            case_threat(case),
            CriminalThreatLevel::Armed | CriminalThreatLevel::ActiveFire
        )
}

/// Rough size signal: offenders × coarse threat rank (replaces multi-threshold scores).
pub fn compute_case_threat_score(case: &CaseRecord) -> i32 {
    let n = case.criminals.len().max(if is_firearm_case(case) { 1 } else { 0 }) as i32;
    let rank = match case_threat(case) {
        CriminalThreatLevel::Melee => 1,
        CriminalThreatLevel::Armed => 2,
        CriminalThreatLevel::ActiveFire => 3,
    };
    n * rank
}

/// Offenders that still count as “active” for scene engagement (armed or active fire, or melee with IDs).
pub fn count_active_threats(case: &CaseRecord) -> i32 {
    match case_threat(case) {
        CriminalThreatLevel::Melee if case.criminals.is_empty() => 0,
        _ => case.criminals.len() as i32,
    }
}

/// Live gunfire offenders (replaces AirShoot-based high-threat count).
pub fn count_high_threats(case: &CaseRecord) -> i32 {
    if matches!(case_threat(case), CriminalThreatLevel::ActiveFire) {
        case.criminals.len().max(1) as i32
    } else {
        0
    }
}

pub fn compute_dispatch_anchor(case: &CaseRecord, criminal_positions: &[WorldPos]) -> WorldPos {
    if criminal_positions.is_empty() {
        return case.dispatch_anchor;
    }
    // Plain centroid — no unused threat weight.
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

/// Unified response sizing: Cat + attack quota + nearby cap from the same inputs.
#[derive(Debug, Clone, Copy)]
pub struct ResponseSize {
    pub category: ResponseCategory,
    pub max_vehicles: i32,
    pub max_foot_cops: i32,
    pub nearby_cops: i32,
}

pub fn response_size(case: &CaseRecord, density: i32) -> ResponseSize {
    let threat = case_threat(case);
    let n = case.criminals.len() as i32;
    let busy = density >= DENSITY_BUSY;
    let multi = n >= GANG_MIN;
    let pair = n >= 2;

    let category = match threat {
        CriminalThreatLevel::ActiveFire => {
            if pair || multi || busy {
                ResponseCategory::Three
            } else {
                ResponseCategory::Two
            }
        }
        CriminalThreatLevel::Armed => {
            if multi || busy {
                ResponseCategory::Three
            } else {
                ResponseCategory::Two
            }
        }
        CriminalThreatLevel::Melee => {
            if multi || busy {
                ResponseCategory::Two
            } else {
                ResponseCategory::One
            }
        }
    };

    // Simple caps players can feel: more cars/feet only when armed / multi.
    let (max_vehicles, max_foot_cops) = match threat {
        CriminalThreatLevel::ActiveFire => (if pair { 3 } else { 2 }, if pair { 4 } else { 3 }),
        CriminalThreatLevel::Armed => (2, if pair { 3 } else { 2 }),
        CriminalThreatLevel::Melee => {
            if multi {
                (2, 2)
            } else {
                (1, 1)
            }
        }
    };

    let nearby_cops = match threat {
        CriminalThreatLevel::ActiveFire | CriminalThreatLevel::Armed if multi || pair => 2,
        CriminalThreatLevel::ActiveFire | CriminalThreatLevel::Armed => 2,
        CriminalThreatLevel::Melee if multi => 2,
        _ => 1,
    };

    ResponseSize {
        category,
        max_vehicles,
        max_foot_cops,
        nearby_cops,
    }
}

pub fn classify_response_category(case: &CaseRecord, density: i32) -> ResponseCategory {
    response_size(case, density).category
}

pub fn compute_response_quota(case: &CaseRecord) -> ResponseQuota {
    let s = response_size(case, 0);
    ResponseQuota::new(s.max_vehicles, s.max_foot_cops)
}

pub fn compute_nearby_dispatch_quota(case: &CaseRecord, density: i32) -> i32 {
    response_size(case, density).nearby_cops
}

/// 1 = non-gun, 2 = gun (reroute / snapshot).
pub fn get_case_threat_tier(case: &CaseRecord) -> i32 {
    if is_firearm_case(case) {
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
    // Nearest criminal — threat weight was case-level constant and added no ranking.
    let mut best: Option<(PedId, f32)> = None;
    for &(id, crim_pos) in criminal_positions {
        let dx = cop_pos.x - crim_pos.x;
        let dy = cop_pos.y - crim_pos.y;
        let dz = cop_pos.z - crim_pos.z;
        let dist = (dx * dx + dy * dy + dz * dz).sqrt();
        if best.map(|(_, d)| dist < d).unwrap_or(true) {
            best = Some((id, dist));
        }
    }
    best.map(|(id, _)| id).or(case.primary)
}

/// SWAT: multi-offender consolidation + severity, or extreme active multi fire.
pub fn swat_warranted(case: &CaseRecord, density: i32) -> bool {
    let n = case.criminals.len() as i32;
    let threat = case_threat(case);
    let gang = n >= GANG_MIN;
    let severe = is_firearm_case(case)
        || matches!(threat, CriminalThreatLevel::ActiveFire)
        || density >= DENSITY_BUSY;

    if gang && severe {
        return true;
    }
    // Exception: active fire with 2+ known offenders.
    matches!(threat, CriminalThreatLevel::ActiveFire) && n >= 2
}

/// FBI rarer: gang + armed/active, or active multi without full gang ID count.
pub fn fbi_warranted(case: &CaseRecord, density: i32) -> bool {
    let n = case.criminals.len() as i32;
    let threat = case_threat(case);
    if n < GANG_MIN {
        return matches!(threat, CriminalThreatLevel::ActiveFire) && n >= 2;
    }
    is_firearm_case(case)
        && (matches!(
            threat,
            CriminalThreatLevel::ActiveFire | CriminalThreatLevel::Armed
        ) || density >= DENSITY_BUSY)
}

pub fn build_category_spawn_plan(
    category: ResponseCategory,
    case: &CaseRecord,
    loc: WorldPos,
    density: i32,
    swat_already: bool,
) -> Vec<PoliceSpawnUnit> {
    let mut plan = Vec::new();
    let allow_swat = swat_warranted(case, density) && !swat_already;
    let allow_fbi = fbi_warranted(case, density);

    match category {
        ResponseCategory::One => push_patrol(&mut plan, loc),
        ResponseCategory::Two => {
            push_patrol(&mut plan, loc);
            if allow_swat {
                push_swat(&mut plan, swat_already);
            } else {
                push_patrol(&mut plan, loc);
            }
        }
        ResponseCategory::Three => {
            // Cap mass response at two patrols unless special units unlock.
            push_patrol(&mut plan, loc);
            push_patrol(&mut plan, loc);
            if allow_swat {
                push_swat(&mut plan, swat_already);
            }
            if allow_fbi {
                push_fbi(&mut plan);
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
) -> Vec<PoliceSpawnUnit> {
    let category = classify_response_category(case, density);
    tracing::info!(
        category = category.name(),
        density,
        firearm = is_firearm_case(case),
        threat = ?case_threat(case),
        "initial spawn plan"
    );
    build_category_spawn_plan(category, case, loc, density, swat_already)
}

/// On-scene top-up: default one patrol car; SWAT/FBI only if warranted. Never bike.
pub fn build_on_scene_topup_plan(
    case: &CaseRecord,
    loc: WorldPos,
    density: i32,
    swat_already: bool,
) -> Vec<PoliceSpawnUnit> {
    let mut plan = Vec::new();
    if swat_warranted(case, density) && !swat_already {
        push_swat(&mut plan, swat_already);
    } else if fbi_warranted(case, density)
        && classify_response_category(case, density) >= ResponseCategory::Three
    {
        push_fbi(&mut plan);
    } else {
        push_patrol(&mut plan, loc);
    }
    tracing::info!(
        threat = ?case_threat(case),
        swat = swat_warranted(case, density),
        fbi = fbi_warranted(case, density),
        units = plan.len(),
        "on-scene top-up"
    );
    plan
}

pub fn on_scene_needs_reinforcement(case: &CaseRecord, density: i32) -> bool {
    if case.criminals.is_empty() || !case.police_script_active() {
        return false;
    }
    let n = case.criminals.len() as i32;
    let threat = case_threat(case);
    if case.has_live_kind(CausalKind::WeaponDischarge) {
        return true;
    }
    if is_firearm_case(case) && n >= 2 {
        return true;
    }
    if matches!(threat, CriminalThreatLevel::ActiveFire) {
        return true;
    }
    if matches!(threat, CriminalThreatLevel::Melee) && n >= 2 && density >= 3 {
        return true;
    }
    classify_response_category(case, density) >= ResponseCategory::Two && n >= 3
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
        let case = sample_case(true);
        assert_eq!(classify_response_category(&case, 1), ResponseCategory::Two);
        assert_eq!(case_threat(&case), CriminalThreatLevel::Armed);

        // Gang-scale armed → Cat3.
        let mut dense = sample_case(true);
        dense.criminals = vec![dispatch_core::PedId::default(); 3];
        assert_eq!(
            classify_response_category(&dense, 1),
            ResponseCategory::Three
        );

        let mut live = sample_case(true);
        live.criminals = vec![dispatch_core::PedId::default()];
        live.mark_live_kind(CausalKind::WeaponDischarge);
        assert_eq!(case_threat(&live), CriminalThreatLevel::ActiveFire);
    }

    #[test]
    fn response_size_unifies_quota_and_category() {
        let mut case = sample_case(true);
        case.criminals = vec![dispatch_core::PedId::default(); 2];
        let s = response_size(&case, 0);
        assert_eq!(s.category, ResponseCategory::Two);
        let q = compute_response_quota(&case);
        assert_eq!(q.max_vehicles, s.max_vehicles);
        assert_eq!(q.max_foot_cops, s.max_foot_cops);
        assert_eq!(compute_nearby_dispatch_quota(&case, 0), s.nearby_cops);
    }

    #[test]
    fn on_scene_topup_defaults_to_patrol_not_swat() {
        let mut case = sample_case(true);
        case.criminals = vec![dispatch_core::PedId::default()];
        let plan = build_on_scene_topup_plan(&case, WorldPos::default(), 2, false);
        assert_eq!(plan.len(), 1);
        assert!(!plan[0].register_swat);
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
    fn cat3_without_special_units_caps_at_two_patrols() {
        // Melee cannot be Cat3; use armed non-gang busy density for Cat3 without SWAT
        // Armed + busy density → Cat3; n=1 → no SWAT → two patrols only.
        let mut case = sample_case(true);
        case.criminals = vec![dispatch_core::PedId::default()];
        assert_eq!(
            classify_response_category(&case, DENSITY_BUSY),
            ResponseCategory::Three
        );
        assert!(!swat_warranted(&case, DENSITY_BUSY));
        let plan = build_initial_spawn_plan(&case, WorldPos::default(), DENSITY_BUSY, false);
        assert_eq!(plan.len(), 2);
        assert!(plan.iter().all(|u| !u.register_swat));
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

    #[test]
    fn dispatch_anchor_is_plain_centroid() {
        let case = sample_case(false);
        let positions = [
            WorldPos {
                x: 0.0,
                y: 0.0,
                z: 0.0,
            },
            WorldPos {
                x: 10.0,
                y: 20.0,
                z: 0.0,
            },
        ];
        let a = compute_dispatch_anchor(&case, &positions);
        assert!((a.x - 5.0).abs() < 0.01);
        assert!((a.y - 10.0).abs() < 0.01);
    }
}
