# Ship dependency inventory (baseline)

Date: 2026-07-15

## Runtime (linked into arm64-v8a.so)

| Dep | Source | Required |
|-----|--------|----------|
| libc / libm / libdl | NDK sysroot | Yes — normal cdylib |
| liblog | NDK / bionic | Yes — Zygisk logging |
| system dlopen/dlsym/dlclose (RTLD_NOLOAD) | bionic | Yes — resolve already-loaded libUE4 (see dispatch-engine `library.rs`) |
| Zygisk API | vendored `dispatch-zygisk/cpp/zygisk/` | Yes — module entry |

## Workspace crates (ship graph)

```
dispatch-zygisk (cdylib)
  └── dispatch-hook
        ├── dispatch-exec
        │     ├── dispatch-case
        │     ├── dispatch-core
        │     └── dispatch-engine
        └── dispatch-engine
```

Cargo workspace deps: anyhow, slotmap, thiserror, tracing(+subscriber), libc, cc (build).

## Explicitly NOT on ship path

| Item | Status |
|------|--------|
| dispatch-native crate | **Deleted** |
| third_party/xDL (if present) | Legacy C++ only |
| third_party/shadowhook* | Legacy C++ only (`build_in_container.sh`) |
| src/main/cpp libpolicemod.so | Legacy; not packed by `pack_module.sh` |
| Crash skip-orig gates (bits 11–16) | Removed from install.rs; mask `0x07ff` |

## Build-only

- NDK clang aarch64-linux-android
- proot-distro ubuntu-build (Termux host bootstrap)
