# Dispatch product model (rewrite authority)

This document freezes the **descriptive core** for the full dispatch rewrite:
vocabulary, open/close rules, departments, response, OnScene behaviour,
policy gates, Light/Full frame steps, and Effects.

It is the product contract for pure-logic work (`dispatch-model` and later
wiring). Prefer this file over older gameplay inventories when they conflict.
Implementation detail (FFI offsets, task ctors, car model tables) lives in
engine/apply code, not here.

**Out of scope for this doc:** crate layout of the old tree, crash-gate history,
tuning constants that are not product rules.

---

## 0. Delivery constraints (stack)

| Item | Choice |
|------|--------|
| Language / artifact | Rust → Zygisk `cdylib`, `aarch64-linux-android` |
| Entry | Zygisk API (thin C++/Rust load + install only) |
| Engine symbols | System `dlopen(RTLD_NOLOAD)` + `dlsym` on already-loaded `libUE4.so` |
| Link surface | `libc` / `libm` / `libdl` + `liblog` — **no** ShadowHook / xDL third-party hook libs |
| Pack | Magisk zip: `module.prop` + `zygisk/arm64-v8a.so` |
| Pure logic | Core rules **zero FFI**; `cargo test` must run offline |

**Hard rules:** no ShadowHook; no skip-orig “crash gates” on generic engine
updaters (ManageTasks / buoyancy / static counters / etc.). Policy hooks may
short-circuit **gameplay** side effects only (wanted, vanilla EMS spawn), not
engine housekeeping.

---

## 1. Architecture layers (brief)

```text
[hooks]   signal collect | frame host | policy intercept | pool recycle listen | (optional) spawn gate
    ↓
[runtime] holds World; only frame host end calls frame_tick
    ↓
[model]   pure logic: state transitions + Effects (no engine pointers)
    ↓
[engine]  consumes Effects: tasks, spawn, pathing, pool reads (minimal FFI)
```

| Layer | Responsibility |
|-------|----------------|
| **hooks** | Thin detours: enqueue signals / despawns, frame clock, policy allow-or-block vanilla. No threat math, no spawn plans in the detour body. |
| **runtime** | Owns `World`, input queues, engine handle. `frame_tick` drains queue, picks Light or Full, commits, applies Effects. |
| **model** | Pure SNAPSHOT → COMPUTE → COMMIT. Produces `World'` + `Vec<Effect>`. Unit-testable with no libUE4. |
| **engine** | `apply(effects)` only: CreateCar, assign tasks, suppress wanted, etc. Fail-closed live checks; no second product state machine. |

**Conventions:**

- Detours default to calling **orig**. Policy may skip vanilla *gameplay* result when the mod owns that lane.
- Signal hooks **enqueue only**; they never open cases or run full dispatch.
- One write path for World per script frame: `frame_tick`.
- Re-entrancy during tick: fail-closed skip nested mod logic.
- Model holds `PedId` / `VehicleId` (or gen handles), never raw engine pointers.

Layers above are the rewrite skeleton; this file’s authority is **§2–§5 product
logic**. Hook symbol names may change; roles do not.

---

## 2. One-line product loop

Event is **reported** (logical `ReportPhase == Connected`, or a cop path that
reaches Connected without civilian delays) → **open case** → dispatch
Police / EMS / Fire as needed → units **respond and act OnScene** → **Done**
and cleanup.

---

## 3. Vocabulary (minimum)

| Name | Values / meaning |
|------|------------------|
| **SignalKind** | `Casualty` \| `Injury` \| `Gunfire` \| `PropertyDamage` \| `Fire` \| `Explosion` |
| **ReportPhase** | `Seeking` → `Calling` → `Connected` → `Ended` — **open case only at Connected**; logical phase only |
| **Phase** (case) | `Open` → `Responding` → `OnScene` → `Done` |
| **Dept** | `Police` / `Ems` / `Fire` (need flags, not a score API) |
| **Threat** | `Low` \| `Armed` \| `ActiveFire` |
| **ResponseSize** | Single function of Threat + criminal count: nearby slots, patrol count, SWAT/FBI eligibility |

**Not in this model:**

- Second-layer score / tier / “Cat” APIs
- Multi-line 911 busy / transfer queues
- **Partial** report quality (binary: not Connected → no case; Connected → open)

### Report phases are logical only

| Requirement | Status |
|-------------|--------|
| Civilian phone **animation** | **Not required** |
| Cop radio **animation** | **Not required** |

`Seeking` / `Calling` / `Connected` are timers and state bits for model
tick. Engine may still play anims later as polish; product rules must not depend
on phone/radio tasks completing.

---

## 4. Rules

### 4.1 Open case

1. Open a case **only** when logical `ReportPhase == Connected`.
2. Civilians advance through phase delays (Seeking → Calling → Connected).
3. Police (in-world cops as reporters) **may go straight to Connected** — no
   civilian delay ladder required.
4. There is **no Partial** report quality; pre-Connected work does not open a case.

### 4.2 Department needs

| Dept | Triggered by |
|------|----------------|
| **Fire** | Fire / explosion clues |
| **Ems** | Casualty / injury |
| **Police** | Violent / illegal clues (default for most criminal cases) |

A single case may set multiple need flags (e.g. gunfight + injury → Police + Ems).

### 4.3 Response order

1. **Nearby first** — mobilize existing on-map units toward the scene.
2. **Then offscreen spawn** — if nearby capacity fails or is insufficient,
   emit spawn Effects (patrol / SWAT / FBI / ambulance / firetruck as decided).

### 4.4 EMS / Fire (not the police threat ladder)

- EMS and Fire **do not** use the police threat / ResponseSize ladder.
- They only need **route-to-scene** behaviour once dispatched.
- **Default:** **1 vehicle per case per department** (EMS and Fire each).
- **Severe reinforcement:** when the scene is severe, the model **may** dispatch
  more, subject to a **hard cap**: at most **2 vehicles of that same department
  simultaneous in the player view / stream radius**.
  - Cap is **per department** (≤2 ambulances *and* ≤2 firetrucks is allowed if
    both depts are needed; not “2 emergency vehicles total”).
  - Cap is **not** unlimited reinforcement; second unit fills up to 2 in view.

### 4.5 Police OnScene

While case `Phase == OnScene`, police run **attack** and **arrest** evaluation
on **intervals** (not every Light frame). Emit `AttackPass` / `ArrestPass`
Effects when FullDispatch decides a pass is due.

### 4.6 Policy (vanilla coexistence)

| Condition | Policy |
|-----------|--------|
| Police case(s) active | **Suppress** vanilla wanted / crime-report escalation that would fight the mod |
| Mod zone / dispatch active | **Block** vanilla EMS/fire car generation; mod owns ambulance / firetruck via Effects |
| Dispatch disabled / mission soft gate | Policy **allows** vanilla (pass orig); see runtime gate notes |

Suppression may be implemented as a policy hook and/or `SuppressWanted` Effect;
semantics are “mod owns the lane while police cases are active.”

### 4.7 Close case

Transition to `Done` when there are no effective criminals, disposition is
complete, or timeout fires — then GC / stand-down bound units (`CloseCase` and
lifecycle cleanup).

---

## 5. Frame order: LightStep vs FullDispatch

### 5.1 Shell

Every script frame (when gated on), **at most one** `frame_tick`:

```text
frame_tick:
  if !dispatch_enabled || reentrant: return
  inputs = queue.drain()
  if should_full_dispatch(world, inputs, now):
      commit = FullDispatch(world, inputs, now)
  else:
      commit = LightStep(world, inputs, now)
  world = commit.world          // single replace
  engine.apply(commit.effects)  // Light usually []
```

| Step | When | Does | Effects |
|------|------|------|---------|
| **LightStep** | **Default** most frames | Account simple inputs; advance clocks (report phases, open delay, OnScene interval timers); mark ready/should_close; **no** geometry, **no** threat/needs recompute, **no** spawn/attack decisions | Default `[]` |
| **FullDispatch** | Triggers (see below) | Full read/compute/commit: merge signals → Connected open → needs/threat/size → mobilize/spawn → OnScene attack/arrest → EMS/Fire (incl. severe view-cap) → optional geometric witness | World' + Effects same commit |

**Typical `should_full_dispatch` (OR, may be tightened later):**

- Queue has inputs that need immediate full handling (first shot/kill, Connected boundary)
- A case’s time since last Full ≥ `full_interval_ms`
- A case is at a phase boundary (open delay due, OnScene attack interval due)
- Otherwise → **LightStep**

**Principles:**

1. No cross-hook half-transactions — detours only enqueue.
2. Prefer Light; budget Full.
3. One commit pipeline for both steps — no third “random World mutate” path.
4. Coherence over per-frame freshness: World and Effects commit together.

### 5.2 Conceptual per-frame order

```text
each frame_tick:
  drain signals / despawns
  ├─ LightStep (default): bookkeeping + clocks (incl. report phases) + ready flags; Effects=[]
  └─ FullDispatch (when triggered):
       merge signals → Connected open case → needs / threat / size
       → response / OnScene / EMS·Fire (severe ≤2 in view) → (optional geo witness) → commit
  pool recycle already accounted in drain; Done/GC on the commit that closes
```

---

## 6. Effects (model → engine only language)

Effects are **intent** produced only by model tick and consumed only by
`engine.apply`. They are not World state; they are discarded after apply.

| Effect | Intent |
|--------|--------|
| `OpenCase` | Case opened (Connected) — engine bookkeeping / presentation if any |
| `CloseCase` | Case finished — stand-down / GC path |
| `MobilizeNearby` | Task existing nearby units toward scene (cap from ResponseSize / policy) |
| `SpawnPatrol` | Spawn police patrol response |
| `SpawnSwat` | Spawn SWAT when ResponseSize says so |
| `SpawnFbi` | Spawn FBI when ResponseSize says so |
| `SpawnAmbulance` | Spawn EMS; subject to **≤2 same dept in player view/stream** on severe stack |
| `SpawnFiretruck` | Spawn Fire; same **≤2** view/stream hard cap |
| `SuppressWanted` | Reinforce wanted suppression while police cases active (optional if policy hook owns it fully) |
| `AttackPass` | OnScene police attack evaluation / tasking for a case |
| `ArrestPass` | OnScene police arrest evaluation / tasking for a case |

- Spawn vehicle model lists, ring coords, and task constructors stay in **apply**,
  not in model.
- `SpawnAmbulance` / `SpawnFiretruck` may be emitted a second time on severe
  scenes until the view-cap is full; never past the hard cap of **2** of that
  department in view/stream.

---

## 7. What this model deliberately omits

| Omitted | Why |
|---------|-----|
| Civilian phone anim / cop radio anim as gates | Phases are logical only |
| Partial report quality | Connected-only open |
| 911 multi-line busy simulation | Out of minimum closure |
| Police threat ladder for EMS/Fire | Route-only + 1 default / ≤2 severe in view |
| Unlimited EMS/Fire stacking | Hard view/stream cap 2 |
| Skip-orig crash gates | Stack constraint §0 |
| Fake ped “body double” signal receivers | Real hooks + real civilian elect only |

---

## 8. Relation to other docs

| Doc | Role |
|-----|------|
| **This file (`MODEL.md`)** | Authoritative **product** model for the rewrite |
| `BASELINE.md` | Current ship baseline / build / hook mask inventory (pre- or mid-rewrite) |
| `GAMEPLAY_SIMPLIFY.md` | Analysis inventory; superseded where it conflicts with MODEL |
| `MEMORY_LIFECYCLE.md` | Safety inventory; orthogonal to product rules |

When rewrite crates land, pure-logic tests should encode §3–§6 directly.
