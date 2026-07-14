use dispatch_case::{classify_weapon, ped_kind_from_type, WeaponClass};
use dispatch_core::PedKind;
use dispatch_engine::CVector;

use crate::gate::hook_logic_allowed;
use crate::runtime::with_runtime;

type OrigGenerateDamage = unsafe extern "C" fn(
    *mut std::ffi::c_void,
    *mut std::ffi::c_void,
    i32,
    i32,
    i32,
    i32,
) -> bool;
type OrigVehicleInflictDamage = unsafe extern "C" fn(
    *mut std::ffi::c_void,
    *mut std::ffi::c_void,
    i32,
    f32,
    CVector,
);

static mut ORIG_GENERATE_DAMAGE: Option<OrigGenerateDamage> = None;
static mut ORIG_VEHICLE_INFlict: Option<OrigVehicleInflictDamage> = None;

pub fn set_orig_generate_damage(f: OrigGenerateDamage) {
    unsafe { ORIG_GENERATE_DAMAGE = Some(f) };
}

pub fn set_orig_vehicle_inflict_damage(f: OrigVehicleInflictDamage) {
    unsafe { ORIG_VEHICLE_INFlict = Some(f) };
}

pub unsafe extern "C" fn detour_generate_damage_event(
    victim: *mut std::ffi::c_void,
    source: *mut std::ffi::c_void,
    weapon: i32,
    damage: i32,
    ped_piece: i32,
    direction: i32,
) -> bool {
    if !hook_logic_allowed() || damage <= 0 {
        return forward_generate_damage(victim, source, weapon, damage, ped_piece, direction);
    }
    if classify_weapon(weapon) == WeaponClass::Firearm {
        return forward_generate_damage(victim, source, weapon, damage, ped_piece, direction);
    }
    with_runtime(|rt| {
        let victim_kind = rt
            .symbols
            .validate_pool_ped(victim as *const _)
            .map(|p| ped_kind_from_type(rt.symbols.ped_type_of_pool(p)))
            .unwrap_or(PedKind::Unknown);
        if victim_kind == PedKind::Cop {
            return;
        }
        if victim_kind == PedKind::Civilian {
            if let Some(v) = rt.symbols.validate_live_ped(victim as *const _) {
                rt.interrupt_ped_action(v.as_ptr(), "weapon_damage");
            }
        }
        let victim_ptr = victim as *const _;
        let source_ptr = source as *const _;
        rt.ingest_weapon_damage(victim_ptr, source_ptr, weapon, damage as f32, victim_kind);
    });
    forward_generate_damage(victim, source, weapon, damage, ped_piece, direction)
}

unsafe fn forward_generate_damage(
    victim: *mut std::ffi::c_void,
    source: *mut std::ffi::c_void,
    weapon: i32,
    damage: i32,
    ped_piece: i32,
    direction: i32,
) -> bool {
    if let Some(orig) = ORIG_GENERATE_DAMAGE {
        orig(victim, source, weapon, damage, ped_piece, direction)
    } else {
        false
    }
}

pub unsafe extern "C" fn detour_vehicle_inflict_damage(
    vehicle: *mut std::ffi::c_void,
    source: *mut std::ffi::c_void,
    weapon: i32,
    damage: f32,
    impact_pos: CVector,
) {
    if hook_logic_allowed() {
        with_runtime(|rt| {
            rt.ingest_vehicle_damage(
                vehicle as *const _,
                source as *const _,
                weapon,
                damage,
                impact_pos.as_array(),
            );
        });
    }
    if let Some(orig) = ORIG_VEHICLE_INFlict {
        orig(vehicle, source, weapon, damage, impact_pos);
    }
}