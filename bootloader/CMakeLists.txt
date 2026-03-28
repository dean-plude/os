# bootloader/CMakeLists.txt — UEFI bootloader build
#
# The bootloader is a UEFI application: a PE32+ shared library with the
# EFI subsystem marker, compiled for x86_64 Windows ABI.
#
# We detect whether to use MinGW-w64 or Clang+LLD and configure
# appropriately.

cmake_minimum_required(VERSION 3.18)

# -------------------------------------------------------------------------
# Detect compiler and set flags accordingly
# -------------------------------------------------------------------------

find_program(MINGW_GCC "x86_64-w64-mingw32-gcc")
find_program(CLANG_BIN "clang")
find_program(LLD_LINK_BIN "lld-link")

if(MINGW_GCC)
    # ---- MinGW-w64 approach ----
    set(BL_CC "${MINGW_GCC}")
    set(BL_LD "${MINGW_GCC}")  # Use GCC as the linker driver

    set(BL_CFLAGS
        -Wall -Wextra -Werror=implicit-function-declaration
        -ffreestanding          # No hosted env
        -fno-stack-protector
        -fno-stack-check
        -fshort-wchar           # UEFI uses 16-bit wchar_t
        -mno-red-zone
        -std=c11
        -O2 -g
        -DGNU_EFI_USE_MS_ABI=1
    )

    set(BL_LDFLAGS
        -nostdlib
        -Wl,-dll                # Produce a DLL
        -Wl,--subsystem,10      # EFI Application subsystem
        -Wl,-e,efi_main         # Entry point
        -Wl,--no-seh            # No SEH (not needed for EFI)
        -Wl,--enable-reloc-section
    )

    message(STATUS "Bootloader: using MinGW-w64 (${MINGW_GCC})")

elseif(CLANG_BIN AND LLD_LINK_BIN)
    # ---- Clang + lld-link approach ----
    set(BL_CC "${CLANG_BIN}")

    set(BL_CFLAGS
        -target x86_64-unknown-windows
        -Wall -Wextra
        -ffreestanding
        -fno-stack-protector
        -fshort-wchar
        -mno-red-zone
        -std=c11
        -O2 -g
        -c
    )

    set(BL_LDFLAGS "")  # We'll use lld-link directly

    message(STATUS "Bootloader: using clang+lld-link")
else()
    message(WARNING
        "Neither MinGW-w64 nor clang+lld-link found.\n"
        "Cannot build UEFI bootloader.\n"
        "Install: sudo apt install gcc-mingw-w64-x86-64\n"
        "      or: sudo apt install clang lld\n"
    )
    return()
endif()

# -------------------------------------------------------------------------
# Source files
# -------------------------------------------------------------------------

set(BOOTLOADER_SOURCES
    src/main.c
    src/elf_loader.c
    src/paging.c
)

set(BOOTLOADER_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include"
)

# -------------------------------------------------------------------------
# Build: MinGW approach
# -------------------------------------------------------------------------

if(MINGW_GCC)
    # Compile each source file
    set(BL_OBJECTS "")
    foreach(SRC IN LISTS BOOTLOADER_SOURCES)
        get_filename_component(OBJ_NAME ${SRC} NAME_WE)
        set(OBJ "${CMAKE_CURRENT_BINARY_DIR}/${OBJ_NAME}.o")

        # Build include flag list
        set(INCLUDE_FLAGS "")
        foreach(INC IN LISTS BOOTLOADER_INCLUDE_DIRS)
            list(APPEND INCLUDE_FLAGS "-I${INC}")
        endforeach()

        add_custom_command(
            OUTPUT  ${OBJ}
            COMMAND ${BL_CC} ${BL_CFLAGS} ${INCLUDE_FLAGS}
                    -c "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
                    -o ${OBJ}
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
            COMMENT "CC (EFI): ${SRC}"
        )
        list(APPEND BL_OBJECTS ${OBJ})
    endforeach()

    # Link to produce bootx64.efi
    add_custom_command(
        OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
        COMMAND ${BL_LD}
                ${BL_OBJECTS}
                ${BL_LDFLAGS}
                -o "${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
        DEPENDS ${BL_OBJECTS}
        COMMENT "Linking bootx64.efi (MinGW)"
    )

    add_custom_target(bootloader ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
    )

    # Copy to build root
    add_custom_command(
        TARGET bootloader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
                "${CMAKE_BINARY_DIR}/bootx64.efi"
        COMMENT "Copying bootx64.efi to build root"
    )
endif()

# -------------------------------------------------------------------------
# Build: Clang+lld-link approach
# -------------------------------------------------------------------------

if(CLANG_BIN AND LLD_LINK_BIN AND NOT MINGW_GCC)
    set(BL_OBJECTS "")
    foreach(SRC IN LISTS BOOTLOADER_SOURCES)
        get_filename_component(OBJ_NAME ${SRC} NAME_WE)
        set(OBJ "${CMAKE_CURRENT_BINARY_DIR}/${OBJ_NAME}.obj")

        set(INCLUDE_FLAGS "")
        foreach(INC IN LISTS BOOTLOADER_INCLUDE_DIRS)
            list(APPEND INCLUDE_FLAGS "-I${INC}")
        endforeach()

        add_custom_command(
            OUTPUT  ${OBJ}
            COMMAND ${BL_CC} ${BL_CFLAGS} ${INCLUDE_FLAGS}
                    "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
                    -o ${OBJ}
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
            COMMENT "Clang (EFI): ${SRC}"
        )
        list(APPEND BL_OBJECTS ${OBJ})
    endforeach()

    add_custom_command(
        OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
        COMMAND ${LLD_LINK_BIN}
                /subsystem:efi_application
                /entry:efi_main
                /out:"${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
                ${BL_OBJECTS}
        DEPENDS ${BL_OBJECTS}
        COMMENT "lld-link: bootx64.efi"
    )

    add_custom_target(bootloader ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
    )

    add_custom_command(
        TARGET bootloader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_BINARY_DIR}/bootx64.efi"
                "${CMAKE_BINARY_DIR}/bootx64.efi"
    )
endif()
