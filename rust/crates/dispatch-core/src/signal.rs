use crate::registry::PoolKey;

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

/// Non-owning engine entity identity for incident correlation.
///
/// Prefer [`EntityRef::with_pool`] when a pool generation is known so recycled
/// slots are not mistaken for the old entity. Raw address alone is same-frame
/// / soft identity only — never dereference without a live pool check.
#[derive(Clone, Copy, Default)]
pub struct EntityRef {
    addr: usize,
    pool: Option<PoolKey>,
}

impl EntityRef {
    pub const EMPTY: Self = Self {
        addr: 0,
        pool: None,
    };

    #[inline]
    pub fn is_plausible_addr(ptr: *const std::ffi::c_void) -> bool {
        const MAX_USER_ADDR: usize = (1u64 << 52) as usize;
        let addr = ptr as usize;
        !ptr.is_null() && (addr & 0x00FF_FFFF_FFFF_FFFF) < MAX_USER_ADDR && addr >= 0x1000
    }

    pub fn new(ptr: *const std::ffi::c_void) -> Option<Self> {
        if !Self::is_plausible_addr(ptr) {
            return None;
        }
        Some(Self {
            addr: ptr as usize,
            pool: None,
        })
    }

    /// Stronger identity: address + pool slot generation.
    pub fn with_pool(ptr: *const std::ffi::c_void, key: PoolKey) -> Option<Self> {
        let mut e = Self::new(ptr)?;
        e.pool = Some(key);
        Some(e)
    }

    pub fn ptr(self) -> *const std::ffi::c_void {
        self.addr as *const std::ffi::c_void
    }

    pub fn pool_key(self) -> Option<PoolKey> {
        self.pool
    }

    pub fn is_empty(self) -> bool {
        self.addr == 0
    }

    pub fn still_plausible(self) -> bool {
        self.addr != 0 && Self::is_plausible_addr(self.ptr())
    }
}

impl PartialEq for EntityRef {
    fn eq(&self, other: &Self) -> bool {
        if self.addr == 0 || other.addr == 0 {
            return self.addr == other.addr;
        }
        // Same pool generation ⇒ same entity even across soft address reuse debates.
        if let (Some(a), Some(b)) = (self.pool, other.pool) {
            if a.slot == b.slot {
                return a.generation == b.generation;
            }
        }
        self.addr == other.addr
    }
}

impl Eq for EntityRef {}

impl std::hash::Hash for EntityRef {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        // Always include addr so Eq-by-addr pairs share hashes when pool is missing.
        self.addr.hash(state);
        if let Some(k) = self.pool {
            k.slot.hash(state);
            k.generation.hash(state);
        }
    }
}

impl std::fmt::Debug for EntityRef {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self.pool {
            Some(k) => write!(
                f,
                "EntityRef({:p}, slot={}, gen={})",
                self.ptr(),
                k.slot,
                k.generation
            ),
            None => write!(f, "EntityRef({:p})", self.ptr()),
        }
    }
}

#[cfg(test)]
mod entity_ref_tests {
    use super::EntityRef;
    use crate::registry::PoolKey;

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

    #[test]
    fn pool_generation_distinguishes_slot_reuse() {
        let p = 0x0000_0071_2345_6000_u64 as *const std::ffi::c_void;
        let a = EntityRef::with_pool(p, PoolKey::from_slot_flag(3, 1)).unwrap();
        let b = EntityRef::with_pool(p, PoolKey::from_slot_flag(3, 2)).unwrap();
        assert_ne!(a, b);
        let c = EntityRef::with_pool(p, PoolKey::from_slot_flag(3, 1)).unwrap();
        assert_eq!(a, c);
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
