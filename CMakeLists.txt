# CMakeLists.txt — NovaOS root build file
#
# Structure:
#   - The kernel and bootloader are built as separate sub-projects because
#     they require fundamentally different compilers and flags.
#   - The root CMakeLists orchestrates both builds using ExternalProject.
#   - A top-level 'disk-image' target creates a bootable UEFI disk image.
#
# Usage:
#   mkdir build && cd build
#   cmake ..
#   make -j$(nproc)
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install cmake ninja-build nasm
#   sudo apt install gcc-mingw-w64-x86-64    # OR clang lld
#   sudo apt install qemu-system-x86 ovmf    # for running
#   sudo apt install mtools                  # for disk image creation
#
# Optional (for a dedicated x86_64-elf cross-compiler):
#   See docs/building.md for crosstool-ng instructions.

cmake_minimum_required(VERSION 3.18)
project(NovaOS VERSION 0.1.0 LANGUAGES NONE)

# -------------------------------------------------------------------------
# Build configuration
# -------------------------------------------------------------------------

set(NOVA_ARCH "x86_64" CACHE STRING "Target architecture")
set(NOVA_BUILD_TYPE "Debug" CACHE STRING "Build type (Debug/Release)")

message(STATUS "NovaOS v${PROJECT_VERSION}")
message(STATUS "Target arch: ${NOVA_ARCH}")
message(STATUS "Build type:  ${NOVA_BUILD_TYPE}")

# -------------------------------------------------------------------------
# Find required tools
# -------------------------------------------------------------------------

find_program(NASM_BIN nasm REQUIRED)
message(STATUS "NASM: ${NASM_BIN}")

find_program(QEMU_BIN qemu-system-x86_64)
if(QEMU_BIN)
    message(STATUS "QEMU: ${QEMU_BIN}")
else()
    message(STATUS "QEMU not found (install qemu-system-x86 to run)")
endif()

# Detect the best available C compiler for the kernel
find_program(X86_64_ELF_GCC "x86_64-elf-gcc")
find_program(X86_64_LINUX_GCC "x86_64-linux-gnu-gcc")
find_program(CLANG_BIN "clang")

if(X86_64_ELF_GCC)
    set(KERNEL_CC "${X86_64_ELF_GCC}")
    message(STATUS "Kernel compiler: ${KERNEL_CC} (x86_64-elf cross-compiler)")
elseif(CLANG_BIN)
    set(KERNEL_CC "${CLANG_BIN}")
    message(STATUS "Kernel compiler: ${KERNEL_CC} (clang, target override)")
else()
    message(FATAL_ERROR
        "No suitable kernel compiler found!\n"
        "Options:\n"
        "  1. Install x86_64-elf-gcc (see docs/building.md)\n"
        "  2. Install clang: sudo apt install clang\n")
endif()

# -------------------------------------------------------------------------
# Sub-build: Kernel
#
# We can't use add_subdirectory() because the kernel uses a different
# compiler.  We use ExternalProject_Add to build it in isolation.
# -------------------------------------------------------------------------

include(ExternalProject)

ExternalProject_Add(kernel_build
    SOURCE_DIR    "${CMAKE_SOURCE_DIR}/kernel"
    BINARY_DIR    "${CMAKE_BINARY_DIR}/kernel_build"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${NOVA_BUILD_TYPE}
        -DCMAKE_C_COMPILER=${KERNEL_CC}
        -DCMAKE_ASM_NASM_COMPILER=${NASM_BIN}
        # Pass source root so kernel can find boot_protocol.h
        -DCMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    BUILD_ALWAYS  TRUE
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/kernel_build/kernel.elf"
    LOG_CONFIGURE TRUE
    LOG_BUILD     TRUE
)

# Copy built kernel.elf to root build directory
ExternalProject_Get_Property(kernel_build BINARY_DIR)
set(KERNEL_ELF "${BINARY_DIR}/kernel.elf")
add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/kernel.elf"
    COMMAND ${CMAKE_COMMAND} -E copy "${KERNEL_ELF}" "${CMAKE_BINARY_DIR}/kernel.elf"
    # The file dependency (not just the target) makes the copy re-run
    # whenever the kernel is rebuilt; a target name alone only orders it.
    DEPENDS kernel_build "${KERNEL_ELF}"
)
add_custom_target(copy_kernel ALL DEPENDS "${CMAKE_BINARY_DIR}/kernel.elf")

# -------------------------------------------------------------------------
# Sub-build: Bootloader
# -------------------------------------------------------------------------

ExternalProject_Add(bootloader_build
    SOURCE_DIR    "${CMAKE_SOURCE_DIR}/bootloader"
    BINARY_DIR    "${CMAKE_BINARY_DIR}/bootloader_build"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${NOVA_BUILD_TYPE}
        -DCMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    BUILD_ALWAYS  TRUE
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/bootloader_build/bootx64.efi"
    LOG_CONFIGURE TRUE
    LOG_BUILD     TRUE
)

ExternalProject_Get_Property(bootloader_build BINARY_DIR)
set(BOOTLOADER_EFI "${BINARY_DIR}/bootx64.efi")
add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/bootx64.efi"
    COMMAND ${CMAKE_COMMAND} -E copy "${BOOTLOADER_EFI}" "${CMAKE_BINARY_DIR}/bootx64.efi"
    DEPENDS bootloader_build "${BOOTLOADER_EFI}"
)
add_custom_target(copy_bootloader ALL DEPENDS "${CMAKE_BINARY_DIR}/bootx64.efi")

# -------------------------------------------------------------------------
# Disk image target (nova.img — raw FAT32 disk image)
#
# Directory layout on the ESP:
#   /EFI/BOOT/BOOTX64.EFI   — the bootloader (standard UEFI boot path)
#   /EFI/NOVA/kernel.elf    — the kernel ELF
# -------------------------------------------------------------------------

find_program(DD_BIN dd)
find_program(MKDOSFS_BIN NAMES mkdosfs mkfs.fat)
find_program(MTOOLS_MCOPY NAMES mcopy)

if(DD_BIN AND MKDOSFS_BIN AND MTOOLS_MCOPY)
    set(DISK_IMG "${CMAKE_BINARY_DIR}/nova.img")
    set(DISK_SIZE_MB 128)

    add_custom_command(
        OUTPUT  "${DISK_IMG}"
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/create-disk.sh"
                "${DISK_IMG}"
                "${CMAKE_BINARY_DIR}/bootx64.efi"
                "${CMAKE_BINARY_DIR}/kernel.elf"
        DEPENDS
                "${CMAKE_BINARY_DIR}/bootx64.efi"
                "${CMAKE_BINARY_DIR}/kernel.elf"
                "${CMAKE_SOURCE_DIR}/scripts/create-disk.sh"
        COMMENT "Creating bootable disk image: nova.img"
    )

    add_custom_target(disk_image ALL
        DEPENDS "${DISK_IMG}"
    )
    add_dependencies(disk_image copy_kernel copy_bootloader)

    message(STATUS "Disk image target: enabled (mtools found)")
else()
    message(STATUS "Disk image target: disabled (install mtools: sudo apt install mtools)")
endif()

# -------------------------------------------------------------------------
# QEMU run targets
# -------------------------------------------------------------------------

if(QEMU_BIN)
    # Look for OVMF UEFI firmware
    find_file(OVMF_CODE NAMES
        OVMF_CODE.fd
        OVMF.fd
        ovmf/OVMF_CODE.fd
        HINTS
            /usr/share/OVMF
            /usr/share/ovmf
            /usr/share/qemu
            /usr/local/share/OVMF
    )
    find_file(OVMF_VARS NAMES
        OVMF_VARS.fd
        HINTS
            /usr/share/OVMF
            /usr/share/ovmf
            /usr/share/qemu
    )

    if(OVMF_CODE)
        message(STATUS "OVMF: ${OVMF_CODE}")
        message(STATUS "OVMF VARS: ${OVMF_VARS}")

        # run: launch QEMU (foreground, interactive)
        add_custom_target(run
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/run-qemu.sh"
                    "${DISK_IMG}"
                    "${OVMF_CODE}"
                    "${OVMF_VARS}"
            DEPENDS disk_image
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Launching NovaOS in QEMU..."
            USES_TERMINAL
        )

        # run-debug: launch QEMU with GDB stub on port 1234
        add_custom_target(run-debug
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/run-qemu.sh"
                    "${DISK_IMG}"
                    "${OVMF_CODE}"
                    "${OVMF_VARS}"
                    "--gdb"
            DEPENDS disk_image
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Launching NovaOS in QEMU (GDB on :1234)..."
            USES_TERMINAL
        )
    else()
        message(STATUS "OVMF not found — install: sudo apt install ovmf")
    endif()
endif()

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------

message(STATUS "")
message(STATUS "Build summary:")
message(STATUS "  Build dir:    ${CMAKE_BINARY_DIR}")
message(STATUS "  Kernel ELF:   ${CMAKE_BINARY_DIR}/kernel.elf")
message(STATUS "  Bootloader:   ${CMAKE_BINARY_DIR}/bootx64.efi")
message(STATUS "  Disk image:   ${CMAKE_BINARY_DIR}/nova.img")
message(STATUS "")
message(STATUS "To build:  cmake --build .")
message(STATUS "To run:    cmake --build . --target run")
message(STATUS "To debug:  cmake --build . --target run-debug")
message(STATUS "           then: gdb -ex 'target remote :1234' kernel.elf")
message(STATUS "")
