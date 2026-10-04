# kernel/CMakeLists.txt — NovaOS kernel (standalone CMake project)
#
# This is built as an ExternalProject from the root CMakeLists.txt,
# so it must be a standalone CMake project with its own project() call.
#
# The kernel uses a freestanding C11 compiler (x86_64-elf-gcc or clang)
# and NASM for assembly stubs.

cmake_minimum_required(VERSION 3.18)
project(NovaKernel C ASM_NASM)

# Locate NASM
enable_language(ASM_NASM)
find_program(NASM_BIN nasm REQUIRED)

# Source root (may be passed from parent CMake)
if(NOT DEFINED CMAKE_SOURCE_DIR)
    set(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# -------------------------------------------------------------------------
# Compiler flags
# -------------------------------------------------------------------------

# Detect if we're using Clang (needs -target flag) or GCC (freestanding)
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" --version
    OUTPUT_VARIABLE COMPILER_VERSION_OUTPUT
    ERROR_VARIABLE  COMPILER_VERSION_OUTPUT
)

if(COMPILER_VERSION_OUTPUT MATCHES "[Cc]lang")
    set(IS_CLANG TRUE)
    message(STATUS "Kernel compiler: clang")
else()
    set(IS_CLANG FALSE)
    message(STATUS "Kernel compiler: GCC (${CMAKE_C_COMPILER})")
endif()

set(KERNEL_CFLAGS
    -ffreestanding
    -fno-stack-protector
    -fno-builtin
    -nostdlib
    -mno-red-zone
    -fshort-wchar
    -mno-mmx
    -mno-sse
    -mno-sse2
    # Several executive globals (e.g. ObpFileType) are tentatively defined
    # in more than one translation unit and merged at link time.  Modern
    # Clang/GCC default to -fno-common, which turns that into a duplicate
    # symbol error, so request the classic common-symbol behavior here.
    -fcommon
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Werror=implicit-function-declaration
    -std=c11
    -O2
    -g
    # frame pointers: kernel faults and panics print a backtrace (ke/ksym.c)
    -fno-omit-frame-pointer
    -mno-omit-leaf-frame-pointer
)

if(IS_CLANG)
    list(APPEND KERNEL_CFLAGS
        -target x86_64-unknown-none-elf
        -mcmodel=kernel
    )
else()
    list(APPEND KERNEL_CFLAGS
        -mcmodel=kernel
    )
endif()

# NASM flags
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
set(CMAKE_ASM_NASM_FLAGS "-f elf64 -g -F dwarf")

# -------------------------------------------------------------------------
# Include paths (accessible to all kernel C files)
# -------------------------------------------------------------------------

set(LWIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lwip/src")
set(MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mbedtls")
set(UACPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/uacpi")
set(VTERM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libvterm")
set(MONOCYPHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher")

set(KERNEL_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/net/port"      # lwIP port: lwipopts.h, arch/cc.h
    "${LWIP_DIR}/include"
    "${MBEDTLS_DIR}/include"
    "${UACPI_DIR}/include"
    "${VTERM_DIR}/include"
    "${MONOCYPHER_DIR}"
)

# -------------------------------------------------------------------------
# Assembly sources
# -------------------------------------------------------------------------

set(KERNEL_ASM_SOURCES
    arch/x86_64/entry.asm
    arch/x86_64/isr_stubs.asm
    arch/x86_64/syscall_entry.asm
    arch/x86_64/ap_trampoline.asm
    arch/x86_64/uaccess.asm
)

# -------------------------------------------------------------------------
# C sources
# -------------------------------------------------------------------------

set(KERNEL_C_SOURCES
    arch/x86_64/gdt.c
    ke/ksym.c
    ke/prof.c
    arch/x86_64/idt.c
    arch/x86_64/apic.c
    arch/x86_64/paging.c
    mm/pmm.c
    mm/vmm.c
    hal/serial.c
    hal/framebuffer.c
    hal/rtc.c
    hal/ps2.c
    hal/pci.c
    hal/acpi.c
    hal/aml.c
    hal/ec.c
    hal/gpio.c
    hal/ioapic.c
    hal/hpet.c
    hal/display.c
    hal/firmware.c
    drivers/e1000.c
    drivers/virtio_net.c
    drivers/virtio_input.c
    drivers/gamepad.c
    drivers/xpad.c
    drivers/virtio_gpu.c
    drivers/ahci.c
    drivers/hda.c
    drivers/sof.c
    drivers/i2c_dw.c
    drivers/i2chid.c
    drivers/audio.c
    drivers/usb.c
    drivers/xhci.c
    drivers/ehci.c
    drivers/ohci.c
    drivers/uhci.c
    drivers/usbhid.c
    drivers/usbhub.c
    drivers/usbmsc.c
    drivers/usbaudio.c
    drivers/nvme.c
    fs/block.c
    fs/fat.c
    fs/ntfs.c
    fs/drives.c
    fs/persist.c
    fs/setup.c
    fs/update.c
    fs/bootlog.c
    net/net.c
    net/tls.c
    net/tls_roots.c
    net/tls_platform.c
    net/http.c
    net/sock.c
    um/um.c
    um/um_console.c
    um/um_syscall.c
    um/um_thread.c
    um/um_exception.c
    um/um_gpfault.c
    um/um_socket.c
    um/um_audio.c
    um/um_gpu.c
    um/um_pipe.c
    um/um_hid.c
    um/um_gui.c
    um/um_services.c
    um/um_registry.c
    um/um_crash.c
    um/um_security.c
    ke/main.c
    ke/version.c
    ke/printf.c
    ke/scheduler.c
    ke/syscall.c
    ke/kpcr.c
    ke/smp.c
    ke/sleep.c
    ke/timezone.c
    ke/waitq.c
    ke/probe.c
    ke/probe_test.c
    ob/ob.c
    ps/ps.c
    ps/ps_test.c
    cm/cm.c
    se/se.c
    io/io.c
    mm/vma.c
    mm/section.c
    ldr/ldr.c
    ldr/user_stubs.c
    ps/csrss.c
    fs/vfs.c
    fs/initrd.c
    fs/ramfs.c
    fs/fsec.c
    gdi/gdi.c
    gdi/syscursor.c
    gdi/png.c
    gdi/icon.c
    gdi/font_inter.c
    wm/wm.c
    wm/desktop.c
    wm/clipboard.c
    wm/input.c
    wm/kbdlayout.c
    wm/tablet.c
    apps/apps.c
    apps/terminal.c
    apps/explorer.c
    apps/notepad.c
    apps/settings.c
    apps/calendar.c
    apps/photos.c
    apps/store.c
    apps/setup.c
    apps/welcome.c
    lib/string.c
)

# -------------------------------------------------------------------------
# Build
# -------------------------------------------------------------------------

# lwIP 2.2.0 (third_party/lwip, BSD licence): IPv4 and IPv6 core + Ethernet
file(GLOB LWIP_SOURCES
    "${LWIP_DIR}/core/*.c"
    "${LWIP_DIR}/core/ipv4/*.c"
    "${LWIP_DIR}/core/ipv6/*.c"
    "${LWIP_DIR}/netif/ethernet.c"
)
set_source_files_properties(${LWIP_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
list(APPEND KERNEL_C_SOURCES ${LWIP_SOURCES})

# Mbed TLS 3.6.7 (third_party/mbedtls, Apache-2.0): TLS 1.2/1.3 and X.509.
# Configured by net/port/mbedtls_nova_config.h (MBEDTLS_CONFIG_FILE, set
# for every kernel source below so all users see the same configuration).
file(GLOB MBEDTLS_SOURCES "${MBEDTLS_DIR}/library/*.c")
set_source_files_properties(${MBEDTLS_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-w;-I${MBEDTLS_DIR}/library")
list(APPEND KERNEL_C_SOURCES ${MBEDTLS_SOURCES})

# uACPI (third_party/uacpi, MIT): the AML interpreter; host functions in
# hal/aml.c
file(GLOB UACPI_SOURCES "${UACPI_DIR}/source/*.c")
set_source_files_properties(${UACPI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
list(APPEND KERNEL_C_SOURCES ${UACPI_SOURCES})

# libvterm 0.3.3 (third_party/libvterm, MIT): the Terminal's VT emulator for
# full-screen console programs; the C library it uses is apps/vterm_libc.h
file(GLOB VTERM_SOURCES "${VTERM_DIR}/src/*.c")
set_source_files_properties(${VTERM_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-w;-include;${CMAKE_CURRENT_SOURCE_DIR}/apps/vterm_libc.h")
list(APPEND KERNEL_C_SOURCES ${VTERM_SOURCES})

# Monocypher 4.0.2 (third_party/monocypher, BSD-2-Clause): Ed25519, which
# checks the update channel's signature (fs/update.c)
set(MONOCYPHER_SOURCES "${MONOCYPHER_DIR}/monocypher.c" "${MONOCYPHER_DIR}/monocypher-ed25519.c")
set_source_files_properties(${MONOCYPHER_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
list(APPEND KERNEL_C_SOURCES ${MONOCYPHER_SOURCES})

# The Windows userland (userland/, tools/build_userland.py): system DLLs and
# programs built with clang for x86_64-pc-windows-msvc, embedded as data and
# installed on drive C: at boot.
file(GLOB_RECURSE USERLAND_DEPS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*.rc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*.ico"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userland/*/build.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/netsurf/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/netsurf/*.h")
set(USERLAND_C "${CMAKE_CURRENT_BINARY_DIR}/userland_files.c")
add_custom_command(
    OUTPUT  "${USERLAND_C}"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_userland.py"
            "${CMAKE_CURRENT_BINARY_DIR}/userland" "${USERLAND_C}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ke/syscall.h"
    DEPENDS ${USERLAND_DEPS}
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_userland.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_netsurf.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/ke/syscall.h"
    COMMENT "Building the Windows userland (ntdll, kernel32, msvcrt, programs, NetSurf)")
list(APPEND KERNEL_C_SOURCES "${USERLAND_C}")

# C object library with kernel flags
add_library(kernel_objects OBJECT ${KERNEL_C_SOURCES})
target_compile_options(kernel_objects PRIVATE ${KERNEL_CFLAGS})
target_compile_definitions(kernel_objects PRIVATE
    "MBEDTLS_CONFIG_FILE=\"mbedtls_nova_config.h\"")
target_include_directories(kernel_objects PRIVATE ${KERNEL_INCLUDE_DIRS})

# ASM objects via NASM
set(ASM_OBJECTS "")
foreach(ASM_SRC IN LISTS KERNEL_ASM_SOURCES)
    get_filename_component(ASM_BASE ${ASM_SRC} NAME_WE)
    set(ASM_OBJ "${CMAKE_CURRENT_BINARY_DIR}/asm_${ASM_BASE}.o")
    add_custom_command(
        OUTPUT  ${ASM_OBJ}
        COMMAND nasm -f elf64 -g -F dwarf
                -I "${CMAKE_CURRENT_SOURCE_DIR}/"
                -o ${ASM_OBJ}
                "${CMAKE_CURRENT_SOURCE_DIR}/${ASM_SRC}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${ASM_SRC}"
        COMMENT "NASM: ${ASM_SRC}"
        VERBATIM
    )
    list(APPEND ASM_OBJECTS ${ASM_OBJ})
endforeach()

add_custom_target(asm_objects DEPENDS ${ASM_OBJECTS})

# -------------------------------------------------------------------------
# Link
# -------------------------------------------------------------------------

# Build the linker flags
set(KERNEL_LINK_FLAGS
    -T "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld"
    -nostdlib
    -static
    -no-pie
    -z max-page-size=0x1000
)

if(IS_CLANG)
    list(APPEND KERNEL_LINK_FLAGS
        -target x86_64-unknown-none-elf
    )
endif()

# Convert to string for use in custom command
string(JOIN " " KERNEL_LINK_FLAGS_STR ${KERNEL_LINK_FLAGS})

# Archive C objects into a static library so the linker gets one argument
add_library(kernel_c_lib STATIC $<TARGET_OBJECTS:kernel_objects>)
set_target_properties(kernel_c_lib PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    OUTPUT_NAME "kernel_c"
)
add_dependencies(kernel_c_lib kernel_objects)

# Final link step — use the archive + ASM objects.  Twice: the first link
# (no symbol table) gives tools/mkksyms.py the functions' addresses, the
# second embeds them (.ksyms, after .text, so nothing in .text moves; the
# --check proves it)
set(KSYMS_S "${CMAKE_CURRENT_BINARY_DIR}/ksyms.S")
set(KSYMS_O "${CMAKE_CURRENT_BINARY_DIR}/ksyms.o")
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
    COMMAND ${CMAKE_C_COMPILER}
            ${KERNEL_LINK_FLAGS}
            -o "${CMAKE_CURRENT_BINARY_DIR}/kernel_nosyms.elf"
            ${ASM_OBJECTS}
            "${CMAKE_CURRENT_BINARY_DIR}/libkernel_c.a"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mkksyms.py"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel_nosyms.elf" "${KSYMS_S}"
    COMMAND ${CMAKE_C_COMPILER} $<$<BOOL:${IS_CLANG}>:-target> $<$<BOOL:${IS_CLANG}>:x86_64-unknown-none-elf>
            -c "${KSYMS_S}" -o "${KSYMS_O}"
    COMMAND ${CMAKE_C_COMPILER}
            ${KERNEL_LINK_FLAGS}
            -o "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
            ${ASM_OBJECTS}
            "${CMAKE_CURRENT_BINARY_DIR}/libkernel_c.a"
            "${KSYMS_O}"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mkksyms.py" --check
            "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf" "${KSYMS_S}"
    DEPENDS
            ${ASM_OBJECTS}
            kernel_c_lib
            "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mkksyms.py"
    COMMENT "Linking kernel.elf (twice: the second with its symbol table)"
    VERBATIM
)

add_custom_target(kernel_elf ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
)
add_dependencies(kernel_elf asm_objects kernel_c_lib)

# Copy to binary dir root (parent CMake looks here)
add_custom_command(
    TARGET  kernel_elf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
            "${CMAKE_BINARY_DIR}/kernel.elf"
)
