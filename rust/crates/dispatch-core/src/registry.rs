use slotmap::{new_key_type, SlotMap};

new_key_type! { pub struct PedId; }
new_key_type! { pub struct VehicleId; }

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct PoolKey {
    pub slot: u16,
    pub generation: u8,
}

impl PoolKey {
    pub fn from_slot_flag(slot: u16, flag: u8) -> Self {
        Self {
            slot,
            generation: flag,
        }
    }

    pub fn handle(self) -> i32 {
        ((self.slot as i32) << 8) | self.generation as i32
    }
}

pub struct PedRecord {
    pub pool_key: PoolKey,
    pub ped_type: i32,
}

pub struct VehicleRecord {
    pub pool_key: PoolKey,
    pub model: u32,
}

pub struct ResourceRegistry {
    peds: SlotMap<PedId, PedRecord>,
    vehicles: SlotMap<VehicleId, VehicleRecord>,
    ped_by_pool: std::collections::HashMap<PoolKey, PedId>,
    vehicle_by_pool: std::collections::HashMap<PoolKey, VehicleId>,
    next_generation: std::collections::HashMap<u16, u8>,
}

impl ResourceRegistry {
    pub fn new() -> Self {
        Self {
            peds: SlotMap::with_key(),
            vehicles: SlotMap::with_key(),
            ped_by_pool: std::collections::HashMap::new(),
            vehicle_by_pool: std::collections::HashMap::new(),
            next_generation: std::collections::HashMap::new(),
        }
    }

    pub fn adopt_ped(&mut self, pool_key: PoolKey, ped_type: i32) -> PedId {
        if let Some(id) = self.ped_by_pool.get(&pool_key).copied() {
            return id;
        }
        let id = self.peds.insert(PedRecord { pool_key, ped_type });
        self.ped_by_pool.insert(pool_key, id);
        id
    }

    pub fn adopt_vehicle(&mut self, pool_key: PoolKey, model: u32) -> VehicleId {
        if let Some(id) = self.vehicle_by_pool.get(&pool_key).copied() {
            return id;
        }
        let id = self.vehicles.insert(VehicleRecord { pool_key, model });
        self.vehicle_by_pool.insert(pool_key, id);
        id
    }

    pub fn release_ped(&mut self, id: PedId) -> Option<PedRecord> {
        let record = self.peds.remove(id)?;
        self.ped_by_pool.remove(&record.pool_key);
        let gen = self
            .next_generation
            .entry(record.pool_key.slot)
            .or_insert(record.pool_key.generation);
        *gen = gen.wrapping_add(1);
        Some(record)
    }

    pub fn release_vehicle(&mut self, id: VehicleId) -> Option<VehicleRecord> {
        let record = self.vehicles.remove(id)?;
        self.vehicle_by_pool.remove(&record.pool_key);
        let gen = self
            .next_generation
            .entry(record.pool_key.slot)
            .or_insert(record.pool_key.generation);
        *gen = gen.wrapping_add(1);
        Some(record)
    }

    pub fn ped(&self, id: PedId) -> Option<&PedRecord> {
        self.peds.get(id)
    }

    pub fn vehicle(&self, id: VehicleId) -> Option<&VehicleRecord> {
        self.vehicles.get(id)
    }

    pub fn ped_by_pool(&self, key: PoolKey) -> Option<PedId> {
        self.ped_by_pool.get(&key).copied()
    }

    pub fn vehicle_by_pool(&self, key: PoolKey) -> Option<VehicleId> {
        self.vehicle_by_pool.get(&key).copied()
    }

    pub fn contains_ped(&self, id: PedId) -> bool {
        self.peds.contains_key(id)
    }

    pub fn contains_vehicle(&self, id: VehicleId) -> bool {
        self.vehicles.contains_key(id)
    }
}