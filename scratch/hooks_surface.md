# Hook surface (business only)

Source: `rust/crates/dispatch-hook/src/install.rs` + `config.rs`

| Bit | Hook | Role |
|-----|------|------|
| 0 | RegisterKill | casualty / registry |
| 1 | RemovePed | pool recycle |
| 2 | PossiblyRemoveVehicle | vehicle pool |
| 3 | TheScripts::Process | frame tick |
| 4 | GenerateDamageEvent | weapon damage ingress |
| 5 | VehicleInflictDamage | vehicle damage ingress |
| 6 | Wanted / ReportCrime (×4) | crime report suppression |
| 7 | GenerateOneEmergencyCar | emergency car |
| 8 | ScriptGenerateOneEmergencyCar | script emergency car |
| 9 | CreateCarForScript | script car creation |
| 10 | EventGroup::Add | native event perception |

`HOOK_INSTALL_MASK = 0x07ff` (bits 0–10).

**Not registered:** ManageTasks / ScanForEvents / Buoyancy / StaticCounter / ControlSubTask / RecordRelationship fail-closed skip-orig gates.

Mod tick still gated by Loader `zone_active` (playable state) — does **not** skip engine task graph.
