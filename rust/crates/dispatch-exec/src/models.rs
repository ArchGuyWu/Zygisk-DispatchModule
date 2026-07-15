//! Vehicle model IDs and map-region helpers (from `game_config.hpp` + `dispatch_police_spawn`).

use dispatch_core::WorldPos;

pub const PED_TYPE_COP: i32 = 6;
pub const PED_TYPE_PLAYER: i32 = 0;

pub const MODEL_POLICE_CAR: u32 = 596;
pub const MODEL_POLICE_CAR_SF: u32 = 597;
pub const MODEL_POLICE_CAR_LV: u32 = 598;
pub const MODEL_POLICE_RANGER: u32 = 599;
pub const MODEL_POLICE_BIKE: u32 = 523;
pub const MODEL_SWAT_VAN: u32 = 427;
pub const MODEL_SWAT_WATER: u32 = 601;
pub const MODEL_FBI_RANCHER: u32 = 490;
pub const MODEL_POLICE_HELI: u32 = 497;
pub const MODEL_AMBULANCE: u32 = 416;
pub const MODEL_FIRETRUCK: u32 = 407;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MapRegion {
    LosSantos,
    SanFierro,
    LasVenturas,
    Countryside,
}

impl MapRegion {
    pub fn name(self) -> &'static str {
        match self {
            Self::LosSantos => "LS",
            Self::SanFierro => "SF",
            Self::LasVenturas => "LV",
            Self::Countryside => "Rural",
        }
    }
}

/// GTA SA regional bounds (CCarCtrl emergency partition approximation).
pub fn detect_map_region(pos: WorldPos) -> MapRegion {
    let in_ls = pos.x > 44.0 && pos.x < 2990.0 && pos.y > -2890.0 && pos.y < -760.0;
    let in_sf = pos.x > -2990.0 && pos.x < -850.0 && pos.y > -1410.0 && pos.y < 1420.0;
    let in_lv = pos.x > 860.0 && pos.x < 2990.0 && pos.y > 590.0 && pos.y < 2990.0;

    if in_lv {
        MapRegion::LasVenturas
    } else if in_sf {
        MapRegion::SanFierro
    } else if in_ls {
        MapRegion::LosSantos
    } else {
        MapRegion::Countryside
    }
}

pub fn local_patrol_model(pos: WorldPos) -> u32 {
    match detect_map_region(pos) {
        MapRegion::LosSantos => MODEL_POLICE_CAR,
        MapRegion::SanFierro => MODEL_POLICE_CAR_SF,
        MapRegion::LasVenturas => MODEL_POLICE_CAR_LV,
        MapRegion::Countryside => MODEL_POLICE_RANGER,
    }
}

pub fn is_swat_model(model: u32) -> bool {
    model == MODEL_SWAT_VAN || model == MODEL_SWAT_WATER
}

pub fn is_police_dispatch_model(model: u32) -> bool {
    matches!(
        model,
        MODEL_POLICE_CAR
            | MODEL_POLICE_CAR_SF
            | MODEL_POLICE_CAR_LV
            | MODEL_POLICE_RANGER
            | MODEL_POLICE_BIKE
            | MODEL_SWAT_VAN
            | MODEL_SWAT_WATER
            | MODEL_FBI_RANCHER
    )
}

pub fn is_ems_dispatch_model(model: u32) -> bool {
    matches!(model, MODEL_AMBULANCE | MODEL_FIRETRUCK)
}

pub fn is_police_bike(model: u32) -> bool {
    model == MODEL_POLICE_BIKE
}

/// Re-export for exec callers; canonical definition in `dispatch-case::spawn_policy`.
pub fn is_native_ems_emergency_model(model: u32) -> bool {
    dispatch_case::is_native_ems_emergency_model(model)
}

#[cfg(test)]
mod model_tests {
    use super::*;

    #[test]
    fn detect_map_region_drives_real_bounds() {
        let ls = WorldPos {
            x: 1000.0,
            y: -1500.0,
            z: 0.0,
        };
        assert_eq!(detect_map_region(ls), MapRegion::LosSantos);
        assert_eq!(local_patrol_model(ls), MODEL_POLICE_CAR);

        let sf = WorldPos {
            x: -2000.0,
            y: 0.0,
            z: 0.0,
        };
        assert_eq!(detect_map_region(sf), MapRegion::SanFierro);
        assert_eq!(local_patrol_model(sf), MODEL_POLICE_CAR_SF);
    }

    #[test]
    fn police_dispatch_model_match_is_shipped_fn() {
        assert!(is_police_dispatch_model(MODEL_POLICE_CAR));
        assert!(is_police_dispatch_model(MODEL_SWAT_VAN));
        assert!(!is_police_dispatch_model(MODEL_AMBULANCE));
        assert!(is_ems_dispatch_model(MODEL_AMBULANCE));
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum ResponseCategory {
    One = 1,
    Two = 2,
    Three = 3,
}

impl ResponseCategory {
    pub fn name(self) -> &'static str {
        match self {
            Self::One => "Cat1",
            Self::Two => "Cat2",
            Self::Three => "Cat3",
        }
    }
}

#[derive(Debug, Clone)]
pub struct PoliceSpawnUnit {
    pub model: u32,
    pub register_swat: bool,
}

#[derive(Debug, Clone)]
pub enum SpawnTask {
    BeginChain { chain_id: u64 },
    SpawnUnit { chain_id: u64, index: usize, attempt: u32 },
    BatchReleaseTimeout { chain_id: u64 },
}