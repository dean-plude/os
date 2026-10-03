#!/usr/bin/env bash
# Build the QEMU that the graphics self-tests run: QEMU 10.2 with
# virglrenderer 1.3 built with Venus, so its virtio-vga-gl has venus=on
# (a 3D virtio-gpu whose Vulkan runs on this machine's GPU; Mesa's lavapipe
# when it has none).  Ubuntu 24.04's QEMU (8.2) has no Venus.
#     tools/ci/build-qemu-venus.sh PREFIX [CACHE]
# Then run QEMU as PREFIX/bin/qemu-system-x86_64 with
# LD_LIBRARY_PATH=PREFIX/lib/x86_64-linux-gnu (NOVARUN_QEMU for
# tools/novarun.py and tools/selftest.py), on an X display (Xvfb) with
# OpenGL (-display sdl,gl=on).  CACHE (default PREFIX/src) keeps the
# sources.  The sources are Ubuntu's (the same tarballs as QEMU's and
# virglrenderer's releases, QEMU's without the firmware binaries, which
# come from the seabios, ipxe-qemu and ovmf packages instead).
# Needs: meson (1.5 or newer) and pycotap from pip; ninja-build, pkg-config,
# libglib2.0-dev, libpixman-1-dev, libsdl2-dev, libepoxy-dev, libgbm-dev,
# libdrm-dev, libvulkan-dev, libpng-dev (screendump's PNGs), glslang-tools, seabios and ipxe-qemu.
set -euo pipefail
PREFIX="$(realpath -m "$1")"
CACHE="$(realpath -m "${2:-$PREFIX/src}")"
mkdir -p "$CACHE"

fetch() {   # fetch NAME SHA256 PATH-IN-THE-UBUNTU-POOL
  local f="$CACHE/$1"
  if ! { [ -s "$f" ] && echo "$2  $f" | sha256sum -c --quiet; }; then
    curl -sSLf --retry 4 -o "$f" "http://archive.ubuntu.com/ubuntu/pool/$3/$1" ||
      curl -sSLf --retry 4 -o "$f" "https://launchpad.net/ubuntu/+archive/primary/+files/$1"
    echo "$2  $f" | sha256sum -c --quiet
  fi
}
fetch virglrenderer_1.3.0.orig.tar.bz2 088040d130eaa0458a978fe7867fbfb1fcf1fdff52bf3b27a00658828bc4189f main/v/virglrenderer
fetch qemu_10.2.1+ds.orig.tar.xz 4f495c2a523f78f50cc83327cabf01e7e89c6b9a5d3d6cadd9e78de94b25c416 main/q/qemu

cd "$CACHE"
rm -rf virglrenderer-1.3.0 qemu-10.2.1
tar xjf virglrenderer_1.3.0.orig.tar.bz2
meson setup virglrenderer-1.3.0/build virglrenderer-1.3.0 --prefix="$PREFIX" \
  -Dvenus=true -Dvideo=false -Dtests=false
ninja -C virglrenderer-1.3.0/build install

tar xJf qemu_10.2.1+ds.orig.tar.xz
cd qemu-10.2.1
# (the firmware binaries are not in Ubuntu's tarball: nothing to unpack)
sed -i 's/^\( *\)unpack_edk2_blobs = .*/\1unpack_edk2_blobs = false/' meson.build
sed -i "s/^subdir('dtb')/# subdir('dtb')/" pc-bios/meson.build
PKG_CONFIG_PATH="$PREFIX/lib/x86_64-linux-gnu/pkgconfig" ./configure --prefix="$PREFIX" \
  --target-list=x86_64-softmmu --enable-opengl --enable-sdl --enable-virglrenderer --enable-png \
  --disable-gtk --disable-docs --disable-install-blobs
make -j"$(nproc)" install
mkdir -p "$PREFIX/share/qemu"
for d in /usr/share/qemu /usr/share/seabios /usr/lib/ipxe/qemu; do
  for f in "$d"/*; do
    [ -e "$PREFIX/share/qemu/$(basename "$f")" ] || ln -s "$f" "$PREFIX/share/qemu/"
  done
done
LD_LIBRARY_PATH="$PREFIX/lib/x86_64-linux-gnu" "$PREFIX/bin/qemu-system-x86_64" --version
