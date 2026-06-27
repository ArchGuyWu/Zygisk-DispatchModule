# android-arm64-toolchain.cmake
# 系统 clang + NDK sysroot + 手动 CRT/runtime 链接

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)

set(NDK_ROOT "/opt/toolchain/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64")
set(NDK_SYSROOT "${NDK_ROOT}/sysroot")
set(NDK_LIBDIR "${NDK_SYSROOT}/usr/lib/aarch64-linux-android/26")
set(NDK_CLANG_RT "${NDK_ROOT}/lib/clang/18/lib/linux")

set(CMAKE_SYSROOT ${NDK_SYSROOT})

set(ANDROID_TARGET "aarch64-linux-android26")
set(CMAKE_C_COMPILER_TARGET ${ANDROID_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${ANDROID_TARGET})
set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
set(CMAKE_ASM_COMPILER_TARGET ${ANDROID_TARGET})

# 模拟 ANDROID_ABI
set(ANDROID_ABI "arm64-v8a")

# 编译标志
set(CMAKE_C_FLAGS_INIT "-fPIC -DANDROID -fdata-sections -ffunction-sections -funwind-tables -fstack-protector-strong -no-canonical-prefixes")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")

# 链接标志：禁用默认 stdlib，手动指定 NDK 的 CRT 和 runtime
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-fuse-ld=lld -nostdlib -Wl,--gc-sections -Wl,--build-id=sha1 -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,-Bsymbolic -L${NDK_LIBDIR} -L${NDK_CLANG_RT} ${NDK_LIBDIR}/crtbegin_so.o ${NDK_LIBDIR}/libc++.a -lc -lm -ldl -llog ${NDK_CLANG_RT}/libclang_rt.builtins-aarch64-android.a ${NDK_CLANG_RT}/aarch64/libunwind.a ${NDK_LIBDIR}/crtend_so.o"
)

# 为 CMake 编译器测试禁用链接（测试程序是可执行文件，不是 .so）
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH ${NDK_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
