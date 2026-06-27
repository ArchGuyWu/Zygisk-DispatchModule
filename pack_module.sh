#!/data/data/com.termux/files/usr/bin/bash
#
# pack_module.sh - 将编译好的 .so 和 module.prop 打包成 Zygisk 模块 Zip
#
# 用法:
#   1. 先在 Android Studio 中执行 Build -> Make Project，或使用 ./gradlew assembleRelease
#   2. 然后运行: bash pack_module.sh
#   3. 输出: Zygisk-PoliceDispatch.zip (可直接在 Magisk/KernelSU 中刷入)
#
# 注意: 脚本会在项目的 build/outputs/ 目录下寻找编译产物

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
OUTPUT_DIR="$PROJECT_ROOT/build/module_output"
ZIP_NAME="Zygisk-PoliceDispatch.zip"

# Gradle 编译输出的 .so 路径 (默认位置)
CMAKE_OUTPUT_BASE="$PROJECT_ROOT/build/intermediates/cmake/release/obj"

echo "=== Zygisk-DispatchModule Module Packer ==="
echo ""

# 清理旧的输出
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/zygisk"

# 复制 module.prop
if [ ! -f "$PROJECT_ROOT/module.prop" ]; then
    echo "ERROR: module.prop not found at $PROJECT_ROOT/module.prop"
    exit 1
fi
cp "$PROJECT_ROOT/module.prop" "$OUTPUT_DIR/"
echo "[✓] module.prop copied"

# 复制各架构的 .so 文件
# Zygisk 要求 .so 文件按架构命名：arm64-v8a.so, armeabi-v7a.so 等
ABIS=("arm64-v8a" "armeabi-v7a")
SO_FOUND=0

for abi in "${ABIS[@]}"; do
    SO_PATH="$CMAKE_OUTPUT_BASE/$abi/libpolicemod.so"
    # 如果 Gradle 默认路径不存在，则尝试从 standalone CMake 编译路径获取
    if [ ! -f "$SO_PATH" ]; then
        SO_PATH="$PROJECT_ROOT/build/$abi/libpolicemod.so"
    fi

    if [ -f "$SO_PATH" ]; then
        cp "$SO_PATH" "$OUTPUT_DIR/zygisk/${abi}.so"
        echo "[✓] ${abi}.so copied"
        SO_FOUND=$((SO_FOUND + 1))
    else
        echo "[!] WARNING: $SO_PATH not found, skipping $abi"
    fi
done

if [ "$SO_FOUND" -eq 0 ]; then
    echo ""
    echo "ERROR: No compiled .so files found!"
    echo "Please build the project first:"
    echo "  cd $PROJECT_ROOT && ./gradlew assembleRelease"
    echo ""
    echo "Or specify a custom path if your build output is elsewhere."
    exit 1
fi

# 打包 zip
cd "$OUTPUT_DIR"
zip -r "$PROJECT_ROOT/$ZIP_NAME" . > /dev/null
cd "$PROJECT_ROOT"

echo ""
echo "=== Done! ==="
echo "Module packaged: $PROJECT_ROOT/$ZIP_NAME"
echo ""
echo "Installation:"
echo "  1. Transfer $ZIP_NAME to your phone"
echo "  2. Open Magisk/KernelSU app"
echo "  3. Go to Modules -> Install from storage"
echo "  4. Select $ZIP_NAME and reboot"
