use dispatch_core::{CausalSignal, CausalSource, PedKind, WorldPos};

use crate::weapon::{
    classify_weapon, WeaponClass, MIN_PED_DAMAGE, MIN_VEHICLE_DAMAGE, WEAPON_EXPLOSION,
};

pub fn signals_from_weapon_hit(
    victim: *const std::ffi::c_void,
    attacker: *const std::ffi::c_void,
    weapon: i32,
    damage: f32,
    victim_kind: PedKind,
    pos: WorldPos,
) -> Vec<CausalSignal> {
    if damage < MIN_PED_DAMAGE || victim.is_null() {
        return Vec::new();
    }

    let class = classify_weapon(weapon);
    let mut out = Vec::with_capacity(2);

    match class {
        WeaponClass::Explosion | WeaponClass::Explosive => {
            out.push(CausalSignal::Explosion {
                pos,
                weapon,
                attacker,
                vehicle: std::ptr::null(),
                source: CausalSource::WeaponDamage,
            });
        }
        WeaponClass::Fire => {
            out.push(CausalSignal::FireOutbreak {
                pos,
                source: CausalSource::WeaponDamage,
            });
        }
        WeaponClass::Firearm => {
            out.push(CausalSignal::WeaponDischarge {
                shooter: attacker,
                pos,
                weapon,
                source: CausalSource::WeaponDamage,
            });
        }
        _ => {}
    }

    if weapon != WEAPON_EXPLOSION {
        out.push(CausalSignal::PedInjury {
            victim,
            attacker,
            weapon,
            ped_kind: victim_kind,
            damage,
            pos,
            source: CausalSource::WeaponDamage,
        });
    }

    out
}

pub fn signals_from_vehicle_hit(
    vehicle: *const std::ffi::c_void,
    attacker: *const std::ffi::c_void,
    weapon: i32,
    damage: f32,
    pos: WorldPos,
    burning: bool,
) -> Vec<CausalSignal> {
    let effective = if burning {
        MIN_VEHICLE_DAMAGE
    } else {
        damage
    };
    if effective < MIN_VEHICLE_DAMAGE || vehicle.is_null() {
        return Vec::new();
    }

    let mut out = vec![CausalSignal::VehiclePropertyDamage {
        vehicle,
        attacker,
        pos,
        damage: effective,
        source: CausalSource::VehicleInflictDamage,
    }];

    if burning {
        out.push(CausalSignal::VehicleBurning {
            vehicle,
            pos,
            source: CausalSource::VehicleInflictDamage,
        });
    }

    let class = classify_weapon(weapon);
    if matches!(class, WeaponClass::Explosion | WeaponClass::Explosive) {
        out.push(CausalSignal::Explosion {
            pos,
            weapon,
            attacker,
            vehicle,
            source: CausalSource::Explosion,
        });
    }

    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_core::CausalKind;

    fn ptr(id: usize) -> *const std::ffi::c_void {
        id as *const std::ffi::c_void
    }

    #[test]
    fn firearm_hit_emits_discharge_and_injury() {
        let signals = signals_from_weapon_hit(
            ptr(0x10000),
            ptr(0x20000),
            24,
            10.0,
            PedKind::Civilian,
            WorldPos {
                x: 1.0,
                y: 2.0,
                z: 3.0,
            },
        );
        assert_eq!(signals.len(), 2);
        assert_eq!(signals[0].kind(), CausalKind::WeaponDischarge);
        assert_eq!(signals[1].kind(), CausalKind::PedInjury);
    }

    #[test]
    fn burning_vehicle_emits_burning_kind() {
        let signals = signals_from_vehicle_hit(
            ptr(0x30000),
            ptr(0x20000),
            51,
            1.0,
            WorldPos::default(),
            true,
        );
        assert!(signals.iter().any(|s| s.kind() == CausalKind::VehicleBurning));
    }
}