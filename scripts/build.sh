#!/usr/bin/env bash
# build.sh — One-shot build script for NovaOS Phase 1
#
# This script detects the available tools, configures CMake, and builds
# the bootloader and kernel.
#
# Usage: ./scripts/build.sh [options]
#   --clean       Clean the build directory before building
#   --debug       Enable debug symbols and verbose output (default: on)
#   --release     Optimize for speed (disables debug symbols)
#   --jobs N      Parallel build jobs (default: nproc)
#   --no-disk     Skip disk image creation
#   --verbose     CMake verbose output

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

# ---- Defaults ----
CLEAN=0
BUILD_TYPE="Debug"
JOBS=$(nproc 2>/dev/null || echo 4)
NO_DISK=0
VERBOSE=""

# ---- Parse arguments ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)    CLEAN=1 ;;
        --debug)    BUILD_TYPE="Debug" ;;
        --release)  BUILD_TYPE="Release" ;;
        --jobs)     JOBS="$2"; shift ;;
        --no-disk)  NO_DISK=1 ;;
        --verbose)  VERBOSE="--verbose" ;;
        -h|--help)
            echo "Usage: $0 [--clean] [--debug|--release] [--jobs N] [--no-disk]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
    shift
done

# ---- Banner ----
echo "================================================================"
echo "  NovaOS Phase 1 Build"
echo "  Build type: $BUILD_TYPE"
echo "  Build dir:  $BUILD_DIR"
echo "================================================================"
echo ""

# ---- Check prerequisites ----
check_tool() {
    local name="$1"
    local pkg="$2"
    if ! command -v "$name" &>/dev/null; then
        echo "MISSING: $name (install: sudo apt install $pkg)"
        return 1
    fi
    echo "  OK: $name ($(command -v "$name"))"
    return 0
}

echo "Checking prerequisites..."
MISSING=0

check_tool cmake    "cmake"          || MISSING=1
check_tool nasm     "nasm"           || MISSING=1

# Kernel compiler
if command -v x86_64-elf-gcc &>/dev/null; then
    echo "  OK: x86_64-elf-gcc (cross-compiler)"
elif command -v clang &>/dev/null; then
    echo "  OK: clang (will use as kernel compiler)"
else
    echo "MISSING: x86_64-elf-gcc or clang"
    echo "  Install cross-compiler: see docs/building.md"
    echo "  OR install clang: sudo apt install clang"
    MISSING=1
fi

# Bootloader compiler
if command -v x86_64-w64-mingw32-gcc &>/dev/null; then
    echo "  OK: x86_64-w64-mingw32-gcc (MinGW bootloader)"
elif command -v clang &>/dev/null && command -v lld-link &>/dev/null; then
    echo "  OK: clang+lld-link (LLVM bootloader)"
else
    echo "MISSING: MinGW-w64 or clang+lld-link for bootloader"
    echo "  Install: sudo apt install gcc-mingw-w64-x86-64"
    echo "        OR: sudo apt install clang lld"
    MISSING=1
fi

if [ $MISSING -ne 0 ]; then
    echo ""
    echo "Some prerequisites are missing. Please install them and retry."
    exit 1
fi
echo ""

# ---- Optional tools ----
echo "Optional tools:"
command -v qemu-system-x86_64 &>/dev/null && echo "  OK: qemu-system-x86_64" \
    || echo "  SKIP: qemu-system-x86_64 (install for running: sudo apt install qemu-system-x86)"
command -v mtools &>/dev/null && echo "  OK: mtools" \
    || echo "  SKIP: mtools (install for disk image: sudo apt install mtools dosfstools)"
echo ""

# ---- Clean ----
if [ $CLEAN -eq 1 ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# ---- Configure ----
echo "Configuring with CMake..."
cmake "$ROOT_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DNOVA_BUILD_TYPE="$BUILD_TYPE" \
    -G "Unix Makefiles" \
    2>&1 | grep -v "^--" || true

echo ""

# ---- Build ----
BUILD_TARGETS="kernel_build bootloader_build"
if [ $NO_DISK -eq 0 ] && command -v mtools &>/dev/null && command -v mkdosfs &>/dev/null; then
    BUILD_TARGETS="$BUILD_TARGETS disk_image"
fi

echo "Building: $BUILD_TARGETS"
make -j"$JOBS" $VERBOSE $BUILD_TARGETS

# ---- Results ----
echo ""
echo "================================================================"
echo "  Build complete!"
echo "================================================================"

if [ -f "$BUILD_DIR/kernel.elf" ]; then
    KERNEL_SIZE=$(du -h "$BUILD_DIR/kernel.elf" | cut -f1)
    echo "  Kernel:     $BUILD_DIR/kernel.elf ($KERNEL_SIZE)"
fi

if [ -f "$BUILD_DIR/bootx64.efi" ]; then
    BL_SIZE=$(du -h "$BUILD_DIR/bootx64.efi" | cut -f1)
    echo "  Bootloader: $BUILD_DIR/bootx64.efi ($BL_SIZE)"
fi

if [ -f "$BUILD_DIR/nova.img" ]; then
    IMG_SIZE=$(du -h "$BUILD_DIR/nova.img" | cut -f1)
    echo "  Disk image: $BUILD_DIR/nova.img ($IMG_SIZE)"
    echo ""
    echo "  Run: $SCRIPT_DIR/run-qemu.sh $BUILD_DIR/nova.img"
fi

echo ""
