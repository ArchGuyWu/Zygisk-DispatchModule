#!/usr/bin/env bash
# Build libdispatch_zygisk.so → build/rust/arm64-v8a/arm64-v8a.so
# Link flags aligned with C++ CMakeLists (exclude-libs + Bsymbolic; no version-script local:*).
set -euo pipefail

IS_IN_CONTAINER=0
if [ -f /etc/os-release ] && grep -qi "ubuntu" /etc/os-release; then
    IS_IN_CONTAINER=1
fi

if [ "$IS_IN_CONTAINER" -eq 0 ]; then
    PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
    WORKSPACE_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
    PROJECT_NAME="$(basename "$PROJECT_DIR")"

    if ! command -v proot-distro &>/dev/null; then
        echo "proot-distro not found — install in Termux or run inside ubuntu-build"
        exit 1
    fi

    exec proot-distro login --isolated --bind "$WORKSPACE_DIR:/workspace" ubuntu-build -- \
        bash "/workspace/${PROJECT_NAME}/rust/$(basename "$0")" "$@"
fi

if [ -f "$HOME/.cargo/env" ]; then
    # shellcheck disable=SC1091
    source "$HOME/.cargo/env"
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-/opt/toolchain/android-ndk-r29}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
ANDROID_API="${ANDROID_API:-28}"
export ANDROID_API
TARGET="aarch64-linux-android"
TRIPLE="${TARGET}${ANDROID_API}"
NDK_SYSROOT="$TOOLCHAIN/sysroot"
CLANG_VER="$(ls "$TOOLCHAIN/lib/clang" | sort -V | tail -1)"
CLANG_RT="$TOOLCHAIN/lib/clang/${CLANG_VER}/lib/linux"
NDK_API_LIB="$NDK_SYSROOT/usr/lib/aarch64-linux-android/${ANDROID_API}"

if [ ! -d "$NDK_SYSROOT" ]; then
    echo "NDK sysroot missing: $NDK_SYSROOT"
    exit 1
fi
if [ ! -d "$NDK_API_LIB" ]; then
    echo "NDK API lib dir missing: $NDK_API_LIB"
    exit 1
fi

if ! command -v cargo &>/dev/null; then
    echo "cargo not found"
    exit 1
fi

if ! rustup target list --installed | grep -q "${TARGET}"; then
    rustup target add "${TARGET}"
fi

COMMON="--target=${TRIPLE} --sysroot=${NDK_SYSROOT}"
export CC_aarch64_linux_android="clang"
export CXX_aarch64_linux_android="clang++"
export CFLAGS_aarch64_linux_android="${COMMON} -fPIC"
export CXXFLAGS_aarch64_linux_android="${COMMON} -fPIC -stdlib=libc++"
export AR_aarch64_linux_android="llvm-ar"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="clang++"

cd "$ROOT"
mkdir -p .cargo
# Match C++ policemod: -Wl,--exclude-libs,ALL -Wl,-Bsymbolic
# Do NOT use version-script local:* — differs from C++ and can break trampolines.
cat > .cargo/config.toml <<EOF
[target.aarch64-linux-android]
linker = "clang++"
rustflags = [
  "-C", "link-arg=--target=${TRIPLE}",
  "-C", "link-arg=--sysroot=${NDK_SYSROOT}",
  "-C", "link-arg=-L${NDK_API_LIB}",
  "-C", "link-arg=-L${CLANG_RT}/aarch64",
  "-C", "link-arg=-Wl,-Bdynamic",
  "-C", "link-arg=-lc",
  "-C", "link-arg=-ldl",
  "-C", "link-arg=-lm",
  "-C", "link-arg=-llog",
  "-C", "link-arg=-Wl,--exclude-libs,ALL",
  "-C", "link-arg=-Wl,-Bsymbolic",
  "-C", "link-arg=-Wl,--export-dynamic-symbol=zygisk_module_entry",
]
EOF

cargo build -p dispatch-zygisk --release --target "${TARGET}"

OUT="$ROOT/../build/rust/arm64-v8a"
mkdir -p "$OUT"
SO="$ROOT/target/${TARGET}/release/libdispatch_zygisk.so"
cp "$SO" "$OUT/arm64-v8a.so"

if command -v llvm-objcopy &>/dev/null; then
    llvm-objcopy --localize-symbol=dispatch_bootstrap_rust "$OUT/arm64-v8a.so" 2>/dev/null || true
fi
if command -v llvm-strip &>/dev/null; then
    llvm-strip --strip-unneeded "$OUT/arm64-v8a.so"
fi

echo "built: $OUT/arm64-v8a.so"

if command -v nm &>/dev/null && command -v readelf &>/dev/null; then
    echo "=== link check: $OUT/arm64-v8a.so ==="
    readelf -d "$OUT/arm64-v8a.so" | grep -E 'NEEDED|SONAME' || true
    if nm -D "$OUT/arm64-v8a.so" 2>/dev/null | grep -q ' U dlopen'; then
        echo "OK: dlopen is dynamic UND"
    elif nm "$OUT/arm64-v8a.so" 2>/dev/null | grep -q ' U dlopen$'; then
        echo "OK: dlopen is UND"
    else
        echo "WARN: check dlopen binding"
        nm "$OUT/arm64-v8a.so" 2>/dev/null | grep -E ' [TtUu] (dlopen|malloc)$' || true
    fi
fi
