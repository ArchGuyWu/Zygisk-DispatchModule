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
*   **Modern ECS Engine**:
    Built on a ultra-lightweight, zero-cost Entity-Component-System (ECS) and Event-Driven architecture designed for zero overhead interfacing with GTA engine pointers.

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
