#!/usr/bin/env bash
# Shared runner dependencies. No toolchain on jobs that only consume an image.
set -euo pipefail
profile=${1:-runtime}
base=(qemu-system-x86 qemu-system-gui ovmf mtools dosfstools acpica-tools
      pulseaudio pulseaudio-utils python3-pil ntfs-3g curl p7zip-full)
case "$profile" in
    build) base+=(cmake make nasm clang lld llvm libc++-dev gcc-mingw-w64-x86-64 xorriso ccache) ;;
    graphics) base+=(gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 g++-mingw-w64-x86-64
        g++-mingw-w64-i686 xvfb xauth mesa-vulkan-drivers libgl1-mesa-dri libegl1
        seabios ipxe-qemu ninja-build pkg-config libglib2.0-dev libpixman-1-dev
        libsdl2-dev libepoxy-dev libgbm-dev libdrm-dev libvulkan-dev libpng-dev
        glslang-tools bison flex python3-mako python3-yaml python3-venv gdb) ;;
    runtime) ;;
    *) echo "unknown dependency profile $profile" >&2; exit 2 ;;
esac
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${base[@]}"
if [ "$profile" = graphics ]; then
    pip install --user --break-system-packages -q 'meson==1.9.0' pycotap
    echo "$HOME/.local/bin" >> "$GITHUB_PATH"
fi
if [ "$profile" = build ]; then
    echo /usr/lib/ccache >> "$GITHUB_PATH"
    echo "CCACHE_DIR=$HOME/.ccache" >> "$GITHUB_ENV"
fi
