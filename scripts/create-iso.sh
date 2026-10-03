#!/usr/bin/env bash
# create-iso.sh — Create a bootable UEFI ISO image for NovaOS
#
# Usage: create-iso.sh <output.iso> <bootx64.efi> <kernel.elf>
#
# Produces an El Torito (no-emulation) UEFI-bootable ISO whose EFI System
# Partition is a FAT image laid out as:
#       /EFI/BOOT/BOOTX64.EFI   — bootloader (auto-discovered by UEFI firmware)
#       /EFI/NOVA/kernel.elf    — kernel ELF (loaded by the bootloader)
#       /EFI/NOVA/bootlog.txt   — 1 MiB set aside for the boot log: started
#                                 from the ISO written to a USB stick, NovaOS
#                                 writes its log into this file on the stick
#                                 (the file also tells the bootloader that it
#                                 started from the installation media)
#
# Write the ISO to a USB stick as it is (dd, or a tool such as Rufus in
# "DD image" mode, balenaEtcher or the GNOME Disks "Restore Disk Image"):
# the ESP is also a GPT partition (step 3), so UEFI firmware starts it from
# the stick as it does from a disc.
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
BOOTLOG_KB=1024
PAYLOAD_KB=$(( ( $(stat -c%s "$BOOTLOADER") + $(stat -c%s "$KERNEL") ) / 1024 + BOOTLOG_KB ))
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
# The boot log's space: one line saying what the file is, then blank lines
# (written last, into an empty ESP's free space, so its clusters follow one
# another, which is what the kernel needs to write it in place)
{
    printf 'NovaOS writes its boot log here when it starts from this USB stick.\n'
    head -c $(( BOOTLOG_KB * 1024 - 68 )) /dev/zero | tr '\0' '\n'
} > "$WORK/bootlog.txt"
mcopy   -i "$ESP" "$WORK/bootlog.txt" ::/EFI/NOVA/bootlog.txt

# -------------------------------------------------------------------------
# 2. Stage the ESP image (as the El Torito EFI boot image) plus copies of
#    the files in the ISO9660 tree (visible when the disc is mounted).
# -------------------------------------------------------------------------
cp "$ESP"        "$ROOT/efiboot.img"
cp "$BOOTLOADER" "$ROOT/EFI/BOOT/BOOTX64.EFI"
cp "$KERNEL"     "$ROOT/EFI/NOVA/kernel.elf"

# -------------------------------------------------------------------------
# 3. Author the ISO. The FAT ESP (efiboot.img) is registered as a
#    no-emulation UEFI El Torito boot image, the form OVMF/real firmware
#    mounts to find \EFI\BOOT\BOOTX64.EFI on a disc, and the same
#    sectors are a partition of a GPT (type EFI System, partition 2:
#    xorriso fills the gaps before and after it with partitions 1 and 3;
#    behind a protective MBR), which is how firmware finds it on a stick.
#    (-isohybrid-gpt-basdat alone wrote no partition table: the ISO booted
#    only as a disc.  An appended partition is not it either: firmware
#    sizes a disc's El Torito image from the ISO9660 volume, which an
#    appended partition lies outside.)
# -------------------------------------------------------------------------
echo "Authoring ISO: $ISO_OUT"
xorriso -as mkisofs \
    -V NOVAOS \
    -o "$ISO_OUT" \
    -e efiboot.img \
    -no-emul-boot \
    -efi-boot-part --efi-boot-image \
    --protective-msdos-label \
    "$ROOT"

echo ""
echo "ISO created: $ISO_OUT ($(du -h "$ISO_OUT" | cut -f1))"
