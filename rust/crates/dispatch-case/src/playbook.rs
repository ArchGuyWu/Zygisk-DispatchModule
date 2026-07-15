//! Dispatch playbook — defines *who* should respond and *when*; native engine completes *how*.
//!
//! Each department has a script:
//! - **CivilianReport**: witness → phone task → clues (native `CTaskComplexUseMobilePhone`)
//! - **Police**: if `needs.police` → delay/mobilize/spawn → `CTaskComplexInvestigateDisturbance` to scene;
//!   on abnormality, native pursuit/arrest overlays via coordinator.
//! - **Ems**: if `needs.ems` → spawn ambulance → investigate-disturbance to scene; medic AI when casualties on scene.
//! - **Fire**: if `needs.fire` → spawn firetruck → investigate-disturbance to scene; fire AI when fire on scene.
//!
//! Rust advances the script; the engine executes tasks, driving, and occupant behavior.

use dispatch_core::CausalKind;

use crate::incident::DepartmentSet;
use crate::record::CaseRecord;

/// Which departments this case requires — single source of truth for all exec paths.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DepartmentNeeds {
    pub police: bool,
    pub ems: bool,
    pub fire: bool,
}

impl DepartmentNeeds {
    pub fn from_incident(deps: DepartmentSet) -> Self {
        Self {
            police: deps.police,
            ems: deps.ems,
            fire: deps.fire,
        }
    }

    pub fn merge_incident(&mut self, deps: DepartmentSet) {
        if deps.police {
            self.police = true;
        }
        if deps.ems {
            self.ems = true;
        }
        if deps.fire {
            self.fire = true;
        }
    }

    /// Recompute from case clues + live signals + casualties. Call after clue/casualty updates.
    pub fn derive(case: &CaseRecord) -> Self {
        let clues = &case.reported_clues;
        let mut needs = Self::default();

        needs.police = !case.criminals.is_empty()
            || clues.has_kind(CausalKind::WeaponDischarge)
            || clues.has_kind(CausalKind::VehiclePropertyDamage)
            || case.has_live_kind(CausalKind::WeaponDischarge)
            || case.has_live_kind(CausalKind::VehiclePropertyDamage);

        needs.fire = clues.has_kind(CausalKind::FireOutbreak)
            || clues.has_kind(CausalKind::VehicleBurning)
            || clues.has_kind(CausalKind::Explosion)
            || case.has_live_kind(CausalKind::FireOutbreak)
            || case.has_live_kind(CausalKind::VehicleBurning)
            || case.has_live_kind(CausalKind::Explosion);

        needs.ems = case.civilian_casualties > 0
            || clues.has_kind(CausalKind::PedCasualty)
            || clues.has_kind(CausalKind::PedInjury)
            || case.has_live_kind(CausalKind::PedCasualty)
            || case.has_live_kind(CausalKind::PedInjury);

        if let Some(seed) = case.department_needs_seed {
            needs.merge_incident(seed);
        }

        needs
    }
}

impl CaseRecord {
    pub fn refresh_department_needs(&mut self) {
        self.department_needs = DepartmentNeeds::derive(self);
    }

    /// Police script is active only when the case warrants a law-enforcement response.
    pub fn police_script_active(&self) -> bool {
        self.department_needs.police
    }

    pub fn ems_script_active(&self) -> bool {
        self.department_needs.ems
    }

    pub fn fire_script_active(&self) -> bool {
        self.department_needs.fire
    }

    /// Live threat still present when police arrive on scene.
    pub fn police_scene_abnormal(&self) -> bool {
        if !self.criminals.is_empty() {
            return true;
        }
        self.has_live_kind(CausalKind::WeaponDischarge)
            || self.has_live_kind(CausalKind::VehiclePropertyDamage)
    }

    /// Casualty still needs EMS attention on scene.
    pub fn ems_scene_abnormal(&self) -> bool {
        self.civilian_casualties > 0
            || self.has_live_kind(CausalKind::PedCasualty)
            || self.has_live_kind(CausalKind::PedInjury)
    }

    /// Active fire still present when fire crew arrives.
    pub fn fire_scene_abnormal(&self) -> bool {
        self.has_live_kind(CausalKind::FireOutbreak)
            || self.has_live_kind(CausalKind::VehicleBurning)
            || self.has_live_kind(CausalKind::Explosion)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::report_channel::ReportClues;

    fn case_with_kinds(kinds: u8, criminals: Vec<dispatch_core::PedId>) -> CaseRecord {
        let mut clues = ReportClues::default();
        clues.kinds = kinds;
        let mut case = CaseRecord::new(1, None, None, None, clues, criminals, 0, 8000);
        case.refresh_department_needs();
        case
    }

    #[test]
    fn fire_only_skips_police() {
        let fire = (1 << CausalKind::FireOutbreak as u8) | (1 << CausalKind::VehicleBurning as u8);
        let case = case_with_kinds(fire, vec![]);
        assert!(!case.police_script_active());
        assert!(case.fire_script_active());
    }

    #[test]
    fn shooter_triggers_police() {
        let kinds = 1 << CausalKind::WeaponDischarge as u8;
        let case = case_with_kinds(kinds, vec![]);
        assert!(case.police_script_active());
    }

    #[test]
    fn false_alarm_no_police_abnormality_on_scene() {
        let kinds = 1 << CausalKind::WeaponDischarge as u8;
        let case = case_with_kinds(kinds, vec![]);
        assert!(!case.police_scene_abnormal());
    }

    #[test]
    fn live_gunfire_is_police_abnormality() {
        let mut case = case_with_kinds(0, vec![]);
        case.mark_live_kind(CausalKind::WeaponDischarge);
        assert!(case.police_scene_abnormal());
    }

    #[test]
    fn cleanup_waits_for_pending_fire_or_ems_spawn() {
        let fire = 1 << CausalKind::FireOutbreak as u8;
        let mut case = case_with_kinds(fire, vec![]);
        case.report_channel = None;
        // Fire needed, truck not yet dispatched → keep case.
        assert!(case.fire_script_active());
        assert!(!case.mod_firetruck_dispatched);
        assert!(!case.should_enter_cleanup());

        case.mod_firetruck_dispatched = true;
        assert!(case.should_enter_cleanup());
    }

    #[test]
    fn cleanup_ok_when_only_police_criminals_gone() {
        let kinds = 1 << CausalKind::WeaponDischarge as u8;
        let mut case = case_with_kinds(kinds, vec![dispatch_core::PedId::default()]);
        case.report_channel = None;
        case.criminals.clear();
        // Firearm clues keep police need, but no pending EMS/fire spawn.
        assert!(case.should_enter_cleanup());
    }
}