# Zygisk-DispatchModule (Zygisk 派发模组)

[![Platform](https://img.shields.io/badge/Platform-Android-green.svg)](https://developer.android.com/)
[![Zygisk](https://img.shields.io/badge/Framework-Zygisk-blue.svg)](https://github.com/topjohnwu/Magisk)
[![ShadowHook](https://img.shields.io/badge/Hook_Library-ShadowHook-orange.svg)](https://github.com/bytedance/android-inline-hook)
[![License](https://img.shields.io/badge/License-MIT-red.svg)](LICENSE)

[English Version](README.md) | **中文版本**

基于 **Zygisk** 和 **ShadowHook** 技术构建的高级警力派发与车辆战术控制模组，专为 **侠盗猎车手：圣安地列斯 终极版 (GTA: SA DE Android)** 开发。

本模组接管并优化了游戏原生的 AI 派发算法，引入了更逼真的警力围捕、强健的防卡死路径规划、区域配额整合、动态追捕配额，以及在战术传送或推开逻辑时的物理动能重置机制。

---

## 🚀 核心特性

*   **动态区域派发与配额整合**：
    根据不同的区域威胁等级和追捕烈度，动态整合警力追捕配额。在目标锁定后，自动扩展和调配周围活跃的警力。
*   **战术动能重置算法 (Tactical Momentum Resetting)**：
    在对追捕车辆进行避障微调、传送或战术推开时，同步重置车辆的瞬时物理动能向量。彻底消除了 GTA SA DE 物理引擎中常见的“推开后高速滑动”、“瞬移后失控撞墙”等物理惯性漏洞。
*   **自适应巡航与碰撞避让 (ACC)**：
    集成了近距离嫌疑人车辆避让、其他车辆减速慢行，以及自定义后退缓冲安全机制，确保追捕车队能够保持最佳的战术队形。
*   **防侧滑保护与自动恢复 (Anti-Spin Guard)**：
    实时监测追踪车辆的角速度和侧滑角，在追击车辆于高速漂移或碰撞失控时，自动介入稳定车身。
*   **高性能环境清理机制**：
    实时监控远端不活跃的行人、空置车辆及废弃警车并进行动态回收，确保高强度追捕场景下游戏帧率的平稳。
*   **极量级 ECS 引擎**：
    底层采用零成本、高性能的实体组件系统 (ECS) 与事件驱动总线设计，与游戏引擎底层指针对接，实现 100% 零运行时开销。

---

## 🛠️ 技术栈与依赖要求

*   **运行平台**：Android (64位 arm64-v8a)
*   **目标游戏**：GTA: San Andreas - The Definitive Edition (Android)
*   **注入框架**：[Zygisk (Magisk v24+)](https://github.com/topjohnwu/Magisk) 或支持 Zygisk 的 KernelSU / APatch。
*   **挂钩库**：[ShadowHook v2.0.1](https://github.com/bytedance/android-inline-hook)
*   **编译系统**：Android NDK (r27c) / 独立 CMake 3.18.1+

---

## 📦 目录结构

```text
Zygisk-DispatchModule/
├── src/main/cpp/
│   ├── zygisk/                  # Zygisk 接口头文件
│   ├── ecs_engine.hpp           # 高性能极轻量级 ECS 及事件驱动总线
│   └── module.cpp               # 模组核心逻辑、游戏 Hook 地址及物理控制
├── third_party/
│   ├── shadowhook/              # ShadowHook 预编译头文件
│   └── shadowhook-src/          # ShadowHook 完整编译源码树
├── android-arm64-toolchain.cmake# Android NDK CMake 工具链配置文件
├── build_in_container.sh       # 适用于 Linux 容器环境的单步隔离编译脚本
├── pack_module.sh              # 模块打包脚本
├── module.prop                 # Magisk 模块属性定义文件
└── build.gradle                # 供 Android Studio 编译的 Gradle 配置
```

---

## 🏗️ 如何编译

模组支持两种编译方式：使用隔离的 PRoot Linux 容器（推荐移动端 / Termux 开发者使用），或在 PC 上通过 Android NDK 直接编译。

### 方法 1：隔离容器构建（推荐 Termux / Linux 命令行）

如果您在 Termux 或纯净的 Linux 环境中开发，可以使用通过 PRoot Distro 运行的自动化编译脚本。

1. 确保系统已安装 `proot-distro`。
2. 运行隔离编译命令：
   ```bash
   proot-distro login --isolated --bind /path/to/your/workspace:/workspace ubuntu-build -- bash /workspace/Zygisk-DispatchModule/build_in_container.sh
   ```
3. 脚本会自动下载并配置 Android NDK (r27c) 以及 ShadowHook 源码，为 `arm64-v8a` 编译出 `libpolicemod.so`，并直接打包成可刷入的 Magisk 模块 Zip：
   *   **输出路径**：`Zygisk-PoliceDispatch.zip`

### 方法 2：Android Studio / 本地 Gradle 编译

1. 在 Android Studio 中将此目录作为项目导入。
2. 运行 `assembleRelease` 任务，或者在终端运行：
   ```bash
   ./gradlew assembleRelease
   ```
3. 运行本地打包脚本：
   ```bash
   bash pack_module.sh
   ```

---

## 📲 安装与使用

1. 将编译好的 `Zygisk-PoliceDispatch.zip` 传输至您的 Android 设备。
2. 打开 **Magisk** 或 **KernelSU** 应用程序。
3. 进入 **模块 (Modules)** -> **从本地安装 (Install from storage)**。
4. 选择并安装 `Zygisk-PoliceDispatch.zip`，然后重启设备。
5. 运行游戏即可，模组会在游戏启动时自动注入并接管派发逻辑。

---

## 📜 开源协议

本项目基于 [MIT License](LICENSE) 开源，欢迎自由学习、修改和分发，使用时请保留原作者署名。
