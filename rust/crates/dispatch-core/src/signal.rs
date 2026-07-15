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

/// Opaque engine entity pointer for **same-frame / identity compare only**.
///
/// Not a strong ref: the engine may free the object while a case still holds this value.
/// Callers must not dereference without a fresh pool/live check. Construction rejects
/// null and implausible user-space patterns (load-time sentinels).
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct EntityRef(pub *const std::ffi::c_void);

impl EntityRef {
    /// ARM64 user VA heuristic (matches task-slot filtering in dispatch-exec).
    #[inline]
    pub fn is_plausible_addr(ptr: *const std::ffi::c_void) -> bool {
        const MAX_USER_ADDR: usize = (1u64 << 52) as usize;
        let addr = ptr as usize;
        !ptr.is_null() && (addr & 0x00FF_FFFF_FFFF_FFFF) < MAX_USER_ADDR && addr >= 0x1000
    }

    pub fn new(ptr: *const std::ffi::c_void) -> Option<Self> {
        Self::is_plausible_addr(ptr).then_some(Self(ptr))
    }

    pub fn ptr(self) -> *const std::ffi::c_void {
        self.0
    }

    /// True if this still looks like a user pointer (not a pool membership check).
    pub fn still_plausible(self) -> bool {
        Self::is_plausible_addr(self.0)
    }
}

impl std::fmt::Debug for EntityRef {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "EntityRef({:p})", self.0)
    }
}

#[cfg(test)]
mod entity_ref_tests {
    use super::EntityRef;

    #[test]
    fn rejects_null_and_low_sentinels() {
        assert!(EntityRef::new(std::ptr::null()).is_none());
        assert!(EntityRef::new(0x8 as *const _).is_none());
        assert!(EntityRef::new(0x00FF_FFFF_FFFF_FFFE_u64 as *const _).is_none());
    }

    #[test]
    fn accepts_typical_user_range_addr() {
        let p = 0x0000_0071_2345_6000_u64 as *const std::ffi::c_void;
        let r = EntityRef::new(p).expect("plausible");
        assert!(r.still_plausible());
        assert_eq!(r.ptr(), p);
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

