/// Inline patch install. Set false for vanilla control (module loads, zero hooks).
pub const HOOKS_ENABLED: bool = true;

/// Dispatch tick/ingest/wanted/spawn logic.
pub const MOD_LOGIC_ENABLED: bool = true;

/// Per-hook install mask (install.rs order, business hooks only):
/// bit0 RegisterKill, bit1 RemovePed, bit2 RemoveVehicle, bit3 Scripts,
/// bit4 GenerateDamage, bit5 VehicleInflict, bit6 Wanted×4, bit7 GenEmergency,
/// bit8 ScriptGenEmergency, bit9 CreateCarForScript, bit10 EventGroupAdd.
/// Bits 11–15 are unused (crash skip-orig gates removed — see docs/BASELINE.md).
/// `0x07ff` enables bits 0–10; `0xffff` is equivalent for current install.rs.
pub const HOOK_INSTALL_MASK: u16 = 0x07ff;