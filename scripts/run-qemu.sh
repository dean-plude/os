#!/usr/bin/env bash
# run-qemu.sh — Launch NovaOS in QEMU with OVMF UEFI firmware
#
# Usage: run-qemu.sh <nova.img> <OVMF_CODE.fd> [OVMF_VARS.fd] [--gdb]
#
# Features:
#   - UEFI boot via OVMF
#   - Serial output to stdio (so you can see kernel debug messages)
#   - QEMU monitor on stdio (Ctrl+A C to switch, Ctrl+A X to quit)
#   - Optionally starts GDB stub on port 1234 (--gdb flag)
#   - 256 MiB RAM (enough for Phase 1)
#   - VGA display + serial for dual output
#
# GDB usage (when --gdb is passed):
#   gdb kernel.elf
#   (gdb) target remote :1234
#   (gdb) hbreak KiSystemStartup
#   (gdb) continue

set -euo pipefail

DISK_IMG="${1:-nova.img}"
OVMF_CODE="${2:-/usr/share/OVMF/OVMF_CODE.fd}"
OVMF_VARS="${3:-/usr/share/OVMF/OVMF_VARS.fd}"
GDB_MODE=0

# Parse remaining args
for arg in "${@:4}"; do
    if [ "$arg" = "--gdb" ]; then
        GDB_MODE=1
    fi
done

# -------------------------------------------------------------------------
# Validation
# -------------------------------------------------------------------------

if ! command -v qemu-system-x86_64 &>/dev/null; then
    echo "ERROR: qemu-system-x86_64 not found."
    echo "Install: sudo apt install qemu-system-x86"
    exit 1
fi

if [ ! -f "$DISK_IMG" ]; then
    echo "ERROR: Disk image not found: $DISK_IMG"
    echo "Run 'cmake --build .' to build first."
    exit 1
fi

if [ ! -f "$OVMF_CODE" ]; then
    echo "ERROR: OVMF firmware not found: $OVMF_CODE"
    echo "Install: sudo apt install ovmf"
    echo "Then set OVMF path: cmake -DOVMF_CODE=/path/to/OVMF_CODE.fd .."
    exit 1
fi

# -------------------------------------------------------------------------
# QEMU command
# -------------------------------------------------------------------------

QEMU_ARGS=(
    -machine q35
    -cpu qemu64,+rdtscp
    -m 256M
    -smp 4
    -drive "if=pflash,format=raw,readonly=on,file=${OVMF_CODE}"
    -drive "format=raw,file=${DISK_IMG}"
    -serial stdio
    -vga std
    -nic none
)

# Sound: an Intel HD Audio card.  NOVA_AUDIO picks QEMU's audio backend
# ("pa", "pipewire", "alsa", "none", or "wav,path=out.wav" to record);
# by default the host's sound server, else a silent card.
if [ -z "${NOVA_AUDIO:-}" ]; then
    RUNDIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    if [ -S "$RUNDIR/pulse/native" ] || [ -n "${PULSE_SERVER:-}" ]; then NOVA_AUDIO=pa
    elif [ -S "$RUNDIR/pipewire-0" ]; then NOVA_AUDIO=pipewire
    elif [ "$(uname)" = Darwin ]; then NOVA_AUDIO=coreaudio
    else NOVA_AUDIO=none
    fi
fi
QEMU_ARGS+=( -audiodev "${NOVA_AUDIO%%,*},id=snd0${NOVA_AUDIO#"${NOVA_AUDIO%%,*}"}"
             -device intel-hda -device hda-output,audiodev=snd0 )

# Data disk: NovaOS keeps drive C: here (it formats an empty disk on first
# boot).  It sits beside the boot image, so rebuilding NovaOS keeps your files.
DATA_IMG="$(dirname "$DISK_IMG")/nova-data.img"
if [ ! -f "$DATA_IMG" ]; then
    truncate -s 256M "$DATA_IMG"
    echo "Created an empty 256 MiB data disk: $DATA_IMG"
fi
QEMU_ARGS+=( -drive "format=raw,file=${DATA_IMG}" )

if [ -f "$OVMF_VARS" ]; then
    VARS_COPY="/tmp/nova_ovmf_vars_$$.fd"
    cp "$OVMF_VARS" "$VARS_COPY"
    trap "rm -f '$VARS_COPY'" EXIT
    QEMU_ARGS+=(
        -drive "if=pflash,format=raw,file=${VARS_COPY}"
    )
fi

# GDB stub
if [ "$GDB_MODE" = "1" ]; then
    QEMU_ARGS+=(
        -s              # Shorthand for -gdb tcp::1234
        -S              # Freeze CPU at startup; wait for GDB 'continue'
    )
    echo "GDB mode: QEMU will wait for GDB connection on port 1234"
    echo "  Launch GDB with: gdb kernel.elf -ex 'target remote :1234'"
fi

# -------------------------------------------------------------------------
# Launch
# -------------------------------------------------------------------------

echo "Starting NovaOS..."
echo "  Disk:  $DISK_IMG"
echo "  OVMF:  $OVMF_CODE"
echo "  RAM:   256 MiB"
echo "  GDB:   $([ $GDB_MODE = 1 ] && echo 'port 1234' || echo 'disabled')"
echo ""
echo "Serial output (COM1) follows. Press Ctrl+C to quit."
echo "============================================================"

exec qemu-system-x86_64 "${QEMU_ARGS[@]}"
