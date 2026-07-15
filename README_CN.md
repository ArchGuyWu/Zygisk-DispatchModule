# Zygisk-DispatchModule (Zygisk 派发模组)

[![Platform](https://img.shields.io/badge/Platform-Android-green.svg)](https://developer.android.com/)
[![Zygisk](https://img.shields.io/badge/Framework-Zygisk-blue.svg)](https://github.com/topjohnwu/Magisk)
[![ShadowHook](https://img.shields.io/badge/Hook_Library-ShadowHook-orange.svg)](https://github.com/bytedance/android-inline-hook)
[![License](https://img.shields.io/badge/License-MIT-red.svg)](LICENSE)

[English Version](README.md) | **中文版本**

面向 **侠盗猎车手：圣安地列斯 终极版 (GTA: SA DE Android)** 的 **应急调度** Zygisk 模组（警察 / 救护 / 消防；警力逻辑占比最大，但并非仅限警方）。**当前交付基线为 Rust** `arm64-v8a.so`（仅业务 hooks）。

> **权威基线文档：** [`docs/BASELINE.md`](docs/BASELINE.md) — 单一打包路径、保留依赖、hook bits 0–10。  
> **不要**再把 C++ `libpolicemod.so` 当作当前基线安装产物。

目击报案开案并计算**部门需求**；按需出动执法、救护与消防，**不包含** fail-closed 的「跳过引擎 orig」崩溃门控。

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
*   **轻量 ECS 调度层**：
    使用 header-only ECS 管理警员/载具组件与每帧系统，与原生引擎指针对接；存在正常的 map 查找与锁开销，并非「零成本」抽象。
*   **911 / 对讲机真实调度**：
    市民走 911 占线排队链路（`[911Busy]` / `[911Connect]` / `[911Transfer]` / `[911Revoke]`）；警员/救护/消防走电台；玩家纵火/伤人不会自报 EMS/火警。详见 [`docs/911_DISPATCH.md`](docs/911_DISPATCH.md)。
*   **感知 v2（AV vs 确认威胁）**：
    听/看分离反应延迟；仅 `CONFIRMED` 或活跃威胁立即追击，普通目击先调查再 mobilize。

---

## 🛡️ 官方引擎漏洞修复与防闪退深层剖析

在游戏原版（GTA SA DE Android）中，玩家在与警方、行人、帮派或伴随任务交互时，可能遇到随机 SIGSEGV。本模组通过逆向定位部分高频崩溃路径，并以 **41 处 ShadowHook 挂钩 + 指针可读性校验** 降低（而非保证消除）相关闪退概率。详见 [`docs/CRASH_STATUS.md`](docs/CRASH_STATUS.md)。

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

为实现更稳妥的指针判定，本模组在关键路径使用 **POSIX pipe 可读性探测**（`write(pipe_fd, ptr, 1)`，非法页返回 `EFAULT`）。该手段可降低误读野指针的概率，但**不能**保证调用方后续逻辑绝对安全。

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

// 注意：盲扫版 sanitize_task_pointers 已在源码中禁用（见 module.cpp 注释）。
// 当前策略：在 Hook 入口做 is_pointer_readable / vtable 校验，而非全对象内存扫描。
```

### 3. Hook 网络概览（41 处，2026-06-29 统计）

模组挂钩了与通缉、犯罪上报、应急载具、任务管理、脚步声、浮力、ICU 字符串等相关的多处符号。完整列表见 `rg shadowhook_hook_sym_name src/main/cpp/hook_install.cpp`。按职责大致分为：

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
2.  **稳定性防御 Hook**（约 29 处，与玩法 Hook 有重叠统计）：
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
    *   `CTaskComplexSequence::Flush` (防止清除任务序列时，由于序列中包含已被释放填零的任务而导致解引用虚表闪退)
    *   `CTaskSimpleEvasiveStep::FinishAnimEvasiveStepCB` (防止闪避动作回调执行时，其关联的任务上下文指针已被提前析构填零导致解引用闪退)
    *   `CTaskComplexBeInGroup::ControlSubTask` / `CPedGroupIntelligence::GetTaskMain` (组任务控制时，若 `GetTaskMain` 返回已析构填零的组任务，原版会在 `vtable+0x28` 处解引用闪退；模组在入口净化任务槽，并对 `GetTaskMain` 返回值做虚表校验，不安全时返回 `nullptr` 走原版空值分支)
    *   `IKChainManager_c::Update` (防止反向动力学链管理器在场景过渡置空时，解引用更新导致的空指针闪退)
    *   `CCam::Process_FollowPed_SA` (防止相机在过渡期失效置空时，解引用跟随角色导致的空指针闪退)
    *   `CTaskComplexLeaveCar::MakeAbortable` (防止强行中断下车动作时，其内部的子任务指针 `m_pSubTask` 为空导致虚表解引用闪退)
    *   `CCarAI::UpdateCarAI` (防止车辆 AI 路线决策更新时，传入已被析构填零的车辆对象引发的解引用闪退)
    *   `CTaskComplexFacial::ControlSubTask` (防止面部动画任务更新时，其内部的子任务指针 `m_pSubTask` 为空导致虚表解引用闪退；子任务槽净化后为空则跳过原版调用)
    *   `CCarEnterExit::GetNearestCarDoor` (上车门检测遍历行人任务槽并调用 `vtable+0x28` 时，若任务已被填零会触发 `Pure virtual function called!`；入口净化行人任务槽)

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
mod-workspace/
├── src/main/cpp/
│   ├── include/                 # log / game_config / game_types / pointer_sanitizer
│   ├── zygisk/                  # Zygisk 接口头文件
│   ├── ecs_engine.hpp           # 轻量 ECS 及事件总线
│   └── module.cpp               # 主逻辑（Hook + 派发 + ECS 装配）
├── docs/
├── rust/                        # **交付**源码（dispatch-* crates + build_rust.sh）
├── docs/
│   ├── BASELINE.md              # **权威**交付路径 / 依赖 / hooks
│   ├── 911_DISPATCH.md
│   ├── CRASH_STATUS.md          # 已废弃（legacy C++ 崩溃笔记）
│   └── MODULE_LAYOUT.md         # 已废弃（legacy C++ 目录树）
├── third_party/                 # 仅 legacy C++（ShadowHook）— 不链入交付 .so
├── build_in_container.sh        # LEGACY C++ libpolicemod（不写交付 zip 名）
├── pack_module.sh               # 交付打包 → Zygisk-PoliceDispatch.zip
├── module.prop                  # Magisk 属性（v2.0.0-baseline）
└── build.gradle                 # 可选 Android Studio / 旧工具链
```

---

## 🏗️ 如何编译（基线交付路径）

**唯一可安装产物：Rust Zygisk 模组。** 详见 [`docs/BASELINE.md`](docs/BASELINE.md)。

### 交付路径（必用）

1. 准备好 `proot-distro` + `ubuntu-build`（或等价 NDK 环境）。Termux 下 `rust/build_rust.sh` 会在需要时自动进入容器。
2. 编译交付二进制：
   ```bash
   bash rust/build_rust.sh
   # → build/rust/arm64-v8a/arm64-v8a.so
   ```
3. 打包 Magisk/KernelSU zip：
   ```bash
   bash pack_module.sh
   # → Zygisk-PoliceDispatch.zip  (module.prop + zygisk/arm64-v8a.so)
   ```

可选纯逻辑单测（无需 libUE4）：

```bash
cd rust && cargo test -p dispatch-core -p dispatch-exec -p dispatch-case --lib
```

### 遗留 C++ 树（非交付路径）

`src/main/cpp` 与 `build_in_container.sh` 仅作历史参考。该路径生成基于 ShadowHook 的 `libpolicemod.so`；若完整跑完 legacy 构建，只会写出**不同文件名**的 zip（`Zygisk-PoliceDispatch-LEGACY-cpp.zip`），**绝不会**覆盖交付用的 `Zygisk-PoliceDispatch.zip`。请使用 `bash rust/build_rust.sh && bash pack_module.sh`。

---

## 📲 安装与使用

1. 将上述 **Rust 交付路径** 生成的 **`Zygisk-PoliceDispatch.zip`** 传到设备。
2. 打开 **Magisk** 或 **KernelSU** 应用程序。
3. 进入 **模块 (Modules)** -> **从本地安装 (Install from storage)**。
4. 选择并安装 `Zygisk-PoliceDispatch.zip`，然后重启设备。
5. 运行游戏即可，模组会在游戏启动时自动注入并接管派发逻辑。

---

## 📜 开源协议

本项目基于 [MIT License](LICENSE) 开源，欢迎自由学习、修改和分发，使用时请保留原作者署名。
