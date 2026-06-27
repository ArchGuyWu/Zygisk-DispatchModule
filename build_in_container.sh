#!/bin/bash
# =============================================================
# build_in_container.sh
# 在 proot-distro Ubuntu 容器内运行
# 工作区挂载在 /workspace
# =============================================================
set -euo pipefail

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

TARGET_ABIS=("arm64-v8a")
# 如需 32 位支持，取消注释：
# TARGET_ABIS=("arm64-v8a" "armeabi-v7a")

echo "========================================"
echo "  Zygisk-DispatchModule Build System"
echo "========================================"
echo ""

# ----- 1. 安装系统依赖 -----
echo "[1/6] Installing build dependencies..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq cmake make wget unzip zip ca-certificates curl > /dev/null 2>&1
echo "  ✅ Dependencies installed"

# ----- 2. 下载 Android NDK -----
echo "[2/6] Setting up Android NDK ${NDK_VERSION}..."
mkdir -p "$TOOLCHAIN_DIR"

if [ -d "$NDK_DIR" ]; then
    echo "  ✅ NDK already exists at $NDK_DIR"
else
    echo "  Downloading NDK (this may take a while)..."
    cd "$TOOLCHAIN_DIR"
    if [ ! -f "$NDK_ZIP" ]; then
        wget -q --show-progress "$NDK_URL" -O "$NDK_ZIP"
    fi
    echo "  Extracting NDK..."
    unzip -q -o "$NDK_ZIP"
    echo "  ✅ NDK extracted"
    # 清理 zip 节省空间
    rm -f "$NDK_ZIP"
fi

CMAKE_TOOLCHAIN="$PROJECT_DIR/android-arm64-toolchain.cmake"
if [ ! -f "$CMAKE_TOOLCHAIN" ]; then
    echo "  ❌ ERROR: CMake toolchain not found at $CMAKE_TOOLCHAIN"
    exit 1
fi
echo "  ✅ CMake toolchain: $CMAKE_TOOLCHAIN"

# ----- 3. 下载 ShadowHook 源码 -----
SHADOWHOOK_SRC_DIR="$PROJECT_DIR/third_party/shadowhook-src"
echo "[3/6] Setting up ShadowHook Source ${SHADOWHOOK_VERSION}..."
mkdir -p "$PROJECT_DIR/third_party"

if [ -d "$SHADOWHOOK_SRC_DIR/shadowhook/src/main/cpp" ]; then
    echo "  ✅ ShadowHook source already exists at $SHADOWHOOK_SRC_DIR"
else
    echo "  Downloading ShadowHook source zip..."
    SRC_ZIP="$PROJECT_DIR/third_party/shadowhook-src.zip"
    if [ ! -f "$SRC_ZIP" ]; then
        curl -L -k -s -o "$SRC_ZIP" "https://github.com/bytedance/android-inline-hook/archive/refs/tags/v${SHADOWHOOK_VERSION}.zip"
    fi
    echo "  Extracting ShadowHook source..."
    cd "$PROJECT_DIR/third_party"
    unzip -q -o "shadowhook-src.zip"
    mv android-inline-hook-* "$SHADOWHOOK_SRC_DIR"
    rm -f "$SRC_ZIP"
    echo "  ✅ ShadowHook source ready"
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

# Zygisk 头文件
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/zygisk)

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
add_library(policemod SHARED module.cpp)
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
echo "[5/6] Compiling..."
for abi in "${TARGET_ABIS[@]}"; do
    echo "  Building for ${abi}..."
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
        echo "  ✅ libpolicemod.so (${abi}) built successfully"
        ls -lh "$BUILD_ABI_DIR/libpolicemod.so"
    else
        echo "  ❌ Build failed for ${abi}"
        exit 1
    fi
done

# ----- 6. 打包 Magisk 模块 -----
echo "[6/6] Packaging Magisk module..."
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
    echo "  Packed: zygisk/${abi}.so"
done

# 打包 zip
cd "$OUTPUT_DIR"
ZIP_PATH="${PROJECT_DIR}/Zygisk-PoliceDispatch.zip"
rm -f "$ZIP_PATH"
zip -r "$ZIP_PATH" . > /dev/null
cd "$PROJECT_DIR"

echo ""
echo "========================================"
echo "  ✅ BUILD COMPLETE"
echo "========================================"
echo ""
echo "Output: $ZIP_PATH"
ls -lh "$ZIP_PATH"
echo ""
echo "Install: adb push Zygisk-PoliceDispatch.zip /sdcard/"
echo "Then flash via Magisk/KernelSU app"
