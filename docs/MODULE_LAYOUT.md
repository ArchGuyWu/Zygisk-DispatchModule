# Module Layout

GTA SA DE Android 警力派发 Zygisk 模组源码结构说明。

## 目录树

```text
mod-workspace/
├── src/main/cpp/
│   ├── include/                 # 从 module.cpp 拆出的可复用头文件
│   │   ├── log.hpp              # Android log 宏
│   │   ├── game_config.hpp      # 包名、PedType、模型 ID
│   │   ├── game_types.hpp       # 引擎结构体、枚举、函数指针 typedef
│   │   └── pointer_sanitizer.hpp# pipe 可读性检测 (is_pointer_readable)
│   ├── zygisk/                  # Zygisk API 头文件
│   ├── ecs_engine.hpp           # 轻量 ECS（警员/载具组件与系统）
│   ├── module.cpp               # 主逻辑：Hook、派发、ECS 系统注册 (~7k 行)
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

后续拆分建议（未实施）：将「稳定性 Hook」与「派发玩法」分别迁入 `hooks_stability.cpp` / `dispatch_logic.cpp`，保持 `module.cpp` 仅作装配入口。

## 构建产物

- `build/<abi>/libpolicemod.so`
- `Zygisk-PoliceDispatch.zip`（Magisk/KernelSU 可刷）

## 相关脚本行为

| 脚本 | 行为 |
|------|------|
| `build_in_container.sh` | 宿主机自动 `proot-distro login`，容器内拉 NDK + ShadowHook 并编译 |
| `auto_commit_push.sh` | 默认先 `build_in_container.sh`，成功后再 `git commit` + `push` |