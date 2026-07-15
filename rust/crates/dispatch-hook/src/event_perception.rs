//! Native event-group perception: track vptrs, queue classified events for the tick.

use std::sync::Mutex;

use dispatch_case::{
    SenseChannel, WitnessObservation, MAX_WITNESS_ENTITIES, MAX_WITNESS_PERPETRATORS,
};
use dispatch_core::{CausalKind, EntityRef};
use dispatch_engine::{event_vptr_identity, NativeEventRegistry, ResolvedNativeEvent};

use crate::gate::hook_logic_allowed;
use crate::orig_slot::OrigSlot;
use crate::witness::witness_ped_type_eligible;

const MAX_TRACKED_EVENTS: usize = 12;
const PENDING_NATIVE_CAP: usize = 64;

type OrigEventGroupAdd = unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void, bool);

static ORIG_EVENT_GROUP_ADD: OrigSlot<OrigEventGroupAdd> = OrigSlot::new();

#[derive(Clone, Copy)]
pub(crate) struct PendingNativeEvent {
    event_group: *const std::ffi::c_void,
    event: *const std::ffi::c_void,
    classified: ResolvedNativeEvent,
}

struct NativeEventBus {
    tracked_keys: [usize; MAX_TRACKED_EVENTS],
    tracked_events: [Option<ResolvedNativeEvent>; MAX_TRACKED_EVENTS],
    tracked_count: usize,
    pending: Vec<PendingNativeEvent>,
}

// SAFETY: only game-thread hook/tick use this bus; raw event ptrs are not sent across threads in practice.
// Mutex still serializes push (detour) vs drain (tick) if re-entered.
unsafe impl Send for NativeEventBus {}

impl NativeEventBus {
    const fn new() -> Self {
        Self {
            tracked_keys: [0; MAX_TRACKED_EVENTS],
            tracked_events: [None; MAX_TRACKED_EVENTS],
            tracked_count: 0,
            pending: Vec::new(),
        }
    }
}

static NATIVE_BUS: Mutex<NativeEventBus> = Mutex::new(NativeEventBus::new());

pub fn set_orig_event_group_add(f: OrigEventGroupAdd) {
    ORIG_EVENT_GROUP_ADD.set(f);
}

pub fn init_tracked_vptrs(registry: &NativeEventRegistry) {
    let Ok(mut bus) = NATIVE_BUS.lock() else {
        return;
    };
    bus.pending.clear();
    bus.tracked_count = 0;
    for resolved in registry.tracked_entries() {
        if bus.tracked_count >= MAX_TRACKED_EVENTS {
            break;
        }
        let i = bus.tracked_count;
        bus.tracked_events[i] = Some(resolved);
        bus.tracked_keys[i] = resolved.vptr_id as usize;
        bus.tracked_count += 1;
    }
}

pub fn reset_pending_native_queue() {
    if let Ok(mut bus) = NATIVE_BUS.lock() {
        bus.pending.clear();
    }
}

pub fn drain_pending_native_events<F: FnMut(PendingNativeEvent)>(mut f: F) {
    let batch = {
        let Ok(mut bus) = NATIVE_BUS.lock() else {
            return;
        };
        std::mem::take(&mut bus.pending)
    };
    for ev in batch {
        f(ev);
    }
}

fn lookup_tracked_event(event: *const std::ffi::c_void) -> Option<ResolvedNativeEvent> {
    if event.is_null() {
        return None;
    }
    let vptr = unsafe { *(event as *const *const std::ffi::c_void) };
    let key = event_vptr_identity(vptr) as usize;
    let Ok(bus) = NATIVE_BUS.lock() else {
        return None;
    };
    for i in 0..bus.tracked_count {
        if bus.tracked_keys[i] == key {
            return bus.tracked_events[i];
        }
    }
    None
}

pub unsafe extern "C" fn detour_event_group_add(
    event_group: *mut std::ffi::c_void,
    event: *mut std::ffi::c_void,
    write_log: bool,
) {
    if let Some(orig) = ORIG_EVENT_GROUP_ADD.get() {
        orig(event_group, event, write_log);
    }
    if !hook_logic_allowed() {
        return;
    }
    let Some(classified) = lookup_tracked_event(event as *const _) else {
        return;
    };
    let Ok(mut bus) = NATIVE_BUS.lock() else {
        return;
    };
    if bus.pending.len() >= PENDING_NATIVE_CAP {
        return;
    }
    bus.pending.push(PendingNativeEvent {
        event_group: event_group as *const _,
        event: event as *const _,
        classified,
    });
}

impl crate::runtime::DispatchRuntime {
    pub fn drain_pending_native_events(&mut self) {
        drain_pending_native_events(|pending| {
            self.ingest_native_event(pending.event_group, pending.event, pending.classified);
        });
    }

    fn ingest_native_event(
        &mut self,
        event_group: *const std::ffi::c_void,
        event: *const std::ffi::c_void,
        classified: ResolvedNativeEvent,
    ) {
        let Some(witness) = self.ped_from_event_group(event_group) else {
            return;
        };
        if !witness_ped_eligible(witness, &self.symbols) {
            return;
        }

        let witness_pos = self.symbols.entity_world_pos(witness);
        let kind_bit = 1 << classified.def.kind as u8;
        let (heard, saw) = channel_flags(classified.def.channel);

        let mut entities = [EntityRef::EMPTY; MAX_WITNESS_ENTITIES];
        let mut entity_count = 0u8;
        let mut perpetrators = [EntityRef::EMPTY; MAX_WITNESS_PERPETRATORS];
        let mut perp_count = 0u8;
        if let Some(offset) = classified.def.primary_entity_offset {
            let entity_ptr = self.native_events.read_entity(event, offset);
            let entity = self.entity_ref_enriched(entity_ptr);
            if let Some(entity) = entity {
                if entity_count < MAX_WITNESS_ENTITIES as u8 {
                    entities[entity_count as usize] = entity;
                    entity_count += 1;
                }
                if matches!(
                    classified.def.kind,
                    CausalKind::WeaponDischarge
                        | CausalKind::PedInjury
                        | CausalKind::PedCasualty
                ) && perp_count < MAX_WITNESS_PERPETRATORS as u8
                {
                    perpetrators[perp_count as usize] = entity;
                    perp_count += 1;
                }
            }
        }

        let suspect_pos = if perp_count > 0 {
            Some(self.symbols.entity_world_pos(perpetrators[0].ptr()))
        } else {
            None
        };

        if !self.should_collect_witness_observation(&entities, entity_count as usize) {
            return;
        }

        let ped_id = if let Some(id) = self.resolve_ped_id(witness) {
            id
        } else {
            let _ = self.track_ped(witness);
            self.resolve_ped_id(witness).unwrap_or_default()
        };
        self.witness_reports.observe(WitnessObservation {
            ped: ped_id,
            pos: witness_pos,
            panicked: false,
            heard,
            saw,
            perceived_entities: entities,
            perceived_entity_count: entity_count,
            perceived_kinds: kind_bit,
            perceived_perpetrators: perpetrators,
            perceived_perpetrator_count: perp_count,
            suspect_pos,
        });
    }
}

fn channel_flags(channel: SenseChannel) -> (bool, bool) {
    match channel {
        SenseChannel::Sound => (true, false),
        SenseChannel::Visual => (false, true),
        SenseChannel::Physical => (true, true),
    }
}

fn witness_ped_eligible(
    ped: *const std::ffi::c_void,
    symbols: &dispatch_exec::ExecSymbols,
) -> bool {
    if ped.is_null() {
        return false;
    }
    if symbols.validate_pool_ped(ped).is_none() {
        return false;
    }
    witness_ped_type_eligible(symbols.ped_type_of(ped))
}
