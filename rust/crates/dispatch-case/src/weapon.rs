#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WeaponClass {
    Unarmed,
    Melee,
    Firearm,
    Explosive,
    Fire,
    VehicleRam,
    Explosion,
}

pub const WEAPON_UNARMED: i32 = 0;
pub const WEAPON_GRENADE: i32 = 16;
pub const WEAPON_MOLOTOV: i32 = 18;
pub const WEAPON_ROCKET: i32 = 35;
pub const WEAPON_HEATSEEK_ROCKET: i32 = 36;
pub const WEAPON_FLAMETHROWER: i32 = 37;
pub const WEAPON_SATCHEL: i32 = 39;
pub const WEAPON_RAMMED_BY_CAR: i32 = 49;
pub const WEAPON_RUN_OVER_BY_CAR: i32 = 50;
pub const WEAPON_EXPLOSION: i32 = 51;

pub const MIN_PED_DAMAGE: f32 = 1.0;
pub const MIN_VEHICLE_DAMAGE: f32 = 8.0;

pub fn classify_weapon(weapon: i32) -> WeaponClass {
    match weapon {
        WEAPON_UNARMED => WeaponClass::Unarmed,
        WEAPON_GRENADE | WEAPON_SATCHEL | WEAPON_ROCKET | WEAPON_HEATSEEK_ROCKET => {
            WeaponClass::Explosive
        }
        WEAPON_MOLOTOV | WEAPON_FLAMETHROWER => WeaponClass::Fire,
        WEAPON_RAMMED_BY_CAR | WEAPON_RUN_OVER_BY_CAR => WeaponClass::VehicleRam,
        WEAPON_EXPLOSION => WeaponClass::Explosion,
        22..=38 => WeaponClass::Firearm,
        1..=15 => WeaponClass::Melee,
        _ => WeaponClass::Melee,
    }
}

pub fn is_audible_weapon(class: WeaponClass) -> bool {
    matches!(
        class,
        WeaponClass::Firearm
            | WeaponClass::Explosive
            | WeaponClass::Fire
            | WeaponClass::Explosion
            | WeaponClass::VehicleRam
    )
}