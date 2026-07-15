# Dispatch module baseline (post-refactor)

This tree is the **new baseline** for GTA SA DE **emergency dispatch**
(police, EMS, fire — police paths are the bulk of the logic, not the only
department). Prefer this document over older crash-gate / dual-tree notes.

**Product authority:** [`docs/MODEL.md`](MODEL.md) freezes vocabulary, open/close
rules, departments, response, OnScene, policy gates, Light/Full frame steps,
and Effects. Prefer MODEL.md over older gameplay inventories when they conflict.
This file documents the **ship path**, packaging, and hook surface only.

## Product rewrite (summary — see MODEL.md)

| Topic | Baseline rule |
|-------|----------------|
| Pure logic | `dispatch-model` (workspace member); zero FFI; SNAPSHOT → COMPUTE → COMMIT |
| Frame path | Runtime `frame_tick` → Light or Full dispatch → commit → apply Effects |
| EMS / Fire view cap | At most **≤2 vehicles per department** simultaneous in player view / stream radius |
| Report anims | Civilian phone / cop radio **animation not required** (logical ReportPhase only) |
| skip-orig | **No** skip-orig crash gates on generic engine updaters |

## Ship path (single)

| Step | Command |
|------|---------|
| Build | `bash rust/build_rust.sh` |
| Pack | `bash pack_module.sh` |
| Artifact | `build/rust/arm64-v8a/arm64-v8a.so` → zip `zygisk/arm64-v8a.so` |

- **Module id:** `zygisk-policedispatch` (`module.prop`)
- **Primary language:** Rust (`rust/crates/dispatch-*` + `dispatch-model` + `dispatch-zygisk` cdylib)
- **Not packaged:** C++ `src/main/cpp` / `libpolicemod.so` / `build_in_container.sh` output (legacy; keep for reference only)

`build_rust.sh` builds `-p dispatch-zygisk` (workspace pulls `dispatch-model` and
other crates via Cargo). `pack_module.sh` copies only
`build/rust/arm64-v8a/arm64-v8a.so` into the Magisk zip as `zygisk/arm64-v8a.so`.

## Third-party / system deps (kept only if required)

| Piece | Why required |
|-------|----------------|
| Android system `dlopen` / `dlsym` / `dlclose` (`RTLD_NOLOAD`) | Resolve symbols in already-loaded `libUE4.so` (`dispatch-engine` `library.rs`) |
| `liblog` | Zygisk module logging (`__android_log_*` via bionic stubs) |
| `libc` / `libm` / `libdl` | Only `NEEDED` libs on ship `.so` (verified by readelf) |
| Zygisk API headers (vendored under `dispatch-zygisk/cpp/zygisk/`) | Module entry contract with Zygisk loaders |
| NDK clang (build-time only) | Cross-compile `aarch64-linux-android` |

**Removed from ship path:**

- `third_party/shadowhook*` — C++ legacy inline hooker; Rust uses memfd dual-view jumps
- `dispatch-native` crate — deleted; never linked into baseline
- Vendored xDL — replaced by system `dlopen(RTLD_NOLOAD)` (module formerly named `xdl.rs`, now `library.rs`)
- Crash fail-closed “skip engine orig” gates (ManageTasks, ScanForEvents, Buoyancy, StaticCounter, ControlSubTask, RecordRelationship) — **do not reintroduce**
- Task-manager slot “unwalkable” scans (11/32-slot style)
- `HOOK_INSTALL_MASK` limited to bits **0–10** (`0x07ff`)

## Runtime hooks kept (business)

Registered from `dispatch-hook` install (bits 0–10 when mask allows):

| Bit | Role |
|-----|------|
| 0 | RegisterKill — casualty / registry |
| 1 | RemovePed — pool recycle |
| 2 | PossiblyRemoveVehicle |
| 3 | TheScripts::Process — frame tick (Light/Full host) |
| 4–5 | Weapon/vehicle damage causal ingress |
| 6 | Wanted / crime report suppression |
| 7–9 | Emergency / script car creation (block vanilla EMS; mod CreateCarForScript) |
| 10 | EventGroup::Add — native event perception |

**Not registered:** any task-graph crash skip-orig gate.

## Departments (all part of this module)

| Dept | When | Ship path |
|------|------|-----------|
| **Police** | criminals / gunfire / property damage | model → Effects → nearby or patrol spawn; attack/arrest; wanted suppress |
| **EMS** | casualties / injury kinds | model needs + route-to-scene; ≤2 ambulances in view |
| **Fire** | fire / burning / explosion | model needs + route-to-scene; ≤2 firetrucks in view |

Civilian report feeds **case + department needs** for all three (logical
Connected only; no phone/radio anim dependency — see MODEL.md §3 / §4).

## Mod business gating (not crash skip)

`Loader` / `zone_active` in `dispatch-core` still decide whether **mod dispatch tick** runs (cutscene/load/player). That does **not** skip engine `ManageTasks` / event scanners. Policy hooks may short-circuit **gameplay** side effects only (wanted, vanilla EMS spawn), not engine housekeeping.

## Tests

Pure logic (no libUE4), run inside build container:

```bash
cd rust && cargo test -p dispatch-core -p dispatch-exec -p dispatch-case -p dispatch-model --lib
```

Ship build + pack:

```bash
bash rust/build_rust.sh
bash pack_module.sh
# Artifact: build/module_output → Zygisk-PoliceDispatch.zip
```

Verification notes live under `scratch/` (`deps_inventory.md`, `hooks_surface.md`, `cargo_unit_tests.log`, `so_symbols.txt`).

Gameplay simplify inventory (core keep vs candidates, analysis only): [`docs/GAMEPLAY_SIMPLIFY.md`](GAMEPLAY_SIMPLIFY.md).
Product contract: [`docs/MODEL.md`](MODEL.md).
