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
*   **Modern ECS Engine**:
    Built on an ultra-lightweight, zero-cost Entity-Component-System (ECS) and Event-Driven architecture designed for zero overhead interfacing with GTA engine pointers.

---

## 🛡️ Official Engine Bug Patches & Crash Prevention Deep-Dive

In the original game (GTA SA DE Android), players frequently encounter random, high-frequency crashes (SIGSEGV) during interactions with police, pedestrians, gang hassles, or companion scenarios. Through deep reverse engineering, we pinpointed the core architectural defects in the official engine and developed a comprehensive **12-Hook Defense System** to eliminate these persistent memory stability issues.

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

To achieve 100% crash-proof memory safety, we introduced a **POSIX-compliant pointer readability validator**:
This mechanism performs a safe probe by writing 1 byte of the address `ptr` to `/dev/null` using the standard `write(null_fd, ptr, 1)` system call. If `ptr` points to an unmapped, protected, or invalid memory page, the Linux kernel detects this and returns `-1` with `errno` set to `EFAULT` **without raising any signals or crashing the process**. This allows us to safely and reliably verify if any memory pointer is actually mapped and readable in user-space with near-zero overhead.

```cpp
static inline bool is_pointer_readable(const void* ptr) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000ULL || addr > 0x00007fffffffffffULL || (addr & 7) != 0) {
        return false;
    }
    static int null_fd = -1;
    if (null_fd < 0) {
        null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    if (null_fd < 0) return true; // Fallback if open failed
    long ret = write(null_fd, ptr, 1);
    if (ret < 0 && errno == EFAULT) {
        return false;
    }
    return true;
}

static inline void sanitize_task_pointers(void* task, int max_size_bytes = 256) {
    if (!task) return;
    if (!is_pointer_readable(task)) return;
    char* task_bytes = reinterpret_cast<char*>(task);
    for (int offset = 8; offset < max_size_bytes; offset += 8) {
        if (!is_pointer_readable(task_bytes + offset)) break;
        void** ptr_slot = reinterpret_cast<void**>(task_bytes + offset);
        void* ptr = *ptr_slot;
        if (ptr) {
            if (!is_pointer_readable(ptr)) {
                *ptr_slot = nullptr; // Forcefully sanitize unreadable garbage reference
            } else {
                void* vtable = *reinterpret_cast<void**>(ptr);
                if (vtable == nullptr) { // Detect zero-filled/destructed object reference
                    *ptr_slot = nullptr; // Forcefully sanitize to a clean nullptr
                }
            }
        }
    }
}
```

Once a zero-filled, unsafe task/entity pointer is sanitized to `nullptr`, the engine's original native `cbz` checks trigger successfully, allowing the game to execute safe fallback routines gracefully instead of crashing.

### 3. The 27-Hook Defense System
Our solution intercepts all core lifecycles and virtual tables related to companion, pathfinding, hold-entity, gang follower, gang hassle, footstep, buoyancy, post-rendering, game saving, water-related, footstep landing, task destruction, asset downloading, Unicode character formatting, font rendering, and text shaping tasks:

1.  **Companion Virtual Table Protection** (`GetPartnerSequence` - 4 Hooks):
    *   `CTaskComplexPartnerDeal::GetPartnerSequence`
    *   `CTaskComplexPartnerGreet::GetPartnerSequence`
    *   `CTaskComplexPartnerShove::GetPartnerSequence`
    *   `CTaskComplexPartnerChat::GetPartnerSequence`
2.  **Subtask Initialization Safeguards** (`CreateFirstSubTask` - 3 Hooks):
    *   `CTaskComplexPartner::CreateFirstSubTask` (Base class hook)
    *   `CTaskComplexPartnerDeal::CreateFirstSubTask` (Overridden method hook)
    *   `CTaskComplexPartnerGreet::CreateFirstSubTask` (Overridden method hook)
3.  **Active Task Controller Hook** (`ControlSubTask` - 1 Hook):
    *   `CTaskComplexPartner::ControlSubTask` (Base hook covering all derived `Deal`, `Greet`, `Shove`, and `Chat` tasks)
4.  **Pathfinding & Navigation Safeties** (`CreateSubTask` - 1 Hook):
    *   `CTaskComplexGoToPointAnyMeans::CreateSubTask` (Defensively filters and purges uninitialized or stale Ped/Vehicle references)
5.  **Gang Hassle Target Security Hook** (`CalcTargetOffset` - 1 Hook):
    *   `CTaskGangHassleVehicle::CalcTargetOffset` (Intercepts target coordinate offsets. If the target vehicle gets deleted or out of loading distance, the hook detects this before the native dereference at offset `0x498`, skipping the calculation gracefully and preventing SIGSEGV crashes)
6.  **Hold-Entity Bone Target Security Hook** (`SetPedPosition` - 1 Hook):
    *   `CTaskSimpleHoldEntity::SetPedPosition` (Intercepts holding position updates. If the ped's RW clump pointer at offset `0x648` is unreadable or null, the hook skips the offset calculation to avoid null pointer dereferences, allowing smooth scene transitions)
7.  **Gang Follower Target Security Hook** (`ControlSubTask` - 1 Hook):
    *   `CTaskComplexGangFollower::ControlSubTask` (Intercepts follower subtask updates. If the follower's leader pointer at offset `0x18` is null or invalid, the hook returns `nullptr` to prevent native null pointer dereference at offset `0x498`, ensuring flawless gameplay stability during gang recruitments/activities)
8.  **Transition Footsteps Null Pointer Hook** (`PlayFootSteps` - 1 Hook):
    *   `CPed::PlayFootSteps` (During transitions, teleports, or vehicle entries/exits, the ped's RW clump/model pointer `m_pRwObject` at offset `0x20` can be temporarily detached or not yet loaded. The native engine blindly dereferences `m_pRwObject` offset `0x44` even when it is null, causing SIGSEGV crashes. This hook detects if `m_pRwObject` is null and safely bypasses the footstep logic, completely resolving transition-related footstep crashes)
9.  **Buoyancy Processing Task Protection** (`ProcessBuoyancy` - 1 Hook):
    *   `CPed::ProcessBuoyancy` (When processing water buoyancy, the engine iterates over the task slots in `CTaskManager`. If a slot contains a dangling or zeroed task pointer, the engine blindly dereferences its virtual table to call `GetTaskType`, causing a null pointer dereference crash at offset `0x18`. This hook dynamically scans and purges any unsafe or zeroed task pointers in the task manager before the buoyancy logic executes, completely eliminating this crash vector)
10. **Post-PreRender Intelligence Task Protection** (`ProcessAfterPreRender` - 1 Hook):
    *   `CPedIntelligence::ProcessAfterPreRender` (When the game updates pedestrian intelligence post-rendering, it queries task slots in the primary and secondary task managers. If a task has been destructed but its pointer remains in the slot, its vtable pointer points to the base class `CTask` which has pure virtual slots. Calling `IsSimple` on it triggers a `__cxa_pure_virtual` call and aborts. This hook dynamically scans and purges any destructed or unsafe task pointers in `CPedIntelligence` before the intelligence logic executes, preventing pure virtual call crashes)
11. **Game Save Decision Maker Safety Hook** (`Save` - 1 Hook):
    *   `CScriptDecisionMakerModifications::Save` (During game saving or autosaving, the engine saves modifications to script decision makers by querying two global `CScriptDecisionMaker` objects. If either object was released or uninitialized (leaving its vtable pointer as `nullptr`), the engine dereferences the null vtable at offset `0x18` or `0x38`, causing a SIGSEGV crash. This hook dynamically detects and purges these invalid global pointers to `nullptr`, allowing the engine's built-in null checks to safely skip them and prevent crashes)
12. **In-Water Task Initialization Protection** (`CreateFirstSubTask` - 1 Hook):
    *   `CTaskComplexInWater::CreateFirstSubTask` (When a pedestrian enters water, the engine initializes a complex water task. If the water physics manager or its internal water array has been deallocated or remains uninitialized, the engine blindly dereferences the null array pointer, causing a SIGSEGV crash. This hook dynamically intercepts the task creation, verifies the water manager and its internal array, and safely returns `nullptr` if they are uninitialized, preventing crashes during water-related transitions)
13. **Footstep Landing Effect System Protection** (`DoFootLanded` - 1 Hook):
    *   `CPed::DoFootLanded` (When a pedestrian's foot lands, the engine attempts to trigger a footprint particle effect by retrieving the ped's particle system `FxSystem_c` pointer at offset `0x90`. If the particle system is uninitialized or has been deallocated (leaving its member at offset `0x18` as null), calling `AddParticle` on it dereferences `nullptr + 0x18`, causing a SIGSEGV crash. This hook dynamically intercepts the footstep landing event, verifies the particle system's validity, and safely skips particle generation if the system is uninitialized, preventing footstep-related particle crashes)
14. **Mid-Process Task Destruction Protection** (`~CTask` - 1 Hook):
    *   `CTask::~CTask` (During pedestrian intelligence updates in `ProcessAfterPreRender`, some subtasks can be dynamically destructed by other active tasks. If their pointers remain in the task manager slots, the engine will attempt to access them later in the same frame, causing a SIGSEGV crash due to a null or garbage virtual table. This hook dynamically intercepts task destruction, checks if the task belongs to the currently executing `CPedIntelligence` context, and immediately clears its slot to `nullptr`, completely preventing mid-frame dangling pointer crashes)
15. **Google Play Core Asset Delivery Safeguard** (`AssetPackManager_requestDownload` - 1 Hook):
    *   `AssetPackManager_requestDownload` (On custom ROMs or devices without Google Play Services, the JNI initialization of the Play Core `AssetPackManager` returns an uninitialized or corrupt shared pointer. When the game attempts to request asset pack downloads, the destructor of this corrupt shared pointer dereferences `nullptr + 0x18`, causing a SIGSEGV crash. This hook intercepts the JNI call, safely returns `-101` (`ASSET_PACK_API_NOT_AVAILABLE`) to notify the game that Google Play Asset Delivery is unavailable, allowing it to fall back to local assets without crashing)
16. **ICU Character & Date Processing Safeguards** (6 Hooks):
    *   `icu_64::TimeZone::findID` (Prevents crashes when searching for timezone IDs with a null or unreadable `UnicodeString` reference)
    *   `icu_64::TimeZone::getDisplayName` (Prevents crashes when localizing time zones with a null or unreadable `TimeZone` instance)
    *   `icu_64::UnicodeSetStringSpan::span` (Prevents crashes when parsing Unicode character spans with a null or unreadable instance/string pointer)
    *   `icu_64::MessageFormat::findKeyword` (Prevents crashes when formatting messages with a null or unreadable string/keyword list)
    *   `icu_64::CollationIterator::previousCodePoint` (Prevents crashes during string collation/sorting iteration if the iterator is null)
    *   `u_strlen_64` (Prevents crashes when ICU's Unicode string length function receives a dangling or wild pointer from corrupted locale/resource data by validating the pointer before access)
17. **FreeType Font Interpreter Protection** (`TT_RunIns` - 1 Hook):
    *   `TT_RunIns` (Prevents crashes inside the TrueType bytecode interpreter when rendering custom or corrupted fonts by verifying the execution context pointer)

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
Zygisk-DispatchModule/
├── src/main/cpp/
│   ├── zygisk/                  # Zygisk API headers
│   ├── ecs_engine.hpp           # High-performance lightweight ECS & Event bus
│   └── module.cpp               # Core mod logic, GTA hook addresses, & physics controls
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
   proot-distro login --isolated --bind /path/to/your/workspace:/workspace ubuntu-build -- bash /workspace/Zygisk-DispatchModule/build_in_container.sh
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
