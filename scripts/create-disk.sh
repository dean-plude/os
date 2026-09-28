#!/usr/bin/env bash
# create-disk.sh — Create a bootable UEFI disk image
#
# Usage: create-disk.sh <output.img> <bootx64.efi> <kernel.elf>
#
# Creates a raw disk image with:
#   - GPT partition table
#   - One EFI System Partition (FAT32, 128 MiB; NovaOS also keeps drive C: here
#     under \NOVA\C when no data disk is attached)
#   - File layout:
#       /EFI/BOOT/BOOTX64.EFI   — bootloader (auto-discovered by UEFI firmware)
#       /EFI/NOVA/kernel.elf    — kernel ELF (loaded by bootloader)
#
# Requires: dd, mkdosfs (mkfs.fat), mtools (mformat, mmd, mcopy)
# Install:  sudo apt install mtools dosfstools

set -euo pipefail

DISK_IMG="${1:-nova.img}"
BOOTLOADER="${2:-bootx64.efi}"
KERNEL="${3:-kernel.elf}"

DISK_SIZE_MB=128

# -------------------------------------------------------------------------
# Validation
# -------------------------------------------------------------------------

for bin in dd mkdosfs mformat mmd mcopy; do
    if ! command -v "$bin" &>/dev/null; then
        echo "ERROR: '$bin' not found. Install with: sudo apt install mtools dosfstools" >&2
        exit 1
    fi
done

if [ ! -f "$BOOTLOADER" ]; then
    echo "ERROR: Bootloader not found: $BOOTLOADER" >&2
    exit 1
fi

if [ ! -f "$KERNEL" ]; then
    echo "ERROR: Kernel not found: $KERNEL" >&2
    exit 1
fi

# -------------------------------------------------------------------------
# Create the raw disk image
# -------------------------------------------------------------------------

echo "Creating ${DISK_SIZE_MB} MiB disk image: $DISK_IMG"

rm -f "$DISK_IMG"
truncate -s ${DISK_SIZE_MB}M "$DISK_IMG"            # sparse: only what is written takes space

# Format as FAT32 with mformat (no partition table — simple ESP image)
# UEFI can boot from a bare FAT32 image or from an image with a GPT + ESP.
# For simplicity in Phase 1 we use a flat FAT32 image that QEMU can use
# directly as a virtual hard disk. OVMF will find BOOTX64.EFI automatically.
mformat -i "$DISK_IMG" -F -v "NOVA_EFI" ::

# -------------------------------------------------------------------------
# Create directory structure
# -------------------------------------------------------------------------

echo "Creating EFI directory structure..."
mmd -i "$DISK_IMG" ::/EFI
mmd -i "$DISK_IMG" ::/EFI/BOOT
mmd -i "$DISK_IMG" ::/EFI/NOVA

# -------------------------------------------------------------------------
# Copy files
# -------------------------------------------------------------------------

echo "Copying bootloader: $BOOTLOADER -> ::/EFI/BOOT/BOOTX64.EFI"
mcopy -i "$DISK_IMG" "$BOOTLOADER" ::/EFI/BOOT/BOOTX64.EFI

echo "Copying kernel: $KERNEL -> ::/EFI/NOVA/kernel.elf"
mcopy -i "$DISK_IMG" "$KERNEL" ::/EFI/NOVA/kernel.elf

# -------------------------------------------------------------------------
# Verify
# -------------------------------------------------------------------------

echo ""
echo "Disk image contents:"
mdir -i "$DISK_IMG" -s ::
echo ""
echo "Image created: $DISK_IMG ($(du -h "$DISK_IMG" | cut -f1))"
