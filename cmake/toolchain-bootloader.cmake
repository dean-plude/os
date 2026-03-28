# toolchain-bootloader.cmake — Toolchain for building the UEFI bootloader
#
# The UEFI bootloader must be compiled as a PE32+ EFI application.
# Two approaches are supported:
#
# Approach A (preferred): x86_64-w64-mingw32-gcc
#   Available on most Linux distros as 'mingw-w64' package.
#   Produces PE32+ natively.  We link with custom flags to produce a DLL
#   with the EFI subsystem number.
#
# Approach B: clang with PE target
#   clang --target=x86_64-unknown-windows
#   Combined with lld-link for linking.
#   Available if LLVM/Clang 8+ is installed.
#
# The CMakeLists.txt detects which approach is available.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Look for MinGW cross-compiler first
find_program(MINGW_GCC "x86_64-w64-mingw32-gcc")
find_program(CLANG clang)
find_program(LLD_LINK lld-link)

if(MINGW_GCC)
    message(STATUS "Bootloader: using MinGW-w64: ${MINGW_GCC}")
    set(BOOTLOADER_USE_MINGW ON CACHE BOOL "" FORCE)
    set(CMAKE_C_COMPILER "${MINGW_GCC}")
elseif(CLANG AND LLD_LINK)
    message(STATUS "Bootloader: using clang+lld-link")
    set(BOOTLOADER_USE_CLANG ON CACHE BOOL "" FORCE)
    set(CMAKE_C_COMPILER "${CLANG}")
    set(CMAKE_C_FLAGS_INIT "-target x86_64-unknown-windows")
else()
    message(FATAL_ERROR
        "No UEFI-capable compiler found!\n"
        "Install one of:\n"
        "  - MinGW-w64: sudo apt install gcc-mingw-w64-x86-64\n"
        "  - LLVM/Clang: sudo apt install clang lld\n")
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
