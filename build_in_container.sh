#!/bin/bash
# =============================================================
# build_in_container.sh
# 在 proot-distro Ubuntu 容器内运行
# 工作区挂载在 /workspace
# =============================================================
set -euo pipefail

# ANSI 终端色彩
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # 清除颜色

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
TOOLCHAIN_DIR="/opt/toolchain"
NDK_VERSION="r27c"
NDK_ZIP="android-ndk-${NDK_VERSION}-linux.zip"
NDK_URL="https://dl.google.com/android/repository/${NDK_ZIP}"
NDK_DIR="${TOOLCHAIN_DIR}/android-ndk-${NDK_VERSION}"

SHADOWHOOK_VERSION="2.0.1"
SHADOWHOOK_AAR="shadowhook-${SHADOWHOOK_VERSION}.aar"
SHADOWHOOK_URL="https://repo1.maven.org/maven2/com/bytedance/android/shadowhook/${SHADOWHOOK_VERSION}/shadowhook-${SHADOWHOOK_VERSION}.aar"
SHADOWHOOK_DIR="$PROJECT_DIR/third_party/shadowhook"

# 检测当前是否运行在 proot-distro 容器内 (Ubuntu)
IS_IN_CONTAINER=0
if [ -f /etc/os-release ] && grep -qi "ubuntu" /etc/os-release; then
    IS_IN_CONTAINER=1
fi

if [ "$IS_IN_CONTAINER" -eq 0 ]; then
    echo -e "${YELLOW}[INFO] Detecting running outside container (Termux Host).${NC}"
    echo -e "${YELLOW}[INFO] Auto-bootstrapping build environment inside proot-distro container (isolated mode)...${NC}"
    
    if ! command -v proot-distro &>/dev/null; then
        echo -e "${RED}❌ ERROR: proot-distro is not installed on Termux host!${NC}"
        echo -e "Please run: pkg install proot-distro"
        exit 1
    fi
    
    CONTAINER_NAME="ubuntu-build"
    WORKSPACE_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
    
    # 自动以 --isolated 隔离模式及绑定工作区执行自身（路径随仓库目录名自动解析）
    PROJECT_NAME="$(basename "$PROJECT_DIR")"
    exec proot-distro login --isolated --bind "$WORKSPACE_DIR:/workspace" "$CONTAINER_NAME" -- bash "/workspace/${PROJECT_NAME}/$(basename "$0")" "$@"
fi

# 允许通过环境变量定制编译架构，例如：ABIS="arm64-v8a armeabi-v7a" ./build_in_container.sh
if [ -n "${ABIS:-}" ]; then
    read -r -a TARGET_ABIS <<< "$ABIS"
else
    TARGET_ABIS=("arm64-v8a")
fi

echo -e "${CYAN}====================================================${NC}"
echo -e "${CYAN}       Zygisk-DispatchModule Build System           ${NC}"
echo -e "${CYAN}====================================================${NC}"
echo ""

# ----- 1. 安装系统依赖 -----
echo -e "${BLUE}[1/6] Setting up build dependencies...${NC}"
export DEBIAN_FRONTEND=noninteractive

NEEDS_INSTALL=0
for cmd in cmake make wget unzip zip curl; do
    if ! command -v "$cmd" &>/dev/null; then
        NEEDS_INSTALL=1
        break
    fi
done

if ! command -v ld.lld &>/dev/null; then
    NEEDS_INSTALL=1
fi

if [ "$NEEDS_INSTALL" -eq 1 ]; then
    echo -e "  ${YELLOW}Installing missing dependencies (cmake, make, wget, unzip, zip, curl, lld)...${NC}"
    apt-get update -qq
    apt-get install -y -qq cmake make wget unzip zip ca-certificates curl lld > /dev/null 2>&1
    echo -e "  ${GREEN}✓ Dependencies installed successfully${NC}"
else
    echo -e "  ${GREEN}✓ All dependencies already satisfied (skipped apt-get)${NC}"
fi

# ----- 2. 下载 Android NDK -----
echo -e "${BLUE}[2/6] Setting up Android NDK ${NDK_VERSION}...${NC}"
mkdir -p "$TOOLCHAIN_DIR"

if [ -d "$NDK_DIR" ]; then
    echo -e "  ${GREEN}✓ NDK already exists at $NDK_DIR${NC}"
else
    echo -e "  ${YELLOW}Downloading NDK (this may take a while)...${NC}"
    cd "$TOOLCHAIN_DIR"
    if [ ! -f "$NDK_ZIP" ]; then
        # 稳健下载，避免因中断保存损坏文件
        wget -q --show-progress "$NDK_URL" -O "${NDK_ZIP}.tmp"
        mv "${NDK_ZIP}.tmp" "$NDK_ZIP"
    fi
    echo -e "  ${YELLOW}Extracting NDK...${NC}"
    unzip -q -o "$NDK_ZIP"
    echo -e "  ${GREEN}✓ NDK extracted successfully${NC}"
    # 清理 zip 节省空间
    rm -f "$NDK_ZIP"
fi

CMAKE_TOOLCHAIN="$PROJECT_DIR/android-arm64-toolchain.cmake"
if [ ! -f "$CMAKE_TOOLCHAIN" ]; then
    echo -e "  ${RED}❌ ERROR: CMake toolchain not found at $CMAKE_TOOLCHAIN${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓ CMake toolchain ready: $CMAKE_TOOLCHAIN${NC}"

# ----- 3. 下载 ShadowHook 源码 -----
SHADOWHOOK_SRC_DIR="$PROJECT_DIR/third_party/shadowhook-src"
echo -e "${BLUE}[3/6] Setting up ShadowHook Source ${SHADOWHOOK_VERSION}...${NC}"
mkdir -p "$PROJECT_DIR/third_party"

if [ -d "$SHADOWHOOK_SRC_DIR/shadowhook/src/main/cpp" ]; then
    echo -e "  ${GREEN}✓ ShadowHook source already exists at $SHADOWHOOK_SRC_DIR${NC}"
else
    echo -e "  ${YELLOW}Downloading ShadowHook source zip...${NC}"
    SRC_ZIP="$PROJECT_DIR/third_party/shadowhook-src.zip"
    if [ ! -f "$SRC_ZIP" ]; then
        # 稳健下载模式
        curl -L -k -s -o "${SRC_ZIP}.tmp" "https://github.com/bytedance/android-inline-hook/archive/refs/tags/v${SHADOWHOOK_VERSION}.zip"
        mv "${SRC_ZIP}.tmp" "$SRC_ZIP"
    fi
    echo -e "  ${YELLOW}Extracting ShadowHook source...${NC}"
    cd "$PROJECT_DIR/third_party"
    unzip -q -o "shadowhook-src.zip"
    # 如果已存在目标文件夹，确保清干净防止嵌套移动
    rm -rf "$SHADOWHOOK_SRC_DIR"
    mv android-inline-hook-* "$SHADOWHOOK_SRC_DIR"
    rm -f "shadowhook-src.zip"
    echo -e "  ${GREEN}✓ ShadowHook source ready${NC}"
fi

# ----- 4. 生成独立 CMakeLists.txt -----
echo "[4/6] Generating standalone CMakeLists.txt..."
cat > "$PROJECT_DIR/src/main/cpp/CMakeLists.txt" << 'CMAKEFILE'
cmake_minimum_required(VERSION 3.18.1)
project(policemod)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
enable_language(ASM)

# Zygisk 与项目头文件
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
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
    dispatch_threat.cpp
    dispatch_ped_registry.cpp
    dispatch_cop_state.cpp
    dispatch_reroute.cpp
    dispatch_timing.cpp
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
    dispatch_emergency_services.cpp
    dispatch_vehicle_escaper.cpp
    dispatch_police_spawn.cpp
    dispatch_heli_support.cpp
    dispatch_hit_and_run.cpp
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
CMAKEFILE
echo "  ✅ CMakeLists.txt generated"

# ----- 5. 编译 -----
echo -e "${BLUE}[5/6] Compiling...${NC}"
for abi in "${TARGET_ABIS[@]}"; do
    echo -e "  ${YELLOW}Building for ${abi}...${NC}"
    BUILD_ABI_DIR="${BUILD_DIR}/${abi}"
    mkdir -p "$BUILD_ABI_DIR"

    cmake \
        -S "$PROJECT_DIR/src/main/cpp" \
        -B "$BUILD_ABI_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM=android-26 \
        -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release

    cmake --build "$BUILD_ABI_DIR" -- -j$(nproc) 2>&1

    if [ -f "$BUILD_ABI_DIR/libpolicemod.so" ]; then
        echo -e "  ${GREEN}✓ libpolicemod.so (${abi}) built successfully${NC}"
        ls -lh "$BUILD_ABI_DIR/libpolicemod.so"
    else
        echo -e "  ${RED}❌ Build failed for ${abi}${NC}"
        exit 1
    fi
done

# ----- 6. 打包 Magisk 模块 -----
echo -e "${BLUE}[6/6] Packaging Magisk module...${NC}"
OUTPUT_DIR="${BUILD_DIR}/module_output"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# 自动生成 META-INF 目录结构及标准文件
META_INF_DIR="$OUTPUT_DIR/META-INF/com/google/android"
mkdir -p "$META_INF_DIR"

# 1. 写入 updater-script
echo "#MAGISK" > "$META_INF_DIR/updater-script"

# 2. 写入 update-binary 脚本
cat > "$META_INF_DIR/update-binary" << 'UPDATEBINARY'
#!/sbin/sh
umask 022
ui_print() { echo "$1"; }
require_new_magisk() {
  ui_print "*******************************"
  ui_print " Please install Magisk v20.4+! "
  ui_print "*******************************"
  exit 1
}
OUTFD=$2
ZIPFILE=$3
mount /data 2>/dev/null
[ -f /data/adb/magisk/util_functions.sh ] || require_new_magisk
. /data/adb/magisk/util_functions.sh
[ $MAGISK_VER_CODE -lt 20400 ] && require_new_magisk
install_module
exit 0
UPDATEBINARY
chmod 755 "$META_INF_DIR/update-binary"

# 3. 复制 module.prop
cp "$PROJECT_DIR/module.prop" "$OUTPUT_DIR/"

# 4. 复制 Zygisk 动态库到正确的文件树中 (重命名为 zygisk/<abi>.so)
mkdir -p "$OUTPUT_DIR/zygisk"
for abi in "${TARGET_ABIS[@]}"; do
    cp "${BUILD_DIR}/${abi}/libpolicemod.so" "$OUTPUT_DIR/zygisk/${abi}.so"
    echo -e "  ${GREEN}Packed: zygisk/${abi}.so${NC}"
done

# 打包 zip
cd "$OUTPUT_DIR"
ZIP_PATH="${PROJECT_DIR}/Zygisk-PoliceDispatch.zip"
rm -f "$ZIP_PATH"
zip -r "$ZIP_PATH" . > /dev/null
cd "$PROJECT_DIR"

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}          BUILD COMPLETE                ${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "Output: ${CYAN}$ZIP_PATH${NC}"
ls -lh "$ZIP_PATH"
echo ""
echo -e "Install: ${YELLOW}adb push Zygisk-PoliceDispatch.zip /sdcard/${NC}"
echo -e "Then flash via Magisk/KernelSU app"
