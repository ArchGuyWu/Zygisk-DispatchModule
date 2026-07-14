#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct WorldPos {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PedKind {
    Unknown,
    Player,
    Cop,
    Civilian,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CausalSource {
    RegisterKill,
    VehicleInflictDamage,
    WeaponDamage,
    ReportCrime,
    EventDamage,
    FireManager,
    Explosion,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CausalKind {
    PedCasualty,
    PedInjury,
    VehicleBurning,
    FireOutbreak,
    Explosion,
    WeaponDischarge,
    VehiclePropertyDamage,
    /// Vanilla `CCrime::ReportCrime` (player-only). Hook swallows; mod never publishes.
    CrimeReported,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct EntityRef(pub *const std::ffi::c_void);

impl EntityRef {
    pub fn new(ptr: *const std::ffi::c_void) -> Option<Self> {
        (!ptr.is_null()).then_some(Self(ptr))
    }

    pub fn ptr(self) -> *const std::ffi::c_void {
        self.0
    }
}

impl std::fmt::Debug for EntityRef {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "EntityRef({:p})", self.0)
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum CausalSignal {
    PedCasualty {
        dead: *const std::ffi::c_void,
        killer: *const std::ffi::c_void,
        weapon: i32,
        ped_kind: PedKind,
        pos: WorldPos,
        source: CausalSource,
    },
    PedInjury {
        victim: *const std::ffi::c_void,
        attacker: *const std::ffi::c_void,
        weapon: i32,
        ped_kind: PedKind,
        damage: f32,
        pos: WorldPos,
        source: CausalSource,
    },
    VehicleBurning {
        vehicle: *const std::ffi::c_void,
        pos: WorldPos,
        source: CausalSource,
    },
    FireOutbreak {
        pos: WorldPos,
        source: CausalSource,
    },
    Explosion {
        pos: WorldPos,
        weapon: i32,
        attacker: *const std::ffi::c_void,
        vehicle: *const std::ffi::c_void,
        source: CausalSource,
    },
    WeaponDischarge {
        shooter: *const std::ffi::c_void,
        pos: WorldPos,
        weapon: i32,
        source: CausalSource,
    },
    VehiclePropertyDamage {
        vehicle: *const std::ffi::c_void,
        attacker: *const std::ffi::c_void,
        pos: WorldPos,
        damage: f32,
        source: CausalSource,
    },
    CrimeReported {
        perpetrator: *const std::ffi::c_void,
        victim: *const std::ffi::c_void,
        crime_type: i32,
        pos: WorldPos,
        source: CausalSource,
    },
}

impl CausalSignal {
    pub fn kind(self) -> CausalKind {
        match self {
            Self::PedCasualty { .. } => CausalKind::PedCasualty,
            Self::PedInjury { .. } => CausalKind::PedInjury,
            Self::VehicleBurning { .. } => CausalKind::VehicleBurning,
            Self::FireOutbreak { .. } => CausalKind::FireOutbreak,
            Self::Explosion { .. } => CausalKind::Explosion,
            Self::WeaponDischarge { .. } => CausalKind::WeaponDischarge,
            Self::VehiclePropertyDamage { .. } => CausalKind::VehiclePropertyDamage,
            Self::CrimeReported { .. } => CausalKind::CrimeReported,
        }
    }
}

