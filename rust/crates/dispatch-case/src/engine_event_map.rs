use dispatch_core::CausalKind;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SenseChannel {
    Sound,
    Visual,
    Physical,
}

/// Native engine perception event → causal kind mapping (hook + vtable registry).
#[derive(Debug, Clone, Copy)]
pub struct NativeEventKind {
    pub name: &'static str,
    pub vtable_sym: &'static str,
    pub channel: SenseChannel,
    pub kind: CausalKind,
    /// First entity pointer field offset on the event object (after vptr).
    pub primary_entity_offset: Option<usize>,
    /// Reference: Itanium ctor mangled name (unwired — classification uses vtable).
    pub mangled_ctor: &'static str,
    /// Reference: vanilla `CEventHandler` response entry.
    pub handler: &'static str,
}

pub const NATIVE_EVENT_KINDS: &[NativeEventKind] = &[
    NativeEventKind {
        name: "gun_shot",
        vtable_sym: "_ZTV13CEventGunShot",
        channel: SenseChannel::Sound,
        kind: CausalKind::WeaponDischarge,
        primary_entity_offset: Some(0x10),
        mangled_ctor: "_ZN13CEventGunShotC1EP7CEntity7CVectorS2_b",
        handler: "CEventHandler::ComputeShotFiredResponse",
    },
    NativeEventKind {
        name: "gun_shot_whizzed_by",
        vtable_sym: "_ZTV22CEventGunShotWhizzedBy",
        channel: SenseChannel::Sound,
        kind: CausalKind::WeaponDischarge,
        primary_entity_offset: Some(0x10),
        mangled_ctor: "_ZN22CEventGunShotWhizzedByC1EP7CEntityRK7CVectorS4_b",
        handler: "CEventHandler::ComputeShotFiredWhizzedByResponse",
    },
    NativeEventKind {
        name: "fire_nearby",
        vtable_sym: "_ZTV16CEventFireNearby",
        channel: SenseChannel::Sound,
        kind: CausalKind::FireOutbreak,
        primary_entity_offset: None,
        mangled_ctor: "_ZN16CEventFireNearbyC1ERK7CVector",
        handler: "CEventHandler::ComputeFireNearbyResponse",
    },
    NativeEventKind {
        name: "seen_panicked_ped",
        vtable_sym: "_ZTV21CEventSeenPanickedPed",
        channel: SenseChannel::Visual,
        kind: CausalKind::PedInjury,
        primary_entity_offset: Some(0x10),
        mangled_ctor: "_ZN21CEventSeenPanickedPedC1EP4CPed",
        handler: "CEventHandler::ComputeSeenPanickedPedResponse",
    },
    NativeEventKind {
        name: "gun_aimed_at",
        vtable_sym: "_ZTV16CEventGunAimedAt",
        channel: SenseChannel::Visual,
        kind: CausalKind::WeaponDischarge,
        primary_entity_offset: Some(0x10),
        mangled_ctor: "_ZN16CEventGunAimedAtC1EP4CPed",
        handler: "CEventHandler::ComputeGunAimedAtResponse",
    },
    NativeEventKind {
        name: "dead_ped",
        vtable_sym: "_ZTV13CEventDeadPed",
        channel: SenseChannel::Visual,
        kind: CausalKind::PedCasualty,
        primary_entity_offset: Some(0x10),
        mangled_ctor: "_ZN13CEventDeadPedC1EP4CPedbi",
        handler: "CEventHandler::ComputeDeadPedResponse",
    },
    NativeEventKind {
        name: "vehicle_on_fire",
        vtable_sym: "_ZTV19CEventVehicleOnFire",
        channel: SenseChannel::Visual,
        kind: CausalKind::VehicleBurning,
        primary_entity_offset: Some(0x10),
        mangled_ctor: "_ZN19CEventVehicleOnFireC1EP8CVehicle",
        handler: "CEventHandler::ComputeVehicleOnFireResponse",
    },
    NativeEventKind {
        name: "damage",
        vtable_sym: "_ZTV12CEventDamage",
        channel: SenseChannel::Physical,
        kind: CausalKind::PedInjury,
        primary_entity_offset: Some(0x10),
        mangled_ctor: "_ZN12CEventDamageC1EP7CEntityj11eWeaponType14ePedPieceTypeshbb",
        handler: "CEventHandler::ComputeDamageResponse",
    },
];

pub const GROUP_ADD_HOOK: &str = "_ZN11CEventGroup3AddER6CEventb";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PerceptionTap {
    pub hook: &'static str,
    pub wired: bool,
}

pub const PERCEPTION_TAPS: &[PerceptionTap] = &[PerceptionTap {
    hook: GROUP_ADD_HOOK,
    wired: true,
}];