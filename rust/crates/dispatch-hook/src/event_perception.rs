use std::mem::MaybeUninit;

use dispatch_case::{SenseChannel, WitnessObservation, MAX_WITNESS_ENTITIES, MAX_WITNESS_PERPETRATORS};
use dispatch_core::{CausalKind, EntityRef};
use dispatch_engine::{event_vptr_identity, NativeEventRegistry, ResolvedNativeEvent};

use crate::gate::hook_logic_allowed;
use crate::orig_slot::OrigSlot;
use crate::witness::witness_ped_type_eligible;
const MAX_TRACKED_EVENTS: usize = 12;
const PENDING_NATIVE_CAP: usize = 64;

type OrigEventGroupAdd = unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void, bool);

static ORIG_EVENT_GROUP_ADD: OrigSlot<OrigEventGroupAdd> = OrigSlot::new();
static mut TRACKED_EVENTS: [Option<ResolvedNativeEvent>; MAX_TRACKED_EVENTS] =
    [None; MAX_TRACKED_EVENTS];
static mut TRACKED_VPTR_KEYS: [usize; MAX_TRACKED_EVENTS] = [0; MAX_TRACKED_EVENTS];
static mut TRACKED_EVENT_COUNT: usize = 0;

#[derive(Clone, Copy)]
pub(crate) struct PendingNativeEvent {
    event_group: *const std::ffi::c_void,
    event: *const std::ffi::c_void,
    classified: ResolvedNativeEvent,
}

static mut PENDING_NATIVE: [MaybeUninit<PendingNativeEvent>; PENDING_NATIVE_CAP] =
    [UNINIT_PENDING; PENDING_NATIVE_CAP];
static mut PENDING_NATIVE_LEN: usize = 0;

const UNINIT_PENDING: MaybeUninit<PendingNativeEvent> = MaybeUninit::uninit();

pub fn set_orig_event_group_add(f: OrigEventGroupAdd) {
    ORIG_EVENT_GROUP_ADD.set(f);
}

pub fn init_tracked_vptrs(registry: &NativeEventRegistry) {
    unsafe {
        PENDING_NATIVE_LEN = 0;
        TRACKED_EVENT_COUNT = 0;
        for resolved in registry.tracked_entries() {
            if TRACKED_EVENT_COUNT >= MAX_TRACKED_EVENTS {
                break;
            }
            TRACKED_EVENTS[TRACKED_EVENT_COUNT] = Some(resolved);
            TRACKED_VPTR_KEYS[TRACKED_EVENT_COUNT] = resolved.vptr_id as usize;
            TRACKED_EVENT_COUNT += 1;
        }
    }
}

pub fn reset_pending_native_queue() {
    unsafe {
        PENDING_NATIVE_LEN = 0;
    }
}

pub fn drain_pending_native_events<F: FnMut(PendingNativeEvent)>(mut f: F) {
    unsafe {
        for slot in PENDING_NATIVE[..PENDING_NATIVE_LEN].iter_mut() {
            f(slot.assume_init_read());
        }
        PENDING_NATIVE_LEN = 0;
    }
}

fn lookup_tracked_event(event: *const std::ffi::c_void) -> Option<ResolvedNativeEvent> {
    if event.is_null() {
        return None;
    }
    let vptr = unsafe { *(event as *const *const std::ffi::c_void) };
    let key = event_vptr_identity(vptr) as usize;
    unsafe {
        for i in 0..TRACKED_EVENT_COUNT {
            if TRACKED_VPTR_KEYS[i] == key {
                return TRACKED_EVENTS[i];
            }
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
    unsafe {
        if PENDING_NATIVE_LEN >= PENDING_NATIVE_CAP {
            return;
        }
        PENDING_NATIVE[PENDING_NATIVE_LEN].write(PendingNativeEvent {
            event_group: event_group as *const _,
            event: event as *const _,
            classified,
        });
        PENDING_NATIVE_LEN += 1;
    }
}

impl crate::runtime::DispatchRuntime {
    pub fn drain_pending_native_events(&mut self) {
        drain_pending_native_events(|pending| {
            self.ingest_native_event(
                pending.event_group,
                pending.event,
                pending.classified,
            );
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
            // Prefer pool generation when entity is a live ped/vehicle.
            let entity = self
                .resolve_ped_pool_key(entity_ptr)
                .or_else(|| self.resolve_vehicle_pool_key(entity_ptr))
                .and_then(|key| EntityRef::with_pool(entity_ptr, key))
                .or_else(|| EntityRef::new(entity_ptr));
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

        if !self.should_collect_witness_observation(
            &entities,
            entity_count as usize,
        ) {
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
    if !symbols.ped_alive(ped) {
        return false;
    }
    let Some(pool_ped) = symbols.validate_pool_ped(ped) else {
        return false;
    };
    witness_ped_type_eligible(symbols.ped_type_of_pool(pool_ped))
}