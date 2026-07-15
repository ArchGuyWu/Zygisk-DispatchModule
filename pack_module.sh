#!/data/data/com.termux/files/usr/bin/bash
#
# pack_module.sh — package the **Rust Zygisk** baseline into Magisk/KernelSU zip.
#
# Ship path (baseline):
#   1. bash rust/build_rust.sh
#   2. bash pack_module.sh
#   → Zygisk-PoliceDispatch.zip with zygisk/arm64-v8a.so
#
# Legacy C++ libpolicemod.so is NOT packaged. See docs/BASELINE.md.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
OUTPUT_DIR="$PROJECT_ROOT/build/module_output"
ZIP_NAME="Zygisk-PoliceDispatch.zip"
ZIP_PATH="$PROJECT_ROOT/$ZIP_NAME"
RUST_SO="$PROJECT_ROOT/build/rust/arm64-v8a/arm64-v8a.so"

echo "=== Zygisk-PoliceDispatch packer (Rust baseline) ==="
echo ""

if [ ! -f "$RUST_SO" ]; then
    echo "[!] $RUST_SO missing — building Rust ship binary..."
    bash "$PROJECT_ROOT/rust/build_rust.sh"
fi

if [ ! -f "$RUST_SO" ]; then
    echo "ERROR: Rust .so not found after build: $RUST_SO"
    exit 1
fi

if [ ! -f "$PROJECT_ROOT/module.prop" ]; then
    echo "ERROR: module.prop not found"
    exit 1
fi

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/zygisk"
cp "$PROJECT_ROOT/module.prop" "$OUTPUT_DIR/"
cp "$RUST_SO" "$OUTPUT_DIR/zygisk/arm64-v8a.so"
echo "[✓] module.prop"
echo "[✓] zygisk/arm64-v8a.so ← $RUST_SO"
ls -lh "$OUTPUT_DIR/zygisk/arm64-v8a.so"

rm -f "$ZIP_PATH"
if command -v zip >/dev/null 2>&1; then
    (cd "$OUTPUT_DIR" && zip -r "$ZIP_PATH" .) > /dev/null
    echo "[✓] $ZIP_NAME (zip)"
elif command -v python3 >/dev/null 2>&1; then
    python3 - "$OUTPUT_DIR" "$ZIP_PATH" <<'PY'
import pathlib, sys, zipfile
src, zip_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
    for p in sorted(src.rglob("*")):
        if p.is_file():
            zf.write(p, p.relative_to(src).as_posix())
print("[✓] packed via python3 zipfile")
PY
else
    echo "ERROR: need 'zip' or 'python3' to create $ZIP_NAME"
    exit 1
fi

if [ ! -f "$ZIP_PATH" ]; then
    echo "ERROR: failed to create $ZIP_PATH"
    exit 1
fi

echo ""
echo "=== Done ==="
echo "Module: $ZIP_PATH"
ls -lh "$ZIP_PATH"
echo "Install: Magisk/KernelSU → Modules → Install from storage → reboot"
