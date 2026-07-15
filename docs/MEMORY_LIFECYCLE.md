# Memory safety & lifecycle risks (Rust ship path)

**Scope:** shipped Zygisk path only (`rust/crates/dispatch-*`).  
**Out of ship:** `src/main/cpp` (legacy; not packed).  
**Goal:** inventory — not a full fix pass. Engine UAF cannot be unit-tested without libUE4; registry/policy issues are partially testable.

| Class | Unit-testable? |
|-------|----------------|
| Engine UAF / bad engine ptr | **No** (needs game) |
| Registry desync / case cleanup policy | **Partial** (pure store/registry tests) |
| Hook install / trampoline | **No** (device) |

---

## 1. Unsafe FFI / hooks

| Issue | Where | What can go wrong | Mitigation now | Severity |
|-------|--------|-------------------|----------------|----------|
| Inline hook trampolines (`mmap`/`mprotect`/`memfd`, patch jumps) | `dispatch-hook/inline/hook.rs` | Bad patch → crash any call; tramp never freed if process lives for game session | Align checks, relocate fail → no patch; process-lifetime OK for Magisk module | **High** (install-time) |
| `static mut` orig function pointers | `lifecycle`, `causation`, `spawn`, `scripts`, `wanted_population`, `event_perception` | Data race if multi-threaded; wrong orig → skip or double-call | Detours assumed on game script thread; zone gate reduces work | **Medium** |
| `std::mem::forget(lib)` after install | `install.rs` ~76 | Intentional leak of `Library` so `dlclose` does not unmap `libUE4` handle used by symbols | Required for process life; not a UAF if forget is only path | **Low** (leak by design) |
| `transmute` of hook tramp addresses to fn types | `install.rs` | ABI mismatch if signature wrong | Fixed mangled names from nm; wrong sig = silent corruption | **High** if symbols drift |
| `CCarAI::GetCarToGoToCoors` return typed as `bool` | `ffi.rs` / `game.rs` | Engine returns **float distance**; using as bool is OK for “non-zero” but loses distance | Call sites only check success loosely | **Low** |
| Detour then always call `orig` | `lifecycle`, `spawn`, etc. | If our path panics/unwinds into engine frame → abort | `panic=abort` in release; hooks avoid heavy panics | **Medium** |

---

## 2. Engine object lifetimes (ped / vehicle / task)

| Issue | Where | What can go wrong | Mitigation now | Severity |
|-------|--------|-------------------|----------------|----------|
| Raw engine pointers in hooks | all detours | UAF if ped recycled mid-hook | `validate_pool_ped` / `validate_pool_vehicle` before registry work; `hook_logic_allowed` | **High** residual |
| `EntityRef(*const c_void)` stored in clues/cases | `dispatch-core/signal.rs`, case clues | Stale entity ptr long after free; later compare/deref | `EntityRef::new` only rejects null; **no** pool generation | **High** |
| Task alloc `task_new(512)` + ctor + `AddTaskPrimary(..., true)` | `tasks.rs` assign_investigate / phone / arrest | If ctor fails mid-way or ped dies before AddTask → orphan heap task; force primary aborts engine tasks mid-graph | `live_intel_for_assign` = pool+alive+intel; fail-closed null | **High** residual |
| Task pointer walks from engine FindTask* | `tasks.rs` `take_task_if_safe` / `is_plausible_ptr` | Null vtable / sentinel slots (load-time) crash walkers | Plausible VA + vtable fn non-null; **do not** write null into engine slots | **Medium** (mitigated) |
| `read_ptr` on ped/intel offsets | `tasks.rs`, `runtime` event-group walk | Wrong offset / dead ped → fault | Null checks; live_intel for assigns | **Medium** |
| Full ped-pool scans | `game.bind_vehicle_occupants_from_vehicle`, geometric witness | Touch many ptrs; one bad flag → skip, but expensive | Flag &lt; 0 skip; pool APIs | **Low–Medium** |
| CarAI go-to without re-validate each frame | `command_vehicle_to_scene` | Driver dies after command → engine owns failure | One-shot live driver check at command | **Medium** |

---

## 3. Registry vs engine pool desync

| Issue | Where | What can go wrong | Mitigation now | Severity |
|-------|--------|-------------------|----------------|----------|
| `PedId`/`VehicleId` outlive engine object | `ResourceRegistry`, cases, `cop_bindings` | Use freed pool slot after recycle → wrong entity or fault | RemovePed/RemoveVehicle hooks release registry; `ped_id_live` on tick; case `criminals.retain(live)` | **High** residual race if hook misses path |
| Kill before RemovePed | `mark_kill_pending` + `release_ped_from_pool` | Window between kill and pool recycle | Kill pending reason; still must wait for RemovePed | **Medium** |
| PoolKey slot+generation | `registry.rs` | Generation bump on release; re-adopt needs new gen from engine flag | `from_slot_flag`; release bumps next_generation | **Medium** (design OK if hooks always run) |
| Case `criminals` empty → Cleanup | `record::remove_criminal`, `store` | Case dropped while EMS/fire still “active” | Policy issue more than UAF; may leave engine vehicles with no case | **Medium** (lifecycle/policy) |
| `case_vehicles` / `spawned_swats` not always purged on vehicle despawn | `exec` globals | Stale VehicleId in sets | Vehicle remove hook despawn_vehicle; incomplete if vehicle never hooked | **Medium** |
| `cop_bindings` retain only on some spawn paths | `vehicle_spawn`, `game` | Stale ped→vehicle maps | Partial retain on spawn | **Medium** |

---

## 4. Static / global / reentrancy

| Issue | Where | What can go wrong | Severity |
|-------|--------|-------------------|----------|
| Single `RUNTIME` + `with_runtime` depth guard | `runtime.rs` | Nested hook during tick → skip nested work (`RuntimeDepthGuard`) | **Medium** — fail-closed skip, not re-enter |
| `static mut` orig + `PENDING_NATIVE` ring | `event_perception` | Cap 64; overflow drops events (not memory corruption) | **Low** (drop) / **Medium** if concurrent write |
| `TRACKED_EVENTS` / vptr tables | `event_perception` | Stale vptr if game patches events | **Low** |
| Zone gate / `MOD_LOGIC` off | `gate`, `config` | Disables logic; hooks may still call orig only | **Low** |
| Inline `HOOKS` Mutex | `inline/hook.rs` | Install-time only | **Low** |

---

## 5. Task assignment / force-primary

| Issue | Where | What can go wrong | Mitigation | Severity |
|-------|--------|-------------------|------------|----------|
| `AddTaskPrimaryMaybeInGroup(..., true)` force | phone, investigate, arrest | Aborts current primary mid-task; engine may leave inconsistent subtask graph | Intentional for interrupt; no ClearTasks on kill (avoids dangling slots) | **High** residual engine risk |
| Phone abort: `MakeAbortable` + `ClearTasks` | `abort_phone_tasks` | Correct teardown path vs bare nulling slots | Only if `ped_has_phone_task` + live ped | **Medium** (mitigated) |
| Arrest ctor requires live criminal | `assign_arrest_task` | Dead criminal → skip | `validate_live_ped` on criminal | **Medium** (mitigated) |
| Vehicle drive vs former foot investigate | `dispatch_disturbance` | Was: force 935 on seated crew (bad). **Now:** CarAI only on vehicle path | Vehicle path no longer force-foot task | **Lowered** |

---

## 6. Mitigations that are working (keep)

1. **Pool membership** before registry release and casualty ingest (`lifecycle` detours).  
2. **Live ped** before task assign (`live_intel_for_assign`).  
3. **Plausible ptr + vtable** when reading engine task finds (no writing null into slots).  
4. **No ClearTasks on RegisterKill** (commented historical crash class).  
5. **Case/registry live filters** each tick (`ped_id_live`, `finalize_after_exec`).  
6. **Zone gate** skips mod logic in load/fade.  
7. **Depth guard** avoids re-entrant tick into same runtime.

---

## 7. Priority if fixing later (not this goal)

1. **EntityRef** → store PoolKey or drop raw ptr after frame (highest lasting stale-ptr risk).  
2. Ensure **all vehicle/ped exit paths** hit registry release (audit non-hook despawn).  
3. Task assign: if `AddTaskPrimary` fails, free `task_new` buffer if engine does not own it.  
4. Type `GetCarToGoToCoors` return as `f32`; treat 0 as “at target”.  
5. Multi-dept case cleanup so EMS/fire vehicles are not orphaned when criminals empty.

---

## 8. Coverage checklist (ship crates)

| Family | Cited |
|--------|--------|
| Unsafe FFI / hooks | `inline/hook.rs`, `install.rs`, `static mut` ORIG_* |
| Engine lifetimes | `tasks.rs`, `lifecycle.rs`, `EntityRef`, CarAI go-to |
| Registry desync | `registry.rs`, `on_ped_despawned`, case Cleanup |
| Static / reentrancy | `with_runtime`, `PENDING_NATIVE`, ORIG statics |
| Force-primary tasks | `tasks.rs` phone/investigate/arrest |

Legacy C++: **out of ship** — not scored as current product risk.

Evidence: `{SCRATCH}/mem_safety_scan.txt`, `memory_evidence.txt`, `unit_smoke.log`.
