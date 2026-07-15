# Gameplay simplify inventory (Rust ship baseline)

**Scope:** only the shipped Zygisk path (`rust/crates/dispatch-*`, hooks bits 0–10).  
**Not product surface:** `src/main/cpp`, ShadowHook, `docs/911_DISPATCH.md` multi-line PSAP narrative.  
**Authority for build/pack:** [`docs/BASELINE.md`](BASELINE.md).

This document partitions **current** gameplay logic into:

1. **Core keep** — defining dispatch chain; preserve behavior and details.  
2. **Simplify candidates** — concrete complexity with call-site / warning evidence; after simplify, stated remainder must still hold.  
3. **Defer / non-core** — out of ship, historical, or intentionally not this inventory’s rewrite targets.  
4. **Low-value / hard to notice** — over-fine tiers, dual policies, and tick cost with little live-play signal (**practical audit**, not only dead code).

Analysis goals do not require implementing deletions.

---

## Core keep

Each item is **wired** on the ship path (install bits 0–10 and/or `DispatchRuntime` / `ExecCoordinator` tick). Do not delete these as “cleanup.”

### A. Causal ingress → signals

| Piece | Where | Why core |
|-------|--------|----------|
| RegisterKill → casualty / kill pending | `dispatch-hook` bit0 `lifecycle::detour_register_kill` → `runtime::publish_casualty` | Opens death/crime causality |
| Weapon / vehicle damage | bits 4–5 `causation.rs` → `ingest_weapon_damage` / `ingest_vehicle_damage` → `publish_signal` | Primary crime signal path |
| EventGroup::Add native events | bit10 `event_perception.rs` | Native perception + some witness observations |
| Signal kinds / weapon class | `dispatch-case` `ingress.rs`, `weapon.rs`, `dispatch-core` `signal.rs` | Shared vocabulary for cases |

### B. Incident / case store

| Piece | Where | Why core |
|-------|--------|----------|
| Incident merge & store | `dispatch-case` `incident.rs`, `store.rs` | Correlates entities/time into one incident |
| CaseRecord + open_from_report | `record.rs`, `store.rs` | Durable dispatch state |
| Department needs / playbook | `playbook.rs`, `department.rs` | Police / EMS / fire script_active flags |
| Loader / zone_active (mod tick only) | `dispatch-core` `loader.rs`, `dispatch-hook` `gate.rs` | Blocks mod logic in load/fade — not engine skip-orig |

### C. Civilian / cop report path

| Piece | Where | Why core |
|-------|--------|----------|
| Witness observations + elect reporter | `witness_report.rs` (`observe`, `try_start_reports`, `elect_reporter`) | Chooses who “calls 911” |
| Report phases Approaching→Dialing→Active→Ended | `report_channel.rs`, `witness_report.rs` | **Case opens on Active (phone_talk)** |
| Phone task driver | `dispatch-hook` `civilian_report.rs` + `dispatch-exec` `tasks::assign_mobile_phone_task` / `poll_report_task_phase` | Engine `CTaskComplexUseMobilePhone` is the dial UX |
| Cop witness shortcut | `civilian_report.rs` (skip phone → immediate active) | Radio-style open without cellphone |
| Interrupt / partial escalate | `on_reporter_interrupted` / panic paths in `runtime` + `witness_report` | Dropped call still can open case |

### D. Commit response (nearby vs offscreen spawn)

| Piece | Where | Why core |
|-------|--------|----------|
| Timing → commit | `dispatch-exec` `coordinator::tick_timing` → `response::commit_police_response` | Gate into OnScene |
| Nearby cop mobilize first | `response::dispatch_nearby_available_cops_*` | Prefer existing units over spawn |
| Offscreen spawn plan + chain | `spawn::schedule_police_vehicle_spawns` → `BeginChain` / `SpawnUnit` | Fallback when nearby fails |
| CreateCarForScript + occupants | `vehicle_spawn::dispatch_spawn_emergency_vehicle` + `spawn_gate` | Real engine spawn |
| Batch hold → release to disturbance | `spawn::release_police_spawn_batch` → `dispatch_disturbance` | Units actually go to scene |
| Regional patrol models | `models::local_patrol_model` / `detect_map_region` | LS/SF/LV/rural car choice |

### E. Threat category / spawn plan (core **detail**)

| Piece | Where | Why core |
|-------|--------|----------|
| `classify_response_category` + Cat1/2/3 plan | `threat.rs` `build_initial_spawn_plan` / `build_category_spawn_plan` | Defines how many/what units |
| Density / consolidated / threat_score thresholds | `threat.rs` `response_thresholds` | Tunable **detail** — keep unless redesigning response feel |
| Firearm flag on case | `CaseRecord.is_firearm` + ingress WeaponDischarge | Escalation input |

> **Keep detail:** threshold numbers and Cat composition are product feel, not “dead complexity,” even if some higher enum levels are hard to reach today (see simplify §2.2).

### F. On-scene execution (wired)

| Piece | Where | Why core |
|-------|--------|----------|
| Attack pass | `coordinator::tick_on_scene` → `make_cops_attack_criminal` → `attack::run_attack_pass` + foot/vehicle | Combat / approach |
| Arrest | `arrest::tick_case_arrests` / `assign_arrest_task` | Custody |
| Wanted / crime report suppression | bit6 `wanted_population.rs` | Prevents vanilla wanted fighting mod cases |
| Native EMS block + mod EMS/fire spawn | bits 7–9 `hook spawn.rs` + `emergency_services::EmergencyCoordinator` | Mod owns ambulance/firetruck |
| Ped/vehicle pool recycle sync | bits 1–2 `lifecycle` | Registry/case cleanup (observe engine recycle, don’t drive it) |
| Frame tick host | bit3 `scripts` → `runtime::frame_tick` | Drives everything above |

### G. Pure tests that lock core (do not drop when simplifying)

| Test area | Package | Guards |
|-----------|---------|--------|
| Loader zone | `dispatch-core` loader tests | Mod tick gating |
| Threat classify / reinforce predicate | `dispatch-exec` threat tests | Cat + reinforcement condition |
| Spawn ring math | `vehicle_spawn` spawn_pos_tests | Offscreen positions |
| Report open on Active | `dispatch-case` witness_report tests | 911 open semantics |
| Playbook departments | `dispatch-case` playbook tests | Police/EMS/fire flags |
| Models region | `dispatch-exec` models tests | Patrol model selection |

---

## Simplify candidates

Each candidate cites **evidence** (unused / no call sites / hollow wiring / dual path). After simplification, **what remains** is stated so core is not erased.

### 1. Dead / unwired public surface (low risk trim)

**Evidence:** `cargo build -p dispatch-exec -p dispatch-hook --release` “never used” warnings; `rg` shows definition + lib re-export only.

| Item | Evidence | After simplify remains |
|------|----------|------------------------|
| `anti_spin_should_bulk_exit`, `queue_deduped_temp_closure` | `escaper.rs`; no callers outside module | Staging closures used by spawn hold/release only |
| `should_trigger_stage2_warp`, `reset_stuck_tracker`, `get_vehicle_staging_exit_radius` (if only used by dead anti_spin) | never used | Vehicle stuck recovery **not** in core chain today |
| `should_prefer_foot_mobilization`, `make_cops_attack_criminal_immediate`, `av_range_sq` | never used; attack uses `make_cops_attack_criminal` + `av_range_for_firearm` | Foot/vehicle attack pass as-is |
| `is_police_engagement_near`, `staging_drive_style` | never used; `is_police_engagement_at` exported | Staging area begin/end on spawn batch |
| `assign_stand_still_task`, `TASK_SIMPLE_STAND_STILL`, `ped_ptr_for_id` | never used | Phone/arrest/clear helpers still used |
| `entity_ref_from_ped` | never used | Entity refs from signals/perception |
| `MODEL_POLICE_HELI`, `is_police_bike`, `is_swat_model` (export-only), `is_ems_dispatch_model` | never used in ship graph | Patrol/SWAT/FBI models still used via spawn plan |
| `build_reinforcement_spawn_plan` | **only** defined + `lib.rs` re-export; **zero** call from coordinator | Nearby/offscreen **initial** spawn + attack remain |
| `criminal_in_custody` / `criminal_eligible_for_arrest` (if unused helpers) | never used warnings | `tick_case_arrests` path remains |
| `reset_pending_native_queue`, `runtime_reentrant`, dead `refresh_zone_gate` pub wrapper | never used | Gate via `hook_logic_allowed` / runtime refresh |

**Why suitable:** trims noise without changing player-visible chain if callers truly absent.

### 2. Hollow or misleading complexity (medium — simplify behavior *or* wire for real)

#### 2.1 On-scene “reinforcement” is nearby-only with empty candidates

**Evidence:**

- `coordinator` queues `PendingTask::ReinforcementNearby` after `on_scene_needs_reinforcement`.
- Drain handler calls `dispatch_nearby_available_cops_for_crime_auto(..., &[], ...)` — **empty candidate slice**.
- `build_reinforcement_spawn_plan` never schedules a second offscreen unit.

**Simplify options (pick one later):**

- **A (minimal):** Drop reinforcement queue + wave bookkeeping; keep `on_scene_needs_reinforcement` only if a future spawn path needs it.  
- **B (product):** Wire reinforcement to real nearby scan **or** `build_reinforcement_spawn_plan` + spawn chain.

**After simplify remains:** initial commit nearby/offscreen spawn; on-scene attack/arrest.

#### 2.2 Threat enum levels never produced by estimator

**Evidence:** `estimate_criminal_threat` only returns `FirearmInactive` / `MeleeActive` / `MeleeInactive`.  
Branches on `FirearmAirShoot` / `FirearmActive` / `count_high_threats` (needs AirShoot+) are largely **unreachable** unless `is_firearm` alone lifts to FirearmInactive.

**Simplify options:**

- Collapse levels to what ingress can actually set (e.g. unarmed / melee / firearm), **or**  
- Feed real per-ped weapon state into estimate (keeps Cat detail, removes dead branches).

**After simplify remains:** Cat1/2 via density/consolidated/threat_score (tested); Cat3 via score≥22 path still valid with multi-criminal firearm cases.

#### 2.3 Attack quotas ignore real case

**Evidence:** `attack::compute_quotas` builds a **fresh empty** `CaseRecord::new(...)` then `compute_response_quota` — does not pass the live case’s `is_firearm` / criminals list density of clues.

**Simplify:** pass `&CaseRecord` into quota (one path) **or** hardcode simple foot/vehicle caps for on-scene pass.

**After simplify remains:** attack pass structure; real case threat scoring for **spawn** plan unchanged.

### 3. Dual / overlapping paths (simplify structure, keep one effective channel)

| Overlap | Evidence | After simplify remains |
|---------|----------|------------------------|
| **Native EventGroup observe** + **geometric_scan** both `witness_reports.observe` | `event_perception.rs`, `witness_scan.rs`, `runtime` deferred geometric | At least one witness feed so `try_start_reports` can elect; prefer native when it covers kind, geometric as supplement only for kinds that need it (`perception::needs_geometric_supplement`) — already partially structured |
| **Nearby mobilize** vs **offscreen spawn** | `commit_police_response` sequential | Keep both: nearby-first is core design |
| **EMS native block** vs **EmergencyCoordinator spawn** | hook `spawn.rs` + `emergency_services.rs` | Keep both: block vanilla, mod spawns intentional |
| **Quota APIs** (`compute_response_quota`, `compute_nearby_dispatch_quota`, Cat plan) | three related policies | Keep Cat for spawn composition; merge/delete unused quota call sites only after audit |

### 4. Staging / cordon edge cases

**Evidence:** `begin_staging_area_closure` / `end_staging_area_closure` used from spawn hold/release; several staging helpers never called; `escaper` temp closures largely unwired.

**Simplify:** keep begin/end around spawn batch; delete unused decoy/exit-radius/anti-spin helpers until a stuck-vehicle feature is product-required.

**After simplify remains:** hold vehicles → release → disturbance dispatch.

### 5. Documentation / narrative debt (not code, but confuses “what to keep”)

| Item | Note |
|------|------|
| `docs/911_DISPATCH.md` busy/redial/PSAP | **Not** implemented in Rust ship; civilian path is phone-task + one session/incident |
| README historical ShadowHook crash deep-dive | Legacy marketing; baseline is business hooks only |
| `ReportChannel` only `Cellphone` | Enum ready for radio channels that don’t exist as separate ship logic (cops skip phone instead) |

**Simplify:** mark docs superseded (already done for CRASH_STATUS/MODULE_LAYOUT); optional one-line in 911_DISPATCH pointing to Rust session model.

---

## Low-value / hard to notice (practical audit)

Focus: **player-visible outcome** vs **CPU/complexity**.  
Action labels: **coarsen/merge** | **remove if unused** | **keep as core detail**.

### L1. Threat enum over-fine — **coarsen/merge** (high priority)

| Now | Path | Why low-value in live play | Coarser keep |
|-----|------|----------------------------|--------------|
| 7-level `CriminalThreatLevel` (Unarmed×2, Melee×2, Firearm×3) | `threat.rs` | `estimate_criminal_threat` only ever returns **4**: `FirearmActive/Inactive`, `MeleeActive/Inactive`. **Never** produces `UnarmedActive`, `FirearmAirShoot`. Player only sees patrol count / SWAT or not. | 3 buckets: `UnarmedOrMelee` / `Armed` / `ActiveGunfire` (or even 2: non-gun / gun) |
| `threat_level_score` 0–6 + sum `compute_case_threat_score` | same | Score thresholds 12 / 16 / 20 / 22 are invisible; mostly re-encode “how many criminals × gun”. | Use `n_criminals` + `is_firearm` + `live_gunfire` bools |
| `count_high_threats` (AirShoot+) | same | With no AirShoot production, only `FirearmActive` hits; redundant with live discharge. | Drop; use `has_live_kind(WeaponDischarge)` |
| Loop in `get_case_max_threat_level` over criminals calling same case-level estimate | same | Every criminal gets **identical** level (case-wide estimate, not per-ped). Wastes iterations, false precision. | Estimate once per case |

**Player-visible remainders after coarsen:** gun vs not, ongoing fire vs not, 1 vs many offenders → Cat1/2/3 and SWAT gates.

### L2. Parallel scoring systems that collapse to the same cars — **coarsen/merge**

| Now | Path | Why low-value | Coarser keep |
|-----|------|---------------|--------------|
| `classify_response_category` (Cat1–3) | `threat.rs` | Dense branches on score/density/level that often still end in **1–3 patrols** after SWAT/FBI gates. | Cat from: firearm? multi? live fire? only |
| `compute_response_quota` (max vehicles/foot) | `threat.rs` + `attack::compute_quotas` | Multi-branch 1/2/3/4 caps; foot/vehicle mix barely readable on device. | Fixed small caps: e.g. 2 veh / 4 foot for gun, 1/2 otherwise |
| `compute_nearby_dispatch_quota` (1/2/4) | `threat.rs` + `response` | Same as above for nearby mobilize. | 1 nearby default, 2 if multi or gun |
| `get_case_threat_tier` (1/2) | `threat.rs` + `snapshot`/reroute | Third encoding of “gun or not”. | Reuse firearm bool |

### L3. Cat3 / density knobs with weak visual delta — **coarsen/merge**

| Now | Path | Why low-value | Coarser keep |
|-----|------|---------------|--------------|
| Density boosts 5 / 7 for Cat | `response_thresholds` | Density is a frame heuristic; player doesn’t see “density 5 vs 7”, only another car sometimes. | Single “busy area” bool or drop density from classify |
| Cat3 “third patrol if no SWAT/FBI” | `build_category_spawn_plan` | Third car vs two is subtle; special units already gated tightly. | Cap initial plan at 2 patrols unless SWAT/FBI warranted |
| Threat-weighted `compute_dispatch_anchor` | `threat.rs` | Weight by threat score then average positions — same as centroid if all criminals share case-level threat. | Plain average / primary position |

### L4. Dual witness / perception cost — **coarsen/merge** (CPU)

| Now | Path | Why costly / low extra signal | Coarser keep |
|-----|------|------------------------------|--------------|
| EventGroup observe + deferred `geometric_scan` full ped pool | `event_perception`, `witness_scan`, `runtime` | Geometric path walks **entire ped pool** (capped candidates) for range; often overlaps native kinds. | Geometric **only** when `needs_geometric_supplement`; hard cap scan slots earlier |
| `clue_score` multi-weights (+3 entity, +2 kind, +1 saw/heard) | `witness_report::elect_reporter` | Elects one reporter; ranking fineness rarely changes who phones. | Prefer nearest non-panic who heard/saw |

### L5. On-scene tick weight — **coarsen/merge** carefully

| Now | Path | Why low-value or heavy | Coarser keep |
|-----|------|------------------------|--------------|
| Attack pass every `ON_SCENE_DISPATCH_INTERVAL_MS` (2.5s) + pool build | `coordinator`, `attack*` | Necessary for combat, but quota fineness (L2) adds little. | Keep pass; simplify quotas only |
| Reroute eval on ordered vehicles | `reroute`, `snapshot`, coordinator interval | Higher-threat case steal mid-route: rare multi-case; subtle. | **Keep as core detail** if multi-case pursuits matter; else interval↑ or disable for single-case sessions |
| Staging cordon begin/end around spawn hold | `staging` + `spawn` | Closure list; weak player-facing unless ambient blocked. | Keep begin/end if spawn hold stays; drop unused helpers (already partly done) |
| Many timing constants (stagger urban/rural, batch timeout 50s, multiple scene timeouts) | `timing.rs` | Rural vs urban stagger is mild; many timeouts are safety not flavor. | One stagger; keep 1–2 safety timeouts |

### L6. Still wired but fine to leave — **keep as core detail**

| Piece | Why keep despite complexity |
|-------|----------------------------|
| Ingress → incident → case → phone Active open | Whole product spine |
| Nearby first, then offscreen spawn chain | Clear player beat |
| `swat_warranted` / `fbi_warranted` gang bars | User-visible special units; gates already intentional |
| Top-up ≤3 attempts + default patrol car | Anti-absorb + simple reinforce |
| Wanted suppress + native EMS block + mod EMS spawn | Prevents vanilla fighting mod |
| Arrest / combat blocked when apprehended | Stops infinite shoot after cuffs |

### L7. Prior dead-export pass — **remove if unused** (already partly applied)

See earlier “Simplify candidates” + **Applied** table (escaper, foot-prefer, stand_still, wave reinforce). Not the focus of this practical audit.

### Suggested coarsen order (if implementing later)

1. Collapse threat enum + kill dead AirShoot/UnarmedActive branches + single case-level estimate.  
2. Merge Cat/quota/nearby into one “response size” function.  
3. Drop density from classify or single threshold.  
4. Gate geometric witness scan strictly.  
5. Only then touch attack/reroute cadence.

Do **not** coarsen by deleting SWAT gates, phone open-on-Active, or nearby-vs-spawn.

---

## Defer / non-core

| Item | Bucket | Why |
|------|--------|-----|
| Entire `src/main/cpp` + ShadowHook | Out of ship | Not packed by `pack_module.sh` |
| Crash skip-orig gates / unwalkable scans | Removed | Must not return |
| Phone **model** streaming | Non-goal | Engine task owns model; not in ship logic |
| Full 911 multi-line busy/redial redesign | Non-goal | Not in Rust baseline |
| Implementing coarsen/deletes from L1–L5 | Out of band for analysis goals | Ship code change is a separate task |
| Heli / pursuit roadblock / tow radio from old docs | Not in ship modules | Not in current spawn kinds |

---

## Applied (post-discussion)

| Decision | Landed |
|----------|--------|
| No mod force board/exit preference | `should_prefer_foot_mobilization` removed |
| Escaper / anti-spin / temp closure later | `escaper.rs` removed; `temp_closures` dropped from globals |
| Stand-still / engagement_near / staging_drive | Removed |
| Pursuit config from **threat level** | `classify_response_category` threat-first; live discharge → `FirearmActive` |
| On-scene top-up ≠ waves | `build_on_scene_topup_plan` + `ReinforcementTopUp { density }`; max **3 attempts** safety cap only |
| Model kinds table | `DispatchVehicleKind` + `is_*` via kinds |
| Arrest thin helpers | `criminal_in_custody` wired in `sync_custody_criminals` |
| Attack quotas | `compute_quotas` uses live `CaseRecord` |

Remaining optional: dual perception policy doc only; further dead export hygiene.

Do **not** reintroduce skip-orig gates, foot-prefer overrides of engine enter/exit, or wave-indexed reinforcement.

---

## Quick map: core chain vs modules

```text
bits 0,4,5,10  →  signals / casualties / events
incident+store →  case
witness+phone  →  open case (Active)
coordinator    →  commit (nearby | spawn chain)
threat         →  Cat plan + (predicate for reinforce)
vehicle_spawn  →  CreateCarForScript
on-scene       →  attack + arrest
bit6 / 7-9     →  wanted suppress + EMS policy
bits 1-2       →  registry on engine recycle
```
