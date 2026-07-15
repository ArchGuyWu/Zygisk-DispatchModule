/// Inline patch install. Set false for vanilla control (module loads, zero hooks).
pub const HOOKS_ENABLED: bool = true;

/// Dispatch tick/ingest/wanted/spawn logic.
pub const MOD_LOGIC_ENABLED: bool = true;

/// Per-hook install mask (install.rs order):
/// bit0 RegisterKill, bit1 RemovePed, bit2 RemoveVehicle, bit3 Scripts,
/// bit4 GenerateDamage, bit5 VehicleInflict, bit6 Wanted×4, bit7 GenEmergency,
/// bit8 ScriptGenEmergency, bit9 CreateCarForScript, bit10 EventGroupAdd,
/// bit11 ProcessBuoyancy fail-closed gate,
/// bit12 ScanForEvents fail-closed gate,
/// bit13 ProcessStaticCounter fail-closed gate.
/// bit14 ControlSubTask fail-closed gate.
/// bit15 ManageTasks fail-closed gate (one hook covers all vtable-offset variants).
/// Gates never sanitize / write task slots — dangerous graphs are not entered.
/// `0xffff` = all.
pub const HOOK_INSTALL_MASK: u16 = 0xffff;