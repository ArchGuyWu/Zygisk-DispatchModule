use dispatch_core::{CausalKind, CausalSignal, PedKind};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Department {
    Police,
    Ems,
    Fire,
}

const WEAPON_GRENADE: i32 = 16;
const WEAPON_MOLOTOV: i32 = 18;
const WEAPON_ROCKET: i32 = 35;
const WEAPON_HEATSEEK_ROCKET: i32 = 36;
const WEAPON_FLAMETHROWER: i32 = 37;
const WEAPON_SATCHEL: i32 = 39;
const WEAPON_EXPLOSION: i32 = 51;

pub fn is_explosive_weapon(weapon: i32) -> bool {
    matches!(
        weapon,
        WEAPON_GRENADE
            | WEAPON_SATCHEL
            | WEAPON_EXPLOSION
            | WEAPON_ROCKET
            | WEAPON_HEATSEEK_ROCKET
    )
}

pub fn is_fire_related_weapon(weapon: i32) -> bool {
    matches!(
        weapon,
        WEAPON_MOLOTOV | WEAPON_FLAMETHROWER | WEAPON_EXPLOSION
    ) || is_explosive_weapon(weapon)
}

pub fn departments_for(signal: CausalSignal) -> &'static [Department] {
    match signal {
        CausalSignal::PedCasualty { ped_kind, weapon, .. } => match ped_kind {
            PedKind::Cop => &[Department::Police],
            PedKind::Player => &[Department::Police],
            PedKind::Civilian | PedKind::Unknown => {
                if is_fire_related_weapon(weapon) {
                    &[Department::Ems]
                } else {
                    &[Department::Ems, Department::Police]
                }
            }
        },
        CausalSignal::PedInjury { ped_kind, weapon, .. } => match ped_kind {
            PedKind::Civilian => {
                if is_fire_related_weapon(weapon) {
                    &[Department::Ems]
                } else {
                    &[Department::Ems]
                }
            }
            PedKind::Cop => &[Department::Police, Department::Ems],
            _ => &[Department::Police],
        },
        CausalSignal::VehicleBurning { .. } => &[Department::Fire],
        CausalSignal::FireOutbreak { .. } => &[Department::Fire],
        CausalSignal::Explosion { .. } => &[Department::Fire, Department::Ems],
        CausalSignal::WeaponDischarge { .. } | CausalSignal::CrimeReported { .. } => {
            &[Department::Police]
        }
        CausalSignal::VehiclePropertyDamage { .. } => &[Department::Police],
    }
}

pub fn ped_kind_from_type(ped_type: i32) -> PedKind {
    match ped_type {
        0 | 1 | 2 => PedKind::Player,
        6 => PedKind::Cop,
        4 | 5 => PedKind::Civilian,
        _ => PedKind::Unknown,
    }
}

pub fn kind_label(kind: CausalKind) -> &'static str {
    match kind {
        CausalKind::PedCasualty => "ped_casualty",
        CausalKind::PedInjury => "ped_injury",
        CausalKind::VehicleBurning => "vehicle_burning",
        CausalKind::FireOutbreak => "fire_outbreak",
        CausalKind::Explosion => "explosion",
        CausalKind::WeaponDischarge => "weapon_discharge",
        CausalKind::VehiclePropertyDamage => "vehicle_property_damage",
        CausalKind::CrimeReported => "crime_reported",
    }
}