# toolchain-kernel.cmake — Cross-compiler toolchain for the NovaOS kernel
#
# Target: x86_64 bare metal (no OS, no libc)
# Compiler: x86_64-elf-gcc (from a cross-compilation toolchain)
#           OR the host gcc/clang with appropriate flags
#
# To use a prebuilt cross-compiler:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-kernel.cmake \
#         -DX86_64_ELF_TRIPLE=x86_64-elf ...
#
# If x86_64-elf-gcc is not available, set USE_HOST_GCC=ON and the build
# system will use the host compiler with -target x86_64-none-elf (clang)
# or rely on the host gcc with freestanding flags.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Try to find the cross-compiler
if(NOT DEFINED X86_64_ELF_TRIPLE)
    set(X86_64_ELF_TRIPLE "x86_64-elf")
endif()

# Look for cross-compiler first, fall back to host
find_program(CROSS_GCC   "${X86_64_ELF_TRIPLE}-gcc")
find_program(CROSS_AS    "${X86_64_ELF_TRIPLE}-as")
find_program(CROSS_LD    "${X86_64_ELF_TRIPLE}-ld")
find_program(CROSS_OBJCOPY "${X86_64_ELF_TRIPLE}-objcopy")

if(CROSS_GCC)
    message(STATUS "Using cross-compiler: ${CROSS_GCC}")
    set(CMAKE_C_COMPILER   "${CROSS_GCC}")
    set(CMAKE_ASM_NASM_COMPILER nasm)
else()
    # Try clang with explicit target
    find_program(CLANG clang)
    if(CLANG)
        message(STATUS "Cross-compiler not found, using clang with -target x86_64-unknown-none-elf")
        set(CMAKE_C_COMPILER   "${CLANG}")
        set(CMAKE_C_FLAGS_INIT "-target x86_64-unknown-none-elf")
        set(CMAKE_ASM_NASM_COMPILER nasm)
    else()
        message(WARNING "No cross-compiler found! Using host gcc (may not work correctly)")
        set(CMAKE_C_COMPILER   gcc)
        set(CMAKE_ASM_NASM_COMPILER nasm)
    endif()
endif()

# Prevent CMake from testing the compiler against a hosted environment
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Don't search host paths for headers/libraries
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
