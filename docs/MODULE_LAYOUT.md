# Module Layout

GTA SA DE Android 警力派发 Zygisk 模组源码结构说明。

## 目录树

```text
mod-workspace/
├── src/main/cpp/
│   ├── include/
│   │   ├── log.hpp / game_config.hpp / game_types.hpp / pointer_sanitizer.hpp
│   │   ├── dispatch_types.hpp   # CrimeEvent、CopVehicleBinding
│   │   ├── hooks_stability_types.hpp  # 稳定性 Hook 回调 typedef
│   │   └── mod_shared.hpp       # 跨文件 extern 全局量与共享 API
│   ├── zygisk/
│   ├── ecs_engine.hpp
│   ├── module.cpp               # 全局符号、辅助函数、Zygisk (~750 行)
│   ├── hook_install.cpp         # dlsym 解析、33-Hook 安装、vtable 修补 (~630 行)
│   ├── dispatch_logic.cpp       # 案件状态、决策引擎、并案 (~400 行)
│   ├── dispatch_hooks.cpp       # 犯罪/伤害/通缉 Hook 代理 (~540 行)
│   ├── dispatch_cop_response.cpp # 警车绑定、武器判定、cop 入场 (~800 行)
│   ├── dispatch_cop_attack.cpp   # make_cops_attack_criminal 编排 (~1476 行)
│   ├── dispatch_tick.cpp        # 主线程 tick、平民避让、案件 GC (~1k 行)
│   ├── dispatch_emergency.cpp   # 急救/消防脱困导航 (~500 行)
│   ├── dispatch_spawn_hooks.cpp # 通缉刷车拦截、应急车距离修正 (~250 行)
│   ├── hooks_stability.cpp      # 稳定性防御 Hook (~810 行)
│   ├── ecs_systems.cpp          # init_ecs_systems() (~530 行)
│   └── CMakeLists.txt
├── third_party/
│   └── shadowhook-src/          # ShadowHook 源码（构建时下载）
├── docs/
│   ├── CRASH_STATUS.md          # 崩溃样本与修复状态
│   └── MODULE_LAYOUT.md         # 本文件
├── build_in_container.sh        # Termux/proot 隔离构建（推荐）
├── auto_commit_push.sh          # 提交前编译 + push（SKIP_BUILD=1 可跳过）
├── pack_module.sh
├── module.prop
└── android-arm64-toolchain.cmake
```

## `module.cpp` 逻辑分区（按注释块）

| 区块 | 内容 |
|------|------|
| 全局符号 / `dlsym` | 运行时解析的 `g_*` 函数指针与池指针 |
| Hook 存根 | `g_stub_*` + proxy 函数 |
| 辅助校验 | `is_ped_pointer_valid_safe`, `is_task_vtable_safe`, pool 遍历 |
| 派发逻辑 | 犯罪上报、警车调度、通缉、增援 |
| 车辆战术 | 动能重置、ACC、防侧滑、路径 |
| 稳定性 Hook | 任务管理、浮力、ICU 字符串等 ~30+ 防御性挂钩 |
| ECS 系统 | `init_ecs_systems()` 内各 System lambda |
| Zygisk 入口 | `PoliceModModule` + `REGISTER_ZYGISK_MODULE` |

拆分结构：`module.cpp`（装配入口）、`dispatch_*.cpp`（派发玩法四文件）、`hooks_stability.cpp`（稳定性 Hook）、`ecs_systems.cpp`（ECS 注册）。

## 构建产物

- `build/<abi>/libpolicemod.so`
- `Zygisk-PoliceDispatch.zip`（Magisk/KernelSU 可刷）

## 相关脚本行为

| 脚本 | 行为 |
|------|------|
| `build_in_container.sh` | 宿主机自动 `proot-distro login`，容器内拉 NDK + ShadowHook 并编译 |
| `auto_commit_push.sh` | 默认先 `build_in_container.sh`，成功后再 `git commit` + `push` |