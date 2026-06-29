cmake_minimum_required(VERSION 3.18.1)
project(policemod)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
enable_language(ASM)

# Zygisk 与项目头文件
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/zygisk)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ShadowHook 源码路径
set(SHADOWHOOK_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/shadowhook-src")
set(SHADOWHOOK_CPP_DIR "${SHADOWHOOK_SRC_DIR}/shadowhook/src/main/cpp")

# 确定架构名称 (arm64 或 arm)
if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(ARCH "arm64")
elseif(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(ARCH "arm")
endif()

# ShadowHook 头文件目录
include_directories(${SHADOWHOOK_CPP_DIR})
include_directories(${SHADOWHOOK_CPP_DIR}/include)

# 收集 ShadowHook 所有源文件并定义为静态库
file(GLOB SHADOWHOOK_SRC
    "${SHADOWHOOK_CPP_DIR}/*.c"
    "${SHADOWHOOK_CPP_DIR}/arch/${ARCH}/*.c"
    "${SHADOWHOOK_CPP_DIR}/arch/${ARCH}/*.S"
    "${SHADOWHOOK_CPP_DIR}/common/*.c"
    "${SHADOWHOOK_CPP_DIR}/third_party/xdl/*.c"
    "${SHADOWHOOK_CPP_DIR}/third_party/bsd/*.c"
    "${SHADOWHOOK_CPP_DIR}/third_party/lss/*.c"
)

add_library(shadowhook STATIC ${SHADOWHOOK_SRC})
target_compile_features(shadowhook PRIVATE c_std_11)
target_include_directories(shadowhook PRIVATE
    ${SHADOWHOOK_CPP_DIR}
    ${SHADOWHOOK_CPP_DIR}/include
    ${SHADOWHOOK_CPP_DIR}/arch/${ARCH}
    ${SHADOWHOOK_CPP_DIR}/common
    ${SHADOWHOOK_CPP_DIR}/third_party/xdl
    ${SHADOWHOOK_CPP_DIR}/third_party/bsd
    ${SHADOWHOOK_CPP_DIR}/third_party/lss
)
target_link_libraries(shadowhook log)

# 编译 libshadowhook_nothing.so 伴生库
add_library(shadowhook_nothing SHARED ${SHADOWHOOK_CPP_DIR}/nothing/sh_nothing.c)

# 编译我们的模块并静态链接 shadowhook
add_library(policemod SHARED
    module.cpp
    hook_install.cpp
    dispatch_logic.cpp
    dispatch_hooks.cpp
    dispatch_cop_response.cpp
    dispatch_cop_attack.cpp
    dispatch_cop_attack_state.cpp
    dispatch_cop_attack_pass.cpp
    dispatch_cop_attack_pass_vehicle.cpp
    dispatch_cop_attack_vehicle_stuck.cpp
    dispatch_cop_attack_vehicle_unstuck.cpp
    dispatch_cop_attack_vehicle_order.cpp
    dispatch_cop_attack_pass_foot.cpp
    dispatch_tick.cpp
    dispatch_tick_main.cpp
    dispatch_tick_civilian.cpp
    dispatch_tick_states.cpp
    dispatch_tick_state_idle.cpp
    dispatch_tick_state_timing.cpp
    dispatch_tick_state_on_scene.cpp
    dispatch_tick_state_cleanup.cpp
    dispatch_emergency.cpp
    dispatch_spawn_hooks.cpp
    hooks_stability.cpp
    pointer_sanitizer_vma.cpp
    ecs_systems.cpp
)
target_link_libraries(policemod
    PRIVATE
    log
    shadowhook
)
target_compile_options(policemod PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)
target_link_options(policemod PRIVATE
    -Wl,--exclude-libs,ALL
    -Wl,-Bsymbolic
)
