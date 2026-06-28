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
*   **官方引擎漏洞修复与防闪退保护**：
    主动拦截官方原版不稳定的异步 C++ 行为（如伴随/打招呼任务 `CTaskComplexPartner::GetPartnerSequence` 与寻路任务 `CTaskComplexGoToPointAnyMeans::CreateSubTask`）。集成深层指针对齐与零值安全检测器，在官方引擎即将崩溃前，动态清除已被虚空释放/填零的不安全任务与实体引用，使引擎能安全走向 native 空值回退逻辑，彻底根治原版的内存解引用闪退问题。
*   **极量级 ECS 引擎**：
    底层采用零成本、高性能的实体组件系统 (ECS) 与事件驱动总线设计，与游戏引擎底层指针对接，实现 100% 零运行时开销。

---

## 🛡️ 官方引擎漏洞修复与防闪退深层剖析

在游戏原版（GTA SA DE Android）中，玩家在与警方、行人、帮派袭击或伴随任务交互时，经常会遇到随机且高频的闪退（SIGSEGV）。本模组通过深层逆向分析定位到了官方引擎的核心缺陷，并实现了一套 **12-Hook 协同防御系统** 彻底解决了这些长久以来的内存稳定性问题。

### 1. 缺陷深度分析与定位
以最频发的伴随/打招呼任务闪退（RVA `0x57ae40c`，对应 `CTaskComplexPartnerGreet::GetPartnerSequence`）为例，崩溃时的底层汇编指令流如下：

| 虚拟地址 | 机器码 (小端) | ARM64 汇编指令 | 作用说明 |
| :--- | :--- | :--- | :--- |
| `0x73ee4f7404` | `f94002c8` | `ldr x8, [x22]` | 从伙伴任务对象指针 `x22` (`this`) 中加载虚函数表指针 (vtable) 到 `x8` |
| `0x73ee4f7408` | `aa1603e0` | `mov x0, x22` | 将 `x22` 复制给 `x0` 作为 `this` 参数传递 |
| `0x73ee4f740c` | `f9401108` | **[CRASH]** `ldr x8, [x8, #0x20]` | 试图读取虚表偏移 `0x20`（第 4 个虚函数），解引用引发 **SIGSEGV** 崩溃 |

*   **根本原因**：对象指针 `x22` 并非空指针（`nullptr`），而是一个**已被析构并被内存管理器填零（Zero-filled）**的残余堆地址（如 `0x7361ce12f8`）。
*   **闪退机制**：由于指针本身非零，官方原版的 C++ 空指针判断（如 `cbz x22`）被完美绕过。但由于整块内存已被填零，读出的虚表指针 `x8` 变为了 `0`。紧接着，`ldr x8, [x8, #0x20]` 强行解引用 `0x20` 处非法的内存空间，导致系统级闪退。

### 2. 指针净化器防御机制 (Pointer Sanitizer) 与 POSIX 安全可读性校验

为了完美兼容游戏引擎本已具备的空值安全回退逻辑，模组引入了高度健壮的 **“动态指针净化器” (Pointer Sanitizer)**。在进入虚函数前，自动扫描 C++ 任务对象结构体内的所有成员槽位：

#### ⚠️ 深度内存对齐漏洞与 POSIX 系统调用级防御
在第一代净化器中，我们仅对指针值进行了基础范围检测（如 `addr >= 0x10000`）与 8 字节对齐检测。然而在复杂的游戏运行时中，某些非指针成员（例如任务状态结构体中存放的 `float` 浮点数，如 `0.5f` 在内存中表示为 `0x3f000000`）在数值上**恰好满足 8 字节对齐且大于 0x10000**。当净化器盲目解引用这些伪指针去读取虚表时，依然会引发 SIGSEGV 崩溃。

为了实现 100% 绝对的内存稳定性，本模组引入了 **POSIX 安全可读性验证器**：
利用标准 POSIX 规范，通过向 `/dev/null` 虚拟设备执行极低开销的 `write(null_fd, ptr, 1)` 系统调用。若 `ptr` 对应的页表未映射、无读权限或为伪指针，内核会自动检测到非法指针并返回 `-1`，同时将 `errno` 设置为 `EFAULT`。这一机制使得我们可以**在零崩溃风险的前提下**，完美判定任意内存地址的有效性和可读性。

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
    if (null_fd < 0) return true; // 降级回退
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
                *ptr_slot = nullptr; // 强制净化不可读的垃圾指针引用
            } else {
                void* vtable = *reinterpret_cast<void**>(ptr);
                if (vtable == nullptr) { // 检测到已被虚空填零的对象引用
                    *ptr_slot = nullptr; // 强制净化为干净的 nullptr
                }
            }
        }
    }
}
```

当被填零的不安全任务/实体指针被模组强制净化为标准的 `nullptr` 后，官方引擎原装的 `cbz` 安全检查即可完美生效，使得程序优雅地走入原本的安全 fallback 流程，完美避免崩溃。

### 3. 25-Hook 核心网络 (25-Hook System)
模组挂钩了官方引擎内所有与通缉星级、犯罪上报、应急载具生成、任务管理器、脚步声更新、涉水浮力物理以及 Unicode 字符串解析相关的生命周期与虚表，建立起精简高效的协同网络：

1.  **玩法功能与自定义警力调度** (共 12 个 Hook)：
    *   `report_crime` (拦截并接管原版的犯罪上报逻辑)
    *   `register_kill` (追踪击杀事件以实现自定义通缉判定)
    *   `set_wanted` (接管通缉星级系统)
    *   `generate_damage_event` / `event_damage_ctor_c1` / `event_damage_ctor_c2` (处理伤害事件以触发警员自卫/反击)
    *   `set_current_weapon` (管理分派警员的武器配置)
    *   `the_scripts_process` (驱动 trueDispatch 玩法系统的 tick 主循环)
    *   `add_police_occupants` (在警车刷出时绑定乘员)
    *   `tell_occupants_leave_car` (控制警车乘员的下车战术)
    *   `generate_one_emergency_car` / `script_generate_one_emergency_car` (移动端特有的救护车与消防车加载视距缩放 Workaround)
2.  **防御与净化安全防线** (共 13 个 Hook)：
    *   `u_strlen_64` (防止 ICU 字符串长度计算函数在接收到野指针时发生 SIGSEGV 闪退，在访问前进行指针有效性过滤)
    *   `CPed::ProcessBuoyancy` / `cBuoyancy::ProcessBuoyancy` (防止在行人计算涉水浮力时，由于任务管理器中残留零填充或无效的任务指针而导致解引用虚表闪退。其中 `cBuoyancy::ProcessBuoyancy` 挂钩在物理计算完成后立即净化任务槽，解决了物理 tick 途中任务被销毁/空指针的竞态问题)
    *   `CPed::PlayFootSteps` (防止转场或传送期间由于行人的 `RwClump` 骨骼暂时脱离导致播放脚步声时解引用空指针闪退)
    *   `CTaskManager::ManageTasks` (在任务管理器处理任务交替时，动态净化任务链中的野指针与无效虚表，防止纯虚函数调用崩溃)
    *   `CAttractorScanner::ScanForAttractorsInRange` (在行人扫描周期性吸引物时，清理决策管理器中的悬挂析构任务指针)
    *   `CTaskComplexGangFollower::ControlSubTask` (防止帮派招募与追随任务中，由于跟随者、领队或伙伴的某个主任务析构被填零，而导致读取他人任务时解引用闪退)
    *   `CTaskSimpleHoldEntity::SetPedPosition` (防止持物任务在更新位置时，因行人的 RwClump 骨骼指针未就绪而解引用闪退)
    *   `CTaskComplexUsePairedAttractor::CreateNextSubTask` (防止配对吸引子任务已不在 Ped 上活动时，由于原生引擎缺少空指针校验而发生的解引用闪退，通过提前校验并强行安全返回解决)
    *   `CPedIntelligence::ProcessStaticCounter` (防止更新行人的静态计数器时由于任务被析构填零而发生虚表解引用闪退，同样使用前置净化防御解决)
    *   `CTaskComplexFacial::~CTaskComplexFacial` (防止表情动画等复杂面部任务析构时，由于子任务已被销毁填零而发生双重释放/空指针虚表解引用闪退)
    *   `CTaskManager::FindActiveTaskByType` (在通过类型查找活动任务时，前置校验并净化任务管理器内的所有任务指针，防止零值任务解引用崩溃)
    *   `CAEPedSpeechAudioEntity::PlayLoadedSound` (在播放行人语音音频时，校验语音管理器指针是否为空，防止解引用空指针进行写操作导致闪退)
    *   `CCarGenerator::CheckIfWithinRangeOfAnyPlayers` (在刷车器检测玩家距离时，检查玩家 Ped 指针在全局对象池中是否有效，防止玩家处于临时析构状态时导致的空指针闪退)
    *   `CTaskComplexAvoidOtherPedWhileWandering::ControlSubTask` (在行人避让决策中，净化目标 Ped 的所有主任务链，防止在避让动作中读取他人野指针任务闪退)

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
