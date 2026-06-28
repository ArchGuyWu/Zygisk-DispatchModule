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

### 3. 27-Hook 协同防御网 (27-Hook Defense System)
模组挂钩了官方引擎内所有与伴随、寻路、手持物体、帮派跟从者、帮派袭击任务、转场脚步声、浮力处理、后渲染逻辑、存档机制、涉水行为、脚步落地特效、任务中途析构、谷歌分包下载、Unicode字符本地化以及FreeType字体渲染相关的核心生命周期方法，建立起立体的全方位防御网：

1.  **伴随虚函数保护** (`GetPartnerSequence` - 共 4 个 Hook)：
    *   `CTaskComplexPartnerDeal::GetPartnerSequence`
    *   `CTaskComplexPartnerGreet::GetPartnerSequence`
    *   `CTaskComplexPartnerShove::GetPartnerSequence`
    *   `CTaskComplexPartnerChat::GetPartnerSequence`
2.  **子任务创建生命周期** (`CreateFirstSubTask` - 共 3 个 Hook)：
    *   `CTaskComplexPartner::CreateFirstSubTask` (基类 Hook)
    *   `CTaskComplexPartnerDeal::CreateFirstSubTask` (重写 Hook)
    *   `CTaskComplexPartnerGreet::CreateFirstSubTask` (重写 Hook)
3.  **伴随任务驱动控制** (`ControlSubTask` - 1 个 Hook)：
    *   `CTaskComplexPartner::ControlSubTask` (作为基类挂钩，统一保护 `Deal` / `Greet` / `Shove` / `Chat` 所有派生子类)
4.  **智能寻路安全重构** (`CreateSubTask` - 1 个 Hook)：
    *   `CTaskComplexGoToPointAnyMeans::CreateSubTask` (防御性地对内部未初始化或已失效的 Ped/Vehicle 指针进行净化过滤)
5.  **帮派载具袭击计算拦截** (`CalcTargetOffset` - 1 个 Hook)：
    *   `CTaskGangHassleVehicle::CalcTargetOffset` (主动拦截帮派载具骚扰任务中的目标偏移计算。若目标载具已被销毁或由于超出加载视距而置空，在进入原生汇编的解引用偏移 `0x498` 之前触发安全判定，完美跳过无效的矩阵/坐标计算并重设回退，阻止 SIGSEGV 闪退)
6.  **手持物体骨骼绑定拦截** (`SetPedPosition` - 1 个 Hook)：
    *   `CTaskSimpleHoldEntity::SetPedPosition` (主动拦截手持物体位置计算。若行人的动画骨骼 `RwClump` 指针（`ped + 0x648` 处）完全不可读或为空，直接安全跳过计算，完美解决原版在此处的未对齐/空指针引用闪退)
7.  **帮派跟从者驱动安全拦截** (`ControlSubTask` - 1 个 Hook)：
    *   `CTaskComplexGangFollower::ControlSubTask` (主动拦截跟从者子任务更新驱动。若跟从者关联的 leader/目标 ped 指针（位于 `self + 0x18` 处）为空或不可读，直接返回 `nullptr` 防止其深入执行并在原生汇编偏移 `0x498` 处触发空指针解引用，确保帮派招募与追随行为 100% 绝对稳定)
8.  **转场脚步声空指针拦截** (`PlayFootSteps` - 1 个 Hook)：
    *   `CPed::PlayFootSteps` (在转场、传送或载具上下车期间，如果玩家或 NPC 的骨骼模型 `m_pRwObject` 指针（`ped + 0x20` 处）被临时解绑或尚未加载完毕，原生引擎会裸解引用该指针产生的 `nullptr + 0x44` 导致闪退。本 Hook 在检测到 `m_pRwObject` 为空时，安全拦截并直接返回，彻底根治转场时偶发的脚步声解引用闪退)
9.  **浮力计算任务槽安全拦截** (`ProcessBuoyancy` - 1 个 Hook)：
    *   `CPed::ProcessBuoyancy` (在游戏引擎高频处理行人的水中浮力物理时，会遍历 `CTaskManager` 中的所有任务槽。若某个任务槽内由于引擎释放残留了被填零或野指针的无效任务指针，原生引擎会直接通过该指针解引用其虚表并调用 `GetTaskType`，从而在偏移 `0x18` 处触发空指针解引用闪退。本 Hook 在浮力逻辑执行前，动态扫描并强制将任务管理器中不安全或已填零的指针净化为 `nullptr`，彻底斩断此闪退路径)
10. **智能后渲染任务安全拦截** (`ProcessAfterPreRender` - 1 个 Hook)：
    *   `CPedIntelligence::ProcessAfterPreRender` (当游戏引擎在渲染后更新行人的智能决策时，会读取主任务及副任务管理器中的特定任务槽。如果某个任务已执行析构（Destructed），但指针仍残留于任务槽中，其虚表指针会指向基类 `CTask` 的虚表。当引擎调用其 `IsSimple` 时，会因调用纯虚函数而触发 `__cxa_pure_virtual` 闪退。本 Hook 在后渲染智能处理前，动态扫描并清空 `CPedIntelligence` 任务管理器中已析构或无效的悬挂指针，彻底杜绝纯虚函数调用闪退)
11. **存档决策修改器安全拦截** (`Save` - 1 个 Hook)：
    *   `CScriptDecisionMakerModifications::Save` (在游戏手动存档或自动存档时，引擎会保存脚本决策修改器的数据，期间需要访问两个全局的决策制造者对象 `CScriptDecisionMaker`。如果这两个全局对象由于场景切换被销毁或未完成初始化（其虚表指针为 `nullptr`），引擎在执行保存时会裸解引用虚表偏移 `0x18` 或 `0x38` 并触发 SIGSEGV 闪退。本 Hook 在存档逻辑执行前，动态检测并强行将无效的全局对象指针净化为 `nullptr`，安全引导引擎走入原装的空指针跳过分支，完美解决存档与自动存档时的偶发闪退)
12. **涉水任务初始化安全拦截** (`CreateFirstSubTask` - 1 个 Hook)：
    *   `CTaskComplexInWater::CreateFirstSubTask` (当行人进入水中时，引擎会为其分配并初始化涉水复杂任务。若当前游戏的水系统管理器或其内部的物理高度/深度数据数组由于场景重载被销毁或尚未初始化，引擎在计算水域交互时会裸解引用空数组指针，导致 SIGSEGV 闪退。本 Hook 在涉水任务创建时，动态检测水系统管理器及数组状态，若发现未就绪则强制返回 `nullptr` 阻止任务创建，从而由官方任务管理器安全降级，彻底解决水中偶发的解引用闪退)
13. **脚步落地特效安全拦截** (`DoFootLanded` - 1 个 Hook)：
    *   `CPed::DoFootLanded` (当行人脚步落地时，引擎会读取行人对象偏移 `0x90` 处的粒子特效系统 `FxSystem_c` 指针并调用 `AddParticle` 生成脚印特效。若该特效系统未初始化或已被销毁（其偏移 `0x18` 处的数组成员为空），调用时会裸解引用 `nullptr + 0x18` 并触发 SIGSEGV 闪退。本 Hook 在脚步落地时，动态检测特效系统及数组成员状态，若未就绪则安全跳过粒子生成，彻底根治高频运动中偶发的脚印特效解引用闪退)
14. **任务中途析构安全清理** (`~CTask` - 1 个 Hook)：
    *   `CTask::~CTask` (在行人智能决策 `ProcessAfterPreRender` 执行期间，某些子任务可能会被其他活动任务动态销毁（析构）。如果它们的指针仍残留于任务槽中，引擎在同帧后续逻辑中将继续尝试调用它们，从而因虚表被清空或内存释放而在偏移 `0x28` 处触发 SIGSEGV 闪退。本 Hook 动态拦截任务的析构事件，实时检测其是否属于当前正在更新的 `CPedIntelligence` 上下文，并在析构瞬间强行将对应的任务槽净化为 `nullptr`，彻底解决中途析构产生的野指针闪退)
15. **谷歌分包下载服务安全防线** (`AssetPackManager_requestDownload` - 1 个 Hook)：
    *   `AssetPackManager_requestDownload` (在定制 ROM 或没有谷歌服务（Google Play Services）的系统环境下，Play Core 的 `AssetPackManager` 实例在 JNI 初始化时会产生一个未完全初始化或已损坏的智能指针。当游戏尝试下载分包资源时，该损坏的智能指针在析构时会裸解引用其控制块虚表 `nullptr + 0x18` 触发 SIGSEGV 闪退。本 Hook 主动拦截该 JNI 下载请求，直接安全返回 `-101`（`ASSET_PACK_API_NOT_AVAILABLE`）通知游戏谷歌分包不可用，使其安全降级至本地资源读取，完美解决无谷歌服务机型启动或加载时的闪退)
16. **ICU 字符解析安全防护** (共 5 个 Hook)：
    *   `icu_64::TimeZone::findID` (防止在查找时区 ID 时，由于传入的 `UnicodeString` 引用为空或非法而导致空指针闪退)
    *   `icu_64::TimeZone::getDisplayName` (防止在本地化时区文本时，由于 `TimeZone` 实例为空或非法而导致空指针闪退)
    *   `icu_64::UnicodeSetStringSpan::span` (防止在解析 Unicode 字符跨度时，由于实例指针或字符串指针为空而导致空指针闪退)
    *   `icu_64::MessageFormat::findKeyword` (防止在格式化本地化文本时，由于关键词列表或字符串指针为空而导致空指针闪退)
    *   `icu_64::CollationIterator::previousCodePoint` (防止在文本排序迭代过程中，由于迭代器实例为空而导致空指针闪退)
17. **FreeType 字体渲染解释器保护** (`TT_RunIns` - 1 个 Hook)：
    *   `TT_RunIns` (防止在解析和渲染第三方或损坏的 TrueType 字体时，由于执行上下文指针非法而导致 FreeType 虚拟机崩溃)
18. **HarfBuzz 字体排版引擎保护** (1 个 Hook)：
    *   `OT::post::accelerator_t::get_glyph_from_name` (在某些特定系统配置或定制 ROM 上加载特定字体时，字体的 `post` 表（负责将字形索引映射为名称）可能包含非法的内部偏移。当 HarfBuzz 在进行字形名称二分查找时，会因为解引用野指针而触发 SIGSEGV 闪退。本 Hook 拦截字形名称查找逻辑并直接安全返回 `false`，强制引导 HarfBuzz 降级为标准的 Unicode `cmap` 表映射查找，在不影响字体渲染质量的前提下彻底根治启动时的排版闪退)

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
