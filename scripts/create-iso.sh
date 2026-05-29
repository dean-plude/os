#!/usr/bin/env bash
# create-iso.sh — Create a bootable UEFI ISO image for NovaOS
#
# Usage: create-iso.sh <output.iso> <bootx64.efi> <kernel.elf>
#
# Produces an El Torito (no-emulation) UEFI-bootable ISO whose EFI System
# Partition is a FAT image laid out as:
#       /EFI/BOOT/BOOTX64.EFI   — bootloader (auto-discovered by UEFI firmware)
#       /EFI/NOVA/kernel.elf    — kernel ELF (loaded by the bootloader)
#
# The same files are also placed in the ISO9660 tree so they are visible
# when the disc is mounted normally.
#
# Requires: dd, mtools (mformat, mmd, mcopy), xorriso
# Install:  sudo apt install mtools xorriso

set -euo pipefail

ISO_OUT="${1:-nova.iso}"
BOOTLOADER="${2:-bootx64.efi}"
KERNEL="${3:-kernel.elf}"

for bin in dd mformat mmd mcopy xorriso; do
    command -v "$bin" >/dev/null 2>&1 || {
        echo "ERROR: '$bin' not found. Install: sudo apt install mtools xorriso" >&2
        exit 1
    }
done
[ -f "$BOOTLOADER" ] || { echo "ERROR: bootloader not found: $BOOTLOADER" >&2; exit 1; }
[ -f "$KERNEL" ]     || { echo "ERROR: kernel not found: $KERNEL" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ESP="$WORK/esp.img"
ROOT="$WORK/root"
mkdir -p "$ROOT/EFI/BOOT" "$ROOT/EFI/NOVA"

# -------------------------------------------------------------------------
# 1. Build the EFI System Partition (FAT) image, sized to fit the payload.
# -------------------------------------------------------------------------
PAYLOAD_KB=$(( ( $(stat -c%s "$BOOTLOADER") + $(stat -c%s "$KERNEL") ) / 1024 ))
ESP_KB=$(( PAYLOAD_KB + 2048 ))           # payload + ~2 MiB slack
ESP_KB=$(( (ESP_KB + 1023) / 1024 * 1024 ))   # round up to whole MiB

echo "Creating ${ESP_KB} KiB EFI System Partition image"
dd if=/dev/zero of="$ESP" bs=1024 count="$ESP_KB" status=none
# Let mtools choose the FAT width from the image size.  A small ESP becomes
# FAT12/FAT16 (all of which UEFI/OVMF mount); forcing FAT32 on a <33 MiB
# image produces a filesystem firmware cannot read.
mformat -i "$ESP" -v NOVA_EFI ::
mmd     -i "$ESP" ::/EFI ::/EFI/BOOT ::/EFI/NOVA
mcopy   -i "$ESP" "$BOOTLOADER" ::/EFI/BOOT/BOOTX64.EFI
mcopy   -i "$ESP" "$KERNEL"     ::/EFI/NOVA/kernel.elf

# -------------------------------------------------------------------------
# 2. Stage the ESP image (as the El Torito EFI boot image) plus copies of
#    the files in the ISO9660 tree (visible when the disc is mounted).
# -------------------------------------------------------------------------
cp "$ESP"        "$ROOT/efiboot.img"
cp "$BOOTLOADER" "$ROOT/EFI/BOOT/BOOTX64.EFI"
cp "$KERNEL"     "$ROOT/EFI/NOVA/kernel.elf"

# -------------------------------------------------------------------------
# 3. Author the ISO. The FAT ESP (efiboot.img) is registered as a
#    no-emulation UEFI El Torito boot image — the form OVMF/real firmware
#    mounts to find \EFI\BOOT\BOOTX64.EFI.
# -------------------------------------------------------------------------
echo "Authoring ISO: $ISO_OUT"
xorriso -as mkisofs \
    -V NOVAOS \
    -o "$ISO_OUT" \
    -e efiboot.img \
    -no-emul-boot \
    -isohybrid-gpt-basdat \
    "$ROOT"

echo ""
echo "ISO created: $ISO_OUT ($(du -h "$ISO_OUT" | cut -f1))"
