use std::collections::HashMap;

use slotmap::{new_key_type, SlotMap};
use tracing::debug;

use dispatch_core::{CausalKind, CausalSignal, EntityRef, WorldPos};

use crate::department::{departments_for, Department};

new_key_type! { pub struct IncidentId; }

const MAX_INCIDENT_ENTITIES: usize = 16;
const MAX_SIGNAL_ENTITIES: usize = 8;

#[derive(Debug, Clone, Copy, Default)]
pub struct DepartmentSet {
    pub police: bool,
    pub ems: bool,
    pub fire: bool,
}

impl DepartmentSet {
    pub fn merge(&mut self, deps: &[Department]) {
        for dep in deps {
            match dep {
                Department::Police => self.police = true,
                Department::Ems => self.ems = true,
                Department::Fire => self.fire = true,
            }
        }
    }

}

#[derive(Debug, Clone)]
pub struct Incident {
    pub serial: u64,
    pub anchor: WorldPos,
    pub opened_ms: i64,
    pub last_ms: i64,
    pub kinds: u8,
    pub departments: DepartmentSet,
    pub entities: Vec<EntityRef>,
    /// One civilian reporter per incident; no follow-up calls.
    pub reporting_exhausted: bool,
}

impl Incident {
    fn mark_kind(&mut self, kind: CausalKind) {
        self.kinds |= 1 << (kind as u8);
    }

    pub fn has_kind(&self, kind: CausalKind) -> bool {
        (self.kinds & (1 << (kind as u8))) != 0
    }

    pub fn has_entity(&self, entity: EntityRef) -> bool {
        self.entities.iter().any(|entry| *entry == entity)
    }

    fn absorb_entities(&mut self, refs: &[EntityRef]) {
        for entity in refs {
            if self.entities.len() >= MAX_INCIDENT_ENTITIES {
                break;
            }
            if self.has_entity(*entity) {
                continue;
            }
            self.entities.push(*entity);
        }
    }
}

pub fn signal_entity_refs(signal: CausalSignal) -> [EntityRef; MAX_SIGNAL_ENTITIES] {
    let mut out = [EntityRef::EMPTY; MAX_SIGNAL_ENTITIES];
    let mut count = 0usize;

    let mut push = |ptr: *const std::ffi::c_void| {
        let Some(entity) = EntityRef::new(ptr) else {
            return;
        };
        if out[..count].contains(&entity) {
            return;
        }
        if count >= MAX_SIGNAL_ENTITIES {
            return;
        }
        out[count] = entity;
        count += 1;
    };

    match signal {
        CausalSignal::PedCasualty { dead, killer, .. } => {
            push(killer);
            push(dead);
        }
        CausalSignal::PedInjury { victim, attacker, .. } => {
            push(attacker);
            push(victim);
        }
        CausalSignal::VehicleBurning { vehicle, .. } => push(vehicle),
        CausalSignal::FireOutbreak { .. } => {}
        CausalSignal::Explosion { attacker, vehicle, .. } => {
            push(attacker);
            push(vehicle);
        }
        CausalSignal::WeaponDischarge { shooter, .. } => push(shooter),
        CausalSignal::VehiclePropertyDamage { vehicle, attacker, .. } => {
            push(vehicle);
            push(attacker);
        }
        CausalSignal::CrimeReported { perpetrator, victim, .. } => {
            push(perpetrator);
            push(victim);
        }
    }

    out
}

/// Perpetrator-side entities only (not victims / vehicles unless sole actor).
pub fn perpetrator_entity_refs(signal: CausalSignal) -> [EntityRef; 4] {
    let mut out = [EntityRef::EMPTY; 4];
    let mut count = 0usize;
    let mut push = |ptr: *const std::ffi::c_void| {
        let Some(entity) = EntityRef::new(ptr) else {
            return;
        };
        if out[..count].contains(&entity) {
            return;
        };
        if count >= 4 {
            return;
        };
        out[count] = entity;
        count += 1;
    };

    match signal {
        CausalSignal::PedCasualty { killer, .. } => push(killer),
        CausalSignal::PedInjury { attacker, .. } => push(attacker),
        CausalSignal::Explosion { attacker, .. } => push(attacker),
        CausalSignal::WeaponDischarge { shooter, .. } => push(shooter),
        CausalSignal::VehiclePropertyDamage { attacker, .. } => push(attacker),
        CausalSignal::CrimeReported { perpetrator, .. } => push(perpetrator),
        CausalSignal::VehicleBurning { .. } | CausalSignal::FireOutbreak { .. } => {}
    }

    out
}

pub struct IncidentStore {
    incidents: SlotMap<IncidentId, Incident>,
    next_serial: u64,
    /// Entity ptr → incident for O(1) correlation on hot signal path.
    entity_incident: HashMap<usize, IncidentId>,
}

impl IncidentStore {
    pub fn new() -> Self {
        Self {
            incidents: SlotMap::with_key(),
            next_serial: 1,
            entity_incident: HashMap::new(),
        }
    }

    pub fn ingest(
        &mut self,
        signal: CausalSignal,
        now_ms: i64,
        attach_if_bare: Option<IncidentId>,
    ) -> IncidentId {
        let pos = signal_pos(signal);
        let kind = signal.kind();
        let deps = departments_for(signal);
        let refs = signal_entity_refs(signal);
        let refs: Vec<_> = refs
            .into_iter()
            .filter(|entity| entity.ptr() != std::ptr::null())
            .collect();

        if let Some(id) = self.find_correlated(&refs, attach_if_bare) {
            let serial = {
                let record = &mut self.incidents[id];
                let fresh_kind = !record.has_kind(kind);
                record.last_ms = now_ms;
                record.absorb_entities(&refs);
                if fresh_kind {
                    record.mark_kind(kind);
                    record.departments.merge(deps);
                }
                record.serial
            };
            self.index_entities(id, &refs);
            debug!(incident = serial, ?kind, "correlated");
            return id;
        }

        let serial = self.next_serial;
        self.next_serial += 1;
        let mut departments = DepartmentSet::default();
        departments.merge(deps);
        let mut record = Incident {
            serial,
            anchor: pos,
            opened_ms: now_ms,
            last_ms: now_ms,
            kinds: 0,
            departments,
            entities: Vec::new(),
            reporting_exhausted: false,
        };
        record.mark_kind(kind);
        record.absorb_entities(&refs);
        let id = self.incidents.insert(record);
        self.index_entities(id, &refs);
        debug!(incident = serial, ?kind, "opened");
        id
    }

    pub fn get(&self, id: IncidentId) -> Option<&Incident> {
        self.incidents.get(id)
    }

    /// Entity ptr → open incident (for hot-path witness / batch gating).
    pub fn incident_for_entity(&self, entity: EntityRef) -> Option<IncidentId> {
        let ptr = entity.ptr() as usize;
        if ptr == 0 {
            return None;
        }
        self.entity_incident
            .get(&ptr)
            .copied()
            .filter(|id| self.incidents.contains_key(*id))
    }

    pub fn iter_ids(&self) -> impl Iterator<Item = IncidentId> + '_ {
        self.incidents.keys()
    }

    pub fn mark_reporting_exhausted(&mut self, id: IncidentId) {
        let Some(record) = self.incidents.get_mut(id) else {
            return;
        };
        record.reporting_exhausted = true;
    }

    fn find_correlated(
        &self,
        refs: &[EntityRef],
        attach_if_bare: Option<IncidentId>,
    ) -> Option<IncidentId> {
        for entity in refs {
            let ptr = entity.ptr() as usize;
            if ptr == 0 {
                continue;
            }
            if let Some(id) = self.entity_incident.get(&ptr).copied() {
                if self.incidents.contains_key(id) {
                    return Some(id);
                }
            }
        }

        attach_if_bare.and_then(|id| self.incidents.contains_key(id).then_some(id))
    }

    fn index_entities(&mut self, incident_id: IncidentId, refs: &[EntityRef]) {
        for entity in refs {
            let ptr = entity.ptr() as usize;
            if ptr == 0 {
                continue;
            }
            self.entity_incident.entry(ptr).or_insert(incident_id);
        }
    }
}

pub fn signal_pos(signal: CausalSignal) -> WorldPos {
    match signal {
        CausalSignal::PedCasualty { pos, .. }
        | CausalSignal::PedInjury { pos, .. }
        | CausalSignal::VehicleBurning { pos, .. }
        | CausalSignal::FireOutbreak { pos, .. }
        | CausalSignal::Explosion { pos, .. }
        | CausalSignal::WeaponDischarge { pos, .. }
        | CausalSignal::VehiclePropertyDamage { pos, .. }
        | CausalSignal::CrimeReported { pos, .. } => pos,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use dispatch_core::{CausalSource, PedKind};

    fn ptr(id: usize) -> *const std::ffi::c_void {
        id as *const std::ffi::c_void
    }

    fn pos(x: f32) -> WorldPos {
        WorldPos { x, y: 0.0, z: 0.0 }
    }

    #[test]
    fn shared_entity_refs_correlate_chain() {
        let player = ptr(0x1000);
        let vehicle = ptr(0x2000);
        let victim = ptr(0x3000);
        let mut store = IncidentStore::new();

        let id1 = store.ingest(
            CausalSignal::WeaponDischarge {
                shooter: player,
                pos: pos(10.0),
                weapon: 35,
                source: CausalSource::WeaponDamage,
            },
            1_000,
            None,
        );
        let id2 = store.ingest(
            CausalSignal::VehiclePropertyDamage {
                vehicle,
                attacker: player,
                pos: pos(12.0),
                damage: 200.0,
                source: CausalSource::VehicleInflictDamage,
            },
            1_500,
            None,
        );
        let id3 = store.ingest(
            CausalSignal::VehicleBurning {
                vehicle,
                pos: pos(12.0),
                source: CausalSource::FireManager,
            },
            2_000,
            None,
        );
        let id4 = store.ingest(
            CausalSignal::Explosion {
                pos: pos(12.0),
                weapon: 51,
                attacker: player,
                vehicle,
                source: CausalSource::Explosion,
            },
            2_500,
            None,
        );
        let id5 = store.ingest(
            CausalSignal::PedCasualty {
                dead: victim,
                killer: player,
                weapon: 51,
                ped_kind: PedKind::Civilian,
                pos: pos(13.0),
                source: CausalSource::RegisterKill,
            },
            3_000,
            None,
        );

        assert_eq!(id1, id2);
        assert_eq!(id2, id3);
        assert_eq!(id3, id4);
        assert_eq!(id4, id5);

        let incident = store.get(id5).expect("incident");
        assert!(incident.has_kind(CausalKind::WeaponDischarge));
        assert!(incident.has_kind(CausalKind::PedCasualty));
        assert!(incident.has_entity(EntityRef::new(player).unwrap()));
        assert!(incident.has_entity(EntityRef::new(vehicle).unwrap()));
        assert!(incident.has_entity(EntityRef::new(victim).unwrap()));
    }

    #[test]
    fn entity_correlation_ignores_elapsed_time() {
        let player = ptr(0x5000);
        let vehicle = ptr(0x6000);
        let mut store = IncidentStore::new();

        let id1 = store.ingest(
            CausalSignal::WeaponDischarge {
                shooter: player,
                pos: pos(0.0),
                weapon: 35,
                source: CausalSource::WeaponDamage,
            },
            0,
            None,
        );
        let id2 = store.ingest(
            CausalSignal::VehiclePropertyDamage {
                vehicle,
                attacker: player,
                pos: pos(1.0),
                damage: 50.0,
                source: CausalSource::VehicleInflictDamage,
            },
            600_000,
            None,
        );

        assert_eq!(id1, id2);
    }

    #[test]
    fn bare_signal_attaches_to_active_report_incident() {
        let mut store = IncidentStore::new();
        let scene = pos(100.0);

        let id1 = store.ingest(
            CausalSignal::Explosion {
                pos: scene,
                weapon: 51,
                attacker: ptr(0x4000),
                vehicle: std::ptr::null(),
                source: CausalSource::Explosion,
            },
            5_000,
            None,
        );
        let id2 = store.ingest(
            CausalSignal::FireOutbreak {
                pos: WorldPos {
                    x: 105.0,
                    y: 0.0,
                    z: 0.0,
                },
                source: CausalSource::FireManager,
            },
            5_500,
            Some(id1),
        );

        assert_eq!(id1, id2);
    }

    #[test]
    fn bare_signal_without_active_report_opens_new_incident() {
        let mut store = IncidentStore::new();

        let id1 = store.ingest(
            CausalSignal::FireOutbreak {
                pos: pos(1.0),
                source: CausalSource::FireManager,
            },
            1_000,
            None,
        );
        let id2 = store.ingest(
            CausalSignal::FireOutbreak {
                pos: pos(2.0),
                source: CausalSource::FireManager,
            },
            1_500,
            None,
        );

        assert_ne!(id1, id2);
    }
}