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
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Werror=implicit-function-declaration
    -std=c11
    -O2
    -g
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

set(KERNEL_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)

# -------------------------------------------------------------------------
# Assembly sources
# -------------------------------------------------------------------------

set(KERNEL_ASM_SOURCES
    arch/x86_64/entry.asm
    arch/x86_64/isr_stubs.asm
    arch/x86_64/syscall_entry.asm
)

# -------------------------------------------------------------------------
# C sources
# -------------------------------------------------------------------------

set(KERNEL_C_SOURCES
    arch/x86_64/gdt.c
    arch/x86_64/idt.c
    arch/x86_64/apic.c
    arch/x86_64/paging.c
    mm/pmm.c
    mm/vmm.c
    hal/serial.c
    hal/framebuffer.c
    ke/main.c
    ke/printf.c
    ke/scheduler.c
    ke/syscall.c
    ke/kpcr.c
    ob/ob.c
    ps/ps.c
    cm/cm.c
    se/se.c
    io/io.c
    mm/vma.c
    mm/section.c
    ldr/ldr.c
    fs/vfs.c
    fs/initrd.c
    lib/string.c
)

# -------------------------------------------------------------------------
# Build
# -------------------------------------------------------------------------

# C object library with kernel flags
add_library(kernel_objects OBJECT ${KERNEL_C_SOURCES})
target_compile_options(kernel_objects PRIVATE ${KERNEL_CFLAGS})
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
    -z max-page-size=0x1000
)

if(IS_CLANG)
    list(APPEND KERNEL_LINK_FLAGS
        -target x86_64-unknown-none-elf
        -fuse-ld=lld
    )
endif()

# Convert to string for use in custom command
string(JOIN " " KERNEL_LINK_FLAGS_STR ${KERNEL_LINK_FLAGS})

# Final link step
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
    COMMAND ${CMAKE_C_COMPILER}
            ${KERNEL_LINK_FLAGS}
            -o "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
            ${ASM_OBJECTS}
            $<TARGET_OBJECTS:kernel_objects>
    DEPENDS
            ${ASM_OBJECTS}
            kernel_objects
            "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld"
    COMMENT "Linking kernel.elf"
    VERBATIM
)

add_custom_target(kernel_elf ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
)
add_dependencies(kernel_elf asm_objects kernel_objects)

# Copy to binary dir root (parent CMake looks here)
add_custom_command(
    TARGET  kernel_elf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/kernel.elf"
            "${CMAKE_BINARY_DIR}/kernel.elf"
)
