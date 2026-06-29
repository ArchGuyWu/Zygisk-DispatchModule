# Zygisk-DispatchModule

[![Platform](https://img.shields.io/badge/Platform-Android-green.svg)](https://developer.android.com/)
[![Zygisk](https://img.shields.io/badge/Framework-Zygisk-blue.svg)](https://github.com/topjohnwu/Magisk)
[![ShadowHook](https://img.shields.io/badge/Hook_Library-ShadowHook-orange.svg)](https://github.com/bytedance/android-inline-hook)
[![License](https://img.shields.io/badge/License-MIT-red.svg)](LICENSE)

**English Version** | [中文版本](README_CN.md)

An advanced police dispatching and intelligent vehicle control module for **GTA: San Andreas – The Definitive Edition (Android)**, powered by **Zygisk** and **ShadowHook**.

This module overwrites and optimizes the game's native AI dispatching algorithms to offer realistic police pursuits, robust anti-stuck pathfinding, regional quota consolidation, dynamic chasing quotas, and synchronized physics-momentum resetting during tactical teleportations/nudges.

---

## 🚀 Key Features

*   **Dynamic Regional Dispatching & Quotas**: 
    Consolidates police chasing quotas dynamically based on regional threat profiles and pursuit severity. Automatically scales active dispatch agents when target tracking is established.
*   **Tactical Momentum Resetting**: 
    Synchronizes physical vehicle momentum during nudge, teleportation, or warp maneuvers, eliminating high-speed sliding or post-nudge wall crashes in the GTA SA DE physics engine.
*   **Adaptive Cruise Control & Collision Avoidance**:
    Integrates close-range criminal avoidance, other-vehicle deceleration, and custom back-nudge safeguards to maintain robust tactical posture.
*   **Anti-Spin Guard & Automated Recovery**:
    Tracks vehicle angular velocity and slip angles in real-time. Automatically intervenes to stabilize out-of-control police units during high-speed chases.
*   **Performance-optimized Ambient Cleanup**:
    Monitors and dynamically recycles distant peds, idle vehicles, and abandoned police units, maintaining native framerates under heavy dispatch scenarios.
*   **Official Engine Bug Patches & Crash Prevention**:
    Proactively intercepts unstable official C++ engine behaviors (such as companion/greet tasks `CTaskComplexPartner::GetPartnerSequence` and pathfinding tasks `CTaskComplexGoToPointAnyMeans::CreateSubTask`). Deploys a deep pointer-sanitizer that detects zero-filled, stale, or destructed official game tasks/entities, dynamically zeroing out unsafe references to bypass engine null-pointer dereferences (SIGSEGV) safely.
*   **Lightweight ECS layer**:
    Header-only ECS for cop/vehicle components and per-frame systems. Uses normal map lookups and mutexes — not a zero-cost abstraction.

---

## 🛡️ Official Engine Bug Patches & Crash Prevention Deep-Dive

In the original game (GTA SA DE Android), players may hit random SIGSEGV crashes during police, ped, gang, or companion interactions. This mod hooks **39 ShadowHook sites** plus pointer readability checks to **reduce** (not guarantee elimination of) known crash paths. See [`docs/CRASH_STATUS.md`](docs/CRASH_STATUS.md).

### 1. Root Cause Analysis & Vulnerability Pinpointing
Using the most frequent companion/greet task crash (RVA `0x57ae40c`, corresponding to `CTaskComplexPartnerGreet::GetPartnerSequence`) as an example, the crashing instruction stream is decoded below:

| Virtual Address | Machine Code (LE) | ARM64 Assembly Instruction | Explanation / Impact |
| :--- | :--- | :--- | :--- |
| `0x73ee4f7404` | `f94002c8` | `ldr x8, [x22]` | Load virtual table pointer (vtable) from partner task object pointer `x22` (`this`) into `x8`. |
| `0x73ee4f7408` | `aa1603e0` | `mov x0, x22` | Copy `x22` to `x0` to pass as the `this` parameter. |
| `0x73ee4f740c` | `f9401108` | **[CRASH]** `ldr x8, [x8, #0x20]` | Dereference vtable offset `0x20` (index 4 for the virtual function), causing a **SIGSEGV**. |

*   **Underlying Issue**: The object pointer `x22` is not a null pointer (`nullptr`). Instead, it is a **stale heap address that has been destructed and filled with zeros** by the game's allocator (e.g., `0x7361ce12f8`).
*   **Crash Mechanism**: Because the pointer itself is non-zero, native C++ null-pointer safeguards (e.g., `cbz x22`) are bypassed. When the engine attempts to resolve the virtual method via the vtable, the loaded vtable address `x8` becomes `0` (since the object was zero-filled). The subsequent `ldr x8, [x8, #0x20]` dereferences `0x20`, causing an immediate null pointer dereference and application crash.

### 2. Intelligent Pointer Sanitizer & POSIX Safe Readability Validation

To leverage the engine's built-in, null-safe fallback logic rather than overriding entire subtasks blindly, we designed a robust **Pointer Sanitizer**. Before executing any virtual task methods, the sanitizer scans member pointer slots inside the C++ task objects.

#### ⚠️ Alignment Alignment Vulnerabilities & POSIX System-Call Defense
In our first-generation sanitizer, we relied solely on standard range-checks (e.g., `addr >= 0x10000`) and 8-byte pointer alignments. However, in a complex game runtime, certain non-pointer fields (such as local task states or floats like `0.5f` represented as `0x3f000000`) can **coincidentally satisfy 8-byte alignment and exceed 0x10000**. When the sanitizer blindly dereferenced these values to read their "vtable," it induced a SIGSEGV.

For safer pointer checks on hot paths, we use a **POSIX pipe probe** (`write(pipe_fd, ptr, 1)`; invalid pages return `EFAULT`). This lowers the chance of dereferencing wild pointers but does **not** make downstream engine logic crash-proof.

```cpp
struct ThreadLocalPipe {
    int fds[2] = {-1, -1};
    ThreadLocalPipe() {
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            fds[0] = -1; fds[1] = -1;
        }
    }
    ~ThreadLocalPipe() {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
};

static inline bool is_pointer_readable(const void* ptr) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000ULL || addr > 0x00007fffffffffffULL) {
        return false;
    }
    thread_local static ThreadLocalPipe tl_pipe;
    if (tl_pipe.fds[1] < 0) return false;
    long ret = write(tl_pipe.fds[1], ptr, 1);
    if (ret >= 0) {
        char dummy;
        read(tl_pipe.fds[0], &dummy, 1);
        return true;
    }
    return errno != EFAULT;
}

// Blind-scan sanitize_task_pointers is disabled in source — see module.cpp comments.
// Current approach: per-hook is_pointer_readable / vtable checks at entry.
```

### 3. Hook overview (39 sites, counted 2026-06-29)

Hooks cover wanted level, crime reporting, emergency spawns, task management, footsteps, buoyancy, ICU strings, and more. Full list: `rg shadowhook_hook_sym_name src/main/cpp/module.cpp`. Rough split:

1.  **Gameplay Features & Custom Dispatching** (12 Hooks):
    *   `report_crime` (Intercepts and manages wanted level crimes)
    *   `register_kill` (Tracks kills for custom wanted logic)
    *   `set_wanted` (Controls the wanted level system)
    *   `generate_damage_event` / `event_damage_ctor_c1` / `event_damage_ctor_c2` (Handles damage events for police reactions)
    *   `set_current_weapon` (Manages weapons for custom dispatch units)
    *   `the_scripts_process` (Ticks the main gameplay loop for trueDispatch)
    *   `add_police_occupants` (Binds occupants to spawned cop cars)
    *   `tell_occupants_leave_car` (Triggers custom vehicle exits)
    *   `generate_one_emergency_car` / `script_generate_one_emergency_car` (Draw distance scaling workarounds for emergency vehicles in mobile versions)
2.  **Stability defense hooks** (~27 sites, overlapping categories):
    *   `u_strlen_64` (Prevents startup crashes in ICU's Unicode string length function by validating wild pointers)
    *   `CPed::ProcessBuoyancy` / `cBuoyancy::ProcessBuoyancy` (Prevents crashes during buoyancy processing. `cBuoyancy::ProcessBuoyancy` is hooked to sanitize task slots immediately after the physics calculation, solving a race condition where a task gets destructed mid-function)
    *   `CPed::PlayFootSteps` (Prevents crashes during transitions when the ped's RW clump/model is temporarily detached)
    *   `CTaskManager::ManageTasks` (Sanitizes the task chain inside CTaskManager to prevent null/invalid virtual table dereferences)
    *   `CAttractorScanner::ScanForAttractorsInRange` (Sanitizes the pedestrian intelligence task slots to prevent crashes when scanning attractors)
    *   `CTaskComplexGangFollower::ControlSubTask` (Prevents crashes in gang follower tasks when the follower, leader, or partner's task managers contain zeroed/invalid tasks)
    *   `CTaskSimpleHoldEntity::SetPedPosition` (Prevents crashes in hold-entity tasks when the ped's clump is unreadable)
    *   `CTaskComplexUsePairedAttractor::CreateNextSubTask` (Prevents crashes when the paired attractor task is no longer active on the ped by verifying and sanitizing it)
    *   `CPedIntelligence::ProcessStaticCounter` (Prevents crashes when updating static counters on peds with zero-filled tasks by sanitizing them beforehand)
    *   `CTaskComplexFacial::~CTaskComplexFacial` (Prevents double free and null pointer dereference crashes when deleting facial tasks with zero-filled subtasks)
    *   `CTaskManager::FindActiveTaskByType` (Prevents crashes inside the task type lookup when dealing with zero-filled tasks)
    *   `CAEPedSpeechAudioEntity::PlayLoadedSound` (Prevents crashes in ped speech audio playing when the ped's speech manager is null)
    *   `CCarGenerator::CheckIfWithinRangeOfAnyPlayers` (Prevents crashes when the car generator checks range against a temporarily null player ped in the pool)
    *   `CTaskComplexAvoidOtherPedWhileWandering::ControlSubTask` (Prevents crashes when wandering peds attempt to avoid other peds with zero-filled tasks)
    *   `CTaskComplexSequence::Flush` (Prevents virtual table dereference crashes during task sequence flushing when the sequence contains destructed/zero-filled tasks)
    *   `CTaskSimpleEvasiveStep::FinishAnimEvasiveStepCB` (Prevents crashes during evasive step callbacks when the task's context pointer has been destructed/zero-filled)
    *   `CTaskComplexBeInGroup::ControlSubTask` (Prevents crashes in group tasks when calling subtask control on a destructed/zero-filled group task)
    *   `IKChainManager_c::Update` (Prevents null pointer dereference crashes in the IK chain manager during scene transitions)
    *   `CCam::Process_FollowPed_SA` (Prevents null pointer dereference crashes in the camera system when the followed target is temporarily null during transitions)
    *   `CTaskComplexLeaveCar::MakeAbortable` (Prevents virtual table dereference crashes when aborting a vehicle leave task whose internal subtask pointer `m_pSubTask` is null)
    *   `CCarAI::UpdateCarAI` (Prevents crashes during vehicle AI updates when a destructed/zero-filled vehicle object is processed)
    *   `CTaskComplexFacial::ControlSubTask` (Prevents virtual table dereference crashes during facial task updates when the internal subtask pointer `m_pSubTask` is null)

---
## 🛠️ Tech Stack & Requirements

*   **Platform**: Android (64-bit arm64-v8a)
*   **Target Game**: GTA: San Andreas - The Definitive Edition (Android)
*   **Core Injection**: [Zygisk (Magisk v24+)](https://github.com/topjohnwu/Magisk) or KernelSU / APatch with Zygisk support.
*   **Inline Hook**: [ShadowHook v2.0.1](https://github.com/bytedance/android-inline-hook)
*   **Development / Build System**: Android NDK (r27c) / standalone CMake 3.18.1+

---

## 📦 Directory Structure

```text
mod-workspace/
├── src/main/cpp/
│   ├── include/                 # log, game_config, game_types, pointer_sanitizer
│   ├── zygisk/                  # Zygisk API headers
│   ├── ecs_engine.hpp           # Lightweight ECS & event bus
│   └── module.cpp               # Main logic (hooks + dispatch + ECS wiring)
├── docs/
│   ├── CRASH_STATUS.md
│   └── MODULE_LAYOUT.md
├── third_party/
│   ├── shadowhook/              # Prebuilt headers for ShadowHook
│   └── shadowhook-src/          # Full compiled source tree of ShadowHook
├── android-arm64-toolchain.cmake# Android NDK CMake toolchain definitions
├── build_in_container.sh       # Script for isolated compilation inside a Linux container
├── pack_module.sh              # Local shell packaging script
├── module.prop                 # Magisk module properties definition
└── build.gradle                # Gradle configuration for Android Studio compilation
```

---

## 🏗️ How to Build

The module can be compiled in two ways: via isolated PRoot Linux containers (recommended for mobile/Termux developers), or locally via standard Android NDK tools.

### Method 1: Isolated Container Build (Termux / Linux CLI)

If you are developing in Termux or a clean Linux system, you can use the automated `build_in_container.sh` via PRoot Distro.

1. Ensure `proot-distro` is installed on your system.
2. Run the isolated build script:
   ```bash
   ./build_in_container.sh
   # Or manually:
   # proot-distro login --isolated --bind /path/to/Projects:/workspace ubuntu-build \
   #   -- bash /workspace/mod-workspace/build_in_container.sh
   ```
3. The script will automatically download the Android NDK (r27c), setup ShadowHook sources, compile `libpolicemod.so` for `arm64-v8a`, and output a flashable Magisk module zip:
   *   **Output**: `Zygisk-PoliceDispatch.zip`

### Method 2: Android Studio / Local Gradle Build

1. Open this directory as a project in Android Studio.
2. Run the `assembleRelease` Gradle task or run:
   ```bash
   ./gradlew assembleRelease
   ```
3. Run the local packaging script to compress the binary and Magisk file structures:
   ```bash
   bash pack_module.sh
   ```

---

## 📲 Installation & Usage

1. Transfer the compiled `Zygisk-PoliceDispatch.zip` to your Android device.
2. Open the **Magisk** or **KernelSU** application.
3. Navigate to **Modules** -> **Install from storage**.
4. Select `Zygisk-PoliceDispatch.zip` and tap install.
5. Reboot your device.
6. Launch GTA SA DE. The dispatch module will automatically load on game startup.

---

## 📜 License

This project is licensed under the [MIT License](LICENSE). Feel free to use, modify, and distribute with attribution.
